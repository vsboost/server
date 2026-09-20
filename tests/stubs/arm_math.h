#pragma once

static inline void arm_dot_prod_f32(const float* a, const float* b,
    unsigned int length, float* result)
{
    float sum = 0.0f;
    for (unsigned int i = 0; i < length; ++i)
        sum += a[i] * b[i];
    *result = sum;
}

static inline void arm_power_f32(const float* input, unsigned int length,
    float* result)
{
    float sum = 0.0f;
    for (unsigned int i = 0; i < length; ++i)
        sum += input[i] * input[i];
    *result = sum;
}
