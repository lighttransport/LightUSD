#ifndef MTLXRENDER_VERIFIED_RANGES_H_
#define MTLXRENDER_VERIFIED_RANGES_H_
#include "verified_bounds.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Compositional real interval arithmetic using outward binary32 scalar bounds.
 * Input valid must be 1; endpoints must be finite and ordered. Invalid inputs,
 * nonfinite required bounds, zero-containing divisors and negative sqrt domains
 * return valid=0/canonical-NaN endpoints. Every successful interval contains the
 * entire real image of the input intervals, not only sampled endpoint values.
 * The strict FP environment is checked and the caller state restored. */
MtlxVerifiedFloatBounds mtlx_vf_range_point(uint32_t x);
MtlxVerifiedFloatBounds mtlx_vf_range_checked(MtlxVerifiedFloatBounds a);
/* 1: entire range <=0, 2: entire range >0, 0: invalid or unresolved. */
int mtlx_vf_range_sign(MtlxVerifiedFloatBounds a);
MtlxVerifiedFloatBounds mtlx_vf_range_add(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b);
MtlxVerifiedFloatBounds mtlx_vf_range_sub(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b);
MtlxVerifiedFloatBounds mtlx_vf_range_neg(MtlxVerifiedFloatBounds a);
MtlxVerifiedFloatBounds mtlx_vf_range_mul(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b);
MtlxVerifiedFloatBounds mtlx_vf_range_inv(MtlxVerifiedFloatBounds a);
MtlxVerifiedFloatBounds mtlx_vf_range_div(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b);
MtlxVerifiedFloatBounds mtlx_vf_range_sqrt(MtlxVerifiedFloatBounds a);

/* Precision-aware real bounds: exact singleton add/multiply return a singleton
 * only when the IEEE result is finite and INEXACT is clear. All other cases
 * retain the original outward operation; they do not shrink general boxes. */
MtlxVerifiedFloatBounds mtlx_vf_range_hull(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b);
MtlxVerifiedFloatBounds mtlx_vf_range_exact_add(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b);
MtlxVerifiedFloatBounds mtlx_vf_range_exact_mul(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b);

#ifdef __cplusplus
}
#endif
#endif
