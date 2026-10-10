#pragma once
#include <mqtt_client.h>
#include <atomic>
#include <freertos/queue.h>
#include "Control_Core.h"

struct Command {
  std::string topic, payload;
};
static QueueHandle_t commandQueue = nullptr;
static std::string nodeId, controlPrefix;
static std::atomic<bool> mqttOnline{ false }, mqttAnnounce{ false };
static std::atomic<bool> mqttReselect{ false };
static std::atomic<int> mqttLastAck{ -1 };
static std::atomic<unsigned> mqttSubscriptions{ 0 };
static std::atomic<unsigned> droppedCommands{ 0 };
static esp_mqtt_client_handle_t mqttHandle = nullptr;
static control::Assembly incoming;
static AppSettings transportSettings;     // Own certificate memory for client's full lifetime.
static char selectedMqttServer[40] = {};  // 39 name bytes plus the terminating NUL.

inline void selectMqttServer(const std::string &uri) {
  const size_t scheme = uri.find("://");
  const size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
  const size_t end = uri.find_first_of(":/", begin);
  // Keep bracketed IPv6 addresses intact, without their port.
  const size_t hostEnd = begin < uri.size() && uri[begin] == '[' ? uri.find(']', begin) : end;
  const size_t length = hostEnd == std::string::npos ? uri.size() - begin
                                                     : hostEnd - begin + (uri[begin] == '[' ? 1 : 0);
  const size_t copied = length < sizeof selectedMqttServer ? length : sizeof selectedMqttServer - 1;
  uri.copy(selectedMqttServer, copied, begin);
  selectedMqttServer[copied] = '\0';
  {
    IPAddress test;
    if (1 == Network.hostByName(selectedMqttServer, test))
      Serial.printf("Using MQTT server '%s' at IP %s\n", selectedMqttServer, test.toString().c_str());
    else Serial.printf("Could not resolve MQTT server at '%s' to an IP\n", selectedMqttServer);
  }
}

struct MqttAdapter {
  bool connected() const {
    return mqttOnline.load();
  }
  int publish(const char *topic, uint8_t qos, bool retain, const char *payload, size_t n) {
    if (!mqttHandle || !connected()) return 0;
    const int result = esp_mqtt_client_enqueue(mqttHandle, topic, payload, int(n), qos, retain, false);
    return result >= 0 ? result + 1 : 0;  // ESP-MQTT uses zero for successful QoS 0.
  }
} mqttClient;

static void mqttEvent(void *, esp_event_base_t, int32_t eventId, void *eventData) {
  auto *e = static_cast<esp_mqtt_event_t *>(eventData);
  switch (eventId) {
    case MQTT_EVENT_CONNECTED:
      {
        IPAddress test;
        if (1 == Network.hostByName(selectedMqttServer, test))
          Serial.printf("Connection to '%s' at IP %s established\n", selectedMqttServer, test.toString().c_str());
        else Serial.printf("Could not resolve '%s' to an IP\n", selectedMqttServer);
      }
      mqttOnline = true;
      mqttReselect = false;
      mqttAnnounce = true;
      mqttSubscriptions = 0;
      esp_mqtt_client_subscribe(mqttHandle, (controlPrefix + "/config/set").c_str(), 1);
      esp_mqtt_client_subscribe(mqttHandle, (controlPrefix + "/config/get").c_str(), 1);
      esp_mqtt_client_subscribe(mqttHandle, (controlPrefix + "/ota/set").c_str(), 1);
      break;
    case MQTT_EVENT_DISCONNECTED:
      Serial.printf("MQTT to '%s' disconnected\n", selectedMqttServer);
      {
        IPAddress test;
        if (1 == Network.hostByName(selectedMqttServer, test))
          Serial.printf("Connection to '%s' at IP %s lost\n", selectedMqttServer, test.toString().c_str());
        else Serial.printf("Could not resolve '%s' to an IP\n", selectedMqttServer);
      }
      break;

    case MQTT_EVENT_SUBSCRIBED: ++mqttSubscriptions; break;
    case MQTT_EVENT_PUBLISHED: mqttLastAck = e->msg_id; break;
    case MQTT_EVENT_ERROR:
      if (e->error_handle) Serial.printf("MQTT error type=%d transport=%d socket=%d refused=%d,server=%s\n",
                                         int(e->error_handle->error_type), int(e->error_handle->esp_tls_last_esp_err),
                                         e->error_handle->esp_transport_sock_errno, int(e->error_handle->connect_return_code),
                                         selectedMqttServer);
      {
        IPAddress test;
        if (1 == Network.hostByName(selectedMqttServer, test))
          Serial.printf("Connection to '%s' at IP %s error\n", selectedMqttServer, test.toString().c_str());
        else Serial.printf("Could not resolve '%s' to an IP\n", selectedMqttServer);
      }
      break;
    case MQTT_EVENT_DATA:
      if (incoming.add(e->topic, e->topic_len, e->data, e->data_len, e->current_data_offset, e->total_data_len, e->retain)) {
        auto *command = new (std::nothrow) Command{ incoming.topic(), incoming.body() };
        if (!command || xQueueSend(commandQueue, &command, 0) != pdTRUE) {
          delete command;
          ++droppedCommands;
        }
        incoming.reset();
      }
      break;
    default: break;
  }
}
inline void stopMqtt() {
  if (mqttHandle) {
    esp_mqtt_client_stop(mqttHandle);
    esp_mqtt_client_destroy(mqttHandle);
    mqttHandle = nullptr;
  }
  mqttOnline = false;
  mqttAnnounce = false;
  mqttReselect = false;
  mqttLastAck = -1;
  mqttSubscriptions = 0;
  incoming.reset();
}
inline bool startMqtt(const AppSettings &s) {
  stopMqtt();
  transportSettings = s;
  selectMqttServer(transportSettings.uri);
  esp_mqtt_client_config_t config{};
  config.broker.address.uri = transportSettings.uri.c_str();
  if (s.uri.compare(0, 8, "mqtts://") == 0) {
    if (s.mqttCa.empty() || time(nullptr) < 1704067200) return false;
    config.broker.verification.certificate = transportSettings.mqttCa.c_str();
    config.broker.verification.skip_cert_common_name_check=true;
  }
  config.credentials.client_id = nodeId.c_str();
  config.credentials.username = transportSettings.user.c_str();
  config.credentials.authentication.password = transportSettings.password.c_str();
  config.session.keepalive = 30;
  config.network.timeout_ms = 10000;
  config.network.reconnect_timeout_ms = 5000;
  config.buffer.size = 1024;
  config.buffer.out_size = 1024;
  config.outbox.limit = 8192;
  mqttHandle = esp_mqtt_client_init(&config);
  if (!mqttHandle) return false;
  if (esp_mqtt_client_register_event(mqttHandle, MQTT_EVENT_ANY, mqttEvent, nullptr) != ESP_OK || esp_mqtt_client_start(mqttHandle) != ESP_OK) {
    stopMqtt();
    return false;
  }
  return true;
}
inline int publishJson(const char *suffix, const JsonDocument &doc, int qos = 1) {
  if (!mqttHandle || !mqttOnline || doc.overflowed()) return -1;
  std::string payload;
  serializeJson(doc, payload);
  return esp_mqtt_client_enqueue(mqttHandle, (controlPrefix + suffix).c_str(), payload.c_str(), int(payload.size()), qos, false, false);
}
inline bool waitForAck(int id, uint32_t timeout = 5000) {
  const uint32_t start = millis();
  while (id >= 0 && mqttOnline && uint32_t(millis() - start) < timeout) {
    if (mqttLastAck == id) return true;
    delay(10);
  }
  return false;
}
