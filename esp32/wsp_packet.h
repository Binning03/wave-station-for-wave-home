#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "wave_station.h"

struct SubscriptionState {
  bool mic_pcm = false;
  bool mic_comp = false;
  bool ir_receive = false;

  bool ambient_light = false;
  bool temperature = false;
  bool humidity = false;

  uint16_t ambient_interval_ms = 1000;
  uint16_t temperature_interval_ms = 2000;
  uint16_t humidity_interval_ms = 2000;

  bool ambient_on_change_only = false;
  bool temperature_on_change_only = false;
  bool humidity_on_change_only = false;
};

bool wspWritePacket(WiFiClient& client, const uint8_t* data, size_t size);

bool wspSendAck(WiFiClient& client, uint32_t requestId, uint8_t status);
bool wspSendError(WiFiClient& client, uint32_t requestId, wsp::ErrorCode code, const char* message);
bool wspSendHeartbeat(WiFiClient& client, uint32_t requestId);

bool wspSendMicPCM(WiFiClient& client, const int16_t* samples, uint16_t sampleCount, uint32_t sequence);
bool wspSendMicComp(WiFiClient& client, const uint8_t* encoded, uint16_t encodedSize, uint32_t sequence);
bool wspSendIrReceive(WiFiClient& client, const uint16_t* rawUs, uint16_t length, bool overflow, uint32_t sequence);
bool wspSendSensor(WiFiClient& client, wsp::Type type, wsp::SensorUnit unit, float value, uint8_t quality, uint32_t sequence);
