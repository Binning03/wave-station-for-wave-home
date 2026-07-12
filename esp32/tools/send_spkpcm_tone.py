#!/usr/bin/env python3
"""Send a simple SpkPCM sine wave to WaveStation for MAX98357 speaker testing."""

from __future__ import annotations

import argparse
import math
import socket
import struct
import time

MAGIC = 0x57535031
PROTO_VER = 1
TCP_PORT = 41737
TYPE_SPK_PCM = 0x0111
TYPE_HEARTBEAT = 0xF020

SAMPLE_RATE = 16000
CHANNELS = 1
BITS = 16
FRAME_SAMPLES = 320  # 20 ms @ 16 kHz


def data_header(typ: int, payload_size: int, sequence: int, flags: int = 0) -> bytes:
    return struct.pack("<IBBHII", MAGIC, PROTO_VER, flags, typ, payload_size, sequence)


def control_header(typ: int, payload_size: int, request_id: int) -> bytes:
    return struct.pack("<IBBHII", MAGIC, PROTO_VER, 0, typ, payload_size, request_id)


def build_spk_pcm_frame(seq: int, samples: list[int]) -> bytes:
    pcm = struct.pack("<" + "h" * len(samples), *samples)
    body = struct.pack("<IBBH", SAMPLE_RATE, CHANNELS, BITS, len(samples)) + pcm
    return data_header(TYPE_SPK_PCM, len(body), seq) + body


def main() -> int:
    parser = argparse.ArgumentParser(description="WaveStation SpkPCM sine tone tester")
    parser.add_argument("host", help="ESP32 LAN IP, e.g. 192.168.0.50")
    parser.add_argument("--port", type=int, default=TCP_PORT)
    parser.add_argument("--freq", type=float, default=880.0, help="tone frequency Hz")
    parser.add_argument("--duration", type=float, default=2.0, help="duration seconds")
    parser.add_argument("--amp", type=float, default=0.15, help="amplitude 0.0~1.0")
    args = parser.parse_args()

    amp = max(0.0, min(1.0, args.amp))
    max_i16 = int(32767 * amp)
    total_frames = max(1, int(args.duration * 1000 / 20))

    with socket.create_connection((args.host, args.port), timeout=5.0) as sock:
        print(f"[connect] {args.host}:{args.port}")
        sock.sendall(control_header(TYPE_HEARTBEAT, 0, 1))
        time.sleep(0.05)

        sample_index = 0
        for seq in range(total_frames):
            samples = []
            for _ in range(FRAME_SAMPLES):
                value = int(max_i16 * math.sin(2.0 * math.pi * args.freq * sample_index / SAMPLE_RATE))
                samples.append(value)
                sample_index += 1
            sock.sendall(build_spk_pcm_frame(seq, samples))
            time.sleep(0.020)

    print(f"[done] sent {total_frames} SpkPCM frames ({args.duration:.2f}s), freq={args.freq}Hz")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
