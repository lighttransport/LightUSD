#ifndef MTLXRENDER_VERIFIED_VNDF_H_
#define MTLXRENDER_VERIFIED_VNDF_H_
#include "verified_direction.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    MtlxVerifiedBitVec3 stretched, cap, raw, half;
    uint32_t u, v, phase, radial, stretched_length, raw_length;
    unsigned valid;
} MtlxVerifiedVndfTrace;

/* Strict verified-trig VNDF word schedule. Inputs are binary32 encodings and
 * PCG output words (top 24 bits form each uniform input). Positive finite
 * axes <=1, finite view and positive view.z are required. Unsupported FP
 * environments, nonfinite stages or nonpositive lengths return valid=0 and
 * canonical NaNs. Caller FP state is preserved.
 *
 * valid means the stages are finite with positive lengths; it is NOT a view,
 * direction, albedo, or energy certificate. Lean's bulk norm bound additionally
 * requires an accepted stretched-view certificate and its bulk-grid premises.
 * Default renderer selection is unchanged. No accumulated operation flags. */
MtlxVerifiedVndfTrace mtlx_vf_vndf24_trace(uint32_t ax, uint32_t ay,
    MtlxVerifiedBitVec3 view, uint32_t azimuth_word, uint32_t height_word);

#ifdef __cplusplus
}
#endif
#endif
