#pragma once

#include "bayes_stream.hpp"
#include "fftw3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdint.h>

#define MAX_CHANNELS 128
#define MAX_INPUT (MAX_CHANNELS * 2)

typedef std::function<void(int, char, int)> OutputCallback;

typedef enum {
    PWR_CALC_AVG_RATIO,
    PWR_CALC_AVG_BOTTOM,
    PWR_CALC_THRESHOLD,
} PwrCalc_t;

class CwSkimmer {
public:
    explicit CwSkimmer(int sampleRate) :
        sampleRate(sampleRate),
        hopSize(std::max(1, std::min(MAX_INPUT, sampleRate / 200))),
        fft(fftwf_plan_dft_r2c_1d(MAX_INPUT, fftIn, fftOut, FFTW_MEASURE))
    {
        for (int i = 0; i < MAX_TRACKS; ++i)
            tracks[i].stream = new BayesCwStream(workspace);
        reset();
    }

    ~CwSkimmer()
    {
        fftwf_destroy_plan(fft);
        for (int i = 0; i < MAX_TRACKS; ++i)
            delete tracks[i].stream;
    }

    void reset()
    {
        remains = 0;
        tickCount = 0;
        historyWritePosition = 0;
        historyAvailable = 0;
        std::memset(activity, 0, sizeof(activity));
        for (int i = 0; i < MAX_TRACKS; ++i) {
            tracks[i].active = false;
            tracks[i].bin = 0;
            tracks[i].strength = 0.0f;
            tracks[i].missedSelections = 0;
            tracks[i].wpm = 0;
            tracks[i].stream->reset();
        }
    }

    void SetParams(PwrCalc_t, bool) {}

    void SetCallback(OutputCallback outputCallback)
    {
        callback = outputCallback;
    }

    void AddSamples(float* samples, size_t count)
    {
        while (count != 0) {
            size_t copy = std::min(count, MAX_INPUT - remains);
            std::memcpy(dataBuf + remains, samples, copy * sizeof(dataBuf[0]));
            remains += copy;
            samples += copy;
            count -= copy;
            if (remains == MAX_INPUT) advance();
        }
    }

    void AddSamples(int16_t* samples, size_t count)
    {
        while (count != 0) {
            size_t copy = std::min(count, MAX_INPUT - remains);
            for (size_t i = 0; i < copy; ++i)
                dataBuf[remains + i] = samples[i] / 32768.0f;
            remains += copy;
            samples += copy;
            count -= copy;
            if (remains == MAX_INPUT) advance();
        }
    }

private:
    enum {
        MAX_TRACKS = 16,
        TRACK_SELECTION_TICKS = 200,
        TRACK_HOLD_SELECTIONS = 5,
        CANDIDATE_NMS_BINS = 1,
        OUTPUT_NMS_BINS = 4
    };

    struct Track {
        bool active;
        int bin;
        float strength;
        int missedSelections;
        int wpm;
        BayesCwStream* stream;
    };

    struct Candidate {
        int bin;
        float strength;
    };

    unsigned int sampleRate;
    int hopSize;
    size_t remains;
    int tickCount;

    float dataBuf[MAX_INPUT];
    float fftIn[MAX_INPUT];
    fftwf_complex fftOut[MAX_INPUT];
    fftwf_plan fft;
    float activity[MAX_CHANNELS];
    float history[MAX_CHANNELS][BayesCwStream::WINDOW_TICKS];
    int historyWritePosition;
    int historyAvailable;

    BayesHsmm::Workspace workspace;
    Track tracks[MAX_TRACKS];
    OutputCallback callback;

    void advance()
    {
        process();
        int retained = MAX_INPUT - hopSize;
        if (retained != 0)
            std::memmove(dataBuf, dataBuf + hopSize,
                retained * sizeof(dataBuf[0]));
        remains = retained;
    }

    void process()
    {
        double windowScale = 2.0 * M_PI / (MAX_INPUT - 1);
        for (int i = 0; i < MAX_INPUT; ++i)
            fftIn[i] = dataBuf[i] *
                (0.54f - 0.46f * std::cos(i * windowScale));
        fftwf_execute(fft);

        float magnitude[MAX_CHANNELS];
        for (int i = 0; i < MAX_CHANNELS; ++i)
            magnitude[i] = std::sqrt(
                fftOut[i][0] * fftOut[i][0] +
                fftOut[i][1] * fftOut[i][1]);

        float contrast[MAX_CHANNELS];
        double tickSeconds = hopSize / (double) sampleRate;
        float activityAlpha =
            (float) (tickSeconds / 0.5 < 1.0 ? tickSeconds / 0.5 : 1.0);
        for (int i = 0; i < MAX_CHANNELS; ++i) {
            int lower = std::max(0, i - 2);
            int upper = std::min(MAX_CHANNELS - 1, i + 2);
            float flank = 0.5f * (magnitude[lower] + magnitude[upper]);
            contrast[i] = std::max(0.0f, magnitude[i] - flank);
            activity[i] += activityAlpha * (contrast[i] - activity[i]);
            history[i][historyWritePosition] = contrast[i];
        }
        historyWritePosition =
            (historyWritePosition + 1) % BayesCwStream::WINDOW_TICKS;
        if (historyAvailable < BayesCwStream::WINDOW_TICKS)
            historyAvailable++;

        for (int i = 0; i < MAX_TRACKS; ++i)
            if (tracks[i].active)
                tracks[i].stream->push(contrast[tracks[i].bin]);

        tickCount++;
        if (tickCount % TRACK_SELECTION_TICKS == 0) {
            selectTracks();
            decodeTracks();
        }
    }

    void selectTracks()
    {
        Candidate candidates[MAX_CHANNELS];
        int candidateCount = 0;
        for (int bin = 1; bin < MAX_CHANNELS - 1; ++bin) {
            if (activity[bin] < activity[bin - 1] ||
                activity[bin] < activity[bin + 1])
                continue;
            Candidate candidate = { bin, activity[bin] };
            int position = candidateCount;
            while (position > 0 &&
                candidates[position - 1].strength < candidate.strength) {
                candidates[position] = candidates[position - 1];
                position--;
            }
            candidates[position] = candidate;
            candidateCount++;
        }

        bool seen[MAX_TRACKS] = {};
        int acceptedBins[MAX_TRACKS];
        int acceptedCount = 0;
        for (int i = 0; i < candidateCount && acceptedCount < MAX_TRACKS; ++i) {
            bool adjacent = false;
            for (int j = 0; j < acceptedCount; ++j)
                adjacent |= std::abs(candidates[i].bin - acceptedBins[j]) <=
                    CANDIDATE_NMS_BINS;
            if (adjacent) continue;

            int track = findTrack(candidates[i].bin);
            if (track < 0) track = findAvailableTrack(seen);
            if (track < 0) continue;
            if (!tracks[track].active ||
                tracks[track].bin != candidates[i].bin) {
                tracks[track].active = true;
                tracks[track].bin = candidates[i].bin;
                preloadTrack(track);
            }
            tracks[track].strength = candidates[i].strength;
            tracks[track].missedSelections = 0;
            seen[track] = true;
            acceptedBins[acceptedCount++] = candidates[i].bin;
        }

        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (!tracks[i].active || seen[i]) continue;
            tracks[i].missedSelections++;
            if (tracks[i].missedSelections >= TRACK_HOLD_SELECTIONS) {
                tracks[i].active = false;
                tracks[i].stream->reset();
            }
        }
    }

    int findTrack(int bin) const
    {
        for (int i = 0; i < MAX_TRACKS; ++i)
            if (tracks[i].active &&
                std::abs(tracks[i].bin - bin) <= CANDIDATE_NMS_BINS)
                return i;
        return -1;
    }

    int findAvailableTrack(const bool* seen) const
    {
        for (int i = 0; i < MAX_TRACKS; ++i)
            if (!tracks[i].active) return i;
        int weakest = -1;
        for (int i = 0; i < MAX_TRACKS; ++i)
            if (!seen[i] &&
                (weakest < 0 || tracks[i].strength < tracks[weakest].strength))
                weakest = i;
        return weakest;
    }

    void preloadTrack(int track)
    {
        tracks[track].stream->reset();
        int start = historyAvailable == BayesCwStream::WINDOW_TICKS ?
            historyWritePosition : 0;
        for (int i = 0; i < historyAvailable; ++i)
            tracks[track].stream->push(
                history[tracks[track].bin]
                    [(start + i) % BayesCwStream::WINDOW_TICKS]);
    }

    void decodeTracks()
    {
        char pending[MAX_TRACKS][BayesHsmm::MAX_OUTPUT];
        int pendingLength[MAX_TRACKS] = {};
        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (!tracks[i].active) continue;
            pendingLength[i] = tracks[i].stream->process(pending[i],
                sizeof(pending[i]), tracks[i].wpm);
        }

        for (int i = 0; i < MAX_TRACKS; ++i) {
            if (!tracks[i].active || pendingLength[i] == 0) continue;
            bool suppressed = false;
            for (int j = 0; j < MAX_TRACKS; ++j) {
                if (i == j || !tracks[j].active ||
                    pendingLength[j] == 0 ||
                    tracks[j].strength <= tracks[i].strength ||
                    std::abs(tracks[j].bin - tracks[i].bin) > OUTPUT_NMS_BINS)
                    continue;
                if (similar(tracks[i].stream->decodedText(),
                        tracks[j].stream->decodedText())) {
                    suppressed = true;
                    break;
                }
            }
            if (suppressed || !callback) continue;
            int frequency = (int) std::floor(
                tracks[i].bin * sampleRate / (double) MAX_INPUT + 0.5);
            for (int j = 0; j < pendingLength[i]; ++j)
                callback(frequency, pending[i][j], tracks[i].wpm);
        }
    }

    static bool similar(const char* left, const char* right)
    {
        char a[BayesHsmm::MAX_OUTPUT];
        char b[BayesHsmm::MAX_OUTPUT];
        int aLength = normalize(left, a);
        int bLength = normalize(right, b);
        int maximum = std::max(aLength, bLength);
        if (maximum < 4) return false;

        int previous[BayesHsmm::MAX_OUTPUT];
        int current[BayesHsmm::MAX_OUTPUT];
        for (int j = 0; j <= bLength; ++j) previous[j] = j;
        for (int i = 1; i <= aLength; ++i) {
            current[0] = i;
            for (int j = 1; j <= bLength; ++j)
                current[j] = std::min(
                    std::min(previous[j] + 1, current[j - 1] + 1),
                    previous[j - 1] + (a[i - 1] != b[j - 1]));
            std::memcpy(previous, current,
                (bLength + 1) * sizeof(previous[0]));
        }
        return previous[bLength] * 3 <= maximum;
    }

    static int normalize(const char* input, char* output)
    {
        int written = 0;
        while (*input && written + 1 < BayesHsmm::MAX_OUTPUT) {
            char value = *input++;
            if ((value >= 'A' && value <= 'Z') ||
                (value >= '0' && value <= '9'))
                output[written++] = value;
        }
        output[written] = '\0';
        return written;
    }
};
