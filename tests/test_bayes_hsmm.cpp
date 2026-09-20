#include "extensions/CW_skimmer/bayes_hsmm.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

static uint32_t read_le32(const unsigned char* p)
{
    return ((uint32_t) p[3] << 24) | ((uint32_t) p[2] << 16) |
        ((uint32_t) p[1] << 8) | p[0];
}

static uint16_t read_le16(const unsigned char* p)
{
    return ((uint16_t) p[1] << 8) | p[0];
}

static bool readWav(const char* filename, std::vector<int16_t>& samples,
    unsigned int& sampleRate, double offsetSeconds)
{
    FILE* file = fopen(filename, "rb");
    if (!file) return false;

    unsigned char riff[12];
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(file);
        return false;
    }

    bool formatFound = false;
    bool dataFound = false;
    long dataOffset = 0;
    uint32_t dataBytes = 0;
    for (;;) {
        unsigned char chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) break;
        uint32_t size = read_le32(chunk + 4);
        long offset = ftell(file);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char format[16];
            if (size < sizeof(format) ||
                fread(format, 1, sizeof(format), file) != sizeof(format))
                break;
            if (read_le16(format) != 1 || read_le16(format + 2) != 1 ||
                read_le16(format + 14) != 16) {
                fclose(file);
                return false;
            }
            sampleRate = read_le32(format + 4);
            formatFound = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            dataOffset = offset;
            dataBytes = size;
            dataFound = true;
        }
        if (formatFound && dataFound) break;
        if (fseek(file, offset + size + (size & 1), SEEK_SET) != 0) break;
    }
    if (!formatFound || !dataFound) {
        fclose(file);
        return false;
    }

    uint32_t availableSamples = dataBytes / 2;
    uint32_t offsetSamples = (uint32_t) (offsetSeconds * sampleRate);
    if (offsetSamples >= availableSamples) {
        fclose(file);
        return false;
    }
    uint32_t maximumSamples = sampleRate *
        (BayesHsmm::MAX_TICKS * BayesHsmm::TICK_MS / 1000 + 1);
    uint32_t samplesToRead = std::min(maximumSamples,
        availableSamples - offsetSamples);
    if (fseek(file, dataOffset + offsetSamples * 2, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    std::vector<unsigned char> bytes(samplesToRead * 2);
    if (fread(&bytes[0], 2, samplesToRead, file) != samplesToRead) {
        fclose(file);
        return false;
    }
    fclose(file);

    samples.resize(samplesToRead);
    for (unsigned int i = 0; i < samples.size(); ++i)
        samples[i] = (int16_t) read_le16(&bytes[i * 2]);
    return true;
}

static int makeEnvelope(const std::vector<int16_t>& samples,
    unsigned int sampleRate, double frequency, double wpm, double* envelope)
{
    int hop = sampleRate / 200;
    int window = (int) (sampleRate * 0.6 * 1.2 / wpm);
    int minimumWindow = (int) (sampleRate * 15 / 1000);
    int maximumWindow = (int) (sampleRate * 30 / 1000);
    window = std::max(minimumWindow, std::min(maximumWindow, window));
    double omega = 2.0 * M_PI * frequency / sampleRate;
    double coefficient = 2.0 * cos(omega);
    double sideOffset = 2.0 * sampleRate / window;
    double lowerCoefficient =
        2.0 * cos(2.0 * M_PI * (frequency - sideOffset) / sampleRate);
    double upperCoefficient =
        2.0 * cos(2.0 * M_PI * (frequency + sideOffset) / sampleRate);
    int count = 0;
    for (unsigned int offset = 0;
        offset + window <= samples.size() && count < BayesHsmm::MAX_TICKS;
        offset += hop) {
        double current = 0.0, first = 0.0, second = 0.0;
        double lowerCurrent = 0.0, lowerFirst = 0.0, lowerSecond = 0.0;
        double upperCurrent = 0.0, upperFirst = 0.0, upperSecond = 0.0;
        for (int i = 0; i < window; ++i) {
            double hamming = 0.54 - 0.46 * cos(2.0 * M_PI * i / (window - 1));
            double sample = samples[offset + i] * hamming;
            current = sample + coefficient * first - second;
            second = first;
            first = current;
            lowerCurrent = sample + lowerCoefficient * lowerFirst - lowerSecond;
            lowerSecond = lowerFirst;
            lowerFirst = lowerCurrent;
            upperCurrent = sample + upperCoefficient * upperFirst - upperSecond;
            upperSecond = upperFirst;
            upperFirst = upperCurrent;
        }
        double power = first * first + second * second - coefficient * first * second;
        double lowerPower = lowerFirst * lowerFirst + lowerSecond * lowerSecond -
            lowerCoefficient * lowerFirst * lowerSecond;
        double upperPower = upperFirst * upperFirst + upperSecond * upperSecond -
            upperCoefficient * upperFirst * upperSecond;
        double center = sqrt(fmax(0.0, power));
        double flank = 0.5 * (sqrt(fmax(0.0, lowerPower)) +
            sqrt(fmax(0.0, upperPower)));
        envelope[count++] = fmax(0.0, center - flank) / window;
    }
    return count;
}

int main(int argc, char** argv)
{
    if (argc < 5 || argc > 7) {
        fprintf(stderr,
            "usage: %s WAV FREQ_HZ WPM EXPECTED [MIN_CONFIDENCE [OFFSET_SECONDS]]\n",
            argv[0]);
        return 2;
    }

    std::vector<int16_t> samples;
    unsigned int sampleRate = 0;
    double offsetSeconds = argc == 7 ? atof(argv[6]) : 0.0;
    if (!readWav(argv[1], samples, sampleRate, offsetSeconds)) {
        fprintf(stderr, "unable to read %s\n", argv[1]);
        return 2;
    }

    double envelope[BayesHsmm::MAX_TICKS];
    double wpm = atof(argv[3]);
    int count = makeEnvelope(samples, sampleRate, atof(argv[2]), wpm, envelope);
    BayesHsmm decoder(wpm, argc >= 6 ? atof(argv[5]) : 0.0);
    char output[BayesHsmm::MAX_OUTPUT];
    decoder.decode(envelope, count, output, sizeof(output));
    printf("%s\n", output);
    return strcmp(argv[4], "-") != 0 && strstr(output, argv[4]) == NULL;
}
