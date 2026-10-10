#pragma once
#ifndef BEACON_CORE_ONLY
#include <ArduinoJson.h>
#include <cstdint>
#include <string>

// BLE scanner entry points used by the application and OTA flow.
void setupBLE(bool active);
bool doBLE();
void pauseBLEForOTA();
void requestActiveBLEScan();
bool bleScannerIsRunning();
bool bleConfigurationRevision(uint32_t &revision);
bool bleReportableDeviceCount(uint32_t &count);
std::string bleRoomTopicName(const std::string &room, const std::string &node);
bool appendBLEConfigurationState(JsonDocument &doc);
const char *editBLEConfiguration(JsonVariantConst command);
bool prepareForFirmwareUpdate();
void resumeAfterFirmwareUpdate();
bool decodeHexString(const std::string &value, uint8_t *out, size_t size);
#endif
