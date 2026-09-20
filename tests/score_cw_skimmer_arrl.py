#!/usr/bin/env python3

import argparse
import os
import re
import subprocess
import tempfile
import wave


FILES = (
    ("260107_10WPM.12k.wav", "260107_10.txt"),
    ("260204_18WPM.12k.wav", "260204_18.txt"),
    ("260217_20WPM.12k.wav", "260217_20.txt"),
    ("250107_25WPM.12k.wav", "250107_25.txt"),
    ("250121_35WPM.12k.wav", "250121_35.txt"),
    ("250204_40WPM.12k.wav", "250204_40.txt"),
)

OFFSETS = (30, 90, 150)
EVENT = re.compile(r"CW_EVENT\s+(\S+)\s+(\d+)\s+([0-9A-F]{2})")


def normalize(value):
    return re.sub("[^A-Z0-9]", "", value.upper())


def substring_distance(expected, actual):
    previous = [0] * (len(expected) + 1)
    for i, character in enumerate(actual, 1):
        current = [i]
        for j, source in enumerate(expected, 1):
            current.append(min(
                previous[j] + 1,
                current[j - 1] + 1,
                previous[j - 1] + (character != source),
            ))
        previous = current
    end = min(range(len(expected) + 1), key=lambda index: previous[index])
    return previous[end]


def write_excerpt(source, destination, offset):
    with wave.open(source, "rb") as input_file:
        sample_rate = input_file.getframerate()
        input_file.setpos(offset * sample_rate)
        frames = input_file.readframes(12 * sample_rate)
        parameters = input_file.getparams()
    with wave.open(destination, "wb") as output_file:
        output_file.setparams(parameters)
        output_file.writeframes(frames)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", required=True)
    parser.add_argument("--corpus", required=True)
    args = parser.parse_args()

    total_distance = 0
    total_characters = 0
    decoded_cases = 0
    false_tracks = 0
    latencies = []
    with tempfile.TemporaryDirectory() as temporary_directory:
        for wav, transcript in FILES:
            with open(os.path.join(args.corpus, transcript), encoding="utf-8",
                      errors="ignore") as source:
                expected = normalize(source.read())
            for offset in OFFSETS:
                excerpt = os.path.join(
                    temporary_directory, f"{wav}-{offset}.wav"
                )
                write_excerpt(os.path.join(args.corpus, wav), excerpt, offset)
                result = subprocess.run(
                    [args.replay, excerpt, "--dump"],
                    text=True,
                    capture_output=True,
                )
                if result.returncode not in (0, 1):
                    print(result.stderr, end="")
                    return 2

                events = []
                for line in result.stdout.splitlines():
                    match = EVENT.match(line)
                    if match:
                        events.append((
                            float(match.group(1)),
                            int(match.group(2)),
                            chr(int(match.group(3), 16)),
                        ))
                frequencies = sorted(set(
                    frequency for _, frequency, _ in events
                ))
                candidates = [
                    frequency for frequency in frequencies
                    if abs(frequency - 750) <= 70
                ]
                selectedFrequency = min(
                    candidates, key=lambda value: abs(value - 750)
                ) if candidates else None
                selected = [
                    (timestamp, character)
                    for timestamp, frequency, character in events
                    if frequency == selectedFrequency
                ]
                actual = normalize("".join(
                    character for _, character in selected
                ))
                false_tracks += len(set(frequencies) -
                    ({selectedFrequency} if selectedFrequency is not None else set()))
                if selected:
                    decoded_cases += 1
                    latencies.append(selected[0][0])
                    distance = substring_distance(expected, actual)
                    total_distance += distance
                    total_characters += len(actual)
                    status = f"edit={distance:2}/{len(actual):2}"
                else:
                    status = "EMPTY"
                print(
                    f"{wav:26} offset={offset:3}s "
                    f"freq={selectedFrequency if selectedFrequency else -1:4} "
                    f"{status} actual={actual!r}"
                )

    cer = total_distance / total_characters if total_characters else 1.0
    average_latency = sum(latencies) / len(latencies) if latencies else 0.0
    print(
        f"TOTAL decoded={decoded_cases}/18 "
        f"edit={total_distance}/{total_characters} CER={cer:.3f} "
        f"false_tracks={false_tracks} average_latency={average_latency:.2f}s"
    )


if __name__ == "__main__":
    raise SystemExit(main())
