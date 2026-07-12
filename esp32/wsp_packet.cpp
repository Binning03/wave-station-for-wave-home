#include "wsp_packet.h"

bool wspWritePacket(WiFiClient& client, const uint8_t* data, size_t size) {
  if (!client || !client.connected() || data == nullptr) return false;

  size_t sent = 0;
  const uint32_t startMs = millis();

  while (sent < size) {
    if (!client || !client.connected()) return false;

    const size_t chunk = client.write(data + sent, size - sent);
    if (chunk > 0) {
      sent += chunk;
      continue;
    }

    // 송신 버퍼가 잠시 가득 찬 경우 즉시 실패하지 않고 제한 시간 동안 재시도합니다.
    if (millis() - startMs > 1000) {
      Serial.printf("[net] write timeout: sent=%u size=%u\n",
                    static_cast<unsigned>(sent),
                    static_cast<unsigned>(size));
      return false;
    }

    delay(1);
  }

  return true;
}

static bool sendControl(WiFiClient& client, wsp::Type type, uint32_t requestId,
                        const uint8_t* payload, uint32_t payloadSize) {
  if (payloadSize > wsp::kMaxPayload) return false;

  static uint8_t packet[wsp::kHeaderSize + 128];
  if (payloadSize > 128) return false;

  wsp::ControlHeader hdr;
  hdr.magic = wsp::kMagic;
  hdr.version = wsp::kProtoVer;
  hdr.reserved = 0;
  hdr.type = static_cast<uint16_t>(type);
  hdr.payloadSize = payloadSize;
  hdr.requestId = requestId;

  memcpy(packet, &hdr, sizeof(hdr));
  if (payloadSize > 0 && payload != nullptr) {
    memcpy(packet + sizeof(hdr), payload, payloadSize);
  }

  return wspWritePacket(client, packet, sizeof(hdr) + payloadSize);
}

bool wspSendAck(WiFiClient& client, uint32_t requestId, uint8_t status) {
  wsp::AckBody body;
  body.requestId = requestId;
  body.status = status;
  body.reserved[0] = 0;
  body.reserved[1] = 0;
  body.reserved[2] = 0;

  Serial.printf("[tx] Ack requestId=%lu status=%u\n",
                static_cast<unsigned long>(requestId),
                status);

  return sendControl(client, wsp::Type::Ack, requestId,
                     reinterpret_cast<const uint8_t*>(&body),
                     sizeof(body));
}

bool wspSendError(WiFiClient& client, uint32_t requestId, wsp::ErrorCode code, const char* message) {
  const size_t msgLen = message ? strlen(message) : 0;
  const size_t maxMsgLen = 96;
  const size_t clippedMsgLen = msgLen > maxMsgLen ? maxMsgLen : msgLen;
  const uint32_t payloadSize = sizeof(wsp::ErrorBody) + clippedMsgLen;

  static uint8_t payload[sizeof(wsp::ErrorBody) + maxMsgLen];

  wsp::ErrorBody body;
  body.requestId = requestId;
  body.code = static_cast<int32_t>(code);

  memcpy(payload, &body, sizeof(body));
  if (clippedMsgLen > 0) {
    memcpy(payload + sizeof(body), message, clippedMsgLen);
  }

  Serial.printf("[tx] Error requestId=%lu code=%ld msg=%s\n",
                static_cast<unsigned long>(requestId),
                static_cast<long>(body.code),
                message ? message : "");

  return sendControl(client, wsp::Type::Error, requestId, payload, payloadSize);
}

bool wspSendHeartbeat(WiFiClient& client, uint32_t requestId) {
  Serial.printf("[tx] Heartbeat requestId=%lu\n", static_cast<unsigned long>(requestId));
  return sendControl(client, wsp::Type::Heartbeat, requestId, nullptr, 0);
}

bool wspSendMicPCM(WiFiClient& client, const int16_t* samples, uint16_t sampleCount, uint32_t sequence) {
  if (samples == nullptr || sampleCount == 0) return false;

  wsp::AudioPCMBody body;
  body.sampleRate = wsp::kDefaultSampleRate;
  body.channels = 1;
  body.bitsPerSample = 16;
  body.sampleCount = sampleCount;

  const size_t pcmBytes = wsp::pcm_data_size(body);
  const uint32_t payloadSize = sizeof(wsp::AudioPCMBody) + pcmBytes;

  if (payloadSize > wsp::kMaxPayload) return false;

  // 20 ms MicPCM 프레임의 전체 패킷 크기는 664 bytes입니다.
  static uint8_t packet[wsp::kHeaderSize + sizeof(wsp::AudioPCMBody) + 640];
  const size_t packetCapacity = sizeof(packet);
  const size_t packetSize = wsp::kHeaderSize + payloadSize;
  if (packetSize > packetCapacity) return false;

  wsp::DataHeader hdr;
  hdr.magic = wsp::kMagic;
  hdr.version = wsp::kProtoVer;
  hdr.flags = wsp::HeaderFlag_None;
  hdr.type = static_cast<uint16_t>(wsp::Type::MicPCM);
  hdr.payloadSize = payloadSize;
  hdr.sequence = sequence;

  size_t offset = 0;
  memcpy(packet + offset, &hdr, sizeof(hdr));
  offset += sizeof(hdr);
  memcpy(packet + offset, &body, sizeof(body));
  offset += sizeof(body);
  memcpy(packet + offset, samples, pcmBytes);
  offset += pcmBytes;

  return wspWritePacket(client, packet, offset);
}

bool wspSendMicComp(WiFiClient& client, const uint8_t* encoded, uint16_t encodedSize, uint32_t sequence) {
  if (encoded == nullptr || encodedSize == 0) return false;

  wsp::AudioCompBody body;
  body.codec = static_cast<uint8_t>(wsp::AudioCodec::Opus);
  body.sampleRate = wsp::kDefaultSampleRate;
  body.channels = 1;
  body.frameDurationMs = wsp::kDefaultFrameMs;
  body.encodedSize = encodedSize;

  const uint32_t payloadSize = sizeof(wsp::AudioCompBody) + encodedSize;
  if (payloadSize > wsp::kMaxPayload) return false;

  // Opus 프레임 크기 변동을 고려해 최대 512 bytes의 인코딩 영역을 확보합니다.
  static uint8_t packet[wsp::kHeaderSize + sizeof(wsp::AudioCompBody) + 512];
  const size_t packetSize = wsp::kHeaderSize + payloadSize;
  if (packetSize > sizeof(packet)) return false;

  wsp::DataHeader hdr;
  hdr.magic = wsp::kMagic;
  hdr.version = wsp::kProtoVer;
  hdr.flags = wsp::HeaderFlag_None;
  hdr.type = static_cast<uint16_t>(wsp::Type::MicComp);
  hdr.payloadSize = payloadSize;
  hdr.sequence = sequence;

  size_t offset = 0;
  memcpy(packet + offset, &hdr, sizeof(hdr));
  offset += sizeof(hdr);
  memcpy(packet + offset, &body, sizeof(body));
  offset += sizeof(body);
  memcpy(packet + offset, encoded, encodedSize);
  offset += encodedSize;

  return wspWritePacket(client, packet, offset);
}

bool wspSendIrReceive(WiFiClient& client, const uint16_t* rawUs, uint16_t length, bool overflow, uint32_t sequence) {
  if (rawUs == nullptr || length == 0) return false;

  const size_t rawBytes = wsp::ir_raw_data_size(length);
  const uint32_t payloadSize = sizeof(wsp::IrReceiveBody) + rawBytes;
  if (payloadSize > wsp::kMaxPayload) return false;

  // IR raw 최대 512개를 담는 1,024-byte 영역입니다.
  static uint8_t packet[wsp::kHeaderSize + sizeof(wsp::IrReceiveBody) + 1024];
  const size_t packetSize = wsp::kHeaderSize + payloadSize;
  if (packetSize > sizeof(packet)) return false;

  wsp::DataHeader hdr;
  hdr.magic = wsp::kMagic;
  hdr.version = wsp::kProtoVer;
  hdr.flags = wsp::HeaderFlag_None;
  hdr.type = static_cast<uint16_t>(wsp::Type::IrReceive);
  hdr.payloadSize = payloadSize;
  hdr.sequence = sequence;

  wsp::IrReceiveBody body;
  body.length = length;
  body.overflow = overflow ? 1 : 0;
  body.reserved = 0;

  size_t offset = 0;
  memcpy(packet + offset, &hdr, sizeof(hdr));
  offset += sizeof(hdr);
  memcpy(packet + offset, &body, sizeof(body));
  offset += sizeof(body);
  memcpy(packet + offset, rawUs, rawBytes);
  offset += rawBytes;

  return wspWritePacket(client, packet, offset);
}

bool wspSendSensor(WiFiClient& client, wsp::Type type, wsp::SensorUnit unit, float value, uint8_t quality, uint32_t sequence) {
  if (type != wsp::Type::AmbientLight && type != wsp::Type::Temperature && type != wsp::Type::Humidity) {
    return false;
  }

  wsp::SensorBody body;
  body.unit = static_cast<uint8_t>(unit);
  body.quality = quality;
  body.reserved = 0;
  body.value = value;

  const uint32_t payloadSize = sizeof(wsp::SensorBody);
  static uint8_t packet[wsp::kHeaderSize + sizeof(wsp::SensorBody)];

  wsp::DataHeader hdr;
  hdr.magic = wsp::kMagic;
  hdr.version = wsp::kProtoVer;
  hdr.flags = wsp::HeaderFlag_None;
  hdr.type = static_cast<uint16_t>(type);
  hdr.payloadSize = payloadSize;
  hdr.sequence = sequence;

  size_t offset = 0;
  memcpy(packet + offset, &hdr, sizeof(hdr));
  offset += sizeof(hdr);
  memcpy(packet + offset, &body, sizeof(body));
  offset += sizeof(body);

  return wspWritePacket(client, packet, offset);
}
