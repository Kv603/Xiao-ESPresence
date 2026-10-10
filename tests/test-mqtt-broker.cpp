// Run the production transport and settings transaction with MQTT operations
// forwarded over stdio to MQTT.js and a local Aedes broker. No device or private
// broker settings are accessed by this adapter.
#define MQTT_BROKER_TEST
#include "test-mqtt.cpp"

static JsonDocument checkpoint(const char *name) {
  JsonDocument request; request["op"] = "check"; request["name"] = name; return brokerCall(request);
}
int main() {
  nodeId = "c6-broker-test"; controlPrefix = "bletracker/" + nodeId;
  autoConnect = true; testCount = 2;
  assert(startMqtt(settings)); serviceHomeAssistant(settings.label, "broker-test");
  checkpoint("startup");
  JsonDocument drop; drop["op"] = "drop"; brokerCall(drop);
  event(MQTT_EVENT_DISCONNECTED); checkpoint("lwt");
  assert(startMqtt(settings)); serviceHomeAssistant(settings.label, "broker-test");
  checkpoint("reconnected");
  JsonDocument birth; birth["op"] = "birth";
  const JsonDocument received = brokerCall(birth);
  const std::string payload = received["payload"].as<std::string>();
  data("homeassistant/status", payload.c_str(), received["retain"].as<bool>());
  clockMs += 1000; serviceHomeAssistant(settings.label, "broker-test");
  checkpoint("ha-restart");
  JsonDocument command; command["revision"] = 0; command["room"] = "Kitchen";
  processSettings(command.as<JsonVariantConst>(), "room-change");
  assert(resultStatus == "saved"); serviceHomeAssistant(settings.label, "broker-test");
  checkpoint("room-change");
  stopMqtt(); checkpoint("manual-offline");
  std::cout << "Broker-backed MQTT acceptance passed.\n";
}
