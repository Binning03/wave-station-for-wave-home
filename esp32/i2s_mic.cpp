#include "i2s_mic.h"
#include "wave_station.h"

#include <driver/i2s.h>

#ifndef I2S_COMM_FORMAT_STAND_I2S
#define I2S_COMM_FORMAT_STAND_I2S I2S_COMM_FORMAT_I2S
#endif

static constexpr i2s_port_t MIC_I2S_PORT = I2S_NUM_0;
static bool g_micReady = false;

// INMP441의 24-bit 유효 샘플은 32-bit I2S 슬롯에 정렬됩니다.
// 상위 유효 비트를 선택해 signed 16-bit PCM으로 변환합니다.
static constexpr int MIC_SHIFT_BITS = 14;

// 마이크의 L/R 설정과 I2S 채널 해석 차이에 대응하기 위해
// 양쪽 슬롯을 읽고 프레임 에너지가 큰 슬롯을 자동으로 선택합니다.
static constexpr bool MIC_AUTO_SELECT_SLOT = true;

static int16_t clamp16(int32_t value) {
  if (value > 32767) return 32767;
  if (value < -32768) return -32768;
  return static_cast<int16_t>(value);
}

static int16_t convertRaw32ToPcm16(int32_t raw) {
  return clamp16(raw >> MIC_SHIFT_BITS);
}

bool micBegin(int bclkPin, int lrckPin, int dataInPin) {
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = wsp::kDefaultSampleRate;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // 두 슬롯을 모두 수신한 뒤 micReadFrame()에서 유효 신호가 있는 채널을 선택합니다.
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = static_cast<i2s_comm_format_t>(I2S_COMM_FORMAT_STAND_I2S);
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 4;
  i2sConfig.dma_buf_len = 320;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = false;
  i2sConfig.fixed_mclk = 0;

  i2s_pin_config_t pinConfig = {};
  pinConfig.bck_io_num = bclkPin;
  pinConfig.ws_io_num = lrckPin;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = dataInPin;

  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &i2sConfig, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[mic] i2s_driver_install failed: %d\n", err);
    g_micReady = false;
    return false;
  }

  err = i2s_set_pin(MIC_I2S_PORT, &pinConfig);
  if (err != ESP_OK) {
    Serial.printf("[mic] i2s_set_pin failed: %d\n", err);
    i2s_driver_uninstall(MIC_I2S_PORT);
    g_micReady = false;
    return false;
  }

  i2s_zero_dma_buffer(MIC_I2S_PORT);
  g_micReady = true;

  Serial.printf("[mic] INMP441 I2S ready: BCLK=%d LRCK=%d DIN=%d rate=%lu slot=auto(L/R)\n",
                bclkPin,
                lrckPin,
                dataInPin,
                static_cast<unsigned long>(wsp::kDefaultSampleRate));

  return true;
}

bool micReadFrame(int16_t* outSamples, uint16_t sampleCount) {
  if (!g_micReady || outSamples == nullptr || sampleCount == 0) return false;
  if (sampleCount > 320) return false;

  // 한 샘플 시점마다 32-bit left/right 슬롯이 차례로 수신됩니다.
  static int32_t rawStereo32[320 * 2];
  const size_t bytesToRead = static_cast<size_t>(sampleCount) * 2 * sizeof(int32_t);
  size_t bytesRead = 0;

  esp_err_t err = i2s_read(MIC_I2S_PORT,
                           rawStereo32,
                           bytesToRead,
                           &bytesRead,
                           pdMS_TO_TICKS(80));

  if (err != ESP_OK || bytesRead != bytesToRead) {
    return false;
  }

  int64_t energy0 = 0;
  int64_t energy1 = 0;
  int16_t tmp0[320];
  int16_t tmp1[320];

  for (uint16_t i = 0; i < sampleCount; ++i) {
    const int16_t a = convertRaw32ToPcm16(rawStereo32[i * 2]);
    const int16_t b = convertRaw32ToPcm16(rawStereo32[i * 2 + 1]);
    tmp0[i] = a;
    tmp1[i] = b;
    energy0 += abs(static_cast<int>(a));
    energy1 += abs(static_cast<int>(b));
  }

  const bool useSlot1 = MIC_AUTO_SELECT_SLOT && (energy1 > energy0);
  const int16_t* selected = useSlot1 ? tmp1 : tmp0;
  for (uint16_t i = 0; i < sampleCount; ++i) {
    outSamples[i] = selected[i];
  }

  static uint32_t frameCounter = 0;
  frameCounter++;
  if (frameCounter == 1 || frameCounter % 250 == 0) {
    Serial.printf("[mic] slotEnergy A=%lld B=%lld selected=%c\n",
                  static_cast<long long>(energy0),
                  static_cast<long long>(energy1),
                  useSlot1 ? 'B' : 'A');
  }

  return true;
}
