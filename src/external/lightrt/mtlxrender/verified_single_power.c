#include "verified_single_power.h"
#include "verified_float.h"
#include "verified_ranges.h"
#include "verified_anisotropic_single_program.h"
#ifdef __FAST_MATH__
#error "verified_single_power.c requires strict floating-point compilation"
#endif
static int finite_bits(uint32_t x){return (x&UINT32_C(0x7fffffff))<UINT32_C(0x7f800000);}
static int nonnegative(uint32_t x){return finite_bits(x) && (!(x&UINT32_C(0x80000000)) || (x&UINT32_C(0x7fffffff))==0);}
static int positive(uint32_t x){return finite_bits(x) && !(x&UINT32_C(0x80000000)) && x!=0;}
MtlxVerifiedAnisotropicSinglePower mtlx_vf_anisotropic_single_power(const uint32_t values[13]){
    MtlxVerifiedAnisotropicSinglePower out={0};
    if(!values || !mtlx_verified_float_supported())return out;
    for(unsigned i=0;i<13;++i)if(!finite_bits(values[i]))return out;
    if(!positive(values[0]) || values[0]>UINT32_C(0x3f800000) || !positive(values[1]) || values[1]>UINT32_C(0x3f800000) ||
       !positive(values[2]) || values[2]==UINT32_C(0x3f800000) || !nonnegative(values[3]) ||
       (values[3]&UINT32_C(0x7fffffff))>UINT32_C(0x3f800000) || !positive(values[6]))return out;
    uint32_t raw[5];mtlx_verified_anisotropic_power_sample(values,raw);
    if(!nonnegative(raw[0]) || (raw[0]&UINT32_C(0x7fffffff))>UINT32_C(0x3f800000) ||
       !nonnegative(raw[1]) || !nonnegative(raw[2]) || !nonnegative(raw[3]) || !nonnegative(raw[4]))return out;
    const uint32_t r=mtlx_vf_clamp_unit(raw[3]),t=mtlx_vf_clamp_unit(raw[4]);
    const MtlxVerifiedFloatBounds rb={r,r,1},tb={t,t,1};
    const MtlxVerifiedFloatBounds total=mtlx_vf_range_exact_add(rb,tb);
    if(!total.valid)return out;
    const uint32_t budget=mtlx_vf_clamp_unit(total.lower);
    MtlxVerifiedSharedAllocation allocation=mtlx_vf_allocate_shared_power(budget,r,t);
    if(!allocation.valid || (allocation.factor&UINT32_C(0x7fffffff))>UINT32_C(0x3f800000))return out;
    out.fresnel=raw[0];out.reflection_mask=raw[1];out.transmission_mask=raw[2];
    out.raw_reflection=raw[3];out.raw_transmission=raw[4];out.budget=budget;out.allocation=allocation;out.valid=1;return out;
}
MtlxVerifiedAnisotropicSingleProposal mtlx_vf_anisotropic_single_proposal(const uint32_t values[13]){
    MtlxVerifiedAnisotropicSingleProposal out={0};
    MtlxVerifiedAnisotropicSinglePower power=mtlx_vf_anisotropic_single_power(values);
    if(!power.valid)return out;
    MtlxVerifiedFloatBounds env[13];for(unsigned i=0;i<13;++i)env[i]=mtlx_vf_range_point(values[i]);
    MtlxVerifiedFloatBounds rb=mtlx_vf_range_hull(mtlx_verified_anisotropic_reflection_real_range(env),mtlx_vf_range_point(power.raw_reflection));
    MtlxVerifiedFloatBounds tb=mtlx_vf_range_hull(mtlx_verified_anisotropic_transmission_real_range(env),mtlx_vf_range_point(power.raw_transmission));
    if(!rb.valid || !tb.valid)return out;
    MtlxVerifiedFloatBounds rw=mtlx_vf_bound_sub(rb.upper,rb.lower),tw=mtlx_vf_bound_sub(tb.upper,tb.lower);
    if(!rw.valid || !tw.valid ||
       (!(rw.upper&UINT32_C(0x80000000)) && rw.upper>UINT32_C(0x37800000)) ||
       (!(tw.upper&UINT32_C(0x80000000)) && tw.upper>UINT32_C(0x37800000)))return out;
    MtlxVerifiedBranchKernel kernel=mtlx_vf_branch_kernel(power.allocation.reflection,power.allocation.transmission);
    if(!kernel.valid)return out;
    out.power=power;out.kernel=kernel;out.reflection_bounds=rb;out.transmission_bounds=tb;out.valid=1;return out;
}
MtlxVerifiedBranchEvent mtlx_vf_anisotropic_single_event(MtlxVerifiedAnisotropicSingleProposal proposal,uint32_t word){
    const MtlxVerifiedBranchEvent invalid={UINT32_C(0x7fc00000),0,0};
    if(proposal.valid!=1 || proposal.power.valid!=1)return invalid;
    MtlxVerifiedBranchEvent out=mtlx_vf_branch_event(proposal.kernel,word);
    if(out.valid)out.weight=mtlx_vf_clamp_unit(out.weight);
    return out;
}
