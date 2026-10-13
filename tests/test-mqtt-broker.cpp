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
  const char *bleTopic = "espresense/devices/iBeacon:699ebc80-e1f3-11e3-9a0f-0cf3ee3bc012-1-52753/c6_10bda39f8f98";
  const std::string blePayload = "{\"id\":\"iBeacon:699ebc80-e1f3-11e3-9a0f-0cf3ee3bc012-1-52753\",\"distance\":1.25}";
  assert(mqttClient.publish(bleTopic, 0, false, blePayload.c_str(), blePayload.size()) == 1);
  checkpoint("ble-qos0");
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
  assert(resultStatus == "saved" && haRetiredDiscovery.size() == 1);
  checkpoint("pending-cleanup");
  drop["room"] = "kitchen"; brokerCall(drop); event(MQTT_EVENT_DISCONNECTED); stopMqtt();
  settings = AppSettings(); hasSavedConnectionSettings = false;
  assert(haRetiredDiscovery.empty() && loadSettings() && haRetiredDiscovery.size() == 1);
  assert(settings.room == "Kitchen");
  assert(startMqtt(settings)); serviceHomeAssistant(settings.label, "broker-test");
  assert(haRetiredDiscovery.empty());
  checkpoint("room-change");
  stopMqtt(); checkpoint("manual-offline");
  std::cout << "Broker-backed MQTT acceptance passed.\n";
}
