#pragma once

#include <Arduino.h>

bool micBegin(int bclkPin, int lrckPin, int dataInPin);
bool micReadFrame(int16_t* outSamples, uint16_t sampleCount);
