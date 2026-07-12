#include "opus_codec.h"
#include "wave_station.h"

// Arduino 빌드 시스템이 Opus 라이브러리 의존성을 확실히 탐지하도록
// 헤더를 직접 포함합니다.
#include <opus.h>
#define WAVE_STATION_HAS_OPUS 1

static OpusEncoder* g_encoder = nullptr;
static OpusDecoder* g_decoder = nullptr;
static bool g_encoderReady = false;
static bool g_decoderReady = false;
static const char* g_status = "not initialized";

bool opusBegin() {
#if WAVE_STATION_HAS_OPUS
  bool ok = true;

  Serial.printf("[opus] free heap before create: %lu bytes\n", static_cast<unsigned long>(ESP.getFreeHeap()));

  if (g_encoder == nullptr) {
    int encErr = OPUS_OK;
    g_encoder = opus_encoder_create(
        static_cast<opus_int32>(wsp::kDefaultSampleRate),
        1,
        OPUS_APPLICATION_VOIP,
        &encErr);

    if (encErr != OPUS_OK || g_encoder == nullptr) {
      Serial.printf("[opus] opus_encoder_create failed: %d, free heap=%lu bytes\n",
                    encErr, static_cast<unsigned long>(ESP.getFreeHeap()));
      g_encoderReady = false;
      ok = false;
    } else {
      opus_encoder_ctl(g_encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(OPUS_TARGET_BITRATE_BPS)));
      opus_encoder_ctl(g_encoder, OPUS_SET_COMPLEXITY(static_cast<opus_int32>(OPUS_COMPLEXITY)));
      opus_encoder_ctl(g_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
      opus_encoder_ctl(g_encoder, OPUS_SET_VBR(0));
      g_encoderReady = true;
      Serial.printf("[opus] encoder ready: %lu Hz, mono, %u ms, bitrate=%lu bps, complexity=%u\n",
                    static_cast<unsigned long>(wsp::kDefaultSampleRate),
                    wsp::kDefaultFrameMs,
                    static_cast<unsigned long>(OPUS_TARGET_BITRATE_BPS),
                    OPUS_COMPLEXITY);
    }
  }

  if (g_decoder == nullptr) {
    int decErr = OPUS_OK;
    g_decoder = opus_decoder_create(
        static_cast<opus_int32>(wsp::kDefaultSampleRate),
        1,
        &decErr);

    if (decErr != OPUS_OK || g_decoder == nullptr) {
      Serial.printf("[opus] opus_decoder_create failed: %d, free heap=%lu bytes\n",
                    decErr, static_cast<unsigned long>(ESP.getFreeHeap()));
      g_decoderReady = false;
      ok = false;
    } else {
      g_decoderReady = true;
      Serial.printf("[opus] decoder ready: %lu Hz, mono\n",
                    static_cast<unsigned long>(wsp::kDefaultSampleRate));
    }
  }

  if (g_encoderReady && g_decoderReady) {
    g_status = "encoder and decoder ready";
  } else if (g_encoderReady) {
    g_status = "encoder ready, decoder failed";
  } else if (g_decoderReady) {
    g_status = "decoder ready, encoder failed";
  } else {
    g_status = "opus encoder/decoder failed";
  }

  return ok;
#else
  g_encoderReady = false;
  g_decoderReady = false;
  g_status = "opus disabled at compile time";
  return false;
#endif
}

bool opusEncoderReady() {
  return g_encoderReady;
}

bool opusDecoderReady() {
  return g_decoderReady;
}

bool opusReady() {
  return g_encoderReady;
}

const char* opusStatus() {
  return g_status;
}

bool opusEncodeFrame(const int16_t* pcm,
                     uint16_t sampleCount,
                     uint8_t* outEncoded,
                     uint16_t outCapacity,
                     uint16_t* outEncodedSize) {
  if (outEncodedSize) *outEncodedSize = 0;
  if (!g_encoderReady || pcm == nullptr || outEncoded == nullptr || outEncodedSize == nullptr) {
    return false;
  }

#if WAVE_STATION_HAS_OPUS
  const opus_int32 n = opus_encode(
      g_encoder,
      reinterpret_cast<const opus_int16*>(pcm),
      static_cast<int>(sampleCount),
      outEncoded,
      static_cast<opus_int32>(outCapacity));

  if (n < 0) {
    Serial.printf("[opus] encode failed: %ld\n", static_cast<long>(n));
    return false;
  }

  if (n == 0 || n > outCapacity) {
    Serial.printf("[opus] invalid encoded size: %ld\n", static_cast<long>(n));
    return false;
  }

  *outEncodedSize = static_cast<uint16_t>(n);
  return true;
#else
  (void)pcm;
  (void)sampleCount;
  (void)outEncoded;
  (void)outCapacity;
  return false;
#endif
}

bool opusDecodeFrame(const uint8_t* encoded,
                     uint16_t encodedSize,
                     int16_t* outPcm,
                     uint16_t outCapacitySamples,
                     uint16_t* outSampleCount) {
  if (outSampleCount) *outSampleCount = 0;
  if (!g_decoderReady || encoded == nullptr || encodedSize == 0 || outPcm == nullptr || outSampleCount == nullptr) {
    return false;
  }

#if WAVE_STATION_HAS_OPUS
  const int n = opus_decode(
      g_decoder,
      encoded,
      static_cast<opus_int32>(encodedSize),
      reinterpret_cast<opus_int16*>(outPcm),
      static_cast<int>(outCapacitySamples),
      0);

  if (n < 0) {
    Serial.printf("[opus] decode failed: %d\n", n);
    return false;
  }

  if (n == 0 || n > outCapacitySamples) {
    Serial.printf("[opus] invalid decoded samples: %d\n", n);
    return false;
  }

  *outSampleCount = static_cast<uint16_t>(n);
  return true;
#else
  (void)encoded;
  (void)encodedSize;
  (void)outPcm;
  (void)outCapacitySamples;
  return false;
#endif
}


bool opusDecoderReset() {
#if WAVE_STATION_HAS_OPUS
  if (!g_decoderReady || g_decoder == nullptr) return false;
  const int rc = opus_decoder_ctl(g_decoder, OPUS_RESET_STATE);
  if (rc != OPUS_OK) {
    Serial.printf("[opus] decoder reset failed: %d\n", rc);
    return false;
  }
  Serial.println("[opus] decoder reset");
  return true;
#else
  return false;
#endif
}
