import argparse
import wave
from pathlib import Path


def raw_to_wav(input_path: Path, output_path: Path, sample_rate: int = 16000):
    raw_data = input_path.read_bytes()

    with wave.open(str(output_path), "wb") as wav:
        wav.setnchannels(1)       # mono
        wav.setsampwidth(2)       # 16-bit = 2 bytes
        wav.setframerate(sample_rate)
        wav.writeframes(raw_data)

    duration = len(raw_data) / 2 / sample_rate

    print(f"saved: {output_path}")
    print(f"duration: {duration:.2f} sec")


def main():
    parser = argparse.ArgumentParser(
        description="Convert 16-bit mono PCM raw audio to WAV"
    )

    parser.add_argument(
        "input",
        help="input .raw file path"
    )

    parser.add_argument(
        "output",
        help="output .wav file path"
    )

    parser.add_argument(
        "--rate",
        type=int,
        default=16000,
        help="sample rate, default: 16000"
    )

    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output)

    if not input_path.exists():
        raise FileNotFoundError(f"Input file not found: {input_path}")

    raw_to_wav(input_path, output_path, args.rate)


if __name__ == "__main__":
    main()