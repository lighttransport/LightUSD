#ifndef MTLXRENDER_VERIFIED_NORMAL_H_
#define MTLXRENDER_VERIFIED_NORMAL_H_
#include "verified_fixed.h"
#include "verified_bounds.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    MtlxVerifiedFixedBounds bounds;
    uint32_t cells;
    uint32_t fallback_cells;
    unsigned certified;
    /* Upward binary32 encoding of bounds.upper/Q, or canonical NaN if invalid.
     * Valid coarse bounds also supply this value; consult certified separately. */
    uint32_t upper_bits;
} MtlxVerifiedNormalAlbedo;

typedef struct {
    MtlxVerifiedFixedBounds bounds;
    unsigned fallback;
} MtlxVerifiedNormalCell;

/* Exact encoding of (word>>8)*2^-24, the word-to-float map in pcg32_f.
 * Pure integer implementation, independent of the caller's FP environment.
 * This does not generate a word or assert a PCG state distribution. */
uint32_t mtlx_vf_pcg_uniform_bits(uint32_t word);

/* Pure exact encoding of index*2^-depth, 0<=index<=2^depth, depth<=23.
 * Invalid arguments return canonical NaN. No FP host precondition is needed. */
uint32_t mtlx_vf_unit_dyadic_bits(uint32_t index,unsigned depth);

/* The checked scalar cosine stage for a 24-bit-input dyadic CDF cell, before
 * direction normalization. Invalid/unresolved stages return valid=0. */
MtlxVerifiedFloatBounds mtlx_vf_ndf24_cos_cell(uint32_t alpha,unsigned depth,uint32_t index);

/* One dyadic cell, with the same domain and depth restrictions as the factory.
 * index must be strictly below 2^depth. */
MtlxVerifiedNormalCell mtlx_vf_normal_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index);

/* Same dyadic integration cell and analytic mass, with a separately supplied
 * finite-input box. Requires 0<=lower<=cell.start<=cell.stop<=upper<=1 (positive
 * binary32 encodings, canonical +0). Encloses ideal normal albedo and projected
 * rounded power if the staged sampler's input lies in this box throughout the
 * cell. It does not establish that sampler containment or outgoing transport.
 * Unresolved interval programs retain the original cell mass, not box mass. */
MtlxVerifiedNormalCell mtlx_vf_normal_cell_expanded(uint32_t alpha,uint32_t eta,unsigned depth,
                                                 uint32_t index,uint32_t lower,uint32_t upper);

/* Diagnostic ideal-real, isotropic, normal-view joint single-scatter albedo.
 * alpha and eta are positive finite binary32 encodings; alpha<=1. Uniform
 * dyadic depth is 0..23. Valid bounds enclose the full integral even when the
 * requested depth is too coarse to certify width<=1/5000. Unresolved cells
 * retain their entire analytic mass. Unsupported FP hosts reject the request.
 * This does not enclose a rounded native BSDF or compensate any lobe. */
MtlxVerifiedNormalAlbedo mtlx_vf_normal_albedo(uint32_t alpha,uint32_t eta,unsigned depth);

/* Strict rounded execution of the normal-view joint-density expression.
 * Finite input encodings only; the physical domain is a separate obligation.
 * Unsupported hosts/nonfinite inputs return canonical NaN. An accepted interval
 * program certifies finite output and containment; this function alone does not
 * establish a whole-integral reserve or a native BSDF energy bound. */
uint32_t mtlx_vf_normal_density_f32(uint32_t t,uint32_t alpha,uint32_t eta);

/* Unit-projected rounded normal-view joint power. For supported hosts and finite
 * input encodings, returns a finite value in [0,1], including when raw power is
 * nonfinite. This explicit projection is separate from the raw density above.
 * Its finite radial-CDF cell cap is proved; outgoing BSDF transport is separate. */
uint32_t mtlx_vf_normal_unit_power_f32(uint32_t t,uint32_t alpha,uint32_t eta);

/* Strict scalar schedule from sample_ggx_h, before azimuth/vector assembly or
 * normalization. alpha>0 and alpha<=1; uniform in [0,1], including signed zero.
 * Invalid inputs/unsupported hosts return canonical NaN. Valid extreme inputs
 * can also produce NaN through rounded denominator cancellation. */
uint32_t mtlx_vf_ggx_ndf_cos(uint32_t alpha,uint32_t uniform);

/* Joint projected power evaluated directly from that sampled cosine. eta is
 * positive finite. Exceptional raw power projects to zero, yielding finite
 * [0,1] power for valid inputs. The proved CDF-cell bound covers this scalar
 * stage only, not normalized directions, outgoing BSDFs or branch weights.
 * No reference renderer path is changed to select this helper. */
uint32_t mtlx_vf_ndf_sample_unit_power(uint32_t alpha,uint32_t eta,uint32_t uniform);

/* Dyadic cells/factory for ideal normal joint albedo AND the concrete finite
 * NDF-cosine/projected-power CDF integral. Uniform input boxes are constructed
 * with checked interval operations, not accepted from a caller. Same domain,
 * depth, width/certified and FP-host rules as normal_albedo. Unresolved stages
 * retain the original analytic mass. The rounded scalar cosine precedes vector
 * assembly/normalization; this does not certify a complete outgoing BSDF. */
MtlxVerifiedNormalCell mtlx_vf_ndf_sample_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index);
MtlxVerifiedNormalAlbedo mtlx_vf_ndf_sample_albedo(uint32_t alpha,uint32_t eta,unsigned depth);

/* The same scalar NDF power/certificate for uniform 24-bit grid inputs, as
 * produced by the word-to-float map in pcg32_f. Uniform boxes are widened down
 * by a complete 2^-24 step. The certified integral equals the finite uniform
 * average of all grid values; PCG word independence is a separate assumption.
 * Same domain/FP/width conventions as ndf_sample_albedo. Still precedes vector
 * normalization and outgoing BSDF geometry; no default renderer selects it. */
uint32_t mtlx_vf_ndf24_sample_unit_power(uint32_t alpha,uint32_t eta,uint32_t word);
MtlxVerifiedNormalCell mtlx_vf_ndf24_sample_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index);
MtlxVerifiedNormalAlbedo mtlx_vf_ndf24_sample_albedo(uint32_t alpha,uint32_t eta,unsigned depth);

/* Lower endpoint is a nonnegative downward shared factor <=(1-reserve)/upper.
 * upper must be positive finite; reserve must be finite in [0,1). Unsupported
 * hosts, unbounded divisions and negative lower endpoints reject the result.
 * The caller must establish upper>=joint ideal albedo and that the post-factor
 * native integrated error is <=reserve. No particular BSDF reserve is assumed. */
MtlxVerifiedFloatBounds mtlx_vf_joint_factor(uint32_t upper,uint32_t reserve);

/* Requires a width-certified factory result with matching converted upper.
 * Applies the same explicit reserve contract as mtlx_vf_joint_factor. */
MtlxVerifiedFloatBounds mtlx_vf_normal_factor(MtlxVerifiedNormalAlbedo certificate,uint32_t reserve);

/* Explicit checked allocation: both branches use one downward factor.
 * Accepted outputs are finite nonnegative and their exact real sum<=budget.
 * Rejection is explicit (valid=0), not a renderer fallback or BSDF guarantee. */
typedef struct { uint32_t factor,reflection,transmission;unsigned valid; } MtlxVerifiedSharedAllocation;
MtlxVerifiedSharedAllocation mtlx_vf_allocate_shared_power(uint32_t budget,uint32_t reflection,uint32_t transmission);

/* Checked discrete branch kernel. threshold is an exact count on the 2^24
 * grid; weights are downward importance weights for those finite probabilities.
 * Accepted uniform-word expected power<=reflection+transmission. Caller must
 * use genuine allocated powers and a genuine kernel for the energy contract. */
typedef struct { uint32_t threshold,reflection_weight,transmission_weight;unsigned valid; } MtlxVerifiedBranchKernel;
typedef struct { uint32_t weight;unsigned reflected,valid; } MtlxVerifiedBranchEvent;
MtlxVerifiedBranchKernel mtlx_vf_branch_kernel(uint32_t reflection,uint32_t transmission);
MtlxVerifiedBranchEvent mtlx_vf_branch_event(MtlxVerifiedBranchKernel kernel,uint32_t word);

/* True exactly for finite nonnegative alpha<=1 whose strict rounded slope
 * (alpha*alpha-1) is exactly -1. All PCG24 words then produce cosine=1, an ATOMIC proposal;
 * this classifier supplies no continuous PDF and is not itself a fallback. */
unsigned mtlx_vf_ndf_alpha_collapsed(uint32_t alpha);

#ifdef __cplusplus
}
#endif
#endif
