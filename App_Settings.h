#pragma once
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/x509_crt.h>
#include <string>
#include "secrets.h"

// Enable Arduino-style password protected OTA
#ifndef ARDUINO_OTA
#define ARDUINO_OTA 1
#endif

// A dedicated password can override the existing private HTTP_PASS credential.
#ifndef ARDUINO_OTA_PASSWORD
#ifdef HTTP_PASS
#define ARDUINO_OTA_PASSWORD HTTP_PASS
#else
#define ARDUINO_OTA_PASSWORD ""
#endif
#endif

#ifndef MQTT_CA_PEM
#define MQTT_CA_PEM ""
#endif
#ifndef OTA_CA_PEM
#define OTA_CA_PEM ""
#endif
#ifndef TRACKER_ROOM
#define TRACKER_ROOM ""
#endif
#ifndef TRACKER_NODE_LABEL
#define TRACKER_NODE_LABEL ""
#endif
#ifndef TRACKER_MQTT_URI
#define TRACKER_MQTT_URI ""
#endif

struct AppSettings {
  std::string room = TRACKER_ROOM, label = TRACKER_NODE_LABEL;
  std::string uri = TRACKER_MQTT_URI, user = MQTT_USER, password = MQTT_PASS;
  std::string mqttCa = MQTT_CA_PEM, otaCa = OTA_CA_PEM;
  uint32_t revision = 0;
};
static AppSettings settings;
static bool hasSavedConnectionSettings = false;
constexpr size_t MAX_SETTINGS_BYTES = 16000;
inline bool safeText(const std::string &s, size_t limit) {
  return s.size() <= limit && s.find('\0') == std::string::npos;
}
inline bool validCa(const std::string &pem) {
  if (pem.empty()) return true;
  if (!safeText(pem, 4096)) return false;
  mbedtls_x509_crt cert;
  mbedtls_x509_crt_init(&cert);
  const int result = mbedtls_x509_crt_parse(&cert, reinterpret_cast<const uint8_t *>(pem.c_str()), pem.size() + 1);
  mbedtls_x509_crt_free(&cert);
  return result == 0;
}
inline bool validSettings(const AppSettings &s) {
  const bool tls = s.uri.compare(0, 8, "mqtts://") == 0;
  const size_t prefix = tls ? 8 : s.uri.compare(0, 7, "mqtt://") == 0 ? 7
                                                                      : 0;
  if (!prefix || s.uri.size() <= prefix || !safeText(s.uri, 255)) return false;
  for (size_t i = prefix; i < s.uri.size(); ++i) {
    const unsigned char c = s.uri[i];
    if (c <= 32 || c >= 127 || c == '@' || c == '/' || c == '?' || c == '#' || c == '\\') return false;
  }
  return safeText(s.room, 63) && safeText(s.label, 63) && safeText(s.user, 128) && safeText(s.password, 256) && validCa(s.mqttCa) && validCa(s.otaCa) && (!tls || !s.mqttCa.empty());
}
inline void settingsJson(JsonDocument &doc, const AppSettings &s, bool secrets) {
  doc["version"] = 1;
  doc["revision"] = s.revision;
  doc["room"] = s.room;
  doc["label"] = s.label;
  doc["uri"] = s.uri;
  doc["user"] = s.user;
  if (secrets) {
    doc["password"] = s.password;
    doc["mqtt_ca"] = s.mqttCa;
    doc["ota_ca"] = s.otaCa;
  } else {
    doc["password_set"] = !s.password.empty();
    doc["mqtt_ca_set"] = !s.mqttCa.empty();
    doc["ota_ca_set"] = !s.otaCa.empty();
  }
}
inline bool patchSettings(JsonVariantConst doc, AppSettings &s) {
  struct Field {
    const char *name;
    std::string *destination;
    size_t limit;
  };
  Field fields[] = { { "room", &s.room, 63 }, { "label", &s.label, 63 }, { "uri", &s.uri, 255 }, { "user", &s.user, 128 }, { "password", &s.password, 256 }, { "mqtt_ca", &s.mqttCa, 4096 }, { "ota_ca", &s.otaCa, 4096 } };
  for (const auto &f : fields)
    if (!doc[f.name].isNull()) {
      if (!doc[f.name].is<const char *>()) return false;
      std::string v = doc[f.name].as<std::string>();
      if (!safeText(v, f.limit)) return false;
      *f.destination = std::move(v);
    }
  return validSettings(s);
}
inline bool saveSettings(const AppSettings &s) {
  JsonDocument doc;
  settingsJson(doc, s, true);
  std::string encoded;
  serializeJson(doc, encoded);
  if (doc.overflowed() || encoded.size() > MAX_SETTINGS_BYTES) return false;
  Preferences prefs;
  if (!prefs.begin("tracker", false)) return false;
  const bool ok = prefs.putBytes("settings", encoded.data(), encoded.size()) == encoded.size();
  prefs.end();
  return ok;
}
inline bool loadSettings() {
  if (settings.uri.empty()) settings.uri = std::string("mqtt://") + MQTT_SERVER + ":" + std::to_string(MQTT_PORT);
  // Compatibility with the previous firmware's namespace and keys.
  Preferences old;
  if (old.begin("mmW32", true)) {
    settings.room = old.getString("Room", settings.room.c_str()).c_str();
    settings.label = old.getString("Where", settings.label.c_str()).c_str();
    if (old.isKey("MQTTServer") && std::string(TRACKER_MQTT_URI).empty())
      settings.uri = std::string("mqtt://") + old.getString("MQTTServer").c_str() + ":" + std::to_string(MQTT_PORT);
    old.end();
  }
  Preferences prefs;
  if (!prefs.begin("tracker", false)) return false;
  if (!prefs.isKey("settings")) {
    prefs.end();
    return validSettings(settings);
  }
  const size_t n = prefs.getBytesLength("settings");
  if (!n || n > MAX_SETTINGS_BYTES) {
    prefs.end();
    return false;
  }
  std::string encoded(n, '\0');
  const size_t read = prefs.getBytes("settings", &encoded[0], n);
  prefs.end();
  JsonDocument doc;
  if (read != n || deserializeJson(doc, encoded) || doc["version"] != 1 || !doc["revision"].is<uint32_t>()) return false;
  AppSettings candidate = settings;
  if (!patchSettings(doc.as<JsonVariantConst>(), candidate)) return false;
  candidate.revision = doc["revision"].as<uint32_t>();
  settings = std::move(candidate);
  hasSavedConnectionSettings = true;
  return true;
}
