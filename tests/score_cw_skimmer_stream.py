#!/usr/bin/env python3

import argparse
import os
import re
import subprocess


CASES = (
    ("single-clean-20wpm.wav", ((703, "CQ DE W1AW K"),)),
    ("single-noisy-20wpm-snr-6.wav", ((703, "CQ DE W1AW K"),)),
    ("multi-clean-3ch.wav", (
        (469, "CQ TEST DE W1AW"),
        (938, "QRZ DE K5ABC"),
        (1406, "TU 599 001"),
    )),
    ("multi-noisy-3ch-snr-8.wav", (
        (469, "CQ TEST DE W1AW"),
        (938, "QRZ DE K5ABC"),
        (1406, "TU 599 001"),
    )),
    ("dense-4ch-snr-4.wav", (
        (375, "CQ DE N0CALL"),
        (656, "CQ TEST DE W1AW"),
        (984, "QRZ DE K5ABC"),
        (1266, "TEST N7XYZ 599 012"),
    )),
)

EVENT = re.compile(r"CW_EVENT\s+(\S+)\s+(\d+)\s+([0-9A-F]{2})")


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
    parser.add_argument("--replay", required=True)
    parser.add_argument(
        "--corpus",
        default=os.path.join(os.path.dirname(__file__), "fixtures", "cw"),
    )
    parser.add_argument("--max-cer", type=float, default=0.10)
    parser.add_argument("--max-false-tracks", type=int, default=0)
    parser.add_argument("--max-latency", type=float, default=5.0)
    args = parser.parse_args()

    total_distance = 0
    total_characters = 0
    exact = 0
    false_tracks = 0
    first_latencies = []
    for filename, channels in CASES:
        result = subprocess.run([
            args.replay,
            os.path.join(args.corpus, filename),
            "--dump",
        ], text=True, capture_output=True)
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
        frequencies = sorted(set(frequency for _, frequency, _ in events))
        matched = set()
        for expectedFrequency, expected in channels:
            candidates = [
                frequency for frequency in frequencies
                if abs(frequency - expectedFrequency) <= 70
            ]
            frequency = min(
                candidates,
                key=lambda value: abs(value - expectedFrequency),
            ) if candidates else None
            if frequency is None:
                actual = ""
                latency = None
            else:
                matched.add(frequency)
                selected = [
                    (timestamp, character)
                    for timestamp, eventFrequency, character in events
                    if eventFrequency == frequency
                ]
                actual = "".join(character for _, character in selected)
                latency = selected[0][0] if selected else None
                if latency is not None:
                    first_latencies.append(latency)

            normalizedExpected = normalize(expected)
            normalizedActual = normalize(actual)
            distance = edit_distance(normalizedExpected, normalizedActual)
            total_distance += distance
            total_characters += len(normalizedExpected)
            exact += distance == 0
            print(
                f"{filename:30} {expectedFrequency:4} Hz "
                f"actual={frequency if frequency is not None else -1:4} Hz "
                f"edit={distance:2}/{len(normalizedExpected):2} "
                f"latency={latency if latency is not None else -1:5.2f}s "
                f"text={actual!r}"
            )
        false_tracks += len(set(frequencies) - matched)

    averageLatency = (
        sum(first_latencies) / len(first_latencies)
        if first_latencies else 0.0
    )
    print(
        f"TOTAL exact={exact}/12 edit={total_distance}/{total_characters} "
        f"CER={total_distance / total_characters:.3f} "
        f"false_tracks={false_tracks} average_latency={averageLatency:.2f}s"
    )
    return int(
        total_distance / total_characters > args.max_cer or
        false_tracks > args.max_false_tracks or
        averageLatency > args.max_latency
    )


if __name__ == "__main__":
    raise SystemExit(main())
