#pragma once

#include <Arduino.h>
#include "wave_station.h"

struct SensorReadings {
  bool hasLux = false;
  bool hasTemperature = false;
  bool hasHumidity = false;
  float lux = 0.0f;
  float temperatureC = 0.0f;
  float humidityPct = 0.0f;
};

bool sensorsBegin(uint8_t dhtPin, int sdaPin, int sclPin);
bool dhtReady();
bool lightReady();
const char* sensorStatus();

// 사용 가능한 센서를 한 번에 읽고 has* 필드로 각 측정값의 유효성을 표시합니다.
SensorReadings sensorsReadAll();
