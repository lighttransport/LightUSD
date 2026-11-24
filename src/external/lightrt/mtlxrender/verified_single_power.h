#ifndef MTLXRENDER_VERIFIED_SINGLE_POWER_H_
#define MTLXRENDER_VERIFIED_SINGLE_POWER_H_
#include "verified_normal.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Prepared local slots: ax,ay,eta,VdotH,Vx,Vy,Vz,Rx,Ry,Rz,Tx,Ty,Tz.
 * Outgoing Z is positive only in its accepted hemisphere (T uses the opposite
 * normal). This component returns conditional power, NOT directions or a PDF.
 * Finite positive roughness<=1, eta>0!=1, VdotH in [0,1], Vz>0 are required.
 * Failed numerical preparation remains explicit. No renderer fallback is implied. */
typedef struct {
    uint32_t fresnel,reflection_mask,transmission_mask,raw_reflection,raw_transmission,budget;
    MtlxVerifiedSharedAllocation allocation;
    unsigned valid;
} MtlxVerifiedAnisotropicSinglePower;
typedef struct {
    MtlxVerifiedAnisotropicSinglePower power;
    MtlxVerifiedBranchKernel kernel;
    MtlxVerifiedFloatBounds reflection_bounds,transmission_bounds;
    unsigned valid;
} MtlxVerifiedAnisotropicSingleProposal;
MtlxVerifiedAnisotropicSinglePower mtlx_vf_anisotropic_single_power(const uint32_t values[13]);
MtlxVerifiedAnisotropicSingleProposal mtlx_vf_anisotropic_single_proposal(const uint32_t values[13]);
/* Proposal preparation also requires real/rounded raw branch enclosures with
 * width <=2^-16 each; these are local errors, NOT an albedo certificate.
 * Preparation is independent of word. With a genuine proposal, the protected
 * event never increases its kernel weight and stays in [0,1] for EVERY word.
 * No marginal half-vector mass, area PDF or radiance coefficient is returned. */
MtlxVerifiedBranchEvent mtlx_vf_anisotropic_single_event(MtlxVerifiedAnisotropicSingleProposal proposal,uint32_t word);
#ifdef __cplusplus
}
#endif
#endif
