#pragma once
// Native regressions never import the workstation's private secrets.h.
#define TRACKER_NATIVE_TEST
#define MQTT_SERVER "test"
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASS ""
#define TRACKER_MQTT_URI "mqtt://test:1883"
#define TRACKER_ROOM "Living Room"
#define TRACKER_NODE_LABEL "Tracker"
#define log_e(...) ((void)0)
#define log_w(...) ((void)0)
#define log_i(...) ((void)0)
#define log_d(...) ((void)0)
