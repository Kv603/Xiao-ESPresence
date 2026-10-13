#pragma once
#include <cstdint>
struct MockMqttClient;
using esp_mqtt_client_handle_t = MockMqttClient *;
using esp_event_base_t = const char *;
constexpr int ESP_OK = 0;
enum { MQTT_EVENT_ANY = -1, MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED,
       MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_PUBLISHED, MQTT_EVENT_ERROR, MQTT_EVENT_DATA };
struct esp_mqtt_error_codes_t {
  int error_type = 0, esp_tls_last_esp_err = 0, esp_transport_sock_errno = 0, connect_return_code = 0;
};
struct esp_mqtt_event_t {
  esp_mqtt_client_handle_t client = nullptr;
  char *topic = nullptr, *data = nullptr;
  int topic_len = 0, data_len = 0, current_data_offset = 0, total_data_len = 0, msg_id = 0;
  bool retain = false;
  esp_mqtt_error_codes_t *error_handle = nullptr;
};
using MqttEventHandler = void (*)(void *, esp_event_base_t, int32_t, void *);
struct esp_mqtt_client_config_t {
  struct {
    struct { const char *uri = nullptr; } address;
    struct { const char *certificate = nullptr; bool skip_cert_common_name_check = false; } verification;
  } broker;
  struct {
    const char *client_id = nullptr, *username = nullptr;
    struct { const char *password = nullptr; } authentication;
  } credentials;
  struct {
    int keepalive = 0;
    struct { const char *topic = nullptr, *msg = nullptr; int msg_len = 0, qos = 0, retain = 0; } last_will;
  } session;
  struct { int timeout_ms = 0, reconnect_timeout_ms = 0; } network;
  struct { int size = 0, out_size = 0; } buffer;
  struct { int limit = 0; } outbox;
};
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
int esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int, MqttEventHandler, void *);
int esp_mqtt_client_start(esp_mqtt_client_handle_t);
int esp_mqtt_client_stop(esp_mqtt_client_handle_t);
int esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *, const char *, int, int, bool, bool);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *, const char *, int, int, bool);
