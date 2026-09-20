# CW skimmer test harness

The repository contains deterministic 12 kHz PCM WAV fixtures in
`tests/fixtures/cw/`. They cover single-channel, multi-channel, dense, and
negative-SNR CW. Fixture labels and SHA-256 hashes are in
`tests/fixtures/cw/README.txt`.

## Build

Configure the native harness once, then build the CW test executables:

```sh
cmake -S . -B build-native -DNATIVE_HARNESS=ON -DENABLE_HDFL=OFF
cmake --build build-native \
  --target test_cw_skimmer_replay test_bayes_hsmm --parallel
```

The native build is useful for replay and browser testing. It is not a
substitute for an Alpine ARMv7 build or Zynq-7010 hardware validation.

## Run the checked-in corpus

Run the production `CwSkimmer` path, including FFT analysis, track selection,
streaming Bayesian decoding, and sideband suppression:

```sh
python3 tests/score_cw_skimmer_stream.py \
  --replay build-native/test_cw_skimmer_replay
```

The scorer defaults to `tests/fixtures/cw/` and fails when:

- aggregate CER exceeds 0.10;
- any unmatched output track is emitted; or
- average first-output latency exceeds 5 seconds.

The same regression is registered with CTest:

```sh
ctest --test-dir build-native \
  -R 'cw_skimmer_(replay|stream_corpus)' --output-on-failure
```

## Inspect one replay

Use `--dump` to print every callback with its audio timestamp, FFT-bin
frequency, and character byte:

```sh
build-native/test_cw_skimmer_replay \
  tests/fixtures/cw/dense-4ch-snr-4.wav --dump
```

Example event:

```text
CW_EVENT 4.032 984 51
```

This means the skimmer emitted ASCII `Q` (`0x51`) for the 984 Hz track at
4.032 seconds.

## Test a known carrier directly

`test_bayes_hsmm` bypasses multi-track selection and tests one known carrier:

```sh
build-native/test_bayes_hsmm \
  tests/fixtures/cw/single-noisy-20wpm-snr-6.wav \
  703 20 'CQ DE W1AW K' 4
```

Add an offset and `--stream` to exercise timestamped rolling-window output:

```sh
build-native/test_bayes_hsmm \
  tests/fixtures/cw/single-noisy-20wpm-snr-6.wav \
  703 20 - 4 0 --stream
```

Use WPM `0` without `--stream` to exercise automatic speed selection.

## Optional independent recordings

Official W1AW recordings are intentionally not committed. If a local corpus
contains the filenames listed in `tests/score_cw_skimmer_arrl.py`, run:

```sh
python3 tests/score_cw_skimmer_arrl.py \
  --replay build-native/test_cw_skimmer_replay \
  --corpus /path/to/arrl-cw-corpus
```

The script extracts 12-second excerpts at 30, 90, and 150 seconds and scores
the live multi-bin scanner against matching transcript text.

## Browser and target validation

After changing CW JavaScript or CSS, run:

```sh
npm run test:native-browser
```

Before claiming device readiness, build the ARM release and verify
`build/websdr.bin` is a 32-bit ARM EABI5 executable using
`/lib/ld-musl-armhf.so.1`. Then follow the temporary RAM-disk deployment and
foreground-run procedure in `AGENTS.md`; confirm `/status`, live sound and
waterfall sockets, CW skimmer output, CPU use, and RSS on the Zynq-7010.
