#!/usr/bin/env python3
"""
WaveStation Protocol 테스트 백엔드

역할:
  - ESP32가 TCP 서버(:41737), 이 프로그램은 백엔드 역할의 TCP 클라이언트입니다.
  - ESP32에 접속한 뒤 Heartbeat / Subscribe / Unsubscribe를 보냅니다.
  - ESP32에서 오는 MicPCM, MicComp, IrReceive, Sensor 패킷을 파싱해서 로그로 보여줍니다.

실행 예:
  python wave_station_test_backend.py 192.168.0.50 --subscribe micpcm
  python wave_station_test_backend.py 192.168.0.50 --subscribe miccomp --opus-out-dir opus_frames
  python wave_station_test_backend.py 192.168.0.50 --subscribe ir ambient temperature humidity
  python wave_station_test_backend.py 192.168.0.50 --subscribe all --pcm-out mic.raw
  python wave_station_test_backend.py 192.168.0.50 --send-ir-raw "9000,4500,560,560,560,1690"
  python wave_station_test_backend.py 192.168.0.50 --send-spkpcm-tone --tone-freq 880 --tone-duration 2
  python wave_station_test_backend.py 192.168.0.50 --send-spkpcm-wav test.wav
  python wave_station_test_backend.py 192.168.0.50 --send-spkcomp-dir opus_frames

"""

from __future__ import annotations

import argparse
import json
import math
import os
import socket
import struct
import sys
import threading
import time
import wave
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import BinaryIO, Optional

# -----------------------------------------------------------------------------
# WSP constants
# -----------------------------------------------------------------------------

MAGIC = 0x57535031  # WSP1 protocol identifier
PROTO_VER = 1
HEADER_SIZE = 16
MAX_PAYLOAD = 4096
TCP_PORT = 41737

# Header flags
FLAG_HAS_TIMESTAMP = 0x01
FLAG_LAST_FRAME = 0x02
FLAG_KEY_FRAME = 0x04

# Subscribe option flags
SUB_ON_CHANGE_ONLY = 1 << 0
SUB_COMPRESSED = 1 << 1

# Audio
AUDIO_CODEC_PCM = 0
AUDIO_CODEC_OPUS = 1

# Sensor units
UNIT_NAME = {
    1: "lux",
    2: "°C",
    3: "%",
}


class WspType(IntEnum):
    MicPCM = 0x0101
    MicComp = 0x0102
    SpkPCM = 0x0111
    SpkComp = 0x0112

    IrReceive = 0x0201
    IrTransmit = 0x0202

    AmbientLight = 0x0301
    Temperature = 0x0302
    Humidity = 0x0303

    Subscribe = 0xF001
    Unsubscribe = 0xF002
    Ack = 0xF010
    Error = 0xF011
    Heartbeat = 0xF020


DATA_TYPES = {
    WspType.MicPCM,
    WspType.MicComp,
    WspType.SpkPCM,
    WspType.SpkComp,
    WspType.IrReceive,
    WspType.IrTransmit,
    WspType.AmbientLight,
    WspType.Temperature,
    WspType.Humidity,
}

CONTROL_TYPES = {
    WspType.Subscribe,
    WspType.Unsubscribe,
    WspType.Ack,
    WspType.Error,
    WspType.Heartbeat,
}

SUBSCRIBE_ALIASES = {
    "micpcm": WspType.MicPCM,
    "miccomp": WspType.MicComp,
    "ir": WspType.IrReceive,
    "irreceive": WspType.IrReceive,
    "ambient": WspType.AmbientLight,
    "light": WspType.AmbientLight,
    "ambientlight": WspType.AmbientLight,
    "temperature": WspType.Temperature,
    "temp": WspType.Temperature,
    "humidity": WspType.Humidity,
    "hum": WspType.Humidity,
}

ALL_SUBSCRIPTIONS = [
    WspType.MicPCM,
    WspType.MicComp,
    WspType.IrReceive,
    WspType.AmbientLight,
    WspType.Temperature,
    WspType.Humidity,
]


@dataclass
class Packet:
    typ: WspType
    flags_or_reserved: int
    request_or_sequence: int
    payload: bytes

    @property
    def is_data(self) -> bool:
        return self.typ in DATA_TYPES

    @property
    def flags(self) -> int:
        return self.flags_or_reserved

    @property
    def sequence(self) -> int:
        return self.request_or_sequence

    @property
    def request_id(self) -> int:
        return self.request_or_sequence


class WspClient:
    def __init__(
        self,
        host: str,
        port: int = TCP_PORT,
        heartbeat_interval: float = 5.0,
        verbose: bool = False,
    ) -> None:
        self.host = host
        self.port = port
        self.heartbeat_interval = heartbeat_interval
        self.verbose = verbose
        self.sock: Optional[socket.socket] = None
        self._stop = threading.Event()
        self._request_id = 1
        self._data_sequence: dict[WspType, int] = {}
        self._send_lock = threading.Lock()
        self._heartbeat_thread: Optional[threading.Thread] = None

    def connect(self, timeout: float = 5.0) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=timeout)
        sock.settimeout(1.0)
        self.sock = sock
        print(f"[connect] {self.host}:{self.port} 연결됨")

    def close(self) -> None:
        self._stop.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def next_request_id(self) -> int:
        rid = self._request_id
        self._request_id += 1
        if self._request_id >= 0xFFFFFFFF:
            self._request_id = 1
        return rid

    def next_data_sequence(self, typ: WspType) -> int:
        seq = self._data_sequence.get(typ, 0)
        self._data_sequence[typ] = (seq + 1) & 0xFFFFFFFF
        return seq

    def send_all(self, data: bytes) -> None:
        if self.sock is None:
            raise RuntimeError("socket is not connected")
        with self._send_lock:
            self.sock.sendall(data)

    # -------------------------------------------------------------------------
    # Packet builders
    # -------------------------------------------------------------------------

    @staticmethod
    def build_control_header(typ: WspType, payload_size: int, request_id: int) -> bytes:
        # ControlHeader: uint32 magic, uint8 version, uint8 reserved, uint16 type,
        #                uint32 payloadSize, uint32 requestId
        return struct.pack("<IBBHII", MAGIC, PROTO_VER, 0, int(typ), payload_size, request_id)

    @staticmethod
    def build_data_header(typ: WspType, flags: int, payload_size: int, sequence: int) -> bytes:
        # DataHeader: uint32 magic, uint8 version, uint8 flags, uint16 type,
        #             uint32 payloadSize, uint32 sequence
        return struct.pack("<IBBHII", MAGIC, PROTO_VER, flags, int(typ), payload_size, sequence)

    def send_heartbeat(self) -> int:
        rid = self.next_request_id()
        pkt = self.build_control_header(WspType.Heartbeat, 0, rid)
        self.send_all(pkt)
        if self.verbose:
            print(f"[tx] Heartbeat requestId={rid}")
        return rid

    def send_subscribe(self, target_type: WspType, interval_ms: int = 0, options: int = 0) -> int:
        rid = self.next_request_id()
        body = struct.pack("<HHI", int(target_type), interval_ms, options)
        pkt = self.build_control_header(WspType.Subscribe, len(body), rid) + body
        self.send_all(pkt)
        print(
            f"[tx] Subscribe target={target_type.name}(0x{int(target_type):04X}) "
            f"intervalMs={interval_ms} options=0x{options:08X} requestId={rid}"
        )
        return rid

    def send_unsubscribe(self, target_type: WspType) -> int:
        rid = self.next_request_id()
        body = struct.pack("<H", int(target_type))
        pkt = self.build_control_header(WspType.Unsubscribe, len(body), rid) + body
        self.send_all(pkt)
        print(f"[tx] Unsubscribe target={target_type.name}(0x{int(target_type):04X}) requestId={rid}")
        return rid

    def send_ir_transmit(self, raw_us: list[int], carrier_hz: int = 38000, repeat: int = 0) -> int:
        if not raw_us:
            raise ValueError("IR raw data is empty")
        if len(raw_us) > 512:
            raise ValueError("IR raw length too large for test client; keep <= 512")
        for value in raw_us:
            if not 0 <= value <= 65535:
                raise ValueError(f"IR raw value out of uint16 range: {value}")

        body = struct.pack("<HIH", len(raw_us), carrier_hz, repeat)
        body += struct.pack("<" + "H" * len(raw_us), *raw_us)
        seq = self.next_data_sequence(WspType.IrTransmit)
        pkt = self.build_data_header(WspType.IrTransmit, 0, len(body), seq) + body
        self.send_all(pkt)
        print(
            f"[tx] IrTransmit length={len(raw_us)} carrierHz={carrier_hz} "
            f"repeat={repeat} raw={raw_us[:12]}{'...' if len(raw_us) > 12 else ''}"
        )
        return seq

    def send_spk_pcm_frame(
        self,
        pcm_i16: list[int],
        sample_rate: int = 16000,
        channels: int = 1,
        bits_per_sample: int = 16,
    ) -> int:
        if channels != 1:
            raise ValueError("현재 테스트 클라이언트는 SpkPCM mono만 지원합니다")
        if bits_per_sample != 16:
            raise ValueError("현재 테스트 클라이언트는 SpkPCM 16-bit만 지원합니다")
        if not pcm_i16:
            raise ValueError("PCM frame is empty")
        if len(pcm_i16) > 960:
            raise ValueError("PCM frame too large; use 20~60ms frame")
        for sample in pcm_i16:
            if not -32768 <= sample <= 32767:
                raise ValueError(f"PCM sample out of int16 range: {sample}")

        body = struct.pack("<IBBH", sample_rate, channels, bits_per_sample, len(pcm_i16))
        body += struct.pack("<" + "h" * len(pcm_i16), *pcm_i16)
        if len(body) > MAX_PAYLOAD:
            raise ValueError(f"SpkPCM payload too large: {len(body)}")
        seq = self.next_data_sequence(WspType.SpkPCM)
        pkt = self.build_data_header(WspType.SpkPCM, 0, len(body), seq) + body
        self.send_all(pkt)
        return seq

    def send_spkpcm_tone(
        self,
        freq_hz: float = 880.0,
        duration_s: float = 2.0,
        amplitude: float = 0.15,
        sample_rate: int = 16000,
        frame_ms: int = 20,
    ) -> None:
        if duration_s <= 0:
            raise ValueError("tone duration must be > 0")
        if not 0.0 < amplitude <= 1.0:
            raise ValueError("tone amplitude must be in (0, 1]")
        frame_samples = max(1, sample_rate * frame_ms // 1000)
        total_samples = int(sample_rate * duration_s)
        max_amp = int(32767 * amplitude)
        sent = 0
        next_deadline = time.perf_counter()
        while sent < total_samples:
            count = min(frame_samples, total_samples - sent)
            frame = [
                int(max_amp * math.sin(2.0 * math.pi * freq_hz * (sent + i) / sample_rate))
                for i in range(count)
            ]
            seq = self.send_spk_pcm_frame(frame, sample_rate=sample_rate)
            sent += count
            if seq == 0 or seq % 50 == 0:
                print(f"[tx] SpkPCM tone seq={seq} sentSamples={sent}/{total_samples}")
            next_deadline += frame_ms / 1000.0
            delay = next_deadline - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
        print(
            f"[tx] SpkPCM tone done freq={freq_hz}Hz duration={duration_s}s "
            f"sampleRate={sample_rate} amp={amplitude}"
        )

    def send_spkpcm_wav(self, wav_path: Path, frame_ms: int = 20) -> None:
        with wave.open(str(wav_path), "rb") as wf:
            channels = wf.getnchannels()
            sample_width = wf.getsampwidth()
            sample_rate = wf.getframerate()
            total_frames = wf.getnframes()
            if channels != 1:
                raise ValueError(f"WAV must be mono. got channels={channels}")
            if sample_width != 2:
                raise ValueError(f"WAV must be 16-bit PCM. got sample_width={sample_width}")
            frame_samples = max(1, sample_rate * frame_ms // 1000)
            sent = 0
            next_deadline = time.perf_counter()
            while True:
                data = wf.readframes(frame_samples)
                if not data:
                    break
                sample_count = len(data) // 2
                pcm = list(struct.unpack("<" + "h" * sample_count, data))
                seq = self.send_spk_pcm_frame(pcm, sample_rate=sample_rate)
                sent += sample_count
                if seq == 0 or seq % 50 == 0:
                    print(f"[tx] SpkPCM wav seq={seq} sentSamples={sent}/{total_frames}")
                next_deadline += frame_ms / 1000.0
                delay = next_deadline - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
        print(f"[tx] SpkPCM wav done path={wav_path} sampleRate={sample_rate} samples={sent}")

    def send_spk_comp_frame(
        self,
        encoded: bytes,
        sample_rate: int = 16000,
        channels: int = 1,
        frame_ms: int = 20,
    ) -> int:
        if not encoded:
            raise ValueError("encoded Opus frame is empty")
        body = struct.pack("<BIBBH", AUDIO_CODEC_OPUS, sample_rate, channels, frame_ms, len(encoded))
        body += encoded
        if len(body) > MAX_PAYLOAD:
            raise ValueError(f"SpkComp payload too large: {len(body)}")
        seq = self.next_data_sequence(WspType.SpkComp)
        pkt = self.build_data_header(WspType.SpkComp, 0, len(body), seq) + body
        self.send_all(pkt)
        return seq

    def send_spkcomp_dir(
        self,
        opus_dir: Path,
        sample_rate: int = 16000,
        frame_ms: int = 20,
    ) -> None:
        files = sorted(opus_dir.glob("*.opusframe"))
        if not files:
            raise ValueError(f"no .opusframe files found in {opus_dir}")
        next_deadline = time.perf_counter()
        for idx, path in enumerate(files):
            encoded = path.read_bytes()
            seq = self.send_spk_comp_frame(encoded, sample_rate=sample_rate, frame_ms=frame_ms)
            if idx == 0 or (idx + 1) % 50 == 0:
                print(f"[tx] SpkComp seq={seq} frames={idx + 1}/{len(files)} encoded={len(encoded)}")
            next_deadline += frame_ms / 1000.0
            delay = next_deadline - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
        print(f"[tx] SpkComp dir done frames={len(files)} path={opus_dir}")

    def start_heartbeat_loop(self) -> None:
        if self.heartbeat_interval <= 0:
            return

        def loop() -> None:
            while not self._stop.wait(self.heartbeat_interval):
                try:
                    self.send_heartbeat()
                except OSError as exc:
                    print(f"[heartbeat] 전송 실패: {exc}")
                    self._stop.set()
                    return

        self._heartbeat_thread = threading.Thread(target=loop, daemon=True)
        self._heartbeat_thread.start()

    # -------------------------------------------------------------------------
    # Packet reader
    # -------------------------------------------------------------------------

    def recv_exact(self, size: int) -> bytes:
        if self.sock is None:
            raise RuntimeError("socket is not connected")
        chunks: list[bytes] = []
        remaining = size
        while remaining > 0:
            try:
                chunk = self.sock.recv(remaining)
            except socket.timeout:
                if self._stop.is_set():
                    raise EOFError("stopped")
                continue
            if not chunk:
                raise EOFError("connection closed")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def read_packet(self) -> Packet:
        header = self.recv_exact(HEADER_SIZE)
        magic, version, flags_or_reserved, type_value, payload_size, req_or_seq = struct.unpack("<IBBHII", header)
        if magic != MAGIC:
            raise ValueError(f"bad magic: 0x{magic:08X}")
        if version != PROTO_VER:
            raise ValueError(f"unsupported version: {version}")
        if payload_size > MAX_PAYLOAD:
            raise ValueError(f"payload too large: {payload_size}")
        try:
            typ = WspType(type_value)
        except ValueError:
            raise ValueError(f"unknown type: 0x{type_value:04X}") from None
        payload = self.recv_exact(payload_size) if payload_size else b""
        return Packet(typ=typ, flags_or_reserved=flags_or_reserved, request_or_sequence=req_or_seq, payload=payload)


class PacketLogger:
    def __init__(
        self,
        pcm_out: Optional[Path] = None,
        opus_out_dir: Optional[Path] = None,
        ir_jsonl: Optional[Path] = None,
        log_every_audio_frames: int = 50,
    ) -> None:
        self.pcm_fp: Optional[BinaryIO] = None
        self.opus_out_dir = opus_out_dir
        self.ir_jsonl = ir_jsonl
        self.log_every_audio_frames = max(1, log_every_audio_frames)
        self.audio_frame_count: dict[WspType, int] = {WspType.MicPCM: 0, WspType.MicComp: 0}
        self.last_seq: dict[WspType, int] = {}
        self.start_ts = time.time()

        if pcm_out:
            pcm_out.parent.mkdir(parents=True, exist_ok=True)
            self.pcm_fp = pcm_out.open("wb")
            print(f"[file] MicPCM 저장: {pcm_out}")
        if opus_out_dir:
            opus_out_dir.mkdir(parents=True, exist_ok=True)
            print(f"[file] MicComp 프레임 저장 폴더: {opus_out_dir}")
        if ir_jsonl:
            ir_jsonl.parent.mkdir(parents=True, exist_ok=True)
            print(f"[file] IR raw JSONL 저장: {ir_jsonl}")

    def close(self) -> None:
        if self.pcm_fp is not None:
            self.pcm_fp.close()
            self.pcm_fp = None

    def handle_packet(self, pkt: Packet) -> None:
        if pkt.typ in CONTROL_TYPES:
            self.handle_control(pkt)
        else:
            self.handle_data(pkt)

    def handle_control(self, pkt: Packet) -> None:
        if pkt.typ == WspType.Ack:
            if len(pkt.payload) != 8:
                print(f"[rx] Ack 잘못된 payloadSize={len(pkt.payload)}")
                return
            request_id, status, r0, r1, r2 = struct.unpack("<IBBBB", pkt.payload)
            print(f"[rx] Ack requestId={request_id} status={status}")
        elif pkt.typ == WspType.Error:
            if len(pkt.payload) < 8:
                print(f"[rx] Error 잘못된 payloadSize={len(pkt.payload)}")
                return
            request_id, code = struct.unpack_from("<Ii", pkt.payload, 0)
            msg = pkt.payload[8:].decode("utf-8", errors="replace")
            print(f"[rx] Error requestId={request_id} code={code} message={msg!r}")
        elif pkt.typ == WspType.Heartbeat:
            print(f"[rx] Heartbeat requestId={pkt.request_id}")
        elif pkt.typ == WspType.Subscribe:
            print(f"[rx] Subscribe from device? payloadSize={len(pkt.payload)}")
        elif pkt.typ == WspType.Unsubscribe:
            print(f"[rx] Unsubscribe from device? payloadSize={len(pkt.payload)}")

    def handle_data(self, pkt: Packet) -> None:
        payload, timestamp_us = self.strip_timestamp_if_needed(pkt)
        self.check_sequence(pkt)

        if pkt.typ == WspType.MicPCM:
            self.handle_mic_pcm(pkt, payload, timestamp_us)
        elif pkt.typ == WspType.MicComp:
            self.handle_mic_comp(pkt, payload, timestamp_us)
        elif pkt.typ == WspType.IrReceive:
            self.handle_ir_receive(pkt, payload, timestamp_us)
        elif pkt.typ in (WspType.AmbientLight, WspType.Temperature, WspType.Humidity):
            self.handle_sensor(pkt, payload, timestamp_us)
        elif pkt.typ in (WspType.SpkPCM, WspType.SpkComp, WspType.IrTransmit):
            print(f"[rx] {pkt.typ.name} from device? seq={pkt.sequence} payloadSize={len(payload)}")
        else:
            print(f"[rx] {pkt.typ.name} seq={pkt.sequence} payloadSize={len(payload)}")

    def strip_timestamp_if_needed(self, pkt: Packet) -> tuple[bytes, Optional[int]]:
        if not (pkt.flags & FLAG_HAS_TIMESTAMP):
            return pkt.payload, None
        if len(pkt.payload) < 8:
            print(f"[rx] {pkt.typ.name} timestamp flag 있지만 payload가 너무 짧음")
            return pkt.payload, None
        timestamp_us = struct.unpack_from("<Q", pkt.payload, 0)[0]
        return pkt.payload[8:], timestamp_us

    def check_sequence(self, pkt: Packet) -> None:
        last = self.last_seq.get(pkt.typ)
        if last is not None and pkt.sequence != ((last + 1) & 0xFFFFFFFF):
            print(f"[warn] {pkt.typ.name} sequence jump: last={last}, now={pkt.sequence}")
        self.last_seq[pkt.typ] = pkt.sequence

    def handle_mic_pcm(self, pkt: Packet, payload: bytes, timestamp_us: Optional[int]) -> None:
        if len(payload) < 8:
            print(f"[rx] MicPCM payload too short: {len(payload)}")
            return
        sample_rate, channels, bits_per_sample, sample_count = struct.unpack_from("<IBBH", payload, 0)
        pcm = payload[8:]
        expected = sample_count * channels * (bits_per_sample // 8)
        self.audio_frame_count[WspType.MicPCM] += 1
        n = self.audio_frame_count[WspType.MicPCM]

        if len(pcm) != expected:
            print(f"[warn] MicPCM bytes mismatch: got={len(pcm)} expected={expected}")
        if self.pcm_fp is not None:
            self.pcm_fp.write(pcm)
            self.pcm_fp.flush()

        if n == 1 or n % self.log_every_audio_frames == 0:
            elapsed = max(0.001, time.time() - self.start_ts)
            fps = n / elapsed
            ts = f" timestampUs={timestamp_us}" if timestamp_us is not None else ""
            print(
                f"[rx] MicPCM seq={pkt.sequence} frames={n} fps≈{fps:.1f} "
                f"{sample_rate}Hz ch={channels} bits={bits_per_sample} samples={sample_count} "
                f"pcmBytes={len(pcm)}{ts}"
            )

    def handle_mic_comp(self, pkt: Packet, payload: bytes, timestamp_us: Optional[int]) -> None:
        if len(payload) < 9:
            print(f"[rx] MicComp payload too short: {len(payload)}")
            return
        codec = payload[0]
        sample_rate = struct.unpack_from("<I", payload, 1)[0]
        channels = payload[5]
        frame_ms = payload[6]
        encoded_size = struct.unpack_from("<H", payload, 7)[0]
        encoded = payload[9:]
        self.audio_frame_count[WspType.MicComp] += 1
        n = self.audio_frame_count[WspType.MicComp]

        if len(encoded) != encoded_size:
            print(f"[warn] MicComp bytes mismatch: got={len(encoded)} expected={encoded_size}")
        if self.opus_out_dir is not None:
            # Opus payload만 프레임별로 저장합니다. 정식 .opus 컨테이너가 아닌 디버그용 raw frame입니다.
            path = self.opus_out_dir / f"miccomp_seq_{pkt.sequence:08d}.opusframe"
            path.write_bytes(encoded)

        if n == 1 or n % self.log_every_audio_frames == 0:
            ts = f" timestampUs={timestamp_us}" if timestamp_us is not None else ""
            codec_name = "Opus" if codec == AUDIO_CODEC_OPUS else f"codec={codec}"
            print(
                f"[rx] MicComp seq={pkt.sequence} frames={n} {codec_name} "
                f"{sample_rate}Hz ch={channels} frameMs={frame_ms} encoded={len(encoded)}{ts}"
            )

    def handle_ir_receive(self, pkt: Packet, payload: bytes, timestamp_us: Optional[int]) -> None:
        if len(payload) < 4:
            print(f"[rx] IrReceive payload too short: {len(payload)}")
            return
        length, overflow, reserved = struct.unpack_from("<HBB", payload, 0)
        raw_bytes = payload[4:]
        expected = length * 2
        if len(raw_bytes) < expected:
            print(f"[rx] IrReceive raw too short: got={len(raw_bytes)} expected={expected}")
            return
        raw = list(struct.unpack_from("<" + "H" * length, raw_bytes, 0)) if length else []
        ts = f" timestampUs={timestamp_us}" if timestamp_us is not None else ""
        print(
            f"[rx] IrReceive seq={pkt.sequence} length={length} overflow={overflow} "
            f"raw={raw[:24]}{'...' if len(raw) > 24 else ''}{ts}"
        )
        if self.ir_jsonl:
            record = {
                "sequence": pkt.sequence,
                "timestampUs": timestamp_us,
                "length": length,
                "overflow": overflow,
                "rawUs": raw,
            }
            with self.ir_jsonl.open("a", encoding="utf-8") as fp:
                fp.write(json.dumps(record, ensure_ascii=False) + "\n")

    def handle_sensor(self, pkt: Packet, payload: bytes, timestamp_us: Optional[int]) -> None:
        if len(payload) != 8:
            print(f"[rx] {pkt.typ.name} invalid SensorBody size: {len(payload)}")
            return
        unit, quality, reserved, value = struct.unpack("<BBHf", payload)
        unit_name = UNIT_NAME.get(unit, f"unit#{unit}")
        ts = f" timestampUs={timestamp_us}" if timestamp_us is not None else ""
        print(
            f"[rx] {pkt.typ.name} seq={pkt.sequence} value={value:.3f} {unit_name} "
            f"quality={quality}{ts}"
        )


def parse_subscriptions(values: list[str]) -> list[WspType]:
    if not values:
        return []
    result: list[WspType] = []
    for value in values:
        key = value.strip().lower()
        if key == "all":
            for typ in ALL_SUBSCRIPTIONS:
                if typ not in result:
                    result.append(typ)
            continue
        typ = SUBSCRIBE_ALIASES.get(key)
        if typ is None:
            valid = ", ".join(sorted(list(SUBSCRIBE_ALIASES.keys()) + ["all"]))
            raise SystemExit(f"알 수 없는 subscribe 대상: {value!r}\n사용 가능: {valid}")
        if typ not in result:
            result.append(typ)
    return result


def parse_ir_raw(text: str) -> list[int]:
    values: list[int] = []
    for item in text.replace(" ", "").split(","):
        if not item:
            continue
        values.append(int(item, 10))
    return values


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="WaveStation WSP 테스트용 백엔드")
    parser.add_argument("host", help="ESP32 LAN IP. 예: 192.168.0.50")
    parser.add_argument("--port", type=int, default=TCP_PORT, help=f"ESP32 TCP 포트. 기본값: {TCP_PORT}")
    parser.add_argument(
        "--subscribe",
        nargs="*",
        default=[],
        help="구독 대상: micpcm miccomp ir ambient temperature humidity all",
    )
    parser.add_argument("--sensor-interval-ms", type=int, default=1000, help="센서 구독 intervalMs. 기본값: 1000")
    parser.add_argument("--heartbeat-interval", type=float, default=5.0, help="Heartbeat 주기 초. 0이면 끔")
    parser.add_argument("--pcm-out", type=Path, default=None, help="MicPCM raw 저장 파일. 예: mic.raw")
    parser.add_argument("--opus-out-dir", type=Path, default=None, help="MicComp Opus raw frame 저장 폴더")
    parser.add_argument("--ir-jsonl", type=Path, default=Path("ir_raw.jsonl"), help="IrReceive JSONL 저장 파일")
    parser.add_argument("--send-ir-raw", default=None, help='IrTransmit 테스트 raw μs 배열. 예: "9000,4500,560,560"')
    parser.add_argument("--carrier-hz", type=int, default=38000, help="IrTransmit carrierHz. 기본값: 38000")
    parser.add_argument("--repeat", type=int, default=0, help="IrTransmit repeat. 0이면 1회")
    parser.add_argument("--send-spkpcm-tone", action="store_true", help="SpkPCM 스피커 테스트용 사인파 톤 송신")
    parser.add_argument("--tone-freq", type=float, default=880.0, help="SpkPCM tone 주파수 Hz. 기본값: 880")
    parser.add_argument("--tone-duration", type=float, default=2.0, help="SpkPCM tone 길이 초. 기본값: 2")
    parser.add_argument("--tone-amp", type=float, default=0.15, help="SpkPCM tone 진폭 0~1. 기본값: 0.15")
    parser.add_argument("--spk-sample-rate", type=int, default=16000, help="SpkPCM/SpkComp sampleRate. 기본값: 16000")
    parser.add_argument("--send-spkpcm-wav", type=Path, default=None, help="SpkPCM으로 보낼 mono 16-bit PCM WAV 파일")
    parser.add_argument("--send-spkcomp-dir", type=Path, default=None, help="SpkComp으로 보낼 .opusframe 폴더")
    parser.add_argument("--log-every-audio-frames", type=int, default=50, help="오디오 로그 출력 프레임 간격")
    parser.add_argument("--verbose", action="store_true", help="Heartbeat 등 상세 로그")
    args = parser.parse_args(argv)

    subscriptions = parse_subscriptions(args.subscribe)

    client = WspClient(
        host=args.host,
        port=args.port,
        heartbeat_interval=args.heartbeat_interval,
        verbose=args.verbose,
    )
    logger = PacketLogger(
        pcm_out=args.pcm_out,
        opus_out_dir=args.opus_out_dir,
        ir_jsonl=args.ir_jsonl,
        log_every_audio_frames=args.log_every_audio_frames,
    )

    try:
        client.connect()
        client.send_heartbeat()
        client.start_heartbeat_loop()

        for typ in subscriptions:
            if typ in (WspType.AmbientLight, WspType.Temperature, WspType.Humidity):
                interval = args.sensor_interval_ms
                options = 0
            elif typ == WspType.MicComp:
                interval = 0
                options = SUB_COMPRESSED
            else:
                interval = 0
                options = 0
            client.send_subscribe(typ, interval_ms=interval, options=options)
            time.sleep(0.05)

        if args.send_ir_raw:
            raw = parse_ir_raw(args.send_ir_raw)
            client.send_ir_transmit(raw, carrier_hz=args.carrier_hz, repeat=args.repeat)

        if args.send_spkpcm_tone:
            client.send_spkpcm_tone(
                freq_hz=args.tone_freq,
                duration_s=args.tone_duration,
                amplitude=args.tone_amp,
                sample_rate=args.spk_sample_rate,
            )

        if args.send_spkpcm_wav:
            client.send_spkpcm_wav(args.send_spkpcm_wav)

        if args.send_spkcomp_dir:
            client.send_spkcomp_dir(args.send_spkcomp_dir, sample_rate=args.spk_sample_rate)

        print("[run] 수신 대기 중... Ctrl+C로 종료")
        while True:
            pkt = client.read_packet()
            logger.handle_packet(pkt)

    except KeyboardInterrupt:
        print("\n[exit] Ctrl+C")
        for typ in subscriptions:
            try:
                client.send_unsubscribe(typ)
                time.sleep(0.02)
            except Exception:
                pass
        return 0
    except (OSError, EOFError) as exc:
        print(f"[disconnect] {exc}")
        return 1
    except Exception as exc:
        print(f"[error] {exc}", file=sys.stderr)
        return 2
    finally:
        logger.close()
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
