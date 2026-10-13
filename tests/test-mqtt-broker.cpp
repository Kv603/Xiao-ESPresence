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
  // Real broker wildcard delivery, including the parent topic and retained rejection.
  ledController.begin(settings.led);
  JsonDocument retained; retained["op"] = "led-preload"; retained["topic"] = "sensor/lobby/blue";
  retained["payload"] = "100"; brokerCall(retained);
  JsonDocument led;
  assert(!deserializeJson(led,R"({"brightness":128,"groups":[{"group":1,"topics":["sensor/lobby/#","sensor/+/red","motion/woodshopdoor/#"]},{"group":2,"topics":["sensor/lobby/#","motion/woodshopdoor/#"]}]})"));
  led["revision"] = settings.revision;
  processLedConfiguration(led.as<JsonVariantConst>(),"led-broker"); assert(resultStatus == "saved");
  auto deliver = [](const char *topic, const char *value, bool publish = true) {
    JsonDocument request; request["op"] = "led-message"; request["topic"] = topic;
    request["payload"] = value; request["publish"] = publish;
    auto reply = brokerCall(request);
    const std::string actualTopic = reply["topic"].as<std::string>(), actualPayload = reply["payload"].as<std::string>();
    data(actualTopic.c_str(),actualPayload.c_str(),reply["retain"].as<bool>());
    ledController.service(clockMs,true);
  };
  deliver("sensor/lobby/blue","100",false); assert(ledController.lastFrame()[1] == leds::RGB{});
  deliver("sensor/lobby/green","100"); deliver("sensor/lobby/red","100");
  assert((ledController.lastFrame()[1] == leds::RGB{255,255,0} && ledController.lastFrame()[5] == leds::RGB{255,255,0}));
  deliver("sensor/lobby","0"); assert(ledController.lastFrame()[1] == leds::RGB{});
  deliver("sensor/lobby/mmwave","100"); assert((ledController.lastFrame()[1] == leds::RGB{255,255,255}));
  clockMs += 60000; ledController.service(clockMs,true); assert(ledController.lastFrame()[1] == leds::RGB{});
  deliver("motion/woodshopdoor","25");
  assert((ledController.lastFrame()[1] == leds::RGB{64,64,64}));
  deliver("motion/woodshopdoor/human","25");
  assert((ledController.lastFrame()[1] == leds::RGB{128,128,128} && ledController.lastFrame()[5] == leds::RGB{128,128,128}));
  deliver("motion/woodshopdoor/blue","100");
  assert((ledController.lastFrame()[1] == leds::RGB{128,128,255}));
  deliver("motion/woodshopdoor/human","0");
  assert((ledController.lastFrame()[1] == leds::RGB{64,64,64}));
  deliver("motion/woodshopdoor","0"); assert(ledController.lastFrame()[1] == leds::RGB{});
  deliver("motion/woodshopdoor/human","lime"); assert((ledController.lastFrame()[1] == leds::RGB{0,191,0}));
  clockMs += 60000; ledController.service(clockMs,true); assert(ledController.lastFrame()[1] == leds::RGB{});
  JsonDocument clear; clear["revision"] = settings.revision;
  clear["groups"][0]["group"] = 1; clear["groups"][0]["topics"].to<JsonArray>();
  clear["groups"][1]["group"] = 2; clear["groups"][1]["topics"].to<JsonArray>();
  processLedConfiguration(clear.as<JsonVariantConst>(),"led-broker-clear"); serviceLedSubscriptions();
  assert(resultStatus == "saved" && unsubscriptions.size() == 3);
  stopMqtt(); checkpoint("manual-offline");
  std::cout << "Broker-backed MQTT acceptance passed.\n";
}
