#ifndef MTLXRENDER_VERIFIED_MATH_H_
#define MTLXRENDER_VERIFIED_MATH_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sin_bits;
    uint32_t cos_bits;
    unsigned invalid;
} MtlxVerifiedSinCos;

/* All finite binary32 inputs use the fixed Payne-Hanek/Horner algorithm modeled
 * in Lean. Returns 0 for an unsupported strict FP environment. NaN/infinity
 * outputs are canonical quiet NaNs. Caller exception flags are preserved. */
int mtlx_verified_sincos(uint32_t input, MtlxVerifiedSinCos *out);

/* Install the strict backend for MaterialX graph sine/cosine. Configure before
 * shading threads start. Enabling rejects unsupported FP environments without
 * changing the current backend. Disabling restores the reference libm path. */
int mtlx_verified_math_enable(int enabled);

#ifdef __cplusplus
}
#endif
#endif
