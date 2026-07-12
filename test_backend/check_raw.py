from pathlib import Path
import struct
import math

path = Path("mic.raw")
data = path.read_bytes()

print("bytes:", len(data))
print("duration_sec:", len(data) / 2 / 16000)

if len(data) < 2:
    print("empty")
    raise SystemExit

samples = struct.unpack("<" + "h" * (len(data) // 2), data[:len(data)//2*2])

mn = min(samples)
mx = max(samples)
mean = sum(samples) / len(samples)
rms = math.sqrt(sum(s * s for s in samples) / len(samples))

zero_count = sum(1 for s in samples if s == 0)
same_ratio = zero_count / len(samples)

print("min:", mn)
print("max:", mx)
print("mean:", mean)
print("rms:", rms)
print("zero_ratio:", same_ratio)

print("first 40 samples:")
print(samples[:40])