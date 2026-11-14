#pragma once
// Portable baseline plus baseline-ISA kernels. No AVX requirement or aligned
// loads; an x64 build runs on any x64 CPU, ARM64 uses the platform's NEON ISA.
#include <cstddef>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif
namespace fengqudb::index::impl {
inline double vector_dot(const float *a, const float *b, size_t count) {
    size_t i = 0;
    double sum = 0;
#if defined(_M_X64) || defined(__SSE2__)
    auto low = _mm_setzero_pd(), high = _mm_setzero_pd();
    for (; i + 4 <= count; i += 4) {
        auto x = _mm_loadu_ps(a + i), y = _mm_loadu_ps(b + i);
        low = _mm_add_pd(low, _mm_mul_pd(_mm_cvtps_pd(x), _mm_cvtps_pd(y)));
        high = _mm_add_pd(
            high, _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(x, x)), _mm_cvtps_pd(_mm_movehl_ps(y, y))));
    }
    double lanes[2];
    _mm_storeu_pd(lanes, _mm_add_pd(low, high));
    sum = lanes[0] + lanes[1];
#elif defined(__aarch64__) && defined(__ARM_NEON)
    float64x2_t low = vdupq_n_f64(0), high = vdupq_n_f64(0);
    for (; i + 4 <= count; i += 4) {
        auto x = vld1q_f32(a + i), y = vld1q_f32(b + i);
        low =
            vaddq_f64(low, vmulq_f64(vcvt_f64_f32(vget_low_f32(x)), vcvt_f64_f32(vget_low_f32(y))));
        high = vaddq_f64(high,
                         vmulq_f64(vcvt_f64_f32(vget_high_f32(x)), vcvt_f64_f32(vget_high_f32(y))));
    }
    double lanes[2];
    vst1q_f64(lanes, vaddq_f64(low, high));
    sum = lanes[0] + lanes[1];
#endif
    for (; i < count; ++i)
        sum += double(a[i]) * b[i];
    return sum;
}
} // namespace fengqudb::index::impl
