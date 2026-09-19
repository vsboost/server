#include "extensions/CW_skimmer/skimmer.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vector>

static uint32_t read_be32(const unsigned char* p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
        ((uint32_t) p[2] << 8) | p[3];
}

static int16_t read_be16(const unsigned char* p)
{
    return (int16_t) (((uint16_t) p[0] << 8) | p[1]);
}

static double monotonic_seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--dump") != 0)) {
        fprintf(stderr, "usage: %s CW.test.au [--dump]\n", argv[0]);
        return 2;
    }
    bool dump = argc == 3;

    FILE* file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 2;
    }

    unsigned char header[24];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fprintf(stderr, "%s: incomplete AU header\n", argv[1]);
        fclose(file);
        return 2;
    }

    uint32_t offset = read_be32(header + 4);
    uint32_t encoding = read_be32(header + 12);
    uint32_t sampleRate = read_be32(header + 16);
    uint32_t channels = read_be32(header + 20);
    if (memcmp(header, ".snd", 4) != 0 || offset < sizeof(header) ||
        encoding != 3 || sampleRate != 12000 || channels != 1) {
        fprintf(stderr, "%s: expected mono 12 kHz 16-bit Sun/NeXT AU audio\n", argv[1]);
        fclose(file);
        return 2;
    }

    if (fseek(file, offset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(file);
        return 2;
    }

    CwSkimmer skimmer(sampleRate);
    skimmer.SetParams(PWR_CALC_AVG_RATIO, true);

    unsigned int samplesFed = 0;
    unsigned int characters = 0;
    unsigned int firstCharacterSample = 0;
    skimmer.SetCallback([&](int freq, char ch, int) {
        if (characters++ == 0) firstCharacterSample = samplesFed;
        if (dump)
            printf("CW_EVENT %.3f %d %02X\n", samplesFed / (double) sampleRate,
                freq, (unsigned char) ch);
    });

    const size_t blockSize = 256;
    std::vector<unsigned char> bytes(blockSize * sizeof(int16_t));
    std::vector<int16_t> samples(blockSize);
    double start = monotonic_seconds();
    for (;;) {
        size_t bytesRead = fread(&bytes[0], 1, bytes.size(), file);
        if (bytesRead == 0) break;
        if (bytesRead % sizeof(int16_t) != 0) {
            fprintf(stderr, "%s: truncated 16-bit sample\n", argv[1]);
            fclose(file);
            return 2;
        }

        size_t count = bytesRead / sizeof(int16_t);
        for (size_t i = 0; i < count; ++i)
            samples[i] = read_be16(&bytes[i * sizeof(int16_t)]);
        samplesFed += count;
        skimmer.AddSamples(&samples[0], count);
    }
    double elapsed = monotonic_seconds() - start;
    fclose(file);

    if (characters == 0) {
        fprintf(stderr, "%s: no CW characters decoded\n", argv[1]);
        return 1;
    }

    double duration = samplesFed / (double) sampleRate;
    printf("CW skimmer replay: %.2fs audio, %u characters, first character %.3fs, "
           "%.2fx real-time\n",
           duration, characters, firstCharacterSample / (double) sampleRate,
           duration / elapsed);
    return 0;
}
