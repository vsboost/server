#pragma once

#include "bayes_hsmm.hpp"

#include <algorithm>

class BayesCwStream {
public:
    enum {
        WINDOW_TICKS = 1600,
        WARMUP_TICKS = 800,
        DECODE_INTERVAL_TICKS = 200,
        STABILITY_TICKS = 100
    };

    explicit BayesCwStream(BayesHsmm::Workspace& workspace) :
        workspace(workspace)
    {
        reset();
    }

    void reset()
    {
        writePosition = 0;
        available = 0;
        sinceDecode = 0;
        totalTicks = 0;
        lastEmittedTick = -1;
        lastWpm = 0;
        modelWpm = 0;
        intervalsSinceSelection = 0;
        recentCount = 0;
        lastDecoded[0] = '\0';
    }

    void push(double value)
    {
        history[writePosition] = (float) value;
        writePosition = (writePosition + 1) % WINDOW_TICKS;
        if (available < WINDOW_TICKS) available++;
        totalTicks++;
    }

    int add(double value, char* output, int outputSize, int& wpm)
    {
        push(value);
        sinceDecode++;
        if (available < WARMUP_TICKS ||
            sinceDecode < DECODE_INTERVAL_TICKS) {
            output[0] = '\0';
            wpm = lastWpm;
            return 0;
        }
        sinceDecode = 0;
        return process(output, outputSize, wpm);
    }

    int process(char* output, int outputSize, int& wpm)
    {
        if (available < WARMUP_TICKS) {
            output[0] = '\0';
            wpm = lastWpm;
            return 0;
        }
        return decode(output, outputSize, wpm, STABILITY_TICKS);
    }

    int flush(char* output, int outputSize, int& wpm)
    {
        return decode(output, outputSize, wpm, 0);
    }

    const char* decodedText() const { return lastDecoded; }

private:
    BayesHsmm::Workspace& workspace;
    float history[WINDOW_TICKS];
    int writePosition;
    int available;
    int sinceDecode;
    long long totalTicks;
    long long lastEmittedTick;
    int lastWpm;
    int modelWpm;
    int intervalsSinceSelection;
    char recentCharacters[16];
    long long recentTicks[16];
    int recentCount;
    char lastDecoded[BayesHsmm::MAX_OUTPUT];

    int decode(char* output, int outputSize, int& wpm, int stabilityTicks)
    {
        int start = available == WINDOW_TICKS ? writePosition : 0;
        for (int i = 0; i < available; ++i)
            workspace.input[i] = history[(start + i) % WINDOW_TICKS];

        char decoded[BayesHsmm::MAX_OUTPUT];
        int endTicks[BayesHsmm::MAX_OUTPUT];
        int length;
        if (modelWpm == 0 || intervalsSinceSelection >= 8) {
            length = BayesHsmm::decodeBest(workspace.input, available,
                decoded, sizeof(decoded), workspace, wpm, 4.0, endTicks,
                &modelWpm);
            intervalsSinceSelection = 0;
        } else {
            BayesHsmm decoder(modelWpm, 4.0);
            BayesHsmm::Metrics metrics;
            length = decoder.decode(workspace.input, available, decoded,
                sizeof(decoded), workspace, &metrics, endTicks);
            wpm = metrics.estimatedWpm ? metrics.estimatedWpm : modelWpm;
            intervalsSinceSelection++;
            if (length == 0) modelWpm = 0;
        }
        std::memcpy(lastDecoded, decoded, length);
        lastDecoded[length] = '\0';
        lastWpm = wpm;
        int stableEnd = available - stabilityTicks;
        long long windowStart = totalTicks - available;
        int written = 0;
        for (int i = 0; i < length && written + 1 < outputSize; ++i) {
            long long absoluteTick = windowStart + endTicks[i];
            if (endTicks[i] <= stableEnd && absoluteTick > lastEmittedTick) {
                lastEmittedTick = absoluteTick;
                if (isDuplicate(decoded[i], absoluteTick)) continue;
                output[written++] = decoded[i];
                int slot = recentCount % 16;
                recentCharacters[slot] = decoded[i];
                recentTicks[slot] = absoluteTick;
                recentCount++;
            }
        }
        output[written] = '\0';
        return written;
    }

    bool isDuplicate(char value, long long tick) const
    {
        int count = std::min(recentCount, 16);
        for (int i = 0; i < count; ++i) {
            int slot = (recentCount - i - 1) % 16;
            if (recentCharacters[slot] == value &&
                tick - recentTicks[slot] <= 16)
                return true;
        }
        return false;
    }
};
