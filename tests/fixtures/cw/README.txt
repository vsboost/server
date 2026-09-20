Deterministic CW skimmer replay corpus

These mono 16-bit PCM, 12 kHz WAV files were generated locally with the
swilcox/cw-dit synthesizer at commit
153fc247ce6e4934c94e0cd2dcbf7887e368ec29. They contain synthesized tones
and noise only; they are not copied recordings.

single-clean-20wpm.wav
  703 Hz, 20 WPM: CQ DE W1AW K
  SHA-256: 2652b96062f6f35dad41d07de021b0c6d83e81d58e28a7a50c201faeba8b2b4a

single-noisy-20wpm-snr-6.wav
  703 Hz, 20 WPM, -6 dB SNR: CQ DE W1AW K
  SHA-256: 1b925369b3189f3bc3d8f81b5f19a78c6f919c02e4e11b961bc954b7441085d4

multi-clean-3ch.wav
  469 Hz, 18 WPM: CQ TEST DE W1AW
  938 Hz, 24 WPM: QRZ DE K5ABC
  1406 Hz, 32 WPM: TU 599 001
  SHA-256: 713a3e3118e517357c6fcdb55bc8d163fd9031641bf022e9e194d73d67a0bbe1

multi-noisy-3ch-snr-8.wav
  The same three channels at -8 dB SNR.
  SHA-256: f0f6d3e15cbd271c643e08a5dfd584630095be1859c5393606965b6ae0d3c374

dense-4ch-snr-4.wav
  375 Hz, 16 WPM: CQ DE N0CALL
  656 Hz, 20 WPM: CQ TEST DE W1AW
  984 Hz, 28 WPM: QRZ DE K5ABC
  1266 Hz, 36 WPM: TEST N7XYZ 599 012
  All channels are at -4 dB SNR.
  SHA-256: f08de1fd403e248d7c39c4ece2cf30456c735f84a07fc3ea13a471288fc31b2a
