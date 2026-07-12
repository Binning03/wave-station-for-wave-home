#include "speaker_driver.h"

#include <driver/i2s.h>

#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

static constexpr i2s_port_t SPK_I2S_PORT = I2S_NUM_1;
static bool g_speakerReady = false;
static constexpr uint16_t SPK_MAX_SAMPLES = 960;  // up to 60 ms @ 16 kHz mono
static int16_t g_stereoBuffer[SPK_MAX_SAMPLES * 2];

bool speakerBegin(int bclkPin, int lrckPin, int dataOutPin) {
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = wsp::kDefaultSampleRate;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 6;
  i2sConfig.dma_buf_len = 256;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  i2sConfig.fixed_mclk = 0;

  i2s_pin_config_t pinConfig = {};
  pinConfig.bck_io_num = bclkPin;
  pinConfig.ws_io_num = lrckPin;
  pinConfig.data_out_num = dataOutPin;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(SPK_I2S_PORT, &i2sConfig, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[spk] i2s_driver_install failed: %d\n", err);
    g_speakerReady = false;
    return false;
  }

  err = i2s_set_pin(SPK_I2S_PORT, &pinConfig);
  if (err != ESP_OK) {
    Serial.printf("[spk] i2s_set_pin failed: %d\n", err);
    i2s_driver_uninstall(SPK_I2S_PORT);
    g_speakerReady = false;
    return false;
  }

  i2s_zero_dma_buffer(SPK_I2S_PORT);
  g_speakerReady = true;

  Serial.printf("[spk] MAX98357 I2S ready: BCLK=%d LRC=%d DIN=%d rate=%lu\n",
                bclkPin, lrckPin, dataOutPin,
                static_cast<unsigned long>(wsp::kDefaultSampleRate));
  return true;
}

bool speakerReady() {
  return g_speakerReady;
}

bool speakerPlayPCM16Mono(const int16_t* samples, uint16_t sampleCount) {
  if (!g_speakerReady || samples == nullptr || sampleCount == 0) return false;
  if (sampleCount > SPK_MAX_SAMPLES) return false;

  for (uint16_t i = 0; i < sampleCount; ++i) {
    // MAX98357 모듈의 채널 선택 설정과 무관하게 재생되도록 양쪽 슬롯에 복제합니다.
    g_stereoBuffer[i * 2] = samples[i];
    g_stereoBuffer[i * 2 + 1] = samples[i];
  }

  const size_t bytesToWrite = static_cast<size_t>(sampleCount) * 2 * sizeof(int16_t);
  size_t bytesWritten = 0;
  esp_err_t err = i2s_write(SPK_I2S_PORT,
                            g_stereoBuffer,
                            bytesToWrite,
                            &bytesWritten,
                            pdMS_TO_TICKS(100));

  return err == ESP_OK && bytesWritten == bytesToWrite;
}
