#include "lms.h"
#include "noise_filter.h"
#include "teensy.h"
#include "wdsp.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <vector>

static const int kSampleRate = 12000;
static const int kBlockSize = 512;
static const int kSampleCount = kBlockSize * 96;

typedef void (*noise_filter_t)(const std::vector<TYPEMONO16>&,
    std::vector<TYPEMONO16>&);

static void filter_spectral(const std::vector<TYPEMONO16>& input,
    std::vector<TYPEMONO16>& output);

static double rms(const std::vector<TYPEMONO16>& samples, int start)
{
    double power = 0.0;
    int count = (int) samples.size() - start;
    for (int i = start; i < (int) samples.size(); ++i) {
        double sample = samples[i] / K_AMPMAX;
        power += sample * sample;
    }
    return sqrt(power / count);
}

static void expect_level_preserved(const char* name, noise_filter_t filter)
{
    std::vector<TYPEMONO16> input(kSampleCount);
    std::vector<TYPEMONO16> output(kSampleCount);
    for (int i = 0; i < kSampleCount; ++i) {
        input[i] = (TYPEMONO16) round(7000.0 * sin(2.0 * M_PI * 700.0 *
            i / kSampleRate));
    }

    filter(input, output);

    const int settle_samples = kSampleRate * 2;
    double input_rms = rms(input, settle_samples);
    double output_rms = rms(output, settle_samples);
    double ratio = output_rms / input_rms;
    printf("%s output/input RMS ratio: %.3f\n", name, ratio);
    assert(ratio >= 0.80 && ratio <= 1.20);
}

static void expect_silence(const char* name, noise_filter_t filter)
{
    std::vector<TYPEMONO16> silence(kSampleCount);
    std::vector<TYPEMONO16> output(kSampleCount);
    filter(silence, output);
    for (int i = 0; i < kSampleCount; ++i)
        assert(output[i] == 0);
    printf("%s silence test passed\n", name);
}

static void expect_spectral_noise_attenuated()
{
    std::vector<TYPEMONO16> noise(kSampleCount);
    std::vector<TYPEMONO16> output(kSampleCount);
    unsigned int random = 0x1f123bb5;

    for (int i = 0; i < kSampleCount; ++i) {
        random = random * 1664525U + 1013904223U;
        noise[i] = (TYPEMONO16) (((int) (random >> 16) - 32768) / 10);
    }

    filter_spectral(noise, output);
    const int start = kSampleRate;
    double attenuation = rms(output, start) / rms(noise, start);
    printf("spectral noise RMS ratio: %.3f\n", attenuation);
    assert(attenuation <= 0.50);
}

static void filter_wdsp(const std::vector<TYPEMONO16>& input,
    std::vector<TYPEMONO16>& output)
{
    TYPEREAL params[NOISE_PARAMS] = {};
    params[NR_TAPS] = 64;
    params[NR_DLY] = 16;
    params[NR_GAIN] = 8.0e-5f;
    params[NR_LEAKAGE] = 0.125f;
    wdsp_ANR_init(0, NR_DENOISE, params);

    for (int i = 0; i < kSampleCount; i += kBlockSize)
        wdsp_ANR_filter(0, NR_DENOISE, kBlockSize,
            const_cast<TYPEMONO16*>(&input[i]), &output[i]);
}

static void filter_lms(const std::vector<TYPEMONO16>& input,
    std::vector<TYPEMONO16>& output)
{
    TYPEREAL params[NOISE_PARAMS] = {};
    params[NR_DELAY] = 1;
    params[NR_BETA] = 0.05f;
    params[NR_DECAY] = 0.98f;
    CLMS filter;
    assert(filter.Initialize(NR_DENOISE, params) == 0);

    for (int i = 0; i < kSampleCount; i += kBlockSize)
        filter.ProcessFilter(kBlockSize, const_cast<TYPEMONO16*>(&input[i]),
            &output[i]);
}

static void filter_spectral(const std::vector<TYPEMONO16>& input,
    std::vector<TYPEMONO16>& output)
{
    TYPEREAL params[NOISE_PARAMS] = {};
    params[NR_S_GAIN] = 1.0f;
    params[NR_ALPHA] = 0.95f;
    params[NR_ASNR] = 1000.0f;
    nr_spectral_init(0, params);

    for (int i = 0; i < kSampleCount; i += kBlockSize)
        nr_spectral_process(0, kBlockSize, const_cast<TYPEMONO16*>(&input[i]),
            &output[i]);
}

int main()
{
    expect_level_preserved("WDSP", filter_wdsp);
    expect_level_preserved("LMS", filter_lms);
    expect_level_preserved("spectral", filter_spectral);
    expect_silence("WDSP", filter_wdsp);
    expect_silence("LMS", filter_lms);
    expect_silence("spectral", filter_spectral);
    expect_spectral_noise_attenuated();
    puts("noise reduction gain tests passed");
    return 0;
}
