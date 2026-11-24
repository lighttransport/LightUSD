#ifndef MTLXRENDER_VERIFIED_FLOAT_H_
#define MTLXRENDER_VERIFIED_FLOAT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MTLX_VF_INVALID = 1,
    MTLX_VF_DIVIDE_BY_ZERO = 2,
    MTLX_VF_OVERFLOW = 4,
    MTLX_VF_UNDERFLOW = 8,
    MTLX_VF_INEXACT = 16
};

typedef struct {
    uint32_t bits;
    unsigned flags;
} MtlxVerifiedFloatResult;

/* Requires IEEE binary32, nearest/even, gradual underflow and strict
 * single-precision evaluation. Does not change the caller's FP environment. */
int mtlx_verified_float_supported(void);

/* Exact operation schedule used by the verified-math evaluator. Compile the
 * implementation without fast-math/FMA contraction. Per-operation exceptions
 * are returned without modifying the caller's sticky exception flags. */
MtlxVerifiedFloatResult mtlx_vf_add(uint32_t x, uint32_t y);
MtlxVerifiedFloatResult mtlx_vf_sub(uint32_t x, uint32_t y);
MtlxVerifiedFloatResult mtlx_vf_mul(uint32_t x, uint32_t y);
MtlxVerifiedFloatResult mtlx_vf_div(uint32_t x, uint32_t y);
MtlxVerifiedFloatResult mtlx_vf_sqrt(uint32_t x);

/* Finite unit-power projection. Nonfinite and nonpositive values become +0;
 * finite values >=1 become 1. Pure integer classification, independent of FP
 * mode/flags. This is an explicit conservative-backend operation, not IEEE RNE. */
uint32_t mtlx_vf_clamp_unit(uint32_t x);

/* Covers final projected-power scaling only; outgoing BSDF/PDF arithmetic and
 * sampler/input-map proof obligations are separate. Exact value is 2^-23. */
#define MTLX_VF_POWER_SCALE_RESERVE_BITS UINT32_C(0x34000000)

/* RNE scaling after unit projection. A finite nonnegative factor is required;
 * invalid factors/unsupported hosts return canonical NaN. Successful output is
 * finite in [0,factor]. Preserves caller FP state. */
uint32_t mtlx_vf_scale_unit_power(uint32_t factor,uint32_t power);

#ifdef __cplusplus
}
#endif
#endif
