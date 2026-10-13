#pragma once
#include <ArduinoJson.h>
#include <string>
#include <vector>

namespace homeassistant {
struct RetiredDiscovery {
  std::string broker, topic;
};
constexpr size_t MAX_RETIRED_DISCOVERY = 16;

inline void cleanupJson(JsonDocument &doc, const std::vector<RetiredDiscovery> &pending) {
  auto entries = doc["discovery_cleanup"].to<JsonArray>();
  for (const auto &old : pending) {
    auto entry = entries.add<JsonObject>();
    entry["broker"] = old.broker;
    entry["topic"] = old.topic;
  }
}

inline bool loadCleanup(JsonVariantConst value, std::vector<RetiredDiscovery> &pending) {
  std::vector<RetiredDiscovery> loaded;
  // Older version-1 settings have no cleanup journal.
  if (!value.isNull()) {
    if (!value.is<JsonArrayConst>() || value.size() > MAX_RETIRED_DISCOVERY) return false;
    for (JsonVariantConst entry : value.as<JsonArrayConst>()) {
      if (!entry["broker"].is<const char *>() || !entry["topic"].is<const char *>()) return false;
      RetiredDiscovery old{ entry["broker"].as<std::string>(), entry["topic"].as<std::string>() };
      if (old.broker.empty() || old.broker.size() > 255 || old.broker.find('\0') != std::string::npos ||
          old.topic.empty() || old.topic.size() > 255 || old.topic.find_first_of("+#") != std::string::npos ||
          old.topic.find('\0') != std::string::npos || old.topic.compare(0, 21, "homeassistant/sensor/") != 0 ||
          old.topic.size() < 7 || old.topic.compare(old.topic.size() - 7, 7, "/config") != 0) return false;
      loaded.push_back(std::move(old));
    }
  }
  pending = std::move(loaded);
  return true;
}
}  // namespace homeassistant
