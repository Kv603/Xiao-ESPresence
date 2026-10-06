#pragma once
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include "Control_Core.h"
 
inline bool otaRecord(const std::string &id, const char *state, const char *detail) {
  JsonDocument doc;
  doc["id"] = id;
  doc["state"] = state;
  doc["detail"] = detail;
  std::string value;
  serializeJson(doc, value);
  Preferences prefs;
  if (!prefs.begin("tracker", false)) return false;
  bool ok = prefs.putString("ota", value.c_str()) == value.size();
  prefs.end();
  return ok;
}
inline std::string lastOtaId() {
  Preferences prefs;
  if (!prefs.begin("tracker", true)) return "";
  String value = prefs.getString("ota");
  prefs.end();
  JsonDocument doc;
  if (deserializeJson(doc, value)) return "";
  return doc["id"] | "";
}
inline void otaReport(const std::string &id, const char *state, const char *detail) {
  JsonDocument doc;
  doc["id"] = id;
  doc["state"] = state;
  doc["detail"] = detail;
  publishJson("/ota/status", doc);
}
inline const char *downloadFirmware(const std::string &url, size_t expected, const uint8_t hash[32]) {
  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || expected < sizeof(esp_image_header_t) || expected > partition->size) return "image does not fit inactive slot";
  const bool tls = url.compare(0, 8, "https://") == 0;
  if (tls && (settings.otaCa.empty() || time(nullptr) < 1704067200)) return "HTTPS requires CA and synchronized clock";
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.timeout_ms = 10000;
  config.buffer_size = 4096;
  config.disable_auto_redirect = true;
  if (tls) config.cert_pem = settings.otaCa.c_str();
  esp_http_client_handle_t http = esp_http_client_init(&config);
  if (!http) return "HTTP allocation failed";
  esp_ota_handle_t handle = 0;
  bool begun = false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  const char *error = nullptr;
  if (esp_http_client_open(http, 0) != ESP_OK) error = "connection failed";
  if (!error) {
    const int64_t length = esp_http_client_fetch_headers(http);
    if (esp_http_client_get_status_code(http) != 200 || length != int64_t(expected) || esp_http_client_is_chunked_response(http))
      error = "HTTP 200 and exact Content-Length required";
  }
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[4096]);
  if (!buffer) error = "insufficient download heap";
  if (!error && mbedtls_sha256_starts(&sha, 0) != 0) error = "hash initialization failed";
  size_t received = 0, headerBytes = 0;
  esp_image_header_t header{};
  uint32_t started = millis(), lastData = started;
  while (!error && received < expected) {
    if (WiFi.status() != WL_CONNECTED || uint32_t(millis() - started) > 300000 || uint32_t(millis() - lastData) > 15000) {
      error = "download timeout or network lost";
      break;
    }
    const int n = esp_http_client_read(http, reinterpret_cast<char *>(buffer.get()), int(std::min(size_t(4096), expected - received)));
    if (n < 0) {
      error = "download read failed";
      break;
    }
    if (!n) {
      if (esp_http_client_is_complete_data_received(http)) error = "truncated image";
      delay(10);
      continue;
    }
    lastData = millis();
    // Buffer the image header before opening the flash writer. Reads may be fragmented.
    size_t consumed = 0;
    if (!begun) {
      const size_t take = std::min(size_t(n), sizeof(header) - headerBytes);
      memcpy(reinterpret_cast<uint8_t *>(&header) + headerBytes, buffer.get(), take);
      headerBytes += take;
      consumed = take;
      if (headerBytes == sizeof(header)) {
        if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32C6) {
          error = "not an ESP32-C6 application image";
          break;
        }
        if (esp_ota_begin(partition, expected, &handle) != ESP_OK) {
          error = "cannot begin OTA";
          break;
        }
        begun = true;
        if (esp_ota_write(handle, &header, sizeof(header)) != ESP_OK) {
          error = "header write failed";
          break;
        }
      }
    }
    if (begun && size_t(n) > consumed && esp_ota_write(handle, buffer.get() + consumed, size_t(n) - consumed) != ESP_OK) {
      error = "flash write failed";
      break;
    }
    if (mbedtls_sha256_update(&sha, buffer.get(), n) != 0) {
      error = "hash failed";
      break;
    }
    received += n;
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
    delay(1);
  }
  uint8_t actual[32]{};
  if (!error && (mbedtls_sha256_finish(&sha, actual) != 0 || memcmp(actual, hash, sizeof actual))) error = "SHA-256 mismatch";
  mbedtls_sha256_free(&sha);
  esp_http_client_close(http);
  esp_http_client_cleanup(http);
  if (error) {
    if (begun) esp_ota_abort(handle);
    return error;
  }
  if (!begun || esp_ota_end(handle) != ESP_OK) return "application validation failed";
  esp_app_desc_t description{};
  if (esp_ota_get_partition_description(partition, &description) != ESP_OK)
    return "application descriptor missing";
  if (esp_ota_set_boot_partition(partition) != ESP_OK) return "cannot activate image";
  return nullptr;
}

inline void processOta(JsonVariantConst command, const std::string &id) {
  const std::string url = command["url"] | "", digest = command["sha256"] | "";
  uint8_t hash[32];
  if (!command["size"].is<uint32_t>() || !control::firmwareUrl(url) || !decodeHexString(digest, hash, 32)) {
    otaReport(id, "rejected", "url, size and sha256 required");
    return;
  }
  const auto *slot = esp_ota_get_next_update_partition(nullptr);
  const uint32_t bytes = command["size"].as<uint32_t>();
  if (!slot || bytes < sizeof(esp_image_header_t) || bytes > slot->size) {
    otaReport(id, "rejected", "image size outside slot");
    return;
  }
  if (id == lastOtaId()) {
    otaReport(id, "duplicate", "use a new request ID to retry");
    return;
  }
  if (!otaRecord(id, "accepted", "download pending")) {
    otaReport(id, "rejected", "NVS unavailable");
    return;
  }
  JsonDocument accepted;
  accepted["id"] = id;
  accepted["state"] = "accepted";
  if (!waitForAck(publishJson("/ota/status", accepted))) {
    otaRecord(id, "failed", "acceptance not acknowledged");
    return;
  }
  ProcessingOTA = true;
  if (!prepareForFirmwareUpdate()) {
    ProcessingOTA = false;
    otaRecord(id, "failed", "cannot stop BLE");
    otaReport(id, "failed", "cannot stop BLE");
    return;
  }
  stopMqtt();
  // No queued setting mutation may run after the OTA transaction begins.
  Command *stale = nullptr;
  while (xQueueReceive(commandQueue, &stale, 0) == pdTRUE) delete stale;
  const char *error = downloadFirmware(url, bytes, hash);
  if (!error) {
    otaRecord(id, "installed", "awaiting reboot");
    delay(100);
    ESP.restart();
    return;
  }
  otaRecord(id, "failed", error);
  Serial.printf("OTA failed: %s\n", error);
  ProcessingOTA = false;
  resumeAfterFirmwareUpdate();
  startMqtt(settings);
}
