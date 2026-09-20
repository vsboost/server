#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

class BayesHsmm {
public:
    enum {
        TICK_MS = 5,
        MAX_TICKS = 2400,
        MAX_SEGMENTS = 512,
        MAX_OUTPUT = 128
    };

    struct Segment {
        int state;
        int duration;
        double evidence;
    };

    struct Workspace {
        double input[MAX_TICKS];
        double ordered[MAX_TICKS];
        double prefix[2][MAX_TICKS + 1];
        double score[2][MAX_TICKS];
        short previousEnd[2][MAX_TICKS];
        unsigned short selectedDuration[2][MAX_TICKS];
        Segment segments[MAX_SEGMENTS];
    };

    struct Metrics {
        double modelGain;
        int characters;
        int unknownCharacters;
        int estimatedWpm;
    };

    explicit BayesHsmm(double wpm, double minimumConfidence = 0.0) :
        ditTicks(1200.0 / (wpm < 15.0 ? 15.0 : wpm) / TICK_MS),
        spacingScale(wpm < 15.0 ? (50.0 * 15.0 / wpm - 31.0) / 19.0 : 1.0),
        minimumConfidence(minimumConfidence)
    {}

    int decode(const double* envelope, int count, char* output, int outputSize,
        Metrics* metrics = NULL, int* endTicks = NULL) const
    {
        Workspace workspace;
        return decode(envelope, count, output, outputSize, workspace, metrics,
            endTicks);
    }

    int decode(const double* envelope, int count, char* output, int outputSize,
        Workspace& workspace, Metrics* metrics = NULL, int* endTicks = NULL) const
    {
        if (metrics) {
            metrics->modelGain = 0.0;
            metrics->characters = 0;
            metrics->unknownCharacters = 0;
            metrics->estimatedWpm = 0;
        }
        if (!envelope || count < 2 || count > MAX_TICKS || !output || outputSize < 2)
            return 0;

        double* ordered = workspace.ordered;
        std::memcpy(ordered, envelope, count * sizeof(ordered[0]));
        std::sort(ordered, ordered + count);
        int noiseCount = std::max(2, count * 3 / 10);
        double noiseMean = mean(ordered, noiseCount);
        double noiseVariance = variance(ordered, noiseCount, noiseMean) +
            maximum(1.0, noiseMean * noiseMean * 0.02);
        int markStart = count * 4 / 5;
        double markMean = mean(ordered + markStart, count - markStart);
        double markVariance = variance(ordered + markStart, count - markStart, markMean) +
            maximum(1.0, markMean * markMean * 0.05);
        int prefixCount = std::min(count, 150);
        double prefixMean = mean(envelope, prefixCount);
        double prefixVariance = variance(envelope, prefixCount, prefixMean);
        double levelRange = markMean - noiseMean;
        if (prefixMean - noiseMean < 0.2 * levelRange &&
            std::sqrt(prefixVariance) < 0.2 * levelRange) {
            noiseMean = prefixMean;
            noiseVariance = prefixVariance +
                maximum(1.0, noiseMean * noiseMean * 0.02);
        }
        double emissionVariance = maximum(noiseVariance, markVariance);

        double activeThreshold = noiseMean + 3.0 * std::sqrt(noiseVariance);
        int first = 0;
        while (first < count && envelope[first] < activeThreshold) first++;
        int last = count - 1;
        while (last > first && envelope[last] < activeThreshold) last--;
        if (first == count) {
            output[0] = '\0';
            return 0;
        }
        int activeCount = last - first + 1;

        double (*prefix)[MAX_TICKS + 1] = workspace.prefix;
        prefix[0][0] = prefix[1][0] = 0.0;
        for (int i = 0; i < activeCount; ++i) {
            double value = envelope[first + i];
            prefix[0][i + 1] = prefix[0][i] +
                logLikelihood(value, noiseMean, emissionVariance);
            prefix[1][i + 1] = prefix[1][i] +
                logLikelihood(value, markMean, emissionVariance);
        }

        const double invalid = -1e30;
        double (*score)[MAX_TICKS] = workspace.score;
        short (*previousEnd)[MAX_TICKS] = workspace.previousEnd;
        unsigned short (*selectedDuration)[MAX_TICKS] =
            workspace.selectedDuration;
        int minimumDuration = std::max(1, (int) (0.35 * ditTicks));

        for (int end = 0; end < activeCount; ++end) {
            for (int state = 0; state < 2; ++state) {
                score[state][end] = invalid;
                previousEnd[state][end] = -1;
                selectedDuration[state][end] = 0;
                int maximumDuration = std::max(minimumDuration,
                    (int) ((state ? 5.0 : 9.0 * spacingScale) * ditTicks));
                int limit = std::min(end + 1, maximumDuration);
                for (int duration = minimumDuration; duration <= limit; ++duration) {
                    int start = end - duration + 1;
                    double prior;
                    if (start == 0)
                        prior = 0.0;
                    else
                        prior = score[1 - state][start - 1];
                    double candidate = prior +
                        prefix[state][end + 1] - prefix[state][start] +
                        durationScore(state, duration);
                    if (candidate > score[state][end]) {
                        score[state][end] = candidate;
                        previousEnd[state][end] = (short) (start - 1);
                        selectedDuration[state][end] = (unsigned short) duration;
                    }
                }
            }
        }

        double nullMean = mean(envelope + first, activeCount);
        double nullVariance = variance(envelope + first, activeCount, nullMean) +
            maximum(1.0, nullMean * nullMean * 0.02);
        double nullScore = 0.0;
        for (int i = 0; i < activeCount; ++i)
            nullScore += logLikelihood(envelope[first + i], nullMean, nullVariance);
        double pathScore = maximum(score[0][activeCount - 1],
            score[1][activeCount - 1]);
        double modelGain = (pathScore - nullScore) / activeCount;
        if (metrics) metrics->modelGain = modelGain;
        if (modelGain < 0.3) {
            output[0] = '\0';
            return 0;
        }

        Segment* segments = workspace.segments;
        int segmentCount = 0;
        int state = score[0][activeCount - 1] > score[1][activeCount - 1] ? 0 : 1;
        int end = activeCount - 1;
        while (end >= 0 && segmentCount < MAX_SEGMENTS) {
            int duration = selectedDuration[state][end];
            if (duration == 0) break;
            int start = end - duration + 1;
            double selected = prefix[state][end + 1] - prefix[state][start];
            double alternate = prefix[1 - state][end + 1] - prefix[1 - state][start];
            segments[segmentCount++] = { state, duration, selected - alternate };
            end = previousEnd[state][end];
            state = 1 - state;
        }

        char symbols[10];
        int symbolCount = 0;
        double characterEvidence = 0.0;
        int characterTicks = 0;
        int written = 0;
        double estimatedDitTicks = 0.0;
        int estimatedDitCount = 0;
        int timelineTick = first;
        for (int i = segmentCount - 1; i >= 0; --i) {
            double units = segments[i].duration / ditTicks;
            timelineTick += segments[i].duration;
            if (segments[i].state) {
                bool isDot = std::fabs(units - 1.0) < std::fabs(units - 3.0);
                if (symbolCount < (int) sizeof(symbols) - 1)
                    symbols[symbolCount++] = isDot ? '.' : '-';
                estimatedDitTicks += segments[i].duration / (isDot ? 1.0 : 3.0);
                estimatedDitCount++;
                characterEvidence += segments[i].evidence;
                characterTicks += segments[i].duration;
            } else if (units >= 2.2 && symbolCount != 0) {
                symbols[symbolCount] = '\0';
                if (characterTicks != 0 &&
                    characterEvidence / characterTicks >= minimumConfidence)
                    append(output, outputSize, written, lookup(symbols),
                        endTicks, timelineTick);
                symbolCount = 0;
                characterEvidence = 0.0;
                characterTicks = 0;
                if (units >= 5.5)
                    append(output, outputSize, written, ' ', endTicks,
                        timelineTick + 1);
            } else if (symbolCount != 0) {
                characterEvidence += segments[i].evidence;
                characterTicks += segments[i].duration;
            }
        }
        if (symbolCount != 0) {
            symbols[symbolCount] = '\0';
            if (characterTicks != 0 &&
                characterEvidence / characterTicks >= minimumConfidence)
                append(output, outputSize, written, lookup(symbols), endTicks,
                    timelineTick);
        }
        output[written] = '\0';
        if (metrics) {
            metrics->characters = written;
            for (int i = 0; i < written; ++i)
                if (output[i] == '?') metrics->unknownCharacters++;
            if (estimatedDitCount != 0)
                metrics->estimatedWpm = (int) std::floor(
                    1200.0 / (estimatedDitTicks / estimatedDitCount * TICK_MS) +
                    0.5);
        }
        return written;
    }

    static int decodeBest(const double* envelope, int count, char* output,
        int outputSize, Workspace& workspace, int& wpm,
        double minimumConfidence = 4.0, int* endTicks = NULL,
        int* modelWpm = NULL)
    {
        static const int speeds[] = { 10, 12, 15, 18, 20, 24, 28, 32, 36, 40 };
        double bestQuality = -1e30;
        int bestLength = 0;
        wpm = 0;
        output[0] = '\0';
        for (unsigned int i = 0; i < sizeof(speeds) / sizeof(speeds[0]); ++i) {
            BayesHsmm decoder(speeds[i], minimumConfidence);
            Metrics metrics;
            char candidate[MAX_OUTPUT];
            int candidateEndTicks[MAX_OUTPUT];
            int length = decoder.decode(envelope, count, candidate,
                sizeof(candidate), workspace, &metrics, candidateEndTicks);
            double quality = metrics.modelGain -
                0.05 * metrics.unknownCharacters - (length == 0 ? 1.0 : 0.0);
            if (quality > bestQuality) {
                bestQuality = quality;
                bestLength = std::min(length, outputSize - 1);
                std::memcpy(output, candidate, bestLength);
                output[bestLength] = '\0';
                if (endTicks)
                    std::memcpy(endTicks, candidateEndTicks,
                        bestLength * sizeof(endTicks[0]));
                wpm = metrics.estimatedWpm ? metrics.estimatedWpm : speeds[i];
                if (modelWpm) *modelWpm = speeds[i];
            }
        }
        return bestLength;
    }

private:
    double ditTicks;
    double spacingScale;
    double minimumConfidence;

    static double maximum(double a, double b) { return a > b ? a : b; }

    static double mean(const double* values, int count)
    {
        double total = 0.0;
        for (int i = 0; i < count; ++i) total += values[i];
        return total / count;
    }

    static double variance(const double* values, int count, double average)
    {
        double total = 0.0;
        for (int i = 0; i < count; ++i) {
            double delta = values[i] - average;
            total += delta * delta;
        }
        return total / count;
    }

    static double logLikelihood(double value, double average, double variance)
    {
        double delta = value - average;
        return -0.5 * (delta * delta / variance + std::log(variance));
    }

    double durationScore(int state, int duration) const
    {
        static const double ratios[2][3] = {
            { 1.0, 3.0, 7.0 },
            { 1.0, 3.0, 0.0 }
        };
        static const double weights[2][3] = {
            { 0.50, 0.35, 0.15 },
            { 0.65, 0.35, 0.0 }
        };
        int choices = state ? 2 : 3;
        double best = -1e30;
        for (int i = 0; i < choices; ++i) {
            double ratio = ratios[state][i];
            if (!state && i != 0) ratio *= spacingScale;
            double sigma = maximum(1.0, ratio * ditTicks * 0.35);
            double z = (duration - ratio * ditTicks) / sigma;
            best = std::max(best, std::log(weights[state][i]) - 0.5 * z * z);
        }
        return best;
    }

    static void append(char* output, int outputSize, int& written, char value,
        int* endTicks, int endTick)
    {
        if (written + 1 < outputSize) {
            output[written] = value;
            if (endTicks) endTicks[written] = endTick;
            written++;
        }
    }

    static char lookup(const char* symbols)
    {
        static const struct {
            const char* symbols;
            char value;
        } table[] = {
            { ".-", 'A' }, { "-...", 'B' }, { "-.-.", 'C' }, { "-..", 'D' },
            { ".", 'E' }, { "..-.", 'F' }, { "--.", 'G' }, { "....", 'H' },
            { "..", 'I' }, { ".---", 'J' }, { "-.-", 'K' }, { ".-..", 'L' },
            { "--", 'M' }, { "-.", 'N' }, { "---", 'O' }, { ".--.", 'P' },
            { "--.-", 'Q' }, { ".-.", 'R' }, { "...", 'S' }, { "-", 'T' },
            { "..-", 'U' }, { "...-", 'V' }, { ".--", 'W' }, { "-..-", 'X' },
            { "-.--", 'Y' }, { "--..", 'Z' }, { "-----", '0' },
            { ".----", '1' }, { "..---", '2' }, { "...--", '3' },
            { "....-", '4' }, { ".....", '5' }, { "-....", '6' },
            { "--...", '7' }, { "---..", '8' }, { "----.", '9' }
        };
        for (unsigned int i = 0; i < sizeof(table) / sizeof(table[0]); ++i)
            if (std::strcmp(symbols, table[i].symbols) == 0) return table[i].value;
        return '?';
    }
};
