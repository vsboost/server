#!/usr/bin/env python3

import argparse
import os
import re
import subprocess


FILES = (
    ("260107_10WPM.12k.wav", "260107_10.txt", 10),
    ("260204_18WPM.12k.wav", "260204_18.txt", 18),
    ("260217_20WPM.12k.wav", "260217_20.txt", 20),
    ("250107_25WPM.12k.wav", "250107_25.txt", 25),
    ("250121_35WPM.12k.wav", "250121_35.txt", 35),
    ("250204_40WPM.12k.wav", "250204_40.txt", 40),
)

OFFSETS = (30, 90, 150)


def normalize(value):
    return re.sub("[^A-Z0-9]", "", value.upper())


def substring_distance(expected, actual):
    if not actual:
        return 0, ""
    previous = [0] * (len(expected) + 1)
    rows = []
    for i, character in enumerate(actual, 1):
        current = [i]
        for j, source in enumerate(expected, 1):
            current.append(min(
                previous[j] + 1,
                current[j - 1] + 1,
                previous[j - 1] + (character != source),
            ))
        rows.append(current)
        previous = current
    end = min(range(len(expected) + 1), key=lambda index: previous[index])
    distance = previous[end]
    start = max(0, end - len(actual) - distance)
    return distance, expected[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decoder", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--confidence", type=float, default=4.0)
    parser.add_argument("--stream", action="store_true")
    args = parser.parse_args()

    total_distance = 0
    total_characters = 0
    cases = 0
    decoded_cases = 0
    for wav, transcript, wpm in FILES:
        with open(os.path.join(args.corpus, transcript), encoding="utf-8",
                  errors="ignore") as source:
            expected = normalize(source.read())
        for offset in OFFSETS:
            command = [
                args.decoder,
                os.path.join(args.corpus, wav),
                "750",
                str(wpm),
                "-",
                str(args.confidence),
                str(offset),
            ]
            if args.stream:
                command.append("--stream")
            result = subprocess.run(
                command, text=True, capture_output=True, check=True
            )
            actual = normalize(result.stdout)
            cases += 1
            if not actual:
                print(
                    f"{wav:26} offset={offset:3}s chars= 0 EMPTY"
                )
                continue
            distance, match = substring_distance(expected, actual)
            total_distance += distance
            total_characters += len(actual)
            decoded_cases += 1
            print(
                f"{wav:26} offset={offset:3}s chars={len(actual):2} "
                f"edit={distance:2} actual={actual!r} match={match!r}"
            )

    cer = total_distance / total_characters if total_characters else 1.0
    print(
        f"TOTAL decoded={decoded_cases}/{cases} "
        f"edit={total_distance}/{total_characters} CER={cer:.3f}"
    )


if __name__ == "__main__":
    main()
