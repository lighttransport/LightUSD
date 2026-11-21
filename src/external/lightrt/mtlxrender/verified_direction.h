#ifndef MTLXRENDER_VERIFIED_DIRECTION_H_
#define MTLXRENDER_VERIFIED_DIRECTION_H_
#include <stdint.h>
#include "verified_bounds.h"
#include "verified_normal.h"
#include "verified_single_power.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t x,y,z; } MtlxVerifiedBitVec3;

typedef struct {
    uint32_t x,y,z;
    uint32_t cosine;
    unsigned valid;
} MtlxVerifiedNdfDirection;

/* Strict local NDF half vector: PCG word-to-float maps, scalar GGX cosine,
 * verified sin/cos, ordered dot/sqrt, rounded reciprocal then component scale.
 * Positive finite alpha<=1 and a supported RNE host are required. All word bit
 * patterns are valid. Unsupported hosts/invalid inputs return valid=0 and NaNs.
 * Proved model bounds: squared length error<=2^-15; cosine=unit-clipped z,
 * differing from the pre-normalization sampled cosine by<=2^-17. These are
 * local-vector bounds, not world-frame/outgoing BSDF or energy guarantees.
 * In particular, do not reuse an unexpanded scalar certificate for cosine.
 * No default renderer path selects this helper. Caller FP state is preserved. */
MtlxVerifiedNdfDirection mtlx_vf_ndf24_half(uint32_t alpha,uint32_t azimuth_word,uint32_t radial_word);

/* Enlarge a valid scalar-cosine range by +/-2^-17 and clip to [0,1]. The proof
 * transfers a range containing the unnormalized cosine to the unit-clipped
 * normalized cosine above. Invalid/unresolved bounds conservatively return
 * [0,1]; unsupported FP hosts return valid=0. This is input enclosure growth,
 * not a radiometric reserve or a replacement for checking integral width. */
MtlxVerifiedFloatBounds mtlx_vf_ndf_direction_cos_box(MtlxVerifiedFloatBounds scalar_box);

/* Joint projected power from the normalized local half cosine. This differs
 * from the old scalar-cosine power helper. eta must be positive finite. */
uint32_t mtlx_vf_ndf24_geometry_unit_power(uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word);

/* Geometry-aware cell/factory: derive the 24-bit scalar cosine range, enlarge
 * it using the proved normalization error, then evaluate the power program.
 * Bounds cover ideal normal power and normalized-cosine finite power for every
 * azimuth word. Same alpha/eta/depth/FP/width conventions as ndf24_sample_albedo.
 * This remains local-normal-view power, not world/outgoing BSDF/PDF transport. */
MtlxVerifiedNormalCell mtlx_vf_ndf24_geometry_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index);
MtlxVerifiedNormalAlbedo mtlx_vf_ndf24_geometry_albedo(uint32_t alpha,uint32_t eta,unsigned depth);

/* Recompute and width-check this geometry factory before deriving the shared
 * factor. reserve is caller-supplied; 2^-23 covers only final scalar scaling,
 * not the complete finite BSDF. Rejected certificates return invalid bounds. */
MtlxVerifiedFloatBounds mtlx_vf_ndf24_geometry_factor(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t reserve);

/* Actual normal-view Fresnel/masking branch expressions at the normalized
 * clipped half cosine. Accepted nonnegative branch allocations sum to at most
 * the rounded scaled joint power. Invalid allocation remains explicit. This
 * does not evaluate a world/outgoing BSDF or supply a renderer fallback. */
MtlxVerifiedSharedAllocation mtlx_vf_ndf24_branch_power(uint32_t factor,uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word);

/* Compose the physical normalized-cosine allocation with its finite branch
 * kernel. Rejection is explicit; world/outgoing BSDF and fallback are separate. */
MtlxVerifiedBranchKernel mtlx_vf_ndf24_branch_kernel(uint32_t factor,uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word);
MtlxVerifiedBranchEvent mtlx_vf_ndf24_branch_event(uint32_t factor,uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word);

/* Match sample_ggx_h -> Duff to_world -> one world normalization. Checked
 * accepted normal norm^2 differs from1 by<=2^-20, half norm^2 by<=2^-15;
 * clipped rounded dot(half,normal) differs from raw sampled cosine by<=2^-17.
 * Failed checks return invalid. This is not an outgoing BSDF/fallback contract. */
MtlxVerifiedNdfDirection mtlx_vf_ndf24_world_half(uint32_t alpha,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word);

/* World-checked cosine -> shared physical power -> finite branch event.
 * A genuine geometry factory factor supplies the proved upper energy budget.
 * Rejection stays explicit. Outgoing directions/BSDF/PDF are separate. */
MtlxVerifiedBranchKernel mtlx_vf_ndf24_world_branch_kernel(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word);
MtlxVerifiedBranchEvent mtlx_vf_ndf24_world_branch_event(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word);

/* Checked outgoing geometry on near-unit finite inputs. kind: 0 reflection,
 * 1 regular transmission, 2 exact-eta=1 delta transmission. Reflection matches
 * the native unnormalized schedule (norm^2 error<=2^-11); transmission normalizes
 * once (checked norm^2 error<=2^-15). Exact decoded-real hemisphere and positive
 * decoded-cosine Snell-discriminant checks reject unresolved/TIR transmissions.
 * Generic-view geometry alone does not establish a BSDF/PDF energy contract. */
typedef struct { MtlxVerifiedBitVec3 direction;unsigned kind,valid; } MtlxVerifiedOutgoing;
typedef struct { MtlxVerifiedBitVec3 direction;uint32_t weight;unsigned kind,valid; } MtlxVerifiedScatter;
/* Local +Z frame, supplied half vector, regular eta!=1. Both candidate
 * geometries and the conditional power certificate are prepared before the
 * branch word. This does not supply a VNDF density or rejection fallback. */
typedef struct {
    MtlxVerifiedOutgoing reflection,transmission;
    uint32_t scalars[13];
    MtlxVerifiedAnisotropicSingleProposal power;
    MtlxVerifiedFloatBounds tir_discriminant;
    MtlxVerifiedFloatBounds reflection_norm,reflection_z;
    MtlxVerifiedFloatBounds transmission_raw_norm,transmission_norm,transmission_z,transmission_discriminant;
    unsigned transmission_absent;
    unsigned reflection_absent;
    unsigned valid;
} MtlxVerifiedAnisotropicLocalProposal;
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
/* Certified nonpositive real Snell discriminant only. Reflection and power
 * must still certify; threshold is exactly 2^24 before reading a word.
 * Numerical refraction failure alone never establishes absent transmission. */
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_tir_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
/* Preserve accepted regular proposals, otherwise try the certified TIR path. */
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_typed_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
/* Certified nonpositive ideal AND rounded reflection Z, finite near-unit
 * reflected candidate, accepted regular transmission and transmission-only
 * threshold. A failed reflection check is insufficient to select this route. */
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_horizon_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
/* Non-TIR refraction above the macro horizon: positive real raw norm and
 * nonnegative real/rounded transmitted Z certify zero transmission masking.
 * Reflection geometry/power and reflection-only threshold must still pass. */
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_transmission_horizon(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
/* Preserve accepted regular/TIR proposals, otherwise try certified horizon. */
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_routed_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
MtlxVerifiedScatter mtlx_vf_anisotropic_local_sample(MtlxVerifiedAnisotropicLocalProposal proposal,uint32_t word);
MtlxVerifiedOutgoing mtlx_vf_checked_outgoing(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half,uint32_t eta,unsigned reflected);
/* Same finite norm, positive eta, NdotV and HdotV preflight as checked outgoing. */
unsigned mtlx_vf_outgoing_inputs_valid(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half,uint32_t eta);
/* A genuine null certificate proves both horizon absence and TIR. It has no
 * outgoing direction and is distinct from an unresolved/invalid proposal. */
typedef struct {
    MtlxVerifiedFloatBounds reflection_norm,reflection_z,discriminant;
    unsigned valid;
} MtlxVerifiedAnisotropicLocalNull;
typedef struct {
    MtlxVerifiedAnisotropicLocalProposal proposal;
    MtlxVerifiedAnisotropicLocalNull null_certificate;
    MtlxVerifiedScatter matched;
    unsigned index_matched;
    unsigned terminated,valid;
} MtlxVerifiedAnisotropicLocalPrepared;
typedef struct {
    MtlxVerifiedScatter scatter;
    uint32_t mass;
    unsigned atomic;
    unsigned terminated,valid;
} MtlxVerifiedAnisotropicLocalEvent;
MtlxVerifiedAnisotropicLocalNull mtlx_vf_anisotropic_local_null(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
MtlxVerifiedAnisotropicLocalPrepared mtlx_vf_anisotropic_local_prepare(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
/* Exact eta=1 first: unit-mass, unit-weight straight-through atom from -view.
 * Roughness and half data are not evaluated in that case. Other eta values
 * preserve local_prepare. No extra normalization or hemisphere test. */
MtlxVerifiedAnisotropicLocalPrepared mtlx_vf_anisotropic_local_prepare_indexed(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
/* Preserve indexed scatter/null/atom preparations; otherwise try the certified
 * non-TIR transmission-horizon route. Explicit failures remain invalid. */
MtlxVerifiedAnisotropicLocalPrepared mtlx_vf_anisotropic_local_prepare_extended(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half);
MtlxVerifiedAnisotropicLocalEvent mtlx_vf_anisotropic_local_event(MtlxVerifiedAnisotropicLocalPrepared prepared,uint32_t word);
/* Normal-view world sampling/event/geometry composition. Failed geometry rejects
 * without increasing event power. Requires a genuine factory factor for the
 * integrated energy contract; this is not a renderer fallback or full BSDF/PDF. */
MtlxVerifiedScatter mtlx_vf_ndf24_world_scatter(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word);

/* Checked regular-transmission reconstruction and base-white evaluator.
 * eta=1 belongs to the delta path. No heuristic eta-gap/hraw cutoff is treated
 * as a proof: unresolved/nonfinite interval programs reject explicitly. Scalar
 * slots: alpha,eta,nv,nl,nh,oh,ih,hraw^2,clipped nh^2,mixture,glass weight.
 * Color/layer and multiscatter-table factors are not part of this evaluator. */
typedef struct { MtlxVerifiedBitVec3 half;uint32_t raw_norm,jacobian;MtlxVerifiedFloatBounds jacobian_bounds;unsigned valid; } MtlxVerifiedTransmissionGeometry;
typedef struct { uint32_t pdf,bsdf,power,jacobian;MtlxVerifiedFloatBounds pdf_bounds,bsdf_bounds,power_bounds;unsigned valid; } MtlxVerifiedTransmissionScalars;
typedef struct { MtlxVerifiedTransmissionGeometry geometry;MtlxVerifiedTransmissionScalars scalar;unsigned valid; } MtlxVerifiedTransmissionEval;
typedef struct { uint32_t bsdf,power;unsigned valid; } MtlxVerifiedBsdfPowerPacket;
typedef struct { MtlxVerifiedScatter scatter;uint32_t pdf,cosine;MtlxVerifiedBsdfPowerPacket pair;unsigned valid; } MtlxVerifiedWorldTransmissionPacket;
MtlxVerifiedTransmissionGeometry mtlx_vf_transmission_geometry(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta);
MtlxVerifiedTransmissionScalars mtlx_vf_transmission_scalars(const uint32_t values[11]);
MtlxVerifiedTransmissionEval mtlx_vf_transmission_eval(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta,uint32_t mixture,uint32_t glass);
/* Exact converted power and rounded throughput are both enclosed below budget.
 * radiance=1 uses eta^2 conversion; radiance=0 uses the power convention. */
MtlxVerifiedBsdfPowerPacket mtlx_vf_bsdf_power_packet(uint32_t budget,uint32_t pdf,uint32_t cosine,uint32_t eta,unsigned radiance);
/* Regular normal-view NDF transmission only; reflection/delta are separate.
 * Uses the exact finite transmission branch probability in the proposal PDF. */
MtlxVerifiedWorldTransmissionPacket mtlx_vf_ndf24_world_transmission_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance);

/* Checked normal-view reflection and typed routing. A surface packet density
 * is a solid-angle PDF when atomic=0, or finite branch probability mass conditional on the sampled half when
 * atomic=1. Atomic coefficient is conditional, with cosine=1; no regular
 * Jacobian is evaluated for eta=1 delta transmission. Reflection is unchanged
 * by transport mode; eta^2 radiance conversion applies to regular transmission. */
typedef MtlxVerifiedTransmissionGeometry MtlxVerifiedReflectionGeometry;
typedef MtlxVerifiedWorldTransmissionPacket MtlxVerifiedWorldReflectionPacket;
typedef struct { MtlxVerifiedScatter scatter;uint32_t density,cosine,coefficient,power;unsigned atomic,valid; } MtlxVerifiedWorldSurfacePacket;
MtlxVerifiedReflectionGeometry mtlx_vf_reflection_geometry(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha);
MtlxVerifiedWorldReflectionPacket mtlx_vf_ndf24_world_reflection_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word);
MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_world_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance);

/* Exact eta=1 only: deterministic arbitrary-view straight-through atom with
 * MARGINAL mass one. No normalization, hemisphere test, RNG, or Jacobian.
 * Requires finite near-unit normal/view and finite nonnegative factor; caps
 * factor at one. Separate from the sampled conditional-mass router above. */
MtlxVerifiedWorldSurfacePacket mtlx_vf_index_matched_packet(uint32_t factor,MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view);

/* Corrected early eta=1 routing, independent of all three words and mode.
 * Other eta values retain the checked conditional normal-view route. */
MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_index_aware_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance);

/* Two fixed labelled atoms: mass is the MARGINAL branch word probability.
 * The R coefficient is mode invariant; atomic T uses eta^2 in radiance
 * mode. Zero budget is valid. Positive pairs require the checked enclosure.
 * Both nonempty branches are certified BEFORE reading the sampling word. */
typedef struct { MtlxVerifiedBranchKernel kernel;unsigned reflected;uint32_t mass;MtlxVerifiedBsdfPowerPacket pair;unsigned valid; } MtlxVerifiedAtomicBranchPacket;
MtlxVerifiedBsdfPowerPacket mtlx_vf_atomic_power_packet(uint32_t budget,uint32_t mass,uint32_t eta,unsigned radiance);
MtlxVerifiedAtomicBranchPacket mtlx_vf_atomic_branch_packet(uint32_t reflection,uint32_t transmission,uint32_t eta,uint32_t word,unsigned radiance);
MtlxVerifiedSharedAllocation mtlx_vf_collapsed_allocation(uint32_t factor,uint32_t alpha,uint32_t eta);
/* Canonical normal-incidence axis, NOT a replay of legacy Duff normalization.
 * Returns fixed +/-normal atoms, kind0 R/kind1 T, atomic=1, cosine=1. Requires
 * certified near-unit normal and rounded-slope collapse; eta=1 is separate.
 * No D or area Jacobian. Explicit rejection still requires renderer policy. */
MtlxVerifiedWorldSurfacePacket mtlx_vf_collapsed_world_surface(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t word,unsigned radiance);

/* Typed dispatcher: exact eta=1 unit atom, collapsed +/-normal R/T atoms,
 * otherwise the checked normal-view solid-angle path. Explicit failures are
 * retained; no zero-on-rejection renderer fallback is implied. */
MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_quantized_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance);

/* Scaled reconstruction uses eta*view+outgoing. Its real half/Jacobian equal
 * the original program for positive eta and nonzero raw norm. Real bounds use
 * exact singleton add/multiply; the new rounded result is enclosed separately.
 * This is a different finite schedule, not bit equality with the old path. */
MtlxVerifiedTransmissionGeometry mtlx_vf_scaled_transmission_geometry(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta);
MtlxVerifiedTransmissionEval mtlx_vf_scaled_transmission_eval(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta,uint32_t mixture,uint32_t glass);
MtlxVerifiedWorldTransmissionPacket mtlx_vf_ndf24_world_scaled_transmission_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance);

/* Retains accepted typed packets; on regular-transmission reconstruction
 * failure, attempts the proved scaled finite schedule. No eta-gap heuristic,
 * proposal change, resampling, or implicit absorption is introduced. */
MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_stable_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance);

#ifdef __cplusplus
}
#endif
#endif
