#include "ir_driver.h"

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// rawbuf[0]의 leading gap까지 고려해 WSP 최대 길이보다 한 칸 크게 확보합니다.
static constexpr uint16_t IR_CAPTURE_BUFFER_SIZE = IR_MAX_RAW_LENGTH + 1;
static constexpr uint8_t IR_TIMEOUT_MS = 50;

static IRrecv* g_irRecv = nullptr;
static IRsend* g_irSend = nullptr;
static decode_results g_results;

void irClearReceive() {
  if (g_irRecv == nullptr) return;

  // 남아 있는 결과를 폐기하고 다음 수신을 준비합니다.
  if (g_irRecv->decode(&g_results)) {
    g_irRecv->resume();
  }
}

void irBegin(uint16_t recvPin, uint16_t sendPin) {
  if (g_irRecv != nullptr) {
    delete g_irRecv;
    g_irRecv = nullptr;
  }
  if (g_irSend != nullptr) {
    delete g_irSend;
    g_irSend = nullptr;
  }

  g_irRecv = new IRrecv(recvPin, IR_CAPTURE_BUFFER_SIZE, IR_TIMEOUT_MS, true);
  g_irSend = new IRsend(sendPin);

  g_irRecv->enableIRIn();
  g_irSend->begin();

  Serial.printf("[ir] ready: recvPin=%u sendPin=%u\n", recvPin, sendPin);
}

bool irReceiveRaw(IrCapture& out) {
  out.length = 0;
  out.overflow = false;

  if (g_irRecv == nullptr) return false;
  if (!g_irRecv->decode(&g_results)) return false;

  // 라이브러리 수신 버퍼가 넘친 경우 WSP overflow 필드에 반영합니다.
  // 불완전한 신호는 학습 데이터로 저장하지 않는 것이 안전합니다.
  out.overflow = g_results.overflow;

  // rawbuf[0]의 leading gap을 제외하고 첫 mark부터 전송합니다.
  uint16_t startIndex = 1;
  if (g_results.rawlen <= startIndex) {
    g_irRecv->resume();
    return false;
  }

  uint16_t len = g_results.rawlen - startIndex;
  if (len > IR_MAX_RAW_LENGTH) {
    len = IR_MAX_RAW_LENGTH;
    out.overflow = true;
  }

  for (uint16_t i = 0; i < len; ++i) {
    const uint32_t us = static_cast<uint32_t>(g_results.rawbuf[startIndex + i]) * kRawTick;
    out.rawUs[i] = us > 65535 ? 65535 : static_cast<uint16_t>(us);
  }

  out.length = len;

  // 다음 신호 수신을 재개합니다.
  g_irRecv->resume();
  return out.length > 0;
}

void irSendRaw(const uint16_t* rawUs, uint16_t length, uint32_t carrierHz, uint16_t repeat) {
  if (g_irSend == nullptr || rawUs == nullptr || length == 0) return;

  uint16_t carrierKHz = 38;
  if (carrierHz >= 1000) {
    carrierKHz = static_cast<uint16_t>(carrierHz / 1000);
  }
  if (carrierKHz == 0) carrierKHz = 38;

  const uint16_t sendCount = repeat + 1;
  for (uint16_t i = 0; i < sendCount; ++i) {
    g_irSend->sendRaw(rawUs, length, carrierKHz);
    delay(50);
  }

  Serial.printf("[ir] sendRaw done length=%u carrier=%ukHz count=%u\n",
                length,
                carrierKHz,
                sendCount);
}
