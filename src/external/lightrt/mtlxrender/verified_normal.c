#include "verified_normal.h"
#include "verified_normal_program.h"
#include "verified_normal_float_program.h"
#include "verified_float.h"
#include "verified_ndf_power_program.h"
#include "verified_ndf_density_program.h"

#ifdef __FAST_MATH__
#error "verified_normal.c requires strict floating-point compilation"
#endif

/* Exact encoding of index*2^-depth for 0<=index<=2^depth, depth<=23.
 * Nonzero inputs are normal, with at most 24 significant binary digits. */
static uint32_t dyadic_point(uint32_t index,unsigned depth) {
    if(index==0)return 0;
    unsigned leading=0;
    for(uint32_t n=index;n>1;n>>=1)++leading;
    return ((127u+leading-depth)<<23) |
        ((index<<(23u-leading))&UINT32_C(0x007fffff));
}

uint32_t mtlx_vf_unit_dyadic_bits(uint32_t index,unsigned depth) {
    if(depth>23 || index>(UINT32_C(1)<<depth))return UINT32_C(0x7fc00000);
    return dyadic_point(index,depth);
}

uint32_t mtlx_vf_pcg_uniform_bits(uint32_t word) {
    return dyadic_point(word>>8,24);
}

uint32_t mtlx_vf_normal_density_f32(uint32_t t,uint32_t alpha,uint32_t eta) {
    if((t&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000) ||
       (alpha&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000) ||
       (eta&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000) ||
       !mtlx_verified_float_supported())return UINT32_C(0x7fc00000);
    const uint32_t env[3]={t,alpha,eta};
    return mtlx_verified_normal_f32_program(env);
}

uint32_t mtlx_vf_normal_unit_power_f32(uint32_t t,uint32_t alpha,uint32_t eta) {
    if((t&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000) ||
       (alpha&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000) ||
       (eta&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000) ||
       !mtlx_verified_float_supported())return UINT32_C(0x7fc00000);
    const uint32_t env[3]={t,alpha,eta};
    return mtlx_vf_clamp_unit(mtlx_verified_normal_power_program(env));
}

uint32_t mtlx_vf_ggx_ndf_cos(uint32_t alpha,uint32_t uniform) {
    const uint32_t um=uniform&UINT32_C(0x7fffffff);
    if(alpha==0 || alpha>UINT32_C(0x3f800000) || um>UINT32_C(0x3f800000) ||
       ((uniform&UINT32_C(0x80000000)) && um!=0) || !mtlx_verified_float_supported())
        return UINT32_C(0x7fc00000);
    const uint32_t one=UINT32_C(0x3f800000);
    const uint32_t numerator=mtlx_vf_sub(one,uniform).bits;
    const uint32_t alpha2=mtlx_vf_mul(alpha,alpha).bits;
    const uint32_t slope=mtlx_vf_sub(alpha2,one).bits;
    const uint32_t product=mtlx_vf_mul(slope,uniform).bits;
    const uint32_t denominator=mtlx_vf_add(one,product).bits;
    return mtlx_vf_sqrt(mtlx_vf_div(numerator,denominator).bits).bits;
}

uint32_t mtlx_vf_ndf_sample_unit_power(uint32_t alpha,uint32_t eta,uint32_t uniform) {
    const uint32_t um=uniform&UINT32_C(0x7fffffff);
    if(alpha==0 || alpha>UINT32_C(0x3f800000) || eta==0 || eta>UINT32_C(0x7f7fffff) ||
       um>UINT32_C(0x3f800000) || ((uniform&UINT32_C(0x80000000)) && um!=0) ||
       !mtlx_verified_float_supported())return UINT32_C(0x7fc00000);
    const uint32_t cosine=mtlx_vf_ggx_ndf_cos(alpha,uniform);
    const uint32_t env[4]={0,alpha,eta,cosine};
    return mtlx_vf_clamp_unit(mtlx_verified_sampled_cos_power(env));
}

static int supported_domain(uint32_t alpha,uint32_t eta,unsigned depth) {
    return alpha>0 && alpha<=UINT32_C(0x3f800000) && eta>0 &&
        eta<=UINT32_C(0x7f7fffff) && depth<=23 && mtlx_verified_float_supported();
}

static MtlxVerifiedNormalCell normal_cell_box(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t i,
                                                uint32_t lower,uint32_t upper) {
    MtlxVerifiedNormalCell result={{0,0,0},0};
    const uint64_t cap=(UINT64_C(2)*i+1)<<(48u-2u*depth);
    MtlxVerifiedFloatBounds env[3]={
        {lower,upper,1},
        {alpha,alpha,1},{eta,eta,1}};
    MtlxVerifiedFloatBounds density=mtlx_verified_normal_density(env);
    MtlxVerifiedFixedBounds cell={0,cap,1};
    if(density.valid==1) {
        MtlxVerifiedFixedBounds lower=mtlx_vf_fixed_scaled(density.lower,depth,cap);
        MtlxVerifiedFixedBounds upper=mtlx_vf_fixed_scaled(density.upper,depth,cap);
        if(lower.valid!=1 || upper.valid!=1 || lower.lower>upper.upper)return result;
        cell.lower=lower.lower;cell.upper=upper.upper;
    } else result.fallback=1;
    result.bounds=cell;
    return result;
}

static MtlxVerifiedNormalCell normal_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t i) {
    return normal_cell_box(alpha,eta,depth,i,dyadic_point(i,depth),dyadic_point(i+1,depth));
}

static MtlxVerifiedFloatBounds ndf_uniform_cell(uint32_t a,uint32_t b) {
    const MtlxVerifiedFloatBounds wide={0,UINT32_C(0x3f800000),1};
    const MtlxVerifiedFloatBounds t={a,b,1};
    MtlxVerifiedFloatBounds square=mtlx_vf_range_mul(t,t);
    MtlxVerifiedFloatBounds one=mtlx_vf_range_point(UINT32_C(0x3f800000));
    MtlxVerifiedFloatBounds uniform=mtlx_vf_range_sub(one,square);
    if(uniform.valid!=1)return wide;
    uniform.lower=mtlx_vf_clamp_unit(uniform.lower);
    uniform.upper=mtlx_vf_clamp_unit(uniform.upper);
    return uniform;
}

static MtlxVerifiedFloatBounds ndf_uniform24_cell(uint32_t a,uint32_t b) {
    const MtlxVerifiedFloatBounds base=ndf_uniform_cell(a,b);
    const MtlxVerifiedFloatBounds point={base.lower,base.lower,1};
    const MtlxVerifiedFloatBounds step={UINT32_C(0x33800000),UINT32_C(0x33800000),1};
    const MtlxVerifiedFloatBounds lower=mtlx_vf_range_sub(point,step);
    if(lower.valid!=1) {
        const MtlxVerifiedFloatBounds wide={0,UINT32_C(0x3f800000),1};return wide;
    }
    MtlxVerifiedFloatBounds out={mtlx_vf_clamp_unit(lower.lower),base.upper,1};return out;
}

static MtlxVerifiedFloatBounds ndf_cosine_range(uint32_t alpha,MtlxVerifiedFloatBounds uniform) {
    MtlxVerifiedFloatBounds one=mtlx_vf_range_point(UINT32_C(0x3f800000));
    MtlxVerifiedFloatBounds a=mtlx_vf_range_point(alpha);
    MtlxVerifiedFloatBounds numerator=mtlx_vf_range_sub(one,uniform);
    MtlxVerifiedFloatBounds slope=mtlx_vf_range_sub(mtlx_vf_range_mul(a,a),one);
    MtlxVerifiedFloatBounds denominator=mtlx_vf_range_add(one,mtlx_vf_range_mul(slope,uniform));
    return mtlx_vf_range_sqrt(mtlx_vf_range_div(numerator,denominator));
}

MtlxVerifiedFloatBounds mtlx_vf_ndf24_cos_cell(uint32_t alpha,unsigned depth,uint32_t index) {
    const MtlxVerifiedFloatBounds invalid={UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),0};
    if(!supported_domain(alpha,UINT32_C(0x3f800000),depth) || index>=(UINT32_C(1)<<depth))return invalid;
    return ndf_cosine_range(alpha,ndf_uniform24_cell(dyadic_point(index,depth),dyadic_point(index+1,depth)));
}

static MtlxVerifiedNormalCell ndf_sample_cell_law(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t i,int grid24) {
    MtlxVerifiedNormalCell result={{0,0,0},0};
    const uint32_t a=dyadic_point(i,depth),b=dyadic_point(i+1,depth);
    const uint64_t cap=(UINT64_C(2)*i+1)<<(48u-2u*depth);
    MtlxVerifiedFixedBounds cell={0,cap,1};
    MtlxVerifiedFloatBounds cosine=ndf_cosine_range(alpha,grid24?ndf_uniform24_cell(a,b):ndf_uniform_cell(a,b));
    MtlxVerifiedFloatBounds env[4]={{a,b,1},{alpha,alpha,1},{eta,eta,1},cosine};
    MtlxVerifiedFloatBounds density=mtlx_verified_sampled_cos_density(env);
    if(cosine.valid==1 && density.valid==1) {
        MtlxVerifiedFixedBounds lower=mtlx_vf_fixed_scaled(density.lower,depth,cap);
        MtlxVerifiedFixedBounds upper=mtlx_vf_fixed_scaled(density.upper,depth,cap);
        if(lower.valid!=1 || upper.valid!=1 || lower.lower>upper.upper)return result;
        cell.lower=lower.lower;cell.upper=upper.upper;
    } else result.fallback=1;
    result.bounds=cell;return result;
}

MtlxVerifiedNormalCell mtlx_vf_ndf_sample_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index) {
    const MtlxVerifiedNormalCell invalid={{0,0,0},0};
    if(!supported_domain(alpha,eta,depth) || index>=(UINT32_C(1)<<depth))return invalid;
    return ndf_sample_cell_law(alpha,eta,depth,index,0);
}

static MtlxVerifiedNormalAlbedo ndf_sample_albedo_law(uint32_t alpha,uint32_t eta,unsigned depth,int grid24) {
    MtlxVerifiedNormalAlbedo result={{0,0,0},0,0,0,UINT32_C(0x7fc00000)};
    if(!supported_domain(alpha,eta,depth))return result;
    const uint32_t count=UINT32_C(1)<<depth;
    MtlxVerifiedFixedBounds sum={0,0,1};
    for(uint32_t i=0;i<count;++i) {
        MtlxVerifiedNormalCell cell=ndf_sample_cell_law(alpha,eta,depth,i,grid24);
        result.fallback_cells+=cell.fallback;
        sum=mtlx_vf_fixed_add(sum,cell.bounds);
        if(sum.valid!=1 || sum.upper>MTLX_VERIFIED_FIXED_SCALE)return result;
    }
    result.bounds=sum;result.cells=count;
    result.certified=(unsigned)mtlx_vf_fixed_width_accepted(sum);
    result.upper_bits=mtlx_vf_fixed_upper(sum.upper);
    return result;
}

MtlxVerifiedNormalAlbedo mtlx_vf_ndf_sample_albedo(uint32_t alpha,uint32_t eta,unsigned depth) {
    return ndf_sample_albedo_law(alpha,eta,depth,0);
}

MtlxVerifiedNormalCell mtlx_vf_ndf24_sample_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index) {
    const MtlxVerifiedNormalCell invalid={{0,0,0},0};
    if(!supported_domain(alpha,eta,depth) || index>=(UINT32_C(1)<<depth))return invalid;
    return ndf_sample_cell_law(alpha,eta,depth,index,1);
}

MtlxVerifiedNormalAlbedo mtlx_vf_ndf24_sample_albedo(uint32_t alpha,uint32_t eta,unsigned depth) {
    return ndf_sample_albedo_law(alpha,eta,depth,1);
}

uint32_t mtlx_vf_ndf24_sample_unit_power(uint32_t alpha,uint32_t eta,uint32_t word) {
    return mtlx_vf_ndf_sample_unit_power(alpha,eta,mtlx_vf_pcg_uniform_bits(word));
}

MtlxVerifiedNormalCell mtlx_vf_normal_cell_expanded(uint32_t alpha,uint32_t eta,unsigned depth,
                                                 uint32_t index,uint32_t lower,uint32_t upper) {
    const MtlxVerifiedNormalCell invalid={{0,0,0},0};
    if(!supported_domain(alpha,eta,depth) || index>=(UINT32_C(1)<<depth) ||
       lower>dyadic_point(index,depth) || upper<dyadic_point(index+1,depth) ||
       upper>UINT32_C(0x3f800000))return invalid;
    return normal_cell_box(alpha,eta,depth,index,lower,upper);
}

MtlxVerifiedNormalCell mtlx_vf_normal_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index) {
    const MtlxVerifiedNormalCell invalid={{0,0,0},0};
    if(!supported_domain(alpha,eta,depth) || index>=(UINT32_C(1)<<depth))return invalid;
    return normal_cell(alpha,eta,depth,index);
}

MtlxVerifiedNormalAlbedo mtlx_vf_normal_albedo(uint32_t alpha,uint32_t eta,unsigned depth) {
    MtlxVerifiedNormalAlbedo result={{0,0,0},0,0,0,UINT32_C(0x7fc00000)};
    if(!supported_domain(alpha,eta,depth))return result;
    const uint32_t count=UINT32_C(1)<<depth;
    MtlxVerifiedFixedBounds sum={0,0,1};
    for(uint32_t i=0;i<count;++i) {
        MtlxVerifiedNormalCell cell=normal_cell(alpha,eta,depth,i);
        result.fallback_cells+=cell.fallback;
        sum=mtlx_vf_fixed_add(sum,cell.bounds);
        if(sum.valid!=1 || sum.upper>MTLX_VERIFIED_FIXED_SCALE)return result;
    }
    result.bounds=sum;result.cells=count;
    result.certified=(unsigned)mtlx_vf_fixed_width_accepted(sum);
    result.upper_bits=mtlx_vf_fixed_upper(sum.upper);
    return result;
}

static MtlxVerifiedFloatBounds invalid_factor(void) {
    MtlxVerifiedFloatBounds r={UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),0};return r;
}

MtlxVerifiedFloatBounds mtlx_vf_joint_factor(uint32_t upper,uint32_t reserve) {
    const uint32_t rm=reserve&UINT32_C(0x7fffffff);
    if(upper==0 || upper>=UINT32_C(0x7f800000) || rm>=UINT32_C(0x3f800000) ||
       ((reserve&UINT32_C(0x80000000)) && rm!=0))return invalid_factor();
    MtlxVerifiedFloatBounds one=mtlx_vf_range_point(UINT32_C(0x3f800000));
    MtlxVerifiedFloatBounds r=mtlx_vf_range_point(reserve);
    MtlxVerifiedFloatBounds numerator=mtlx_vf_range_sub(one,r);
    MtlxVerifiedFloatBounds denominator=mtlx_vf_range_point(upper);
    MtlxVerifiedFloatBounds out=mtlx_vf_range_div(numerator,denominator);
    if(out.valid!=1 || ((out.lower&UINT32_C(0x80000000)) &&
       (out.lower&UINT32_C(0x7fffffff))!=0))return invalid_factor();
    return out;
}

MtlxVerifiedFloatBounds mtlx_vf_normal_factor(MtlxVerifiedNormalAlbedo certificate,uint32_t reserve) {
    if(certificate.certified!=1 || certificate.cells==0 ||
       !mtlx_vf_fixed_width_accepted(certificate.bounds) ||
       certificate.bounds.upper>MTLX_VERIFIED_FIXED_SCALE ||
       certificate.upper_bits!=mtlx_vf_fixed_upper(certificate.bounds.upper))return invalid_factor();
    return mtlx_vf_joint_factor(certificate.upper_bits,reserve);
}

static int allocation_nonnegative(uint32_t bits) {
    return bits<UINT32_C(0x7f800000) || bits==UINT32_C(0x80000000);
}
MtlxVerifiedSharedAllocation mtlx_vf_allocate_shared_power(uint32_t budget,uint32_t reflection,uint32_t transmission) {
    const MtlxVerifiedSharedAllocation invalid={0,0,0,0};
    MtlxVerifiedFloatBounds p=mtlx_vf_range_point(budget),r=mtlx_vf_range_point(reflection),t=mtlx_vf_range_point(transmission);
    if(!p.valid || !r.valid || !t.valid || !allocation_nonnegative(budget) ||
       !allocation_nonnegative(reflection) || !allocation_nonnegative(transmission))return invalid;
    if((reflection&UINT32_C(0x7fffffff))==0 && (transmission&UINT32_C(0x7fffffff))==0) {
        const MtlxVerifiedSharedAllocation zero={0,0,0,1};return zero;
    }
    MtlxVerifiedFloatBounds total=mtlx_vf_range_add(r,t);
    MtlxVerifiedFloatBounds ratio=mtlx_vf_range_div(p,total);
    if(!ratio.valid)return invalid;
    MtlxVerifiedFloatBounds common=mtlx_vf_range_point(ratio.lower);
    MtlxVerifiedFloatBounds rout=(reflection&UINT32_C(0x7fffffff))==0 ? mtlx_vf_range_point(0) : mtlx_vf_range_mul(common,r);
    MtlxVerifiedFloatBounds tout=(transmission&UINT32_C(0x7fffffff))==0 ? mtlx_vf_range_point(0) : mtlx_vf_range_mul(common,t);
    if(!rout.valid || !tout.valid || !allocation_nonnegative(ratio.lower) ||
       !allocation_nonnegative(rout.lower) || !allocation_nonnegative(tout.lower))return invalid;
    const MtlxVerifiedFloatBounds rp={rout.lower,rout.lower,1},tp={tout.lower,tout.lower,1};
    MtlxVerifiedFloatBounds sum=mtlx_vf_range_add(rp,tp);
    if(!sum.valid || (sum.upper&UINT32_C(0x7fffffff))>(budget&UINT32_C(0x7fffffff)))return invalid;
    const MtlxVerifiedSharedAllocation out={ratio.lower,rout.lower,tout.lower,1};return out;
}

/* Exact ceil(2^24*x), x a nonnegative finite unit encoding. */
static uint32_t branch_unit_ceil24(uint32_t bits) {
    const uint32_t magnitude=bits&UINT32_C(0x7fffffff);
    if(magnitude==0)return 0;
    const unsigned exponent=magnitude>>23;
    const uint32_t significand=(magnitude&UINT32_C(0x7fffff))|(exponent ? UINT32_C(0x800000) : 0);
    if(exponent>=126)return significand<<(exponent-126);
    const unsigned shift=exponent ? 126-exponent : 125;
    if(shift>=32)return 1;
    return (uint32_t)(((uint64_t)significand+((UINT64_C(1)<<shift)-1))>>shift);
}
MtlxVerifiedBranchKernel mtlx_vf_branch_kernel(uint32_t reflection,uint32_t transmission) {
    const MtlxVerifiedBranchKernel invalid={0,0,0,0};
    MtlxVerifiedFloatBounds r=mtlx_vf_range_point(reflection),t=mtlx_vf_range_point(transmission);
    if(!r.valid || !t.valid || !allocation_nonnegative(reflection) || !allocation_nonnegative(transmission))return invalid;
    if((reflection&UINT32_C(0x7fffffff))==0) {
        const MtlxVerifiedBranchKernel out={0,0,transmission,1};return out;
    }
    if((transmission&UINT32_C(0x7fffffff))==0) {
        const MtlxVerifiedBranchKernel out={UINT32_C(16777216),reflection,0,1};return out;
    }
    MtlxVerifiedFloatBounds total=mtlx_vf_range_add(r,t),ratio=mtlx_vf_range_div(r,total);
    if(!ratio.valid)return invalid;
    uint32_t k=branch_unit_ceil24(mtlx_vf_clamp_unit(ratio.lower));
    if(k==0)k=1;
    if(k>=UINT32_C(16777216))k=UINT32_C(16777215);
    MtlxVerifiedFloatBounds p=mtlx_vf_range_point(mtlx_vf_pcg_uniform_bits(k<<8));
    MtlxVerifiedFloatBounds q=mtlx_vf_range_point(mtlx_vf_pcg_uniform_bits((UINT32_C(16777216)-k)<<8));
    MtlxVerifiedFloatBounds wr=mtlx_vf_range_div(r,p),wt=mtlx_vf_range_div(t,q);
    if(!wr.valid || !wt.valid || !allocation_nonnegative(wr.lower) || !allocation_nonnegative(wt.lower))return invalid;
    const MtlxVerifiedBranchKernel out={k,wr.lower,wt.lower,1};return out;
}
MtlxVerifiedBranchEvent mtlx_vf_branch_event(MtlxVerifiedBranchKernel kernel,uint32_t word) {
    const MtlxVerifiedBranchEvent invalid={UINT32_C(0x7fc00000),0,0};
    if(kernel.valid!=1 || kernel.threshold>UINT32_C(16777216) ||
       !mtlx_verified_float_supported() || !allocation_nonnegative(kernel.reflection_weight) ||
       !allocation_nonnegative(kernel.transmission_weight))return invalid;
    const unsigned reflected=(word>>8)<kernel.threshold;
    const MtlxVerifiedBranchEvent out={reflected ? kernel.reflection_weight : kernel.transmission_weight,reflected,1};return out;
}

unsigned mtlx_vf_ndf_alpha_collapsed(uint32_t alpha) {
    const uint32_t magnitude=alpha&UINT32_C(0x7fffffff);
    if(!mtlx_verified_float_supported() || magnitude>UINT32_C(0x3f800000) ||
       ((alpha&UINT32_C(0x80000000)) && magnitude!=0))return 0;
    return mtlx_vf_sub(mtlx_vf_mul(alpha,alpha).bits,UINT32_C(0x3f800000)).bits==UINT32_C(0xbf800000);
}
