#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>
#include "stubs/mqtt_client.h"

#include "../App_Settings.h"
struct { void printf(const char *, ...) {} void println(const char *) {} } Serial;
struct IPAddress {
  struct Text { const char *c_str() const { return "127.0.0.1"; } };
  Text toString() const { return {}; }
};
struct { int hostByName(const char *, IPAddress &) { return 1; } } Network;
static uint32_t clockMs = 0, testCount = 0;
static bool countAvailable = true, enqueueAccept = true, enqueueFull = false, publishAccept = true, ackPublish = true, startAccept = true;
static bool autoConnect = false, rejectProbe = false, disconnectOnCleanup = false;
static std::string rejectRoomTopic, rejectBrokerUri;
static std::vector<std::string> attemptedBrokers;
uint32_t millis() { return clockMs; }
void delay(unsigned ms);
#include "../build/native-c6/mqtt-room-test.inc"
bool bleReportableDeviceCount(uint32_t &count) {
  if (!countAvailable) return false;
  count = testCount; return true;
}
struct Message { std::string topic, payload; int qos; bool retain, synchronous; int id; };
struct MockMqttClient {
  esp_mqtt_client_config_t config;
  std::string willTopic, willPayload;
  MqttEventHandler handler = nullptr;
};
static std::vector<Message> messages;
static std::vector<std::pair<std::string, int>> subscriptions;
static std::vector<std::string> unsubscriptions;
static std::vector<std::string> operations;
static int nextId = 1, pendingAck = -1;
#include "../MQTT_Transport.h"
static std::vector<Command *> commands;
#ifdef MQTT_BROKER_TEST
static JsonDocument brokerCall(const JsonDocument &request) {
  std::string encoded; serializeJson(request, encoded);
  std::cout << "WIRE " << encoded << std::endl;
  std::string response; assert(std::getline(std::cin, response));
  JsonDocument reply; assert(!deserializeJson(reply, response));
  if (!reply["ok"].as<bool>()) std::cerr << response << std::endl;
  assert(reply["ok"].as<bool>()); return reply;
}
#endif

int xQueueSend(QueueHandle_t, const void *item, unsigned) {
  commands.push_back(*static_cast<Command *const *>(item)); return pdTRUE;
}
static void event(int id, int msgId = 0) {
  esp_mqtt_event_t e; e.client = mqttHandle; e.msg_id = msgId;
  mqttEvent(nullptr, nullptr, id, &e);
}
void delay(unsigned ms) {
  clockMs += ms;
  if (ackPublish && pendingAck >= 0) { const int id = pendingAck; pendingAck = -1; event(MQTT_EVENT_PUBLISHED, id); }
}
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config) {
  auto *client = new MockMqttClient;
  client->config = *config;
  client->willTopic = config->session.last_will.topic;
  client->willPayload.assign(config->session.last_will.msg, config->session.last_will.msg_len);
  return client;
}
int esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int, MqttEventHandler handler, void *) {
  c->handler = handler; return ESP_OK;
}
int esp_mqtt_client_start(esp_mqtt_client_handle_t c) {
  attemptedBrokers.emplace_back(c->config.broker.address.uri);
  if (!startAccept || c->willTopic == rejectRoomTopic || attemptedBrokers.back() == rejectBrokerUri) return -1;
#ifdef MQTT_BROKER_TEST
  JsonDocument request; request["op"] = "connect"; request["clientId"] = nodeId;
  request["will"]["topic"] = c->willTopic; request["will"]["payload"] = c->willPayload;
  request["will"]["qos"] = c->config.session.last_will.qos;
  request["will"]["retain"] = bool(c->config.session.last_will.retain);
  brokerCall(request);
#endif
  if (autoConnect) {
    event(MQTT_EVENT_CONNECTED);
    for (int id : controlSubscriptionIds) event(MQTT_EVENT_SUBSCRIBED, id);
  }
  return ESP_OK;
}
int esp_mqtt_client_stop(esp_mqtt_client_handle_t) {
#ifdef MQTT_BROKER_TEST
  JsonDocument request; request["op"] = "stop"; brokerCall(request);
#endif
  operations.push_back("stop"); return ESP_OK;
}
int esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) { operations.push_back("destroy"); delete c; return ESP_OK; }
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *topic, int qos) {
#ifdef MQTT_BROKER_TEST
  JsonDocument request; request["op"] = "subscribe"; request["topic"] = topic; request["qos"] = qos;
  brokerCall(request);
#endif
  assert(qos == 1); const int id = nextId++; subscriptions.emplace_back(topic, id); return id;
}
int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t, const char *topic) {
#ifdef MQTT_BROKER_TEST
  JsonDocument request; request["op"] = "unsubscribe"; request["topic"] = topic;
  brokerCall(request);
#endif
  unsubscriptions.emplace_back(topic); return nextId++;
}
static int send(const char *topic, const char *data, int len, int qos, bool retain, bool synchronous) {
  if (!(synchronous ? publishAccept : enqueueAccept)) return -1;
  if (rejectProbe && std::string(data, len).find("candidate_connected") != std::string::npos) return -1;
  const int id = qos ? nextId++ : 0;
  messages.push_back({ topic, std::string(data, len ? size_t(len) : std::strlen(data)), qos, retain, synchronous, id });
#ifdef MQTT_BROKER_TEST
  JsonDocument request; request["op"] = "publish"; request["topic"] = topic;
  request["payload"] = messages.back().payload; request["qos"] = qos; request["retain"] = retain;
  brokerCall(request);
#endif
  operations.push_back(std::string(topic) + ":" + messages.back().payload);
  if (qos) pendingAck = id;
  if (disconnectOnCleanup && synchronous && retain && messages.back().payload.empty()) event(MQTT_EVENT_DISCONNECTED);
  return id;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *topic, const char *data, int len, int qos, bool retain, bool store) {
  // Mirror ESP-MQTT: QoS 0 requires store=true, and success returns ID zero.
  if (qos == 0 && !store) return -1;
  if (enqueueFull) return -2;
  return send(topic, data, len, qos, retain, false);
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *topic, const char *data, int len, int qos, bool retain) {
  return send(topic, data, len, qos, retain, true);
}
static size_t countMessages(const std::string &topic) {
  size_t n = 0; for (const auto &m : messages) n += m.topic == topic; return n;
}
static const Message &lastMessage(const std::string &topic) {
  for (auto i = messages.rbegin(); i != messages.rend(); ++i) if (i->topic == topic) return *i;
  assert(false); return messages.front();
}
static void data(const char *topic, const char *payload, bool retained, int offset = 0, int total = -1) {
  esp_mqtt_event_t e; e.client = mqttHandle;
  e.topic = const_cast<char *>(topic); e.topic_len = topic ? int(std::strlen(topic)) : 0;
  e.data = const_cast<char *>(payload); e.data_len = int(std::strlen(payload));
  e.total_data_len = total < 0 ? e.data_len : total; e.current_data_offset = offset; e.retain = retained;
  mqttEvent(nullptr, nullptr, MQTT_EVENT_DATA, &e);
}

static char Room[64] = {};
static std::string resultStatus, resultDetail;
bool waitForConnection(uint32_t) { return mqttOnline && mqttSubscriptions >= 3; }
void result(const std::string &, const char *status, const char *detail) { resultStatus = status; resultDetail = detail; }
size_t strlcpy(char *out, const char *value, size_t size) {
  const size_t length = std::strlen(value), n = length < size ? length : size - 1;
  std::memcpy(out, value, n); out[n] = 0; return length;
}
#include "../build/native-c6/settings-test.inc"

#ifndef MQTT_BROKER_TEST
int main() {
  nodeId = "c6-test"; controlPrefix = "bletracker/" + nodeId;
  AppSettings settings;
  assert(startMqtt(settings));
  const std::string discovery = "homeassistant/sensor/espresense_c6-test/living_room/config";
  const std::string status = "devices/espresense_c6-test/living_room/status";
  const std::string count = "devices/espresense_c6-test/living_room/count";
  assert(mqttHandle->willTopic == status && mqttHandle->willPayload == "offline");
  assert(mqttHandle->config.session.last_will.qos == 1 && mqttHandle->config.session.last_will.retain);
  assert(countMessages(discovery) == 0); // Discovery is deferred until the connection succeeds.
  event(MQTT_EVENT_CONNECTED);
  assert(lastMessage(status).payload == "online" && !lastMessage(status).retain);
  assert(messages[messages.size() - 2].topic == status && messages[messages.size() - 2].payload.empty() && messages[messages.size() - 2].retain);
  assert(subscriptions.size() == 4);
  event(MQTT_EVENT_SUBSCRIBED, subscriptions[3].second); assert(mqttSubscriptions == 0);
  event(MQTT_EVENT_SUBSCRIBED, subscriptions[1].second);
  event(MQTT_EVENT_SUBSCRIBED, subscriptions[1].second); assert(mqttSubscriptions == 1);
  event(MQTT_EVENT_SUBSCRIBED, subscriptions[0].second);
  event(MQTT_EVENT_SUBSCRIBED, subscriptions[2].second); assert(mqttSubscriptions == 3);
  testCount = 2;
  serviceHomeAssistant("My tracker", "test-version");
  JsonDocument config;
  assert(!deserializeJson(config, lastMessage(discovery).payload));
  assert(config["name"] == "Tracked BLE devices" && config["unique_id"] == "espresense_c6-test_device_count");
  assert(config["state_topic"] == count && config["availability_topic"] == status);
  assert(config["device"]["identifiers"][0] == "espresense_c6-test");
  assert(config["device"]["name"] == "My tracker" && config["device"]["sw_version"] == "test-version");
  assert(config["state_class"] == "measurement" && config["unit_of_measurement"] == "devices");
  assert(config["expire_after"].isNull() && lastMessage(discovery).retain);
  assert(lastMessage(count).payload == "2" && !lastMessage(count).retain);
  testCount = 3; clockMs = 999; serviceHomeAssistant("My tracker", "test-version"); assert(countMessages(count) == 1);
  clockMs = 1000; serviceHomeAssistant("My tracker", "test-version"); assert(countMessages(count) == 2);
  clockMs = 31999; serviceHomeAssistant("My tracker", "test-version"); assert(countMessages(count) == 2);
  clockMs = 32000; serviceHomeAssistant("My tracker", "test-version"); assert(countMessages(count) == 3);
  testCount = 0; clockMs = 33000; serviceHomeAssistant("My tracker", "test-version"); assert(lastMessage(count).payload == "0");
  const size_t beforeBirth = countMessages(discovery);
  data("homeassistant/status", "online", true);
  clockMs = 34000; serviceHomeAssistant("My tracker", "test-version");
  assert(countMessages(discovery) == beforeBirth + 1 && countMessages(count) == 5 && commands.empty());
  data("homeassistant/status", "onl", true, 0, 6); assert(!haReplay);
  data(nullptr, "ine", true, 3, 6); assert(haReplay);
  haReplay = false;
  data("homeassistant/status", "offline", false); assert(!haReplay);
  data("bletracker/c6-test/config/set", "{}", true); assert(commands.empty());
  data("bletracker/c6-test/config/set", "{}", false); assert(commands.size() == 1);
  delete commands.back(); commands.clear();
  countAvailable = false; clockMs += 31000;
  const size_t beforeBusy = countMessages(count); serviceHomeAssistant("My tracker", "test-version"); assert(countMessages(count) == beforeBusy);
  countAvailable = true; enqueueAccept = false;
  serviceHomeAssistant("My tracker", "test-version"); assert(countMessages(count) == beforeBusy);
  enqueueAccept = true; serviceHomeAssistant("My tracker", "test-version"); assert(countMessages(count) == beforeBusy + 1);
  for (const auto &m : messages) { assert(m.qos == 1); if (m.payload == "online") assert(!m.retain); }

  const std::string bleTopic = "espresense/devices/iBeacon:699ebc80-e1f3-11e3-9a0f-0cf3ee3bc012-1-52753/c6_10bda39f8f98";
  const std::string blePayload(173, 'x');
  assert(esp_mqtt_client_enqueue(mqttHandle, bleTopic.c_str(), blePayload.c_str(), int(blePayload.size()), 0, false, false) == -1);
  assert(mqttClient.publish(bleTopic.c_str(), 0, false, blePayload.c_str(), blePayload.size()) == 1);
  assert(lastMessage(bleTopic).id == 0 && lastMessage(bleTopic).qos == 0 && !lastMessage(bleTopic).retain);
  assert(lastMessage(bleTopic).payload == blePayload);
  const size_t beforeQos0Failure = messages.size();
  enqueueAccept = false;
  assert(mqttClient.publish(bleTopic.c_str(), 0, false, blePayload.c_str(), blePayload.size()) == 0);
  enqueueAccept = true; enqueueFull = true;
  assert(mqttClient.publish(bleTopic.c_str(), 0, false, blePayload.c_str(), blePayload.size()) == 0);
  enqueueFull = false; mqttOnline = false;
  assert(mqttClient.publish(bleTopic.c_str(), 0, false, blePayload.c_str(), blePayload.size()) == 0);
  mqttOnline = true; assert(messages.size() == beforeQos0Failure);
  JsonDocument qos0Json; qos0Json["state"] = "qos0-test";
  assert(publishJson("/test/qos0", qos0Json, 0) == 0);
  assert(lastMessage(controlPrefix + "/test/qos0").qos == 0 && !lastMessage(controlPrefix + "/test/qos0").retain);

  // A lost connection cannot keep the application reporting online.
  incoming.add("t", 1, "a", 1, 0, 2, false);
  event(MQTT_EVENT_DISCONNECTED);
  assert(!mqttOnline && mqttReselect && incoming.body().empty());
  const size_t beforeReconnect = countMessages(discovery);
  event(MQTT_EVENT_CONNECTED); serviceHomeAssistant("My tracker", "test-version");
  assert(countMessages(discovery) == beforeReconnect + 1 && !mqttReselect);

  // Manual offline is sent and acknowledged before stop/destroy; no online
  // announcement or command handling can run during the shutdown.
  operations.clear(); stopMqtt();
  assert(operations.size() == 3 && operations[0] == status + ":offline" && operations[1] == "stop" && operations[2] == "destroy");
  assert(lastMessage(status).retain && !lastMessage(status).synchronous && !mqttHandle && mqttStopping);
  const size_t stoppedMessages = messages.size(); serviceHomeAssistant("Tracker", "test-version"); assert(messages.size() == stoppedMessages);
  assert(startMqtt(settings)); event(MQTT_EVENT_CONNECTED);
  ackPublish = false; const uint32_t beforeTimeout = clockMs;
  stopMqtt(); assert(uint32_t(clockMs - beforeTimeout) == 2000 && !mqttHandle);
  pendingAck = -1; ackPublish = true;

  // Failed candidates leave no discovery behind; restoration retains the old
  // topic. Successful room changes retire it and keep the entity's unique ID.
  AppSettings candidate = settings; candidate.room = "Kitchen";
  startAccept = false; assert(!startMqtt(candidate)); startAccept = true;
  assert(haRetiredDiscovery.empty());
  assert(startMqtt(settings)); event(MQTT_EVENT_CONNECTED); serviceHomeAssistant(settings.label, "test-version");
  assert(haTopics.discovery == discovery);
  assert(startMqtt(candidate)); event(MQTT_EVENT_CONNECTED);
  assert(mqttHandle->willTopic == "devices/espresense_c6-test/kitchen/status");
  assert(retireHaDiscovery(settings, candidate));
  assert(saveSettings(candidate)); ::settings = candidate; haReplay = true;
  serviceHomeAssistant(candidate.label, "test-version");
  assert(lastMessage(discovery).payload.empty() && lastMessage(discovery).retain && haRetiredDiscovery.empty());
  assert(!deserializeJson(config, lastMessage(haTopics.discovery).payload));
  assert(config["unique_id"] == "espresense_c6-test_device_count");
  const auto willTopic = mqttHandle->willTopic;
  candidate.room = "Office"; assert(std::string(mqttHandle->config.session.last_will.topic) == willTopic);
  stopMqtt();
  settings.room.clear(); assert(startMqtt(settings)); assert(haTopics.status == "devices/espresense_c6-test/c6_test/status"); stopMqtt();
  // Exercise the actual sketch transaction: connection/probe/NVS failures
  // restore the prior room and cannot delete or announce candidate discovery.
  autoConnect = true; ::settings = AppSettings();
  assert(startMqtt(::settings)); serviceHomeAssistant(::settings.label, "test-version");
  JsonDocument command; command["revision"] = 0; command["room"] = "Kitchen";
  const size_t kitchenBefore = countMessages("homeassistant/sensor/espresense_c6-test/kitchen/config");
  Preferences::failWrite = true;
  processSettings(command.as<JsonVariantConst>(), "save-fail");
  assert(resultStatus == "error" && ::settings.room == "Living Room" && haTopics.discovery == discovery);
  assert(haRetiredDiscovery.empty() && countMessages("homeassistant/sensor/espresense_c6-test/kitchen/config") == kitchenBefore);
  Preferences::failWrite = false; rejectRoomTopic = "devices/espresense_c6-test/kitchen/status";
  processSettings(command.as<JsonVariantConst>(), "connect-fail");
  assert(resultStatus == "error" && haTopics.discovery == discovery && haRetiredDiscovery.empty());
  rejectRoomTopic.clear(); rejectProbe = true;
  processSettings(command.as<JsonVariantConst>(), "probe-fail");
  assert(resultStatus == "error" && haTopics.discovery == discovery && haRetiredDiscovery.empty());
  rejectProbe = false;
  processSettings(command.as<JsonVariantConst>(), "room-success");
  assert(resultStatus == "saved" && ::settings.room == "Kitchen" && ::settings.revision == 1 && hasSavedConnectionSettings);
  assert(std::string(Room) == "Kitchen" && haRetiredDiscovery.size() == 1);
  serviceHomeAssistant(::settings.label, "test-version");
  assert(haRetiredDiscovery.empty() && lastMessage(discovery).payload.empty());
  assert(!networkChanged(::settings, ::settings));
  AppSettings equivalent = ::settings; equivalent.room = "KITCHEN";
  assert(!networkChanged(::settings, equivalent));
  stopMqtt();

  // Commit a new room, lose connectivity before its tombstone is acknowledged,
  // then boot from the real settings blob. No RAM journal survives this reset.
  ::settings = AppSettings(); assert(saveSettings(::settings));
  assert(startMqtt(::settings));
  command["revision"] = 0; command["room"] = "Kitchen";
  processSettings(command.as<JsonVariantConst>(), "durable-room");
  assert(resultStatus == "saved" && haRetiredDiscovery.size() == 1);
  disconnectOnCleanup = true;
  serviceHomeAssistant(::settings.label, "test-version");
  assert(!mqttOnline && haRetiredDiscovery.size() == 1);
  disconnectOnCleanup = false; stopMqtt();
  ::settings = AppSettings(); hasSavedConnectionSettings = false;
  assert(haRetiredDiscovery.empty() && loadSettings());
  assert(::settings.room == "Kitchen" && haRetiredDiscovery.size() == 1);
  assert(haRetiredDiscovery[0].topic == discovery);
  assert(startMqtt(::settings));
  // Even after PUBACK, failure to save the journal removal leaves it retryable.
  Preferences::failWrite = true;
  serviceHomeAssistant(::settings.label, "test-version");
  assert(haRetiredDiscovery.size() == 1);
  Preferences::failWrite = false; stopMqtt();
  ::settings = AppSettings(); assert(loadSettings() && haRetiredDiscovery.size() == 1);
  assert(startMqtt(::settings)); serviceHomeAssistant(::settings.label, "test-version");
  assert(haRetiredDiscovery.empty() && lastMessage(discovery).payload.empty());
  stopMqtt(); ::settings = AppSettings(); assert(loadSettings() && haRetiredDiscovery.empty());

  // Renaming back before cleanup must never tombstone the active discovery.
  ::settings = AppSettings(); assert(saveSettings(::settings)); assert(startMqtt(::settings));
  command["revision"] = 0; command["room"] = "Kitchen";
  processSettings(command.as<JsonVariantConst>(), "rename-out");
  command["revision"] = 1; command["room"] = "Living Room";
  processSettings(command.as<JsonVariantConst>(), "rename-back");
  assert(resultStatus == "saved" && haRetiredDiscovery.size() == 2);
  const size_t beforeRenameCleanup = countMessages(discovery);
  serviceHomeAssistant(::settings.label, "test-version");
  assert(haRetiredDiscovery.empty() && countMessages(discovery) == beforeRenameCleanup + 1);
  assert(!lastMessage(discovery).payload.empty()); stopMqtt();

  // Room-only edits and every failed-candidate rollback stay on the healthy
  // backup, while NVS continues to hold the user's configured primary URI.
  ::settings = AppSettings(); ::settings.uri = "mqtt://primary:1883";
  assert(saveSettings(::settings));
  AppSettings backup = ::settings; backup.uri = "mqtt://backup:1883";
  rejectBrokerUri = ::settings.uri; assert(startMqtt(backup)); attemptedBrokers.clear();
  command["revision"] = 0; command["room"] = "Kitchen";
  processSettings(command.as<JsonVariantConst>(), "backup-room");
  assert(resultStatus == "saved" && ::settings.uri == "mqtt://primary:1883");
  assert(transportSettings.uri == backup.uri && mqttHandle->willTopic == "devices/espresense_c6-test/kitchen/status");
  assert(attemptedBrokers.size() == 1 && attemptedBrokers[0] == backup.uri);
  assert(haRetiredDiscovery.size() == 1 && haRetiredDiscovery[0].broker == backup.uri);
  serviceHomeAssistant(::settings.label, "test-version");
  command["revision"] = 1; command["room"] = "Office";
  for (unsigned failure = 0; failure < 3; ++failure) {
    Preferences::failWrite = failure == 0;
    rejectProbe = failure == 1;
    rejectRoomTopic = failure == 2 ? "devices/espresense_c6-test/office/status" : "";
    attemptedBrokers.clear(); processSettings(command.as<JsonVariantConst>(), "backup-failure");
    assert(resultStatus == "error" && ::settings.room == "Kitchen" && transportSettings.uri == backup.uri);
    assert(mqttHandle->willTopic == "devices/espresense_c6-test/kitchen/status");
    for (const auto &broker : attemptedBrokers) assert(broker == backup.uri);
  }
  Preferences::failWrite = false; rejectProbe = false; rejectRoomTopic.clear();
  command["uri"] = "mqtt://primary:1883"; command["user"] = "new-user";
  processSettings(command.as<JsonVariantConst>(), "explicit-broker-failure");
  assert(resultStatus == "error" && transportSettings.uri == backup.uri);
  stopMqtt(); ::settings = AppSettings(); assert(loadSettings());
  assert(::settings.uri == "mqtt://primary:1883" && ::settings.room == "Kitchen");
  rejectBrokerUri.clear();

  // Cleanup remains durable while using another broker, and resumes when the
  // broker tagged in its record is selected again after reboot.
  ::settings = AppSettings(); ::settings.room = "Kitchen";
  ::settings.discoveryCleanup.push_back({ backup.uri, discovery }); assert(saveSettings(::settings));
  ::settings = AppSettings(); assert(loadSettings()); assert(startMqtt(::settings));
  const size_t beforeOtherBroker = countMessages(discovery);
  serviceHomeAssistant(::settings.label, "test-version");
  assert(haRetiredDiscovery.size() == 1 && countMessages(discovery) == beforeOtherBroker);
  backup = ::settings; backup.uri = "mqtt://backup:1883"; assert(startMqtt(backup));
  serviceHomeAssistant(::settings.label, "test-version"); assert(haRetiredDiscovery.empty()); stopMqtt();

  // Old blobs without a journal stay compatible; corrupt journal records fail
  // loading without replacing active settings or erasing their stored bytes.
  JsonDocument stored; settingsJson(stored, ::settings, true); stored.remove("discovery_cleanup");
  std::string encoded; serializeJson(stored, encoded);
  Preferences::saved.assign(encoded.begin(), encoded.end()); assert(loadSettings() && haRetiredDiscovery.empty());
  stored["discovery_cleanup"][0]["broker"] = "mqtt://backup:1883";
  stored["discovery_cleanup"][0]["topic"] = "homeassistant/sensor/+/config";
  encoded.clear(); serializeJson(stored, encoded); Preferences::saved.assign(encoded.begin(), encoded.end());
  assert(!loadSettings() && haRetiredDiscovery.empty());
  AppSettings full = ::settings;
  for (size_t i = 0; i < homeassistant::MAX_RETIRED_DISCOVERY; ++i)
    full.discoveryCleanup.push_back({ "mqtt://test:1883", homeassistant::Topics(nodeId, "room" + std::to_string(i)).discovery });
  full.room = "Office"; assert(!retireHaDiscovery(::settings, full));
  homeassistant::Reporting rollover;
  rollover.published(1, UINT32_MAX - 500);
  assert(!rollover.due(2, 498) && rollover.due(2, 499));
  // Exercise the production LED transaction, settings blob and event routing.
  ::settings = AppSettings(); assert(saveSettings(::settings));
  ledController.begin(::settings.led); assert(startMqtt(::settings));
  JsonDocument ledCommand;
  assert(!deserializeJson(ledCommand,R"({"revision":0,"brightness":128,"groups":[{"group":1,"topics":["sensor/lobby/#","sensor/+/red"]},{"group":2,"topics":["sensor/lobby/#"]}]})"));
  const size_t beforeLedSubscriptions = subscriptions.size();
  processLedConfiguration(ledCommand.as<JsonVariantConst>(),"led-save");
  assert(resultStatus == "saved" && ::settings.revision == 1 && ::settings.led.brightness == 128);
  assert(subscriptions.size() == beforeLedSubscriptions+2 && ledSubscriptions.pending() == 2);
  event(MQTT_EVENT_SUBSCRIBED,subscriptions[subscriptions.size()-2].second);
  event(MQTT_EVENT_SUBSCRIBED,subscriptions.back().second); assert(ledSubscriptions.pending() == 0);
  ledCommand["brightness"] = 200;
  processLedConfiguration(ledCommand.as<JsonVariantConst>(),"led-stale");
  assert(resultStatus == "error" && ::settings.led.brightness == 128);
  ledCommand["revision"] = 1; Preferences::failWrite = true;
  processLedConfiguration(ledCommand.as<JsonVariantConst>(),"led-failed-save");
  assert(resultStatus == "error" && ::settings.revision == 1 && ::settings.led.brightness == 128);
  Preferences::failWrite = false;
  data("sensor/lobby/green","100",false); data("sensor/lobby/red","100",false);
  ledController.service(clockMs,true);
  assert((ledController.lastFrame()[1] == leds::RGB{255,255,0} && ledController.lastFrame()[5] == leds::RGB{255,255,0}));
  data("sensor/lobby/blue","100",true); data("sensor/lobby/blue","invalid",false);
  ledController.service(clockMs,true); assert(ledController.lastFrame()[1].b == 0);
  data("sensor/lobby/blue","1",false,0,3); data(nullptr,"00",false,1,3);
  ledController.service(clockMs,true); assert(ledController.lastFrame()[1].b == 255 && commands.empty());
  const size_t subscriptionsBeforeReconnect = subscriptions.size();
  event(MQTT_EVENT_DISCONNECTED); event(MQTT_EVENT_CONNECTED); serviceLedSubscriptions();
  assert(subscriptions.size() == subscriptionsBeforeReconnect+6); // 4 built-in, 2 distinct LED filters
  ::settings = AppSettings(); assert(loadSettings());
  assert(::settings.led.brightness == 128 && ::settings.led.topics[0].size() == 2 && ::settings.revision == 1);
  JsonDocument clearing; clearing["revision"] = 1; clearing["groups"][0]["group"] = 1;
  clearing["groups"][0]["topics"].to<JsonArray>(); clearing["groups"][1]["group"] = 2;
  clearing["groups"][1]["topics"].to<JsonArray>();
  processLedConfiguration(clearing.as<JsonVariantConst>(),"led-clear");
  clockMs += 5000; serviceLedSubscriptions();
  assert(resultStatus == "saved" && unsubscriptions.size() == 2 && ledSubscriptions.pending() == 0);
  // A broad filter cannot route our own status/state/result messages back as LED/control input.
  JsonDocument broad; broad["revision"] = 2; broad["groups"][0]["group"] = 1;
  broad["groups"][0]["topics"][0] = "#";
  processLedConfiguration(broad.as<JsonVariantConst>(),"led-broad");
  ledController.pause(); ledController.resume();
  data("bletracker/c6-test/config/ip","100",false);
  data("bletracker/c6-test/config/result",R"({"id":"own","status":"saved"})",false);
  data("homeassistant/status","online",true); ledController.service(clockMs,true);
  assert(commands.empty() && ledController.lastFrame()[1] == leds::RGB{});
  data("sensor/lobby","100",false); ledController.service(clockMs,true);
  assert((ledController.lastFrame()[1] == leds::RGB{255,255,255}));
  JsonDocument reserved; reserved["revision"] = 3; reserved["groups"][0]["group"] = 1;
  reserved["groups"][0]["topics"][0] = controlPrefix + "/config/set";
  reserved["groups"][0]["topics"][1] = "homeassistant/status";
  const size_t beforeReservedSubscriptions = subscriptions.size();
  processLedConfiguration(reserved.as<JsonVariantConst>(),"led-reserved");
  assert(resultStatus == "saved" && subscriptions.size() == beforeReservedSubscriptions && ledSubscriptions.pending() == 0);
  JsonDocument removeReserved; removeReserved["revision"] = 4; removeReserved["groups"][0]["group"] = 1;
  removeReserved["groups"][0]["topics"].to<JsonArray>();
  processLedConfiguration(removeReserved.as<JsonVariantConst>(),"led-remove-reserved");
  for (const auto &filter : unsubscriptions) assert(filter != controlPrefix + "/config/set" && filter != "homeassistant/status");
  data("bletracker/c6-test/config/get",R"({"id":"still-subscribed"})",false);
  assert(commands.size() == 1); delete commands.back(); commands.clear();
  stopMqtt();
  // Missing LED fields restore defaults, not whatever configuration was in RAM.
  settingsJson(stored,::settings,true); stored.remove("led"); encoded.clear(); serializeJson(stored,encoded);
  Preferences::saved.assign(encoded.begin(),encoded.end()); assert(loadSettings());
  assert(::settings.led.brightness == 255 && leds::filters(::settings.led).empty());
  stored["led"]["brightness"] = 999; encoded.clear(); serializeJson(stored,encoded);
  Preferences::saved.assign(encoded.begin(),encoded.end()); assert(!loadSettings() && ::settings.led.brightness == 255);
  // Oversize LED configuration plus CA fields must preserve the committed blob.
  AppSettings oversized = ::settings; oversized.mqttCa = std::string(4096,'a'); oversized.otaCa = std::string(4096,'b');
  for(unsigned i=0;i<64;++i) oversized.led.topics[0].push_back(std::string(155,'a')+std::to_string(i));
  const auto priorBlob = Preferences::saved; assert(!saveSettings(oversized) && Preferences::saved == priorBlob);
  std::cout << "MQTT tests passed: discovery, count, availability, durable cleanup, reboot recovery, backup broker, rollback and LED settings/routing.\n";
}
#endif
