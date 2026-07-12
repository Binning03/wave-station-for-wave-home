#pragma once

#include <Arduino.h>

// ESP32용 Opus 인코더/디코더 래퍼입니다.
// Arduino Library Manager에서 esp32_opus 라이브러리를 사용합니다.

constexpr uint16_t OPUS_MAX_ENCODED_BYTES = 512;
constexpr uint32_t OPUS_TARGET_BITRATE_BPS = 16000;
constexpr uint8_t OPUS_COMPLEXITY = 0;
constexpr uint16_t OPUS_MAX_DECODED_SAMPLES = 960;  // 60 ms @ 16 kHz mono

bool opusBegin();
bool opusEncoderReady();
bool opusDecoderReady();
// MicComp 사용 가능 여부를 확인하는 간단한 별칭입니다.
bool opusReady();
const char* opusStatus();

bool opusEncodeFrame(const int16_t* pcm,
                     uint16_t sampleCount,
                     uint8_t* outEncoded,
                     uint16_t outCapacity,
                     uint16_t* outEncodedSize);

bool opusDecodeFrame(const uint8_t* encoded,
                     uint16_t encodedSize,
                     int16_t* outPcm,
                     uint16_t outCapacitySamples,
                     uint16_t* outSampleCount);

bool opusDecoderReset();
