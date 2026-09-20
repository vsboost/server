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

static uint32_t read_le32(const unsigned char* p)
{
    return ((uint32_t) p[3] << 24) | ((uint32_t) p[2] << 16) |
        ((uint32_t) p[1] << 8) | p[0];
}

static uint16_t read_le16(const unsigned char* p)
{
    return ((uint16_t) p[1] << 8) | p[0];
}

static int16_t read_be16(const unsigned char* p)
{
    return (int16_t) (((uint16_t) p[0] << 8) | p[1]);
}

static int16_t read_sample(const unsigned char* p, bool bigEndian)
{
    return bigEndian ? read_be16(p) : (int16_t) read_le16(p);
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
        fprintf(stderr, "usage: %s CW.test.au-or-wav [--dump]\n", argv[0]);
        return 2;
    }
    bool dump = argc == 3;

    FILE* file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 2;
    }

    unsigned char header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fprintf(stderr, "%s: incomplete audio header\n", argv[1]);
        fclose(file);
        return 2;
    }

    bool bigEndian = false;
    uint32_t sampleRate = 0;
    uint32_t dataBytes = 0;
    long dataOffset = 0;
    if (memcmp(header, ".snd", 4) == 0) {
        unsigned char auHeader[24];
        if (fseek(file, 0, SEEK_SET) != 0 ||
            fread(auHeader, 1, sizeof(auHeader), file) != sizeof(auHeader)) {
            fprintf(stderr, "%s: incomplete AU header\n", argv[1]);
            fclose(file);
            return 2;
        }

        uint32_t offset = read_be32(auHeader + 4);
        uint32_t encoding = read_be32(auHeader + 12);
        uint32_t channels = read_be32(auHeader + 20);
        if (offset < sizeof(auHeader) || encoding != 3 || channels != 1) {
            fprintf(stderr, "%s: expected mono 16-bit Sun/NeXT AU audio\n", argv[1]);
            fclose(file);
            return 2;
        }
        sampleRate = read_be32(auHeader + 16);
        dataBytes = read_be32(auHeader + 8);
        dataOffset = offset;
        bigEndian = true;
    } else if (memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
        bool foundFormat = false;
        bool foundData = false;
        for (;;) {
            unsigned char chunk[8];
            if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) break;
            uint32_t chunkBytes = read_le32(chunk + 4);
            long chunkOffset = ftell(file);
            if (memcmp(chunk, "fmt ", 4) == 0) {
                unsigned char format[16];
                if (chunkBytes < sizeof(format) ||
                    fread(format, 1, sizeof(format), file) != sizeof(format)) {
                    break;
                }
                if (read_le16(format) != 1 || read_le16(format + 2) != 1 ||
                    read_le16(format + 14) != 16) {
                    fprintf(stderr, "%s: expected mono 16-bit PCM WAV audio\n", argv[1]);
                    fclose(file);
                    return 2;
                }
                sampleRate = read_le32(format + 4);
                foundFormat = true;
            } else if (memcmp(chunk, "data", 4) == 0) {
                dataOffset = chunkOffset;
                dataBytes = chunkBytes;
                foundData = true;
            }
            if (foundFormat && foundData) break;
            if (fseek(file, chunkOffset + chunkBytes + (chunkBytes & 1), SEEK_SET) != 0) break;
        }
        if (!foundFormat || !foundData) {
            fprintf(stderr, "%s: incomplete PCM WAV audio\n", argv[1]);
            fclose(file);
            return 2;
        }
    } else {
        fprintf(stderr, "%s: expected Sun/NeXT AU or PCM WAV audio\n", argv[1]);
        fclose(file);
        return 2;
    }

    if (fseek(file, dataOffset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(file);
        return 2;
    }

    CwSkimmer skimmer(sampleRate);

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
    while (dataBytes != 0) {
        size_t bytesToRead = dataBytes < bytes.size() ? dataBytes : bytes.size();
        size_t bytesRead = fread(&bytes[0], 1, bytesToRead, file);
        if (bytesRead == 0) break;
        if (bytesRead % sizeof(int16_t) != 0) {
            fprintf(stderr, "%s: truncated 16-bit sample\n", argv[1]);
            fclose(file);
            return 2;
        }

        size_t count = bytesRead / sizeof(int16_t);
        for (size_t i = 0; i < count; ++i)
            samples[i] = read_sample(&bytes[i * sizeof(int16_t)], bigEndian);
        samplesFed += count;
        skimmer.AddSamples(&samples[0], count);
        dataBytes -= bytesRead;
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
