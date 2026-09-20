#pragma once

#include <math.h>

class NoiseLeveler {
public:
    NoiseLeveler()
    {
        Reset();
    }

    void Reset()
    {
        m_input_power = 0.0f;
        m_output_power = 0.0f;
        m_gain = 1.0f;
    }

    f32_t Apply(f32_t input, f32_t output)
    {
        const f32_t input_power = input * input;
        const f32_t output_power = output * output;
        const f32_t alpha = 0.001f;
        const f32_t min_power = 1.0e-8f;

        m_input_power += alpha * (input_power - m_input_power);
        m_output_power += alpha * (output_power - m_output_power);

        if (m_input_power > min_power && m_output_power > min_power) {
            f32_t target_gain = sqrtf(m_input_power / m_output_power);
            if (target_gain < 0.25f) target_gain = 0.25f;
            if (target_gain > 4.0f) target_gain = 4.0f;

            const f32_t rate = (target_gain > m_gain) ? 0.002f : 0.0005f;
            m_gain += rate * (target_gain - m_gain);
        }

        return output * m_gain;
    }

private:
    f32_t m_input_power;
    f32_t m_output_power;
    f32_t m_gain;
};
