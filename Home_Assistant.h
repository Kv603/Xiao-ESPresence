#pragma once
#include <ArduinoJson.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace homeassistant {
struct Topics {
  std::string device, discovery, count, status;
  Topics() = default;
  Topics(const std::string &node, const std::string &room) {
    device = "espresense_" + node;
    discovery = "homeassistant/sensor/" + device + "/" + room + "/config";
    const std::string base = "devices/" + device + "/" + room;
    count = base + "/count";
    status = base + "/status";
  }
};

inline void discoveryJson(JsonDocument &doc, const Topics &topics, const std::string &label,
                          const std::string &node, const char *firmware) {
  doc["name"] = "Tracked BLE devices";
  doc["unique_id"] = topics.device + "_device_count";
  doc["state_topic"] = topics.count;
  doc["availability_topic"] = topics.status;
  doc["payload_available"] = "online";
  doc["payload_not_available"] = "offline";
  doc["state_class"] = "measurement";
  doc["unit_of_measurement"] = "devices";
  doc["qos"] = 1;
  doc["device"]["identifiers"].to<JsonArray>().add(topics.device);
  doc["device"]["name"] = label.empty() ? node : label;
  doc["device"]["manufacturer"] = "Seeed Studio";
  doc["device"]["model"] = "XIAO ESP32-C6";
  doc["device"]["sw_version"] = firmware;
}

struct Reporting {
  static constexpr uint32_t MIN_INTERVAL = 1000, REFRESH_INTERVAL = 31000;
  bool sent = false, replay = true;
  uint32_t lastCount = 0, lastSent = 0;
  void reset() { sent = false; replay = true; }
  bool due(uint32_t count, uint32_t now) const {
    const uint32_t elapsed = now - lastSent;
    return !sent || (elapsed >= MIN_INTERVAL && (replay || count != lastCount || elapsed >= REFRESH_INTERVAL));
  }
  void published(uint32_t count, uint32_t now) {
    sent = true; replay = false; lastCount = count; lastSent = now;
  }
};

// Birth messages have their own small assembly so retained HA messages never
// bypass the retained-command rejection in control::Assembly.
class Birth {
  char payload[6] = {};
  size_t used = 0;
  bool active = false;
 public:
  void reset() { used = 0; active = false; }
  bool accepts(const char *topic, size_t length, size_t offset) const {
    return offset ? active : topic && length == 20 && std::memcmp(topic, "homeassistant/status", 20) == 0;
  }
  bool add(const char *data, size_t length, size_t offset, size_t total) {
    if (!offset) { reset(); active = total == sizeof payload; }
    if (!active || !data || offset != used || total != sizeof payload || length > sizeof payload - used) {
      reset(); return false;
    }
    std::memcpy(payload + used, data, length); used += length;
    if (used != sizeof payload) return false;
    const bool online = std::memcmp(payload, "online", sizeof payload) == 0;
    reset(); return online;
  }
};
}  // namespace homeassistant
