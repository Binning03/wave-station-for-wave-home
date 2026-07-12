#pragma once

#include <Arduino.h>

constexpr uint16_t IR_MAX_RAW_LENGTH = 512;

struct IrCapture {
  uint16_t rawUs[IR_MAX_RAW_LENGTH];
  uint16_t length = 0;
  bool overflow = false;
};

void irClearReceive();
void irBegin(uint16_t recvPin, uint16_t sendPin);
bool irReceiveRaw(IrCapture& out);
void irSendRaw(const uint16_t* rawUs, uint16_t length, uint32_t carrierHz, uint16_t repeat);
