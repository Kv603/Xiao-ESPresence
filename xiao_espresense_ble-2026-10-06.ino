#include <Arduino.h>


#if !defined(CONFIG_IDF_TARGET_ESP32C6) || !defined(ARDUINO_XIAO_ESP32C6)
#error "Select XIAO_ESP32C6 with Arduino-ESP32 3.3.8"
#endif
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <esp_partition.h>
#include <atomic>
#include <memory>
#include <time.h>
#include "App_Settings.h"
#include "MQTT_Transport.h"

#define SCAN_BLE
constexpr char FIRMWARE_VERSION[] = "c6-ble-1.0.0";
std::atomic<bool> ProcessingOTA{ false };
char Room[64] = {};
unsigned long Publish_BLE_Attempts = 0, Total_BLE_Records_Last_Pub = 0, Skipped_BLE_Records_Last_Pub = 0;
time_t LastPublishTimeBLE = 0, TimeNow = 0;
std::string hostName() {
  return nodeId;
}
#include "Beacon_Scanner.h"
#include "OTA_Update.h"

// Optionally enable the ArduinoOTA from the ESP32-Arduino core
#if ARDUINO_OTA
#include <ArduinoOTA.h>
#endif


static bool settingsReady = false;
static uint32_t nextConnect = 0, lastActiveScan = 0, lastDiagnostics = 0;
static bool networkChanged(const AppSettings &a, const AppSettings &b) {
  return a.uri != b.uri || a.user != b.user || a.password != b.password || a.mqttCa != b.mqttCa;
}
static void result(const std::string &id, const char *status, const char *detail) {
  JsonDocument doc;
  doc["id"] = id;
  doc["status"] = status;
  doc["detail"] = detail;
  uint32_t bleRevision;
  if (bleConfigurationRevision(bleRevision)) doc["ble_revision"] = bleRevision;
  doc["revision"] = settings.revision;
  publishJson("/config/result", doc);
}
static void state(const std::string &id) {
  JsonDocument doc;
  settingsJson(doc, settings, false);
  doc["id"] = id;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["node_id"] = nodeId;
  doc["unix_time"] = int64_t(time(nullptr));
  doc["free_heap"] = ESP.getFreeHeap();
  doc["minimum_free_heap"] = ESP.getMinFreeHeap();
  doc["running_slot"] = esp_ota_get_running_partition()->label;
  if (!appendBLEConfigurationState(doc)) {
    result(id, "error", "BLE busy");
    return;
  }
  // Full whitelist responses exceed the reliable outbox bound; send directly at QoS 0.
  if (!doc.overflowed() && mqttHandle && mqttOnline) {
    std::string payload;
    serializeJson(doc, payload);
    esp_mqtt_client_publish(mqttHandle, (controlPrefix + "/config/state").c_str(), payload.c_str(), int(payload.size()), 0, false);
  }
}
static bool waitForConnection(uint32_t timeout) {
  const uint32_t start = millis();
  while (uint32_t(millis() - start) < timeout) {
    if (mqttOnline && mqttSubscriptions >= 3) return true;
    doBLE();
    delay(20);
  }
  return false;
}
static void processSettings(JsonVariantConst command, const std::string &id) {
  if (!command["revision"].is<uint32_t>() || command["revision"].as<uint32_t>() != settings.revision || settings.revision == UINT32_MAX) {
    result(id, "error", "stale or missing settings revision");
    return;
  }
  AppSettings candidate = settings;
  if (!patchSettings(command, candidate)) {
    result(id, "error", "invalid settings or CA certificate");
    return;
  }
  ++candidate.revision;
  const bool reconnect = networkChanged(settings, candidate);
  if (reconnect) {
    JsonDocument accepted;
    accepted["id"] = id;
    accepted["status"] = "testing";
    if (!waitForAck(publishJson("/config/result", accepted))) {
      result(id, "error", "acknowledgement timeout");
      return;
    }
    if (!startMqtt(candidate) || !waitForConnection(30000)) {
      startMqtt(settings);
      waitForConnection(15000);
      result(id, "error", "candidate broker failed; previous settings restored");
      return;
    }
    JsonDocument probe;
    probe["id"] = id;
    probe["status"] = "candidate_connected";
    if (!waitForAck(publishJson("/config/result", probe))) {
      startMqtt(settings);
      waitForConnection(15000);
      result(id, "error", "candidate publish failed; previous settings restored");
      return;
    }
  }
  if (!saveSettings(candidate)) {
    if (reconnect) {
      startMqtt(settings);
      waitForConnection(15000);
    }
    result(id, "error", "NVS write failed; previous settings retained");
    return;
  }
  settings = std::move(candidate);
  hasSavedConnectionSettings = true;
  strlcpy(Room, settings.room.c_str(), sizeof Room);
  result(id, "saved", "settings activated");
}
static void processCommand(const Command &command) {
  JsonDocument doc;
  if (deserializeJson(doc, command.payload, DeserializationOption::NestingLimit(4)) || !doc.is<JsonObject>()) {
    result("", "error", "invalid JSON");
    return;
  }
  const std::string id = doc["id"] | "";
  if (!control::requestId(id)) {
    result("", "error", "id must be 1-64 letters, digits, hyphens or underscores");
    return;
  }
  if (command.topic == controlPrefix + "/config/get") {
    state(id);
    return;
  }
  if (command.topic == controlPrefix + "/ota/set") {
    processOta(doc.as<JsonVariantConst>(), id);
    return;
  }
  if (command.topic != controlPrefix + "/config/set") return;
  const std::string operation = doc["operation"] | "";
  if (operation == "settings") processSettings(doc.as<JsonVariantConst>(), id);
  else if (operation == "active_scan") {
    requestActiveBLEScan();
    result(id, "accepted", "five-second active scan queued");
  } else {
    const char *error = editBLEConfiguration(doc.as<JsonVariantConst>());
    result(id, error ? "error" : "saved", error ? error : "BLE configuration activated");
  }
}
static void printPartitions() {
  auto iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (iterator) {
    const auto *p = esp_partition_get(iterator);
    Serial.printf("PARTITION %s offset=0x%06lx size=0x%06lx\n", p->label, (unsigned long)p->address, (unsigned long)p->size);
    iterator = esp_partition_next(iterator);
  }
  esp_partition_iterator_release(iterator);
}


#if ARDUINO_OTA
static bool arduinoOtaStarted = false, arduinoOtaOwnsUpdate = false, arduinoOtaFailed = false;
#endif

void SetMyHostname(const char *hostname) {
  Serial.printf("\nHostname '%s'\n", hostname);

  WiFi.setHostname(hostname);
#if ARDUINO_OTA
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
  ArduinoOTA.setRebootOnSuccess(true);
  ArduinoOTA.onStart([]() {
    // Both update paths run synchronously in loop(); never run their writers together.
    if (ArduinoOTA.getCommand() != U_FLASH || ProcessingOTA.exchange(true)) {
      Update.abort();
      return;
    }
    arduinoOtaOwnsUpdate = true;
    pauseBLEForOTA();
    if (bleScannerIsRunning()) {
      arduinoOtaFailed = true;
      Update.abort();
      return;
    }
    stopMqtt();
    Command *stale = nullptr;
    while (commandQueue && xQueueReceive(commandQueue, &stale, 0) == pdTRUE) delete stale;
    Serial.println("ArduinoOTA application update started");
  });
  ArduinoOTA.onEnd([]() {
    // Keep BLE/MQTT paused until the core reboots into the new image.
    Serial.println("ArduinoOTA complete; rebooting");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("ArduinoOTA error=%u\n", unsigned(error));
    arduinoOtaFailed = arduinoOtaOwnsUpdate;
  });
  if (!ARDUINO_OTA_PASSWORD[0]) Serial.println("ArduinoOTA disabled: configure ARDUINO_OTA_PASSWORD");
#endif
}

#if ARDUINO_OTA
static void serviceArduinoOta() {
  if (ProcessingOTA || !ARDUINO_OTA_PASSWORD[0]) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (arduinoOtaStarted) ArduinoOTA.end();
    arduinoOtaStarted = false;
    return;
  }
  if (!arduinoOtaStarted) {
    ArduinoOTA.begin();
    arduinoOtaStarted = true;
    Serial.printf("ArduinoOTA ready: %s.local port 3232\n", nodeId.c_str());
  }
  ArduinoOTA.handle();
  // Error callbacks can run before the core finishes cleanup. Resume afterwards.
  if (arduinoOtaFailed) {
    Update.abort();
    arduinoOtaFailed = arduinoOtaOwnsUpdate = false;
    ProcessingOTA = false;
    setupBLE(false);
    // The normal loop reconnects MQTT using the saved connection settings.
  }
}
#endif


void setup() {
  Serial.begin(115200);
  delay(300);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char id[13];
  snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  nodeId = std::string("c6-") + id;
  controlPrefix = "bletracker/" + nodeId;
  Serial.printf("BOOT %s %s flash=%lu\n", FIRMWARE_VERSION, nodeId.c_str(), (unsigned long)ESP.getFlashChipSize());
  printPartitions();
  commandQueue = xQueueCreate(2, sizeof(Command *));
  settingsReady = commandQueue && loadSettings();
  if (!settingsReady) Serial.println("Settings unavailable/invalid; USB reprovisioning required (NVS preserved)");
  strlcpy(Room, settings.room.c_str(), sizeof Room);


#if defined(CONFIG_IDF_TARGET_ESP32C6) || defined(ARDUINO_XIAO_ESP32C6)
  //Seeed Studio XIAO ESP32-C6
  Serial.print("Activating RF switch control...");
#ifndef WIFI_ENABLE
  Serial.print("missing WIFI_ENABLE definition, using pin 3...");
#define WIFI_ENABLE 3
#endif
  digitalWrite(WIFI_ENABLE, LOW);  // digitalWrite(3, LOW); // Activate RF switch control
  delay(100);
#ifndef WIFI_ANT_CONFIG
  Serial.println("missing WIFI_ANT_CONFIG definition, using pin 14...");
#define WIFI_ANT_CONFIG 14
#endif
  pinMode(WIFI_ANT_CONFIG, OUTPUT);     // pinMode(14, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, HIGH);  // digitalWrite(14, HIGH); // Use external antenna
  Serial.println(".  Now using external antenna");
#else
  Serial.println("No C6 selectable antenna defined!");
#endif

  WiFi.mode(WIFI_STA);

  SetMyHostname(nodeId.c_str());

#if !defined(ARDUINO_OTA) || !defined(ARDUINO_OTA_PASSWORD)
  Serial.println("ARDUINO_OTA or ARDUINO_OTA_PASSWORD missing!");
#endif

  WiFi.setAutoReconnect(true);
  Serial.printf("Connecting WiFi to '%s'\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setupBLE(false);
  Serial.printf("READY BLE=%d heap=%lu\n", bleScannerIsRunning(), (unsigned long)ESP.getFreeHeap());
}


void loop() {
#if ARDUINO_OTA
  serviceArduinoOta();
#endif
  if (Serial.available() && Serial.read() == '?') {
    Serial.printf("NETWORK ip=%s broker=%s settings_ready=%d\n", WiFi.localIP().toString().c_str(), settings.uri.c_str(), settingsReady);
    Serial.printf("STATUS %s node=%s wifi=%d mqtt=%d heap=%lu min_heap=%lu\n", FIRMWARE_VERSION, nodeId.c_str(),
                  WiFi.status() == WL_CONNECTED, mqttOnline.load(), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
    printPartitions();
  }
  TimeNow = time(nullptr);
#if defined(MQTT_SERVER_BACKUP) && defined(TESTNETPREFIX)
  // Preserve the private configuration's existing LAN bootstrap fallback. Explicit
  // URI and persisted settings always win; this is not a TLS downgrade path.
  if (settingsReady && !hasSavedConnectionSettings && std::string(TRACKER_MQTT_URI).empty() && !mqttOnline && millis() > 30000 && WiFi.status() == WL_CONNECTED && WiFi.localIP().toString().startsWith(TESTNETPREFIX) && settings.uri == std::string("mqtt://") + MQTT_SERVER + ":" + std::to_string(MQTT_PORT)) {
    settings.uri = std::string("mqtt://") + MQTT_SERVER_BACKUP + ":" + std::to_string(MQTT_PORT);
    stopMqtt();
    Serial.println("Using configured LAN bootstrap broker");
  }
#endif
  if (settingsReady && !ProcessingOTA && WiFi.status() == WL_CONNECTED && !mqttHandle && int32_t(millis() - nextConnect) >= 0) {
    startMqtt(settings);
    nextConnect = millis() + 5000;
  }
  if (mqttAnnounce.exchange(false)) {
    JsonDocument doc;
    doc["state"] = "running";
    doc["firmware"] = FIRMWARE_VERSION;
    doc["free_heap"] = ESP.getFreeHeap();
    Preferences prefs;
    if (prefs.begin("tracker", true)) {
      JsonDocument previous;
      const String saved = prefs.getString("ota");
      prefs.end();
      if (!deserializeJson(previous, saved)) doc["last_update"].set(previous.as<JsonVariantConst>());
    }
    publishJson("/ota/status", doc);
    Serial.printf("MQTT connected heap=%lu\n", (unsigned long)ESP.getFreeHeap());
  }
  Command *command = nullptr;
  if (commandQueue && xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
    std::unique_ptr<Command> owned(command);
    processCommand(*owned);
  }
  if (!ProcessingOTA) {
    doBLE();
    if (uint32_t(millis() - lastActiveScan) > 997000) {
      requestActiveBLEScan();
      lastActiveScan = millis();
    }
  }
  if (uint32_t(millis() - lastDiagnostics) >= 61000) {
    Serial.printf("HEALTH wifi=%d mqtt=%d heap=%lu min_heap=%lu dropped=%u\n", WiFi.status() == WL_CONNECTED,
                  mqttOnline.load(), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(), droppedCommands.load());
    lastDiagnostics = millis();

    if (mqttOnline) {
      const char *myip = WiFi.localIP().toString().c_str();
      esp_mqtt_client_publish(mqttHandle, (controlPrefix + "/config/ip").c_str(), myip, strlen(myip), 0, false);
    }
  }

  delay(10);
}
///EOF///