#pragma once

#include <Arduino.h>
#include "wave_station.h"

bool speakerBegin(int bclkPin, int lrckPin, int dataOutPin);
bool speakerReady();

// mono signed 16-bit PCM을 재생하며, I2S 출력에서는 좌우 슬롯에 복제합니다.
bool speakerPlayPCM16Mono(const int16_t* samples, uint16_t sampleCount);
