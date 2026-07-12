#include "sensor_driver.h"

#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>

static DHT* g_dht = nullptr;
static BH1750 g_lightMeter;
static bool g_dhtReady = false;
static bool g_lightReady = false;
static const char* g_sensorStatus = "not initialized";

bool sensorsBegin(uint8_t dhtPin, int sdaPin, int sclPin) {
  Serial.printf("[sensor] begin: DHT22 DATA=%u, BH1750 SDA=%d SCL=%d\n", dhtPin, sdaPin, sclPin);

  static DHT dhtStatic(dhtPin, DHT22);
  g_dht = &dhtStatic;
  g_dht->begin();
  g_dhtReady = true;

  Wire.begin(sdaPin, sclPin);
  g_lightReady = g_lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);

  if (g_dhtReady && g_lightReady) {
    g_sensorStatus = "DHT22 and BH1750 ready";
  } else if (g_dhtReady) {
    g_sensorStatus = "DHT22 ready, BH1750 not detected";
  } else if (g_lightReady) {
    g_sensorStatus = "BH1750 ready, DHT22 not ready";
  } else {
    g_sensorStatus = "sensors not ready";
  }

  Serial.printf("[sensor] %s\n", g_sensorStatus);
  return g_dhtReady || g_lightReady;
}

bool dhtReady() {
  return g_dhtReady && g_dht != nullptr;
}

bool lightReady() {
  return g_lightReady;
}

const char* sensorStatus() {
  return g_sensorStatus;
}

SensorReadings sensorsReadAll() {
  SensorReadings out;

  if (g_lightReady) {
    const float lux = g_lightMeter.readLightLevel();
    if (!isnan(lux) && lux >= 0.0f) {
      out.hasLux = true;
      out.lux = lux;
    }
  }

  if (dhtReady()) {
    const float t = g_dht->readTemperature();
    const float h = g_dht->readHumidity();

    if (!isnan(t)) {
      out.hasTemperature = true;
      out.temperatureC = t;
    }
    if (!isnan(h)) {
      out.hasHumidity = true;
      out.humidityPct = h;
    }
  }

  return out;
}
