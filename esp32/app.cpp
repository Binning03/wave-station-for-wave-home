#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <math.h>

// Opus 인코딩/디코딩과 오디오 버퍼가 사용하는 스택을 고려해
// Arduino Core가 생성하는 loopTask의 스택 크기를 확장합니다.
size_t getArduinoLoopTaskStackSize(void) {
  return 49152;
}

#include "wave_station.h"
#include "wsp_packet.h"
#include "i2s_mic.h"
#include "opus_codec.h"
#include "ir_driver.h"
#include "sensor_driver.h"
#include "speaker_driver.h"

// -----------------------------------------------------------------------------
// Wi-Fi 접속 정보
// -----------------------------------------------------------------------------
const char* WIFI_SSID = "PUT_YOUR_WIFI_SSID";
const char* WIFI_PASS = "PUT_YOUR_WIFI_PASSWORD";

// -----------------------------------------------------------------------------
// 하드웨어 핀
// -----------------------------------------------------------------------------
constexpr int I2S_MIC_BCLK_PIN = 14;  // INMP441 SCK
constexpr int I2S_MIC_LRCK_PIN = 15;  // INMP441 WS
constexpr int I2S_MIC_DATA_PIN = 32;  // INMP441 SD

constexpr uint16_t IR_RECV_PIN = 27;  // IR 수신 OUT
constexpr uint16_t IR_SEND_PIN = 26;  // IR 송신 S

constexpr uint8_t DHT22_DATA_PIN = 25;

constexpr int BH1750_SDA_PIN = 21;
constexpr int BH1750_SCL_PIN = 22;

constexpr int SPK_BCLK_PIN = 18;  // MAX98357 BCLK
constexpr int SPK_LRCK_PIN = 19;  // MAX98357 LRC
constexpr int SPK_DIN_PIN  = 23;  // MAX98357 DIN

// -----------------------------------------------------------------------------
// 네트워크 및 세션 상태
// -----------------------------------------------------------------------------
WiFiServer server(wsp::kTcpPort);
WiFiClient client;

SubscriptionState sub;

uint32_t micPcmSeq = 0;
uint32_t micCompSeq = 0;
uint32_t irReceiveSeq = 0;
uint32_t ambientSeq = 0;
uint32_t temperatureSeq = 0;
uint32_t humiditySeq = 0;

// TCP는 스트림이므로 수신 데이터를 누적한 뒤 완성된 WSP 패킷 단위로 파싱합니다.
constexpr size_t RX_BUFFER_SIZE = 8192;
uint8_t rxBuffer[RX_BUFFER_SIZE];
size_t rxSize = 0;

// MicPCM 한 프레임: 16 kHz × 20 ms = 320 samples = 640 bytes (mono, 16-bit).
constexpr uint16_t MIC_FRAME_SAMPLES = 320;
int16_t micFrame[MIC_FRAME_SAMPLES];

static int16_t spkDecodePcm[OPUS_MAX_DECODED_SAMPLES];

// 센서 전송 주기 및 변화 감지 상태
uint32_t lastAmbientMs = 0;
uint32_t lastTempMs = 0;
uint32_t lastHumMs = 0;
bool haveLastAmbient = false;
bool haveLastTemp = false;
bool haveLastHum = false;
float lastAmbientValue = 0.0f;
float lastTempValue = 0.0f;
float lastHumValue = 0.0f;

// -----------------------------------------------------------------------------
// 공통 유틸리티
// -----------------------------------------------------------------------------
static uint16_t normalizedSensorInterval(uint16_t intervalMs, uint16_t minMs, uint16_t defaultMs) {
  uint16_t value = intervalMs == 0 ? defaultMs : intervalMs;
  if (value < minMs) value = minMs;
  return value;
}

static bool changedEnough(float prev, float now, float threshold) {
  return fabsf(now - prev) >= threshold;
}

void resetSessionState() {
  sub = SubscriptionState{};
  rxSize = 0;
  micPcmSeq = 0;
  micCompSeq = 0;
  irReceiveSeq = 0;
  ambientSeq = 0;
  temperatureSeq = 0;
  humiditySeq = 0;
  lastAmbientMs = 0;
  lastTempMs = 0;
  lastHumMs = 0;
  haveLastAmbient = false;
  haveLastTemp = false;
  haveLastHum = false;
}

void disconnectClient(const char* reason) {
  Serial.print("[net] disconnect: ");
  Serial.println(reason);

  if (client) {
    client.stop();
  }

  resetSessionState();
}

void printIpInfo() {
  Serial.print("WiFi connected, IP=");
  Serial.print(WiFi.localIP());
  Serial.print(", listening on ");
  Serial.println(wsp::kTcpPort);
}

void connectWifi() {
  Serial.print("[wifi] connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }

  Serial.println();
  printIpInfo();
}

// -----------------------------------------------------------------------------
// WSP 제어 및 데이터 패킷 처리
// -----------------------------------------------------------------------------
void handleSubscribe(uint32_t requestId, const uint8_t* payload, uint32_t payloadSize) {
  if (payloadSize != sizeof(wsp::SubscribeBody)) {
    wspSendError(client, requestId, wsp::ErrorInvalidPayload, "invalid SubscribeBody size");
    return;
  }

  wsp::SubscribeBody body;
  memcpy(&body, payload, sizeof(body));

  const uint16_t micPcmType = static_cast<uint16_t>(wsp::Type::MicPCM);
  const uint16_t micCompType = static_cast<uint16_t>(wsp::Type::MicComp);
  const uint16_t irReceiveType = static_cast<uint16_t>(wsp::Type::IrReceive);
  const uint16_t ambientType = static_cast<uint16_t>(wsp::Type::AmbientLight);
  const uint16_t temperatureType = static_cast<uint16_t>(wsp::Type::Temperature);
  const uint16_t humidityType = static_cast<uint16_t>(wsp::Type::Humidity);

  const bool onChangeOnly = (body.options & wsp::SubscribeOptionFlag_OnChangeOnlyBits) != 0;

  Serial.printf("[rx] Subscribe target=0x%04X intervalMs=%u options=0x%08lX requestId=%lu\n",
                body.targetType,
                body.intervalMs,
                static_cast<unsigned long>(body.options),
                static_cast<unsigned long>(requestId));

  if (body.targetType == micPcmType) {
    sub.mic_pcm = true;
    wspSendAck(client, requestId, 0);
    Serial.println("[sub] MicPCM ON");
    return;
  }

  if (body.targetType == micCompType) {
    if (!opusEncoderReady()) {
      Serial.printf("[sub] MicComp rejected: %s\n", opusStatus());
      wspSendError(client, requestId, wsp::ErrorUnsupported, "MicComp Opus encoder not ready");
      return;
    }
    sub.mic_comp = true;
    wspSendAck(client, requestId, 0);
    Serial.println("[sub] MicComp ON");
    return;
  }

  if (body.targetType == irReceiveType) {
    irClearReceive();  // 구독 이전에 남아 있던 수신 결과를 폐기합니다.
    sub.ir_receive = true;
    wspSendAck(client, requestId, 0);
    Serial.println("[sub] IrReceive ON");
    return;
  }

  if (body.targetType == ambientType) {
    if (!lightReady()) {
      wspSendError(client, requestId, wsp::ErrorUnsupported, "BH1750 ambient light sensor not ready");
      return;
    }
    sub.ambient_light = true;
    sub.ambient_interval_ms = normalizedSensorInterval(body.intervalMs, 200, 1000);
    sub.ambient_on_change_only = onChangeOnly;
    lastAmbientMs = 0;
    haveLastAmbient = false;
    wspSendAck(client, requestId, 0);
    Serial.printf("[sub] AmbientLight ON interval=%u onChange=%u\n", sub.ambient_interval_ms, onChangeOnly ? 1 : 0);
    return;
  }

  if (body.targetType == temperatureType) {
    if (!dhtReady()) {
      wspSendError(client, requestId, wsp::ErrorUnsupported, "DHT22 not ready");
      return;
    }
    sub.temperature = true;
    sub.temperature_interval_ms = normalizedSensorInterval(body.intervalMs, 2000, 2000);
    sub.temperature_on_change_only = onChangeOnly;
    lastTempMs = 0;
    haveLastTemp = false;
    wspSendAck(client, requestId, 0);
    Serial.printf("[sub] Temperature ON interval=%u onChange=%u\n", sub.temperature_interval_ms, onChangeOnly ? 1 : 0);
    return;
  }

  if (body.targetType == humidityType) {
    if (!dhtReady()) {
      wspSendError(client, requestId, wsp::ErrorUnsupported, "DHT22 not ready");
      return;
    }
    sub.humidity = true;
    sub.humidity_interval_ms = normalizedSensorInterval(body.intervalMs, 2000, 2000);
    sub.humidity_on_change_only = onChangeOnly;
    lastHumMs = 0;
    haveLastHum = false;
    wspSendAck(client, requestId, 0);
    Serial.printf("[sub] Humidity ON interval=%u onChange=%u\n", sub.humidity_interval_ms, onChangeOnly ? 1 : 0);
    return;
  }

  // 스피커 출력은 호스트가 보내는 일회성 Data 패킷으로 처리합니다.
  wspSendError(client, requestId, wsp::ErrorUnsupported, "target type not supported for Subscribe");
}

void handleUnsubscribe(uint32_t requestId, const uint8_t* payload, uint32_t payloadSize) {
  if (payloadSize != sizeof(wsp::UnsubscribeBody)) {
    wspSendError(client, requestId, wsp::ErrorInvalidPayload, "invalid UnsubscribeBody size");
    return;
  }

  wsp::UnsubscribeBody body;
  memcpy(&body, payload, sizeof(body));

  Serial.printf("[rx] Unsubscribe target=0x%04X requestId=%lu\n",
                body.targetType,
                static_cast<unsigned long>(requestId));

  if (body.targetType == static_cast<uint16_t>(wsp::Type::MicPCM)) {
    sub.mic_pcm = false;
    Serial.println("[sub] MicPCM OFF");
    wspSendAck(client, requestId, 0);
    return;
  }

  if (body.targetType == static_cast<uint16_t>(wsp::Type::MicComp)) {
    sub.mic_comp = false;
    Serial.println("[sub] MicComp OFF");
    wspSendAck(client, requestId, 0);
    return;
  }

  if (body.targetType == static_cast<uint16_t>(wsp::Type::IrReceive)) {
    sub.ir_receive = false;
    Serial.println("[sub] IrReceive OFF");
    wspSendAck(client, requestId, 0);
    return;
  }

  if (body.targetType == static_cast<uint16_t>(wsp::Type::AmbientLight)) {
    sub.ambient_light = false;
    Serial.println("[sub] AmbientLight OFF");
    wspSendAck(client, requestId, 0);
    return;
  }

  if (body.targetType == static_cast<uint16_t>(wsp::Type::Temperature)) {
    sub.temperature = false;
    Serial.println("[sub] Temperature OFF");
    wspSendAck(client, requestId, 0);
    return;
  }

  if (body.targetType == static_cast<uint16_t>(wsp::Type::Humidity)) {
    sub.humidity = false;
    Serial.println("[sub] Humidity OFF");
    wspSendAck(client, requestId, 0);
    return;
  }

  wspSendError(client, requestId, wsp::ErrorUnsupported, "unsubscribe target type not supported");
}

void handleIrTransmit(uint32_t sequence, const uint8_t* payload, uint32_t payloadSize) {
  if (payloadSize < sizeof(wsp::IrTransmitBody)) {
    Serial.println("[rx] IrTransmit invalid: payload too short");
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "invalid IrTransmit payload too short");
    return;
  }

  wsp::IrTransmitBody body;
  memcpy(&body, payload, sizeof(body));

  const uint32_t expected = sizeof(wsp::IrTransmitBody) + wsp::ir_raw_data_size(body.length);
  if (payloadSize != expected) {
    Serial.printf("[rx] IrTransmit invalid size: got=%lu expected=%lu\n",
                  static_cast<unsigned long>(payloadSize),
                  static_cast<unsigned long>(expected));
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "invalid IrTransmit payload size");
    return;
  }

  if (body.length == 0 || body.length > IR_MAX_RAW_LENGTH) {
    Serial.printf("[rx] IrTransmit invalid length=%u\n", body.length);
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "invalid IrTransmit raw length");
    return;
  }

  const uint8_t* rawPtr = payload + sizeof(wsp::IrTransmitBody);
  static uint16_t raw[IR_MAX_RAW_LENGTH];
  memcpy(raw, rawPtr, body.length * sizeof(uint16_t));

  Serial.printf("[rx] IrTransmit seq=%lu length=%u carrierHz=%lu repeat=%u\n",
                static_cast<unsigned long>(sequence),
                body.length,
                static_cast<unsigned long>(body.carrierHz),
                body.repeat);

  irSendRaw(raw, body.length, body.carrierHz, body.repeat);
}

void handleSpkPCM(uint32_t sequence, const uint8_t* payload, uint32_t payloadSize) {
  if (!speakerReady()) {
    wspSendError(client, sequence, wsp::ErrorUnsupported, "speaker not ready");
    return;
  }

  if (payloadSize < sizeof(wsp::AudioPCMBody)) {
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "SpkPCM payload too short");
    return;
  }

  wsp::AudioPCMBody body;
  memcpy(&body, payload, sizeof(body));

  if (body.sampleRate != wsp::kDefaultSampleRate || body.channels != 1 || body.bitsPerSample != 16) {
    wspSendError(client, sequence, wsp::ErrorUnsupported, "unsupported SpkPCM format");
    return;
  }

  const size_t pcmBytes = wsp::pcm_data_size(body);
  const uint32_t expected = sizeof(wsp::AudioPCMBody) + pcmBytes;
  if (payloadSize != expected) {
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "invalid SpkPCM payload size");
    return;
  }

  if (body.sampleCount == 0 || body.sampleCount > OPUS_MAX_DECODED_SAMPLES) {
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "invalid SpkPCM sample count");
    return;
  }

  const int16_t* samples = reinterpret_cast<const int16_t*>(payload + sizeof(wsp::AudioPCMBody));
  if (!speakerPlayPCM16Mono(samples, body.sampleCount)) {
    wspSendError(client, sequence, wsp::ErrorBusy, "SpkPCM playback failed");
    return;
  }

  static uint32_t lastLogMs = 0;
  if (millis() - lastLogMs > 1000) {
    lastLogMs = millis();
    Serial.printf("[rx] SpkPCM seq=%lu samples=%u\n",
                  static_cast<unsigned long>(sequence),
                  body.sampleCount);
  }
}

void handleSpkComp(uint32_t sequence, uint8_t flags, const uint8_t* payload, uint32_t payloadSize) {
  if (!speakerReady()) {
    wspSendError(client, sequence, wsp::ErrorUnsupported, "speaker not ready");
    return;
  }
  if (!opusDecoderReady()) {
    wspSendError(client, sequence, wsp::ErrorUnsupported, "Opus decoder not ready");
    return;
  }

  if (payloadSize < sizeof(wsp::AudioCompBody)) {
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "SpkComp payload too short");
    return;
  }

  wsp::AudioCompBody body;
  memcpy(&body, payload, sizeof(body));

  if (body.codec != static_cast<uint8_t>(wsp::AudioCodec::Opus) ||
      body.sampleRate != wsp::kDefaultSampleRate ||
      body.channels != 1) {
    wspSendError(client, sequence, wsp::ErrorUnsupported, "unsupported SpkComp format");
    return;
  }

  const uint32_t expected = sizeof(wsp::AudioCompBody) + body.encodedSize;
  if (payloadSize != expected || body.encodedSize == 0) {
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "invalid SpkComp payload size");
    return;
  }

  if ((flags & wsp::HeaderFlag_KeyFrameBit) != 0) {
    opusDecoderReset();
  }

  const uint8_t* encoded = payload + sizeof(wsp::AudioCompBody);
  uint16_t decodedSamples = 0;
  if (!opusDecodeFrame(encoded, body.encodedSize, spkDecodePcm, OPUS_MAX_DECODED_SAMPLES, &decodedSamples)) {
    wspSendError(client, sequence, wsp::ErrorInvalidPayload, "SpkComp decode failed");
    return;
  }

  if (!speakerPlayPCM16Mono(spkDecodePcm, decodedSamples)) {
    wspSendError(client, sequence, wsp::ErrorBusy, "SpkComp playback failed");
    return;
  }

  static uint32_t lastLogMs = 0;
  if (millis() - lastLogMs > 1000) {
    lastLogMs = millis();
    Serial.printf("[rx] SpkComp seq=%lu encoded=%u decodedSamples=%u\n",
                  static_cast<unsigned long>(sequence),
                  body.encodedSize,
                  decodedSamples);
  }
}

void dispatchPacket(const wsp::ControlHeader& hdr, const uint8_t* payload) {
  const uint16_t type = hdr.type;

  if (type == static_cast<uint16_t>(wsp::Type::Heartbeat)) {
    Serial.printf("[rx] Heartbeat requestId=%lu\n", static_cast<unsigned long>(hdr.requestId));
    wspSendHeartbeat(client, hdr.requestId);
    return;
  }

  if (type == static_cast<uint16_t>(wsp::Type::Subscribe)) {
    handleSubscribe(hdr.requestId, payload, hdr.payloadSize);
    return;
  }

  if (type == static_cast<uint16_t>(wsp::Type::Unsubscribe)) {
    handleUnsubscribe(hdr.requestId, payload, hdr.payloadSize);
    return;
  }

  if (type == static_cast<uint16_t>(wsp::Type::IrTransmit)) {
    handleIrTransmit(hdr.requestId, payload, hdr.payloadSize);
    return;
  }

  if (type == static_cast<uint16_t>(wsp::Type::SpkPCM)) {
    handleSpkPCM(hdr.requestId, payload, hdr.payloadSize);
    return;
  }

  if (type == static_cast<uint16_t>(wsp::Type::SpkComp)) {
    handleSpkComp(hdr.requestId, hdr.reserved, payload, hdr.payloadSize);
    return;
  }

  Serial.printf("[rx] unsupported packet type=0x%04X payloadSize=%lu requestOrSeq=%lu\n",
                type,
                static_cast<unsigned long>(hdr.payloadSize),
                static_cast<unsigned long>(hdr.requestId));

  wspSendError(client, hdr.requestId, wsp::ErrorUnsupported, "packet type not supported in firmware");
}

void processRxBuffer() {
  while (rxSize >= wsp::kHeaderSize) {
    wsp::ControlHeader hdr;
    memcpy(&hdr, rxBuffer, sizeof(hdr));

    if (hdr.magic != wsp::kMagic) {
      Serial.printf("[wsp] bad magic: 0x%08lX\n", static_cast<unsigned long>(hdr.magic));
      disconnectClient("bad magic");
      return;
    }

    if (hdr.version != wsp::kProtoVer) {
      Serial.printf("[wsp] bad version: %u\n", hdr.version);
      disconnectClient("bad version");
      return;
    }

    if (hdr.payloadSize > wsp::kMaxPayload) {
      Serial.printf("[wsp] payload too large: %lu\n", static_cast<unsigned long>(hdr.payloadSize));
      disconnectClient("payload too large");
      return;
    }

    const size_t totalSize = wsp::kHeaderSize + static_cast<size_t>(hdr.payloadSize);
    if (rxSize < totalSize) {
      return;
    }

    const uint8_t* payload = rxBuffer + wsp::kHeaderSize;
    dispatchPacket(hdr, payload);

    const size_t remain = rxSize - totalSize;
    if (remain > 0) {
      memmove(rxBuffer, rxBuffer + totalSize, remain);
    }
    rxSize = remain;
  }
}

void pollTcpReceive() {
  if (!client || !client.connected()) return;

  while (client.available() > 0) {
    const size_t freeSpace = RX_BUFFER_SIZE - rxSize;
    if (freeSpace == 0) {
      disconnectClient("rx buffer overflow");
      return;
    }

    int toRead = client.available();
    if (toRead > static_cast<int>(freeSpace)) {
      toRead = static_cast<int>(freeSpace);
    }

    const int n = client.read(rxBuffer + rxSize, toRead);
    if (n <= 0) break;

    rxSize += static_cast<size_t>(n);
  }

  processRxBuffer();
}

// -----------------------------------------------------------------------------
// 기능별 비차단 폴링 루틴
// -----------------------------------------------------------------------------
void pollMic() {
  if (!client || !client.connected()) return;
  if (!sub.mic_pcm && !sub.mic_comp) return;

  if (!micReadFrame(micFrame, MIC_FRAME_SAMPLES)) {
    return;
  }

  if (sub.mic_pcm) {
    const bool ok = wspSendMicPCM(client, micFrame, MIC_FRAME_SAMPLES, micPcmSeq++);
    if (!ok) {
      disconnectClient("MicPCM send failed");
      return;
    }
  }

  if (sub.mic_comp) {
    static uint8_t encoded[OPUS_MAX_ENCODED_BYTES];
    uint16_t encodedSize = 0;

    if (!opusEncodeFrame(micFrame, MIC_FRAME_SAMPLES, encoded, sizeof(encoded), &encodedSize)) {
      Serial.println("[opus] encode failed. MicComp frame dropped.");
      return;
    }

    const bool ok = wspSendMicComp(client, encoded, encodedSize, micCompSeq++);
    if (!ok) {
      Serial.println("[net] MicComp send failed. frame dropped.");
      return;
    }

    static uint32_t lastMicCompLogMs = 0;
    if (millis() - lastMicCompLogMs > 2000) {
      lastMicCompLogMs = millis();
      Serial.printf("[tx] MicComp seq=%lu encoded=%u heap=%lu\n",
                    static_cast<unsigned long>(micCompSeq - 1),
                    encodedSize,
                    static_cast<unsigned long>(ESP.getFreeHeap()));
    }
  }
}

void pollIrReceive() {
  if (!client || !client.connected()) return;
  if (!sub.ir_receive) return;

  IrCapture capture;
  if (!irReceiveRaw(capture)) {
    return;
  }

  const bool ok = wspSendIrReceive(client, capture.rawUs, capture.length, capture.overflow, irReceiveSeq++);
  if (!ok) {
    disconnectClient("IrReceive send failed");
    return;
  }

  Serial.printf("[tx] IrReceive seq=%lu length=%u overflow=%u\n",
                static_cast<unsigned long>(irReceiveSeq - 1),
                capture.length,
                capture.overflow ? 1 : 0);
}

void maybeSendSensor(wsp::Type type,
                     wsp::SensorUnit unit,
                     bool valid,
                     float value,
                     uint32_t& seq,
                     bool onChangeOnly,
                     bool& haveLast,
                     float& lastValue,
                     float threshold) {
  const uint8_t quality = valid ? 255 : 0;
  if (valid && onChangeOnly && haveLast && !changedEnough(lastValue, value, threshold)) {
    return;
  }

  const bool ok = wspSendSensor(client, type, unit, value, quality, seq++);
  if (!ok) {
    disconnectClient("Sensor send failed");
    return;
  }

  if (valid) {
    lastValue = value;
    haveLast = true;
  }

  Serial.printf("[tx] Sensor type=0x%04X seq=%lu value=%.2f quality=%u\n",
                static_cast<uint16_t>(type),
                static_cast<unsigned long>(seq - 1),
                value,
                quality);
}

void pollSensors() {
  if (!client || !client.connected()) return;
  if (!sub.ambient_light && !sub.temperature && !sub.humidity) return;

  const uint32_t now = millis();
  const bool needAmbient = sub.ambient_light && (lastAmbientMs == 0 || now - lastAmbientMs >= sub.ambient_interval_ms);
  const bool needTemp = sub.temperature && (lastTempMs == 0 || now - lastTempMs >= sub.temperature_interval_ms);
  const bool needHum = sub.humidity && (lastHumMs == 0 || now - lastHumMs >= sub.humidity_interval_ms);

  if (!needAmbient && !needTemp && !needHum) return;

  SensorReadings r = sensorsReadAll();

  if (needAmbient) {
    lastAmbientMs = now;
    maybeSendSensor(wsp::Type::AmbientLight,
                    wsp::SensorUnit::UnitLux,
                    r.hasLux,
                    r.hasLux ? r.lux : 0.0f,
                    ambientSeq,
                    sub.ambient_on_change_only,
                    haveLastAmbient,
                    lastAmbientValue,
                    1.0f);
  }

  if (needTemp) {
    lastTempMs = now;
    maybeSendSensor(wsp::Type::Temperature,
                    wsp::SensorUnit::UnitCelsius,
                    r.hasTemperature,
                    r.hasTemperature ? r.temperatureC : 0.0f,
                    temperatureSeq,
                    sub.temperature_on_change_only,
                    haveLastTemp,
                    lastTempValue,
                    0.2f);
  }

  if (needHum) {
    lastHumMs = now;
    maybeSendSensor(wsp::Type::Humidity,
                    wsp::SensorUnit::UnitPercent,
                    r.hasHumidity,
                    r.hasHumidity ? r.humidityPct : 0.0f,
                    humiditySeq,
                    sub.humidity_on_change_only,
                    haveLastHum,
                    lastHumValue,
                    1.0f);
  }
}

// -----------------------------------------------------------------------------
// Arduino 진입점
// -----------------------------------------------------------------------------
void appSetup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== WaveStation firmware boot ===");
  Serial.printf("[boot] reset_reason=%d free_heap=%lu loop_stack=49152\n",
                static_cast<int>(esp_reset_reason()),
                static_cast<unsigned long>(ESP.getFreeHeap()));

  connectWifi();

  if (!micBegin(I2S_MIC_BCLK_PIN, I2S_MIC_LRCK_PIN, I2S_MIC_DATA_PIN)) {
    Serial.println("[mic] init failed. MicPCM/MicComp will not work.");
  }

  opusBegin();

  irBegin(IR_RECV_PIN, IR_SEND_PIN);

  sensorsBegin(DHT22_DATA_PIN, BH1750_SDA_PIN, BH1750_SCL_PIN);

  if (!speakerBegin(SPK_BCLK_PIN, SPK_LRCK_PIN, SPK_DIN_PIN)) {
    Serial.println("[spk] init failed. SpkPCM/SpkComp will not work.");
  }

  server.begin();
  server.setNoDelay(true);
  Serial.printf("[net] TCP server listening on %u\n", wsp::kTcpPort);
}

void appLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] disconnected. reconnecting...");
    connectWifi();
  }

  if (!client || !client.connected()) {
    WiFiClient newClient = server.available();
    if (newClient) {
      if (client) client.stop();
      client = newClient;
      client.setNoDelay(true);
      resetSessionState();
      Serial.println("[net] backend connected");
    }

    delay(5);
    return;
  }

  pollTcpReceive();
  pollIrReceive();
  pollSensors();
  pollMic();

  delay(1);
}
