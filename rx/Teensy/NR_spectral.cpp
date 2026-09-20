//
// Compact spectral noise reduction.
//
// This is an independently implemented, low-memory variant of the
// decision-directed spectral estimation used by WDSP NR2, based on the
// Ephraim-Malah MMSE short-time spectral-amplitude estimator (1984,
// doi:10.1109/TASSP.1984.1164453). It deliberately avoids NR2's
// double-precision tables and minimum-statistics history.
//

#include "config.h"
#include "datatypes.h"
#include "noise_filter.h"
#include "rx_noise.h"
#include "teensy.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#define NR_FFT_SIZE 128
#define NR_HOP_SIZE (NR_FFT_SIZE / 2)
#define NR_BINS     (NR_HOP_SIZE + 1)

struct nr_spectral_t {
    bool initialized;
    int warmup_frames;
    f32_t final_gain;
    f32_t alpha;
    f32_t min_gain;
    s2_t window[NR_FFT_SIZE];
    TYPEMONO16 history[NR_HOP_SIZE];
    f32_t overlap[NR_HOP_SIZE];
    TYPECPX fft[NR_FFT_SIZE];
    f32_t noise[NR_BINS];
    f32_t speech[NR_BINS];
    f32_t gain[NR_BINS];
};

static nr_spectral_t nr_spectral[MAX_RX_CHANS];

static_assert(sizeof(nr_spectral_t) <= 3072,
    "compact spectral NR state exceeds its per-channel memory budget");

static const TYPECPX fft_roots[7] = {
    { -1.0f, 0.0f },
    { 0.0f, -1.0f },
    { 0.7071067812f, -0.7071067812f },
    { 0.9238795325f, -0.3826834324f },
    { 0.9807852804f, -0.1950903220f },
    { 0.9951847267f, -0.0980171403f },
    { 0.9987954562f, -0.0490676743f }
};

static inline TYPEMONO16 nr_clip_sample(f32_t sample)
{
    if (sample > K_AMPMAX) return (TYPEMONO16) K_AMPMAX;
    if (sample < -K_AMPMAX) return (TYPEMONO16) -K_AMPMAX;
    return (TYPEMONO16) MROUND(sample);
}

static void nr_fft(TYPECPX* data, bool inverse)
{
    for (int i = 1, j = 0; i < NR_FFT_SIZE; ++i) {
        int bit = NR_HOP_SIZE;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            TYPECPX temp = data[i];
            data[i] = data[j];
            data[j] = temp;
        }
    }

    for (int stage = 0, length = 2; length <= NR_FFT_SIZE; ++stage, length <<= 1) {
        TYPECPX root = fft_roots[stage];
        if (inverse) root.im = -root.im;

        int half = length >> 1;
        for (int first = 0; first < NR_FFT_SIZE; first += length) {
            TYPECPX twiddle = { 1.0f, 0.0f };
            for (int i = 0; i < half; ++i) {
                TYPECPX* even = &data[first + i];
                TYPECPX* odd = &data[first + i + half];
                TYPECPX value = {
                    odd->re * twiddle.re - odd->im * twiddle.im,
                    odd->re * twiddle.im + odd->im * twiddle.re
                };
                f32_t even_re = even->re;
                f32_t even_im = even->im;
                even->re = even_re + value.re;
                even->im = even_im + value.im;
                odd->re = even_re - value.re;
                odd->im = even_im - value.im;

                f32_t twiddle_re = twiddle.re * root.re - twiddle.im * root.im;
                twiddle.im = twiddle.re * root.im + twiddle.im * root.re;
                twiddle.re = twiddle_re;
            }
        }
    }

    if (inverse) {
        const f32_t scale = 1.0f / NR_FFT_SIZE;
        for (int i = 0; i < NR_FFT_SIZE; ++i) {
            data[i].re *= scale;
            data[i].im *= scale;
        }
    }
}

static void nr_spectral_process_hop(nr_spectral_t* s, TYPEMONO16* input,
    TYPEMONO16* output)
{
    TYPEMONO16 current[NR_HOP_SIZE];
    memcpy(current, input, sizeof(current));

    for (int i = 0; i < NR_HOP_SIZE; ++i) {
        s->fft[i].re = s->history[i] * ((f32_t) s->window[i] / K_AMPMAX);
        s->fft[i].im = 0.0f;
        s->fft[i + NR_HOP_SIZE].re =
            current[i] * ((f32_t) s->window[i + NR_HOP_SIZE] / K_AMPMAX);
        s->fft[i + NR_HOP_SIZE].im = 0.0f;
    }

    nr_fft(s->fft, false);

    f32_t power[NR_BINS];
    for (int bin = 0; bin < NR_BINS; ++bin) {
        power[bin] = s->fft[bin].re * s->fft[bin].re +
            s->fft[bin].im * s->fft[bin].im;
    }

    for (int bin = 0; bin < NR_BINS; ++bin) {
        f32_t noise_candidate = power[bin];
        if (bin > 1 && bin < NR_HOP_SIZE - 1) {
            noise_candidate = 0.5f * (power[bin - 2] + power[bin + 2]);
        }

        if (s->warmup_frames > 0) {
            f32_t frame_count = (f32_t) (25 - s->warmup_frames);
            s->noise[bin] += (noise_candidate - s->noise[bin]) / frame_count;
            s->gain[bin] = 1.0f;
        }
        else {
            const f32_t noise_attack = 0.05f;
            const f32_t noise_release = 0.01f;
            f32_t noise_rate = (noise_candidate < s->noise[bin]) ?
                noise_attack : noise_release;
            s->noise[bin] += noise_rate * (noise_candidate - s->noise[bin]);
            if (s->noise[bin] < 1.0e-12f) s->noise[bin] = 1.0e-12f;

            f32_t post_snr = power[bin] / s->noise[bin];
            f32_t instant_snr = post_snr - 1.0f;
            if (instant_snr < 0.0f) instant_snr = 0.0f;
            f32_t prior_snr = s->alpha * s->speech[bin] +
                (1.0f - s->alpha) * instant_snr;
            f32_t gain = prior_snr / (1.0f + prior_snr);
            if (gain < s->min_gain) gain = s->min_gain;

            s->speech[bin] = gain * gain * post_snr;
            s->gain[bin] = gain;
        }
    }

    if (s->warmup_frames > 0)
        --s->warmup_frames;

    for (int bin = 1; bin < NR_HOP_SIZE; ++bin) {
        f32_t gain = 0.125f * s->gain[bin - 1] + 0.75f * s->gain[bin] +
            0.125f * s->gain[bin + 1];
        s->fft[bin].re *= gain;
        s->fft[bin].im *= gain;
        s->fft[NR_FFT_SIZE - bin].re *= gain;
        s->fft[NR_FFT_SIZE - bin].im *= gain;
    }

    nr_fft(s->fft, true);

    for (int i = 0; i < NR_HOP_SIZE; ++i) {
        f32_t window = (f32_t) s->window[i] / K_AMPMAX;
        f32_t sample = s->fft[i].re * window + s->overlap[i];
        sample *= s->final_gain;
        output[i] = nr_clip_sample(sample);

        window = (f32_t) s->window[i + NR_HOP_SIZE] / K_AMPMAX;
        s->overlap[i] = s->fft[i + NR_HOP_SIZE].re * window;
    }

    memcpy(s->history, current, sizeof(s->history));
}

void nr_spectral_init(int rx_chan, TYPEREAL nr_param[NOISE_PARAMS])
{
    nr_spectral_t* s = &nr_spectral[rx_chan];
    if (!s->initialized) {
        *s = nr_spectral_t();
        for (int i = 0; i < NR_FFT_SIZE; ++i) {
            f32_t window = sqrtf(0.5f - 0.5f *
                cosf(2.0f * (f32_t) M_PI * i / NR_FFT_SIZE));
            s->window[i] = (s2_t) MROUND(window * K_AMPMAX);
        }
        s->initialized = true;
    }

    s->final_gain = nr_param[NR_S_GAIN];
    s->alpha = nr_param[NR_ALPHA];
    s->min_gain = 1.0f / sqrtf(nr_param[NR_ASNR]);
    if (s->min_gain < 0.25f) s->min_gain = 0.25f;
    if (s->min_gain > 0.50f) s->min_gain = 0.50f;

    memset(s->history, 0, sizeof(s->history));
    memset(s->overlap, 0, sizeof(s->overlap));
    memset(s->noise, 0, sizeof(s->noise));
    memset(s->speech, 0, sizeof(s->speech));
    s->warmup_frames = 24;
}

void nr_spectral_process(int rx_chan, int nsamps, TYPEMONO16* input,
    TYPEMONO16* output)
{
    nr_spectral_t* s = &nr_spectral[rx_chan];
    assert((nsamps % NR_HOP_SIZE) == 0);
    for (int offset = 0; offset < nsamps; offset += NR_HOP_SIZE)
        nr_spectral_process_hop(s, input + offset, output + offset);
}
