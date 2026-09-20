#!/usr/bin/env python3

import argparse
import os
import subprocess


CASES = (
    ("single-clean-20wpm.wav", 703, 20, "CQ DE W1AW K"),
    ("single-noisy-20wpm-snr-6.wav", 703, 20, "CQ DE W1AW K"),
    ("multi-clean-3ch.wav", 469, 18, "CQ TEST DE W1AW"),
    ("multi-clean-3ch.wav", 938, 24, "QRZ DE K5ABC"),
    ("multi-clean-3ch.wav", 1406, 32, "TU 599 001"),
    ("multi-noisy-3ch-snr-8.wav", 469, 18, "CQ TEST DE W1AW"),
    ("multi-noisy-3ch-snr-8.wav", 938, 24, "QRZ DE K5ABC"),
    ("multi-noisy-3ch-snr-8.wav", 1406, 32, "TU 599 001"),
    ("dense-4ch-snr-4.wav", 375, 16, "CQ DE N0CALL"),
    ("dense-4ch-snr-4.wav", 656, 20, "CQ TEST DE W1AW"),
    ("dense-4ch-snr-4.wav", 984, 28, "QRZ DE K5ABC"),
    ("dense-4ch-snr-4.wav", 1266, 36, "TEST N7XYZ 599 012"),
)


def normalize(value):
    return "".join(character for character in value if character.isalnum())


def edit_distance(expected, actual):
    row = list(range(len(actual) + 1))
    for i, left in enumerate(expected, 1):
        previous, row[0] = row[0], i
        for j, right in enumerate(actual, 1):
            old = row[j]
            row[j] = min(row[j] + 1, row[j - 1] + 1,
                         previous + (left != right))
            previous = old
    return row[-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--decoder", required=True)
    parser.add_argument("--corpus", required=True)
    parser.add_argument("--confidence", type=float)
    parser.add_argument("--stream", action="store_true")
    args = parser.parse_args()

    total_distance = 0
    total_characters = 0
    exact = 0
    failed = False
    for filename, frequency, wpm, expected in CASES:
        command = [
            args.decoder,
            os.path.join(args.corpus, filename),
            str(frequency),
            str(wpm),
            expected,
        ]
        if args.confidence is not None:
            command.append(str(args.confidence))
        if args.stream:
            if args.confidence is None:
                command.append("4")
            command.extend(("0", "--stream"))
        result = subprocess.run(command, text=True, capture_output=True)
        actual = result.stdout.strip()
        normalized_expected = normalize(expected)
        normalized_actual = normalize(actual)
        distance = edit_distance(normalized_expected, normalized_actual)
        total_distance += distance
        total_characters += len(normalized_expected)
        exact += distance == 0
        failed |= result.returncode not in (0, 1)
        print(
            f"{filename:30} {frequency:4} Hz {wpm:2} WPM "
            f"edit={distance:2}/{len(normalized_expected):2} "
            f"actual={actual!r}"
        )

    print(
        f"TOTAL exact={exact}/{len(CASES)} "
        f"edit={total_distance}/{total_characters} "
        f"CER={total_distance / total_characters:.3f}"
    )
    return 2 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
