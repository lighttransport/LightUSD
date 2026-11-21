#ifndef MTLXRENDER_VERIFIED_BOUNDS_H_
#define MTLXRENDER_VERIFIED_BOUNDS_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t lower;
    uint32_t upper;
    unsigned valid;
} MtlxVerifiedFloatBounds;

/* Outward adjacent binary32 endpoints around one strict elementary operation.
 * Arguments and endpoints are IEEE bit encodings. Nonfinite inputs, invalid/exceptional results,
 * endpoints reaching infinity, or unsupported FP environments return valid=0
 * with canonical-NaN endpoints. Valid endpoints enclose the exact real result;
 * cancellation, signed zeros and gradual underflow are included. Caller FP
 * state is preserved. Compile under the same strict policy as verified_float.c.
 * This is a scalar enclosure primitive, not an albedo certificate/integrator. */
MtlxVerifiedFloatBounds mtlx_vf_bound_add(uint32_t x, uint32_t y);
MtlxVerifiedFloatBounds mtlx_vf_bound_sub(uint32_t x, uint32_t y);
MtlxVerifiedFloatBounds mtlx_vf_bound_mul(uint32_t x, uint32_t y);
MtlxVerifiedFloatBounds mtlx_vf_bound_div(uint32_t x, uint32_t y);
MtlxVerifiedFloatBounds mtlx_vf_bound_sqrt(uint32_t x);

#ifdef __cplusplus
}
#endif
#endif
