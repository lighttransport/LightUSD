#ifndef MTLXRENDER_VERIFIED_FIXED_H_
#define MTLXRENDER_VERIFIED_FIXED_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MTLX_VERIFIED_FIXED_SCALE UINT64_C(281474976710656)
typedef struct {
    uint64_t lower;
    uint64_t upper;
    unsigned valid;
} MtlxVerifiedFixedBounds;
/* Outward integer floor/ceil of Q*min(cap/Q,max(0,float(bits)*2^-shift)),
 * Q=2^48. Finite encodings, shift<=46, cap<=Q. Pure integer implementation;
 * independent of rounding mode, flags and denormal controls. Invalid returns
 * valid=0 and zero endpoints. No claim about an albedo/integrand is made here. */
MtlxVerifiedFixedBounds mtlx_vf_fixed_scaled(uint32_t bits,unsigned shift,uint64_t cap);
/* Checked nonnegative ordered 64-bit sum. Overflow rejects; never wraps. */
MtlxVerifiedFixedBounds mtlx_vf_fixed_add(MtlxVerifiedFixedBounds a,MtlxVerifiedFixedBounds b);
/* Exact integer equivalent of width<=1/5000 on the 2^-48 grid. */
int mtlx_vf_fixed_width_accepted(MtlxVerifiedFixedBounds a);
/* Upward binary32 encoding of upper/Q, 0<=upper<=Q. Integer-only significand
 * rounding, including carry into the next binade; zero stays positive zero.
 * Rejects upper>Q with canonical quiet NaN. Preserves all FP state. */
uint32_t mtlx_vf_fixed_upper(uint64_t upper);
#ifdef __cplusplus
}
#endif
#endif
