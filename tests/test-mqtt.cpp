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

struct AppSettings {
  std::string uri = "mqtt://test:1883", user, password, mqttCa, room = "Living Room", label = "Tracker";
  uint32_t revision = 0;
};
struct { void printf(const char *, ...) {} void println(const char *) {} } Serial;
struct IPAddress {
  struct Text { const char *c_str() const { return "127.0.0.1"; } };
  Text toString() const { return {}; }
};
struct { int hostByName(const char *, IPAddress &) { return 1; } } Network;
static uint32_t clockMs = 0, testCount = 0;
static bool countAvailable = true, enqueueAccept = true, publishAccept = true, ackPublish = true, startAccept = true;
static bool autoConnect = false, saveSucceeds = true, rejectProbe = false;
static std::string rejectRoomTopic;
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
  if (!startAccept || c->willTopic == rejectRoomTopic) return -1;
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
static int send(const char *topic, const char *data, int len, int qos, bool retain, bool synchronous) {
  if (!(synchronous ? publishAccept : enqueueAccept)) return -1;
  if (rejectProbe && std::string(data, len).find("candidate_connected") != std::string::npos) return -1;
  const int id = nextId++;
  messages.push_back({ topic, std::string(data, len ? size_t(len) : std::strlen(data)), qos, retain, synchronous, id });
#ifdef MQTT_BROKER_TEST
  JsonDocument request; request["op"] = "publish"; request["topic"] = topic;
  request["payload"] = messages.back().payload; request["qos"] = qos; request["retain"] = retain;
  brokerCall(request);
#endif
  operations.push_back(std::string(topic) + ":" + messages.back().payload);
  pendingAck = id;
  return id;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *topic, const char *data, int len, int qos, bool retain, bool) {
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

static AppSettings settings;
static bool hasSavedConnectionSettings = false;
static char Room[64] = {};
static std::string resultStatus, resultDetail;
bool patchSettings(JsonVariantConst command, AppSettings &s) {
  if (command["room"].is<const char *>()) s.room = command["room"].as<std::string>();
  return true;
}
bool saveSettings(const AppSettings &) { return saveSucceeds; }
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
  assert(messages.empty()); // Discovery is deferred until the connection succeeds.
  event(MQTT_EVENT_CONNECTED);
  assert(messages.size() == 2 && messages[0].topic == status && messages[0].payload.empty() && messages[0].retain);
  assert(messages[1].payload == "online" && !messages[1].retain);
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
  retireHaDiscovery(settings, candidate); serviceHomeAssistant(candidate.label, "test-version");
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
  saveSucceeds = false;
  processSettings(command.as<JsonVariantConst>(), "save-fail");
  assert(resultStatus == "error" && ::settings.room == "Living Room" && haTopics.discovery == discovery);
  assert(haRetiredDiscovery.empty() && countMessages("homeassistant/sensor/espresense_c6-test/kitchen/config") == kitchenBefore);
  saveSucceeds = true; rejectRoomTopic = "devices/espresense_c6-test/kitchen/status";
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
  homeassistant::Reporting rollover;
  rollover.published(1, UINT32_MAX - 500);
  assert(!rollover.due(2, 498) && rollover.due(2, 499));
  std::cout << "MQTT discovery/count/availability tests passed: QoS, retain, LWT, birth, subscriptions, cadence, shutdown and rollback.\n";
}
#endif
