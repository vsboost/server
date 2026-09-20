#!/usr/bin/env python3

import argparse
import os
import random
import struct
import subprocess
import tempfile
import wave


NOISE_DEVIATIONS = (1000, 3000, 10000, 20000)
NOISE_SEEDS = range(10)
OFF_TONE_FREQUENCIES = (
    300, 450, 600, 650, 700, 800, 850, 900, 1050, 1200, 1500
)


def decode(decoder, wav, frequency, wpm, offset=None):
    command = [decoder, wav, str(frequency), str(wpm), "-", "4"]
    if offset is not None:
        command.append(str(offset))
    result = subprocess.run(command, text=True, capture_output=True, check=True)
    return result.stdout.strip()


def write_noise(path, deviation, seed):
    sample_rate = 12000
    random_generator = random.Random(seed)
    samples = [
        max(-32768, min(32767, int(random_generator.gauss(0, deviation))))
        for _ in range(sample_rate * 12)
    ]
    with wave.open(path, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decoder", required=True)
    parser.add_argument("--arrl-corpus", required=True)
    args = parser.parse_args()

    noise_outputs = []
    with tempfile.TemporaryDirectory() as temporary_directory:
        for deviation in NOISE_DEVIATIONS:
            for seed in NOISE_SEEDS:
                path = os.path.join(
                    temporary_directory, f"noise-{deviation}-{seed}.wav"
                )
                write_noise(path, deviation, seed)
                output = decode(args.decoder, path, 750, 20)
                if output:
                    noise_outputs.append((deviation, seed, output))

    wav = os.path.join(args.arrl_corpus, "260217_20WPM.12k.wav")
    off_tone_outputs = []
    for frequency in OFF_TONE_FREQUENCIES:
        output = decode(args.decoder, wav, frequency, 20, 90)
        if output:
            off_tone_outputs.append((frequency, output))

    print(
        f"NOISE cases={len(NOISE_DEVIATIONS) * len(NOISE_SEEDS)} "
        f"active={len(noise_outputs)}"
    )
    for deviation, seed, output in noise_outputs:
        print(f"  deviation={deviation:5} seed={seed} output={output!r}")
    print(
        f"OFF_TONE tracks={len(OFF_TONE_FREQUENCIES)} "
        f"active={len(off_tone_outputs)}"
    )
    for frequency, output in off_tone_outputs:
        print(f"  frequency={frequency:4} Hz output={output!r}")
    return 1 if noise_outputs else 0


if __name__ == "__main__":
    raise SystemExit(main())
