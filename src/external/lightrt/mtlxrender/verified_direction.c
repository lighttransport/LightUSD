#include "verified_direction.h"
#include "verified_normal.h"
#include "verified_float.h"
#include "verified_math.h"
#include "verified_ranges.h"
#include "verified_ndf_power_program.h"
#include "verified_branch_power_program.h"
#include "verified_world_program.h"
#include "verified_outgoing_program.h"
#include "verified_transmission_program.h"
#include "verified_surface_program.h"
#include "verified_ndf_density_program.h"
#include "verified_scaled_transmission_program.h"

#ifdef __FAST_MATH__
#error "verified_direction.c requires strict floating-point compilation"
#endif

static MtlxVerifiedNdfDirection invalid_direction(void) {
    const uint32_t nan=UINT32_C(0x7fc00000);
    MtlxVerifiedNdfDirection out={nan,nan,nan,nan,0};return out;
}
static int finite_bits(uint32_t x) {return (x&UINT32_C(0x7fffffff))<UINT32_C(0x7f800000);}

MtlxVerifiedNdfDirection mtlx_vf_ndf24_half(uint32_t alpha,uint32_t azimuth_word,uint32_t radial_word) {
    if(alpha==0 || alpha>UINT32_C(0x3f800000) || !mtlx_verified_float_supported())return invalid_direction();
    const uint32_t one=UINT32_C(0x3f800000);
    const uint32_t cosine=mtlx_vf_ggx_ndf_cos(alpha,mtlx_vf_pcg_uniform_bits(radial_word));
    const uint32_t phase=mtlx_vf_mul(UINT32_C(0x40c90fdb),mtlx_vf_pcg_uniform_bits(azimuth_word)).bits;
    MtlxVerifiedSinCos trig;
    if(!mtlx_verified_sincos(phase,&trig))return invalid_direction();
    const uint32_t square=mtlx_vf_mul(cosine,cosine).bits;
    const uint32_t radius=mtlx_vf_sqrt(mtlx_vf_sub(one,square).bits).bits;
    const uint32_t x=mtlx_vf_mul(radius,trig.cos_bits).bits;
    const uint32_t y=mtlx_vf_mul(radius,trig.sin_bits).bits;
    const uint32_t xx=mtlx_vf_mul(x,x).bits,yy=mtlx_vf_mul(y,y).bits,zz=mtlx_vf_mul(cosine,cosine).bits;
    const uint32_t sum=mtlx_vf_add(mtlx_vf_add(xx,yy).bits,zz).bits;
    const uint32_t length=mtlx_vf_sqrt(sum).bits;
    if(!finite_bits(length) || length==0 || (length&UINT32_C(0x80000000)))return invalid_direction();
    const uint32_t inverse=mtlx_vf_div(one,length).bits;
    MtlxVerifiedNdfDirection out={mtlx_vf_mul(x,inverse).bits,mtlx_vf_mul(y,inverse).bits,
        mtlx_vf_mul(cosine,inverse).bits,0,1};
    if(!finite_bits(out.x) || !finite_bits(out.y) || !finite_bits(out.z))return invalid_direction();
    out.cosine=mtlx_vf_clamp_unit(out.z);
    return out;
}

MtlxVerifiedFloatBounds mtlx_vf_ndf_direction_cos_box(MtlxVerifiedFloatBounds scalar_box) {
    const MtlxVerifiedFloatBounds unit={0,UINT32_C(0x3f800000),1};
    const MtlxVerifiedFloatBounds invalid={UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),0};
    if(!mtlx_verified_float_supported())return invalid;
    if(mtlx_vf_range_checked(scalar_box).valid!=1)return unit;
    const MtlxVerifiedFloatBounds step=mtlx_vf_range_point(UINT32_C(0x37000000));
    const MtlxVerifiedFloatBounds lower=mtlx_vf_range_sub(mtlx_vf_range_point(scalar_box.lower),step);
    const MtlxVerifiedFloatBounds upper=mtlx_vf_range_add(mtlx_vf_range_point(scalar_box.upper),step);
    if(lower.valid!=1 || upper.valid!=1)return unit;
    MtlxVerifiedFloatBounds out={mtlx_vf_clamp_unit(lower.lower),mtlx_vf_clamp_unit(upper.upper),1};return out;
}

uint32_t mtlx_vf_ndf24_geometry_unit_power(uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word) {
    if(eta==0 || eta>UINT32_C(0x7f7fffff))return UINT32_C(0x7fc00000);
    MtlxVerifiedNdfDirection direction=mtlx_vf_ndf24_half(alpha,azimuth_word,radial_word);
    if(direction.valid!=1)return UINT32_C(0x7fc00000);
    const uint32_t env[4]={0,alpha,eta,direction.cosine};
    return mtlx_vf_clamp_unit(mtlx_verified_sampled_cos_power(env));
}

static int geometry_domain(uint32_t alpha,uint32_t eta,unsigned depth) {
    return alpha>0 && alpha<=UINT32_C(0x3f800000) && eta>0 &&
        eta<=UINT32_C(0x7f7fffff) && depth<=23 && mtlx_verified_float_supported();
}

static MtlxVerifiedNormalCell geometry_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index) {
    MtlxVerifiedNormalCell result={{0,0,0},0};
    const uint32_t a=mtlx_vf_unit_dyadic_bits(index,depth),b=mtlx_vf_unit_dyadic_bits(index+1,depth);
    const uint64_t cap=(UINT64_C(2)*index+1)<<(48u-2u*depth);
    MtlxVerifiedFixedBounds cell={0,cap,1};
    MtlxVerifiedFloatBounds scalar=mtlx_vf_ndf24_cos_cell(alpha,depth,index);
    if(scalar.valid==1) {
        MtlxVerifiedFloatBounds cosine=mtlx_vf_ndf_direction_cos_box(scalar);
        const MtlxVerifiedFloatBounds env[4]={{a,b,1},{alpha,alpha,1},{eta,eta,1},cosine};
        MtlxVerifiedFloatBounds density=mtlx_verified_sampled_cos_density(env);
        if(cosine.valid==1 && density.valid==1) {
            MtlxVerifiedFixedBounds lower=mtlx_vf_fixed_scaled(density.lower,depth,cap);
            MtlxVerifiedFixedBounds upper=mtlx_vf_fixed_scaled(density.upper,depth,cap);
            if(lower.valid!=1 || upper.valid!=1 || lower.lower>upper.upper)return result;
            cell.lower=lower.lower;cell.upper=upper.upper;
        } else result.fallback=1;
    } else result.fallback=1;
    result.bounds=cell;return result;
}

MtlxVerifiedNormalCell mtlx_vf_ndf24_geometry_cell(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t index) {
    const MtlxVerifiedNormalCell invalid={{0,0,0},0};
    if(!geometry_domain(alpha,eta,depth) || index>=(UINT32_C(1)<<depth))return invalid;
    return geometry_cell(alpha,eta,depth,index);
}

MtlxVerifiedNormalAlbedo mtlx_vf_ndf24_geometry_albedo(uint32_t alpha,uint32_t eta,unsigned depth) {
    MtlxVerifiedNormalAlbedo result={{0,0,0},0,0,0,UINT32_C(0x7fc00000)};
    if(!geometry_domain(alpha,eta,depth))return result;
    const uint32_t count=UINT32_C(1)<<depth;
    MtlxVerifiedFixedBounds sum={0,0,1};
    for(uint32_t i=0;i<count;++i) {
        MtlxVerifiedNormalCell cell=geometry_cell(alpha,eta,depth,i);
        result.fallback_cells+=cell.fallback;
        sum=mtlx_vf_fixed_add(sum,cell.bounds);
        if(sum.valid!=1 || sum.upper>MTLX_VERIFIED_FIXED_SCALE)return result;
    }
    result.bounds=sum;result.cells=count;
    result.certified=(unsigned)mtlx_vf_fixed_width_accepted(sum);
    result.upper_bits=mtlx_vf_fixed_upper(sum.upper);
    return result;
}

MtlxVerifiedFloatBounds mtlx_vf_ndf24_geometry_factor(uint32_t alpha,uint32_t eta,unsigned depth,uint32_t reserve) {
    return mtlx_vf_normal_factor(mtlx_vf_ndf24_geometry_albedo(alpha,eta,depth),reserve);
}

MtlxVerifiedSharedAllocation mtlx_vf_ndf24_branch_power(uint32_t factor,uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word) {
    const MtlxVerifiedSharedAllocation invalid={0,0,0,0};
    if(eta==0 || eta>UINT32_C(0x7f7fffff))return invalid;
    MtlxVerifiedNdfDirection half=mtlx_vf_ndf24_half(alpha,azimuth_word,radial_word);
    if(!half.valid)return invalid;
    const uint32_t env[4]={0,alpha,eta,half.cosine};
    uint32_t budget=mtlx_vf_scale_unit_power(factor,mtlx_verified_sampled_cos_power(env));
    return mtlx_vf_allocate_shared_power(budget,
        mtlx_vf_clamp_unit(mtlx_verified_reflection_cos_power(env)),
        mtlx_vf_clamp_unit(mtlx_verified_transmission_cos_power(env)));
}

MtlxVerifiedBranchKernel mtlx_vf_ndf24_branch_kernel(uint32_t factor,uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word) {
    MtlxVerifiedSharedAllocation allocation=mtlx_vf_ndf24_branch_power(factor,alpha,eta,azimuth_word,radial_word);
    if(!allocation.valid) { const MtlxVerifiedBranchKernel invalid={0,0,0,0};return invalid; }
    return mtlx_vf_branch_kernel(allocation.reflection,allocation.transmission);
}
MtlxVerifiedBranchEvent mtlx_vf_ndf24_branch_event(uint32_t factor,uint32_t alpha,uint32_t eta,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word) {
    return mtlx_vf_branch_event(mtlx_vf_ndf24_branch_kernel(factor,alpha,eta,azimuth_word,radial_word),branch_word);
}

static MtlxVerifiedFloatBounds world_norm_box(MtlxVerifiedBitVec3 v) {
    const MtlxVerifiedFloatBounds env[3]={{v.x,v.x,1},{v.y,v.y,1},{v.z,v.z,1}};
    return mtlx_verified_vector_norm(env);
}
static int world_norm_band(MtlxVerifiedFloatBounds box,uint32_t lower,uint32_t upper) {
    return box.valid==1 && box.lower<=box.upper && box.lower>=lower && box.upper<=upper;
}
static uint32_t world_dot(MtlxVerifiedBitVec3 v,MtlxVerifiedBitVec3 w) {
    return mtlx_vf_add(mtlx_vf_add(mtlx_vf_mul(v.x,w.x).bits,mtlx_vf_mul(v.y,w.y).bits).bits,mtlx_vf_mul(v.z,w.z).bits).bits;
}
static uint32_t world_order(uint32_t x) {return (x&UINT32_C(0x80000000)) ? ~x : x^UINT32_C(0x80000000);}
MtlxVerifiedNdfDirection mtlx_vf_ndf24_world_half(uint32_t alpha,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word) {
    if(alpha==0 || alpha>UINT32_C(0x3f800000) || !mtlx_verified_float_supported())return invalid_direction();
    if(!world_norm_band(world_norm_box(normal),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)))return invalid_direction();
    const uint32_t env[4]={normal.x,normal.y,normal.z,(normal.z&UINT32_C(0x80000000)) ? UINT32_C(0xbf800000) : UINT32_C(0x3f800000)};
    MtlxVerifiedBitVec3 tangent=mtlx_verified_duff_t(env),bitangent=mtlx_verified_duff_b(env);
    const uint32_t cosine=mtlx_vf_ggx_ndf_cos(alpha,mtlx_vf_pcg_uniform_bits(radial_word));
    const uint32_t phase=mtlx_vf_mul(UINT32_C(0x40c90fdb),mtlx_vf_pcg_uniform_bits(azimuth_word)).bits;
    MtlxVerifiedSinCos trig;
    if(!mtlx_verified_sincos(phase,&trig))return invalid_direction();
    const uint32_t radius=mtlx_vf_sqrt(mtlx_vf_sub(UINT32_C(0x3f800000),mtlx_vf_mul(cosine,cosine).bits).bits).bits;
    const MtlxVerifiedBitVec3 local={mtlx_vf_mul(radius,trig.cos_bits).bits,mtlx_vf_mul(radius,trig.sin_bits).bits,cosine};
    const uint32_t worldEnv[12]={local.x,local.y,local.z,tangent.x,tangent.y,tangent.z,
        bitangent.x,bitangent.y,bitangent.z,normal.x,normal.y,normal.z};
    MtlxVerifiedBitVec3 raw=mtlx_verified_world_vector(worldEnv);
    const uint32_t length=mtlx_vf_sqrt(world_dot(raw,raw)).bits;
    if(!finite_bits(length) || length==0 || (length&UINT32_C(0x80000000)))return invalid_direction();
    const uint32_t inverse=mtlx_vf_div(UINT32_C(0x3f800000),length).bits;
    const MtlxVerifiedBitVec3 half={mtlx_vf_mul(raw.x,inverse).bits,mtlx_vf_mul(raw.y,inverse).bits,mtlx_vf_mul(raw.z,inverse).bits};
    if(!world_norm_band(world_norm_box(half),UINT32_C(0x3f7ffe00),UINT32_C(0x3f800100)))return invalid_direction();
    const uint32_t dot=world_dot(half,normal);
    if(!finite_bits(dot))return invalid_direction();
    const uint32_t output=mtlx_vf_clamp_unit(dot);
    MtlxVerifiedFloatBounds difference=mtlx_vf_range_sub(mtlx_vf_range_point(output),mtlx_vf_range_point(cosine));
    if(!difference.valid || world_order(difference.lower)<world_order(UINT32_C(0xb7000000)) ||
       world_order(difference.upper)>world_order(UINT32_C(0x37000000)))return invalid_direction();
    const MtlxVerifiedNdfDirection out={half.x,half.y,half.z,output,1};return out;
}

MtlxVerifiedBranchKernel mtlx_vf_ndf24_world_branch_kernel(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word) {
    const MtlxVerifiedBranchKernel invalid={0,0,0,0};
    if(eta==0 || eta>UINT32_C(0x7f7fffff))return invalid;
    MtlxVerifiedNdfDirection half=mtlx_vf_ndf24_world_half(alpha,normal,azimuth_word,radial_word);
    if(!half.valid)return invalid;
    const uint32_t env[4]={0,alpha,eta,half.cosine};
    MtlxVerifiedSharedAllocation allocated=mtlx_vf_allocate_shared_power(
        mtlx_vf_scale_unit_power(factor,mtlx_verified_sampled_cos_power(env)),
        mtlx_vf_clamp_unit(mtlx_verified_reflection_cos_power(env)),
        mtlx_vf_clamp_unit(mtlx_verified_transmission_cos_power(env)));
    if(!allocated.valid)return invalid;
    return mtlx_vf_branch_kernel(allocated.reflection,allocated.transmission);
}
MtlxVerifiedBranchEvent mtlx_vf_ndf24_world_branch_event(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word) {
    return mtlx_vf_branch_event(mtlx_vf_ndf24_world_branch_kernel(factor,alpha,eta,normal,azimuth_word,radial_word),branch_word);
}

static MtlxVerifiedOutgoing invalid_outgoing(void) {
    const MtlxVerifiedOutgoing out={{UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),UINT32_C(0x7fc00000)},0,0};return out;
}
static MtlxVerifiedFloatBounds outgoing_dot_box(MtlxVerifiedBitVec3 v,MtlxVerifiedBitVec3 w) {
    const MtlxVerifiedFloatBounds env[6]={{v.x,v.x,1},{v.y,v.y,1},{v.z,v.z,1},{w.x,w.x,1},{w.y,w.y,1},{w.z,w.z,1}};
    return mtlx_verified_outgoing_dot_range(env);
}
static int outgoing_positive(uint32_t bits) {return bits>0 && bits<UINT32_C(0x7f800000);}
static int outgoing_negative(uint32_t bits) {return (bits&UINT32_C(0x80000000)) &&
    (bits&UINT32_C(0x7fffffff))>0 && (bits&UINT32_C(0x7fffffff))<UINT32_C(0x7f800000);}
static int outgoing_normalize(MtlxVerifiedBitVec3 raw,MtlxVerifiedBitVec3 *out) {
    const uint32_t length=mtlx_vf_sqrt(world_dot(raw,raw)).bits;
    if(!outgoing_positive(length))return 0;
    const uint32_t inverse=mtlx_vf_div(UINT32_C(0x3f800000),length).bits;
    out->x=mtlx_vf_mul(raw.x,inverse).bits;out->y=mtlx_vf_mul(raw.y,inverse).bits;out->z=mtlx_vf_mul(raw.z,inverse).bits;return 1;
}
static MtlxVerifiedOutgoing outgoing_finish(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 direction,unsigned reflected,unsigned kind) {
    const uint32_t lower=reflected ? UINT32_C(0x3f7fe000) : UINT32_C(0x3f7ffe00);
    const uint32_t upper=reflected ? UINT32_C(0x3f801000) : UINT32_C(0x3f800100);
    if(!world_norm_band(world_norm_box(direction),lower,upper))return invalid_outgoing();
    MtlxVerifiedFloatBounds hemisphere=outgoing_dot_box(direction,normal);
    if(!hemisphere.valid || (reflected ? !outgoing_positive(hemisphere.lower) : !outgoing_negative(hemisphere.upper)))return invalid_outgoing();
    const MtlxVerifiedOutgoing out={direction,kind,1};return out;
}
unsigned mtlx_vf_outgoing_inputs_valid(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half,uint32_t eta) {
    if(!outgoing_positive(eta) || !mtlx_verified_float_supported())return 0;
    if(!world_norm_band(world_norm_box(normal),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(view),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(half),UINT32_C(0x3f7ffe00),UINT32_C(0x3f800100)))return 0;
    MtlxVerifiedFloatBounds nv=outgoing_dot_box(view,normal),hv=outgoing_dot_box(half,view);
    if(!nv.valid || !hv.valid || !outgoing_positive(nv.lower) || !outgoing_positive(hv.lower))return 0;
    return 1;
}
MtlxVerifiedOutgoing mtlx_vf_checked_outgoing(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half,uint32_t eta,unsigned reflected) {
    if(reflected>1 || !mtlx_vf_outgoing_inputs_valid(normal,view,half,eta))return invalid_outgoing();
    const uint32_t cosine=mtlx_vf_clamp_unit(world_dot(half,view));
    const uint32_t env[8]={half.x,half.y,half.z,view.x,view.y,view.z,eta,cosine};
    if(reflected)return outgoing_finish(normal,mtlx_verified_reflected_vector(env),1,0);
    if(eta==UINT32_C(0x3f800000)) {
        const MtlxVerifiedBitVec3 negative={view.x^UINT32_C(0x80000000),view.y^UINT32_C(0x80000000),view.z^UINT32_C(0x80000000)};
        MtlxVerifiedBitVec3 delta;if(!outgoing_normalize(negative,&delta))return invalid_outgoing();
        return outgoing_finish(normal,delta,0,2);
    }
    MtlxVerifiedFloatBounds box[8];for(unsigned i=0;i<8;++i)box[i]=mtlx_vf_range_point(env[i]);
    MtlxVerifiedFloatBounds discriminant=mtlx_verified_outgoing_discriminant_range(box);
    const uint32_t sinSquared=mtlx_verified_outgoing_sin_squared(env);
    if(!discriminant.valid || !outgoing_positive(discriminant.lower) || !finite_bits(sinSquared) ||
       ((sinSquared&UINT32_C(0x80000000)) && (sinSquared&UINT32_C(0x7fffffff))!=0) ||
       (sinSquared&UINT32_C(0x7fffffff))>=UINT32_C(0x3f800000))return invalid_outgoing();
    MtlxVerifiedBitVec3 transmitted;
    if(!outgoing_normalize(mtlx_verified_transmitted_raw(env),&transmitted))return invalid_outgoing();
    return outgoing_finish(normal,transmitted,0,1);
}
MtlxVerifiedScatter mtlx_vf_ndf24_world_scatter(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word) {
    const MtlxVerifiedScatter invalid={{UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),UINT32_C(0x7fc00000)},UINT32_C(0x7fc00000),0,0};
    MtlxVerifiedNdfDirection half=mtlx_vf_ndf24_world_half(alpha,normal,azimuth_word,radial_word);
    if(!half.valid)return invalid;
    MtlxVerifiedBranchEvent event=mtlx_vf_ndf24_world_branch_event(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word);
    if(!event.valid)return invalid;
    const MtlxVerifiedBitVec3 halfVector={half.x,half.y,half.z};
    MtlxVerifiedOutgoing outgoing=mtlx_vf_checked_outgoing(normal,normal,halfVector,eta,event.reflected);
    if(!outgoing.valid)return invalid;
    const MtlxVerifiedScatter out={outgoing.direction,event.weight,outgoing.kind,1};return out;
}

static int transmission_nonnegative(uint32_t x) {return finite_bits(x) && (!(x&UINT32_C(0x80000000)) || (x&UINT32_C(0x7fffffff))==0);}
MtlxVerifiedTransmissionGeometry mtlx_vf_transmission_geometry(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta) {
    MtlxVerifiedTransmissionGeometry result={0};
    if(!mtlx_verified_float_supported() || !outgoing_positive(alpha) || alpha>UINT32_C(0x3f800000) ||
       !outgoing_positive(eta) || eta==UINT32_C(0x3f800000))return result;
    if(!world_norm_band(world_norm_box(normal),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(view),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(outgoing),UINT32_C(0x3f7ffe00),UINT32_C(0x3f800100)))return result;
    MtlxVerifiedFloatBounds nv=outgoing_dot_box(view,normal),wn=outgoing_dot_box(outgoing,normal);
    if(!nv.valid || !wn.valid || !outgoing_positive(nv.lower) || !outgoing_negative(wn.upper))return result;
    const uint32_t env[11]={normal.x,normal.y,normal.z,view.x,view.y,view.z,outgoing.x,outgoing.y,outgoing.z,alpha,eta};
    MtlxVerifiedFloatBounds box[11];for(unsigned i=0;i<11;++i)box[i]=mtlx_vf_range_point(env[i]);
    MtlxVerifiedFloatBounds jac=mtlx_verified_transmission_full_jacobian_range(box);
    if(!jac.valid || !outgoing_positive(jac.lower))return result;
    MtlxVerifiedBitVec3 half=mtlx_verified_transmission_half(env);
    if(!world_norm_band(world_norm_box(half),UINT32_C(0x3f7ffe00),UINT32_C(0x3f800100)))return result;
    result.half=half;result.raw_norm=mtlx_verified_transmission_raw_norm(env);
    result.jacobian=mtlx_verified_transmission_full_jacobian(env);result.jacobian_bounds=jac;result.valid=1;return result;
}
MtlxVerifiedTransmissionScalars mtlx_vf_transmission_scalars(const uint32_t values[11]) {
    MtlxVerifiedTransmissionScalars out={0};
    if(!values || !mtlx_verified_float_supported())return out;
    for(unsigned i=0;i<11;++i)if(!finite_bits(values[i]))return out;
    if(!outgoing_positive(values[0]) || values[0]>UINT32_C(0x3f800000) || !outgoing_positive(values[1]) || values[1]==UINT32_C(0x3f800000) ||
       !outgoing_positive(values[2]) || !outgoing_positive(values[3]) || !outgoing_positive(values[4]) || !outgoing_positive(values[5]) ||
       !outgoing_negative(values[6]) || !outgoing_positive(values[7]) || !transmission_nonnegative(values[8]) || (values[8]&UINT32_C(0x7fffffff))>UINT32_C(0x3f800000) ||
       !outgoing_positive(values[9]) || values[9]>UINT32_C(0x3f800000) || !transmission_nonnegative(values[10]) || (values[10]&UINT32_C(0x7fffffff))>UINT32_C(0x3f800000))return out;
    MtlxVerifiedFloatBounds box[11];for(unsigned i=0;i<11;++i)box[i]=mtlx_vf_range_point(values[i]);
    MtlxVerifiedFloatBounds jac=mtlx_verified_transmission_jacobian_range(box);
    MtlxVerifiedFloatBounds pdf=mtlx_verified_transmission_pdf_range(box);
    MtlxVerifiedFloatBounds bsdf=mtlx_verified_transmission_bsdf_range(box);
    MtlxVerifiedFloatBounds power=mtlx_verified_transmission_power_range(box);
    if(!jac.valid || !pdf.valid || !bsdf.valid || !power.valid || !outgoing_positive(jac.lower) ||
       !outgoing_positive(pdf.lower) || !transmission_nonnegative(bsdf.lower) || !transmission_nonnegative(power.lower))return out;
    out.pdf=mtlx_verified_transmission_pdf(values);out.bsdf=mtlx_verified_transmission_bsdf(values);
    out.power=mtlx_verified_transmission_power(values);out.jacobian=mtlx_verified_transmission_jacobian(values);
    out.pdf_bounds=pdf;out.bsdf_bounds=bsdf;out.power_bounds=power;out.valid=1;return out;
}
static void transmission_values(uint32_t values[11],MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,
                                uint32_t alpha,uint32_t eta,uint32_t mixture,uint32_t glass,MtlxVerifiedTransmissionGeometry geometry) {
    const uint32_t nh=world_dot(normal,geometry.half);
    values[0]=alpha;values[1]=eta;values[2]=world_dot(normal,view);values[3]=world_dot(normal,outgoing)^UINT32_C(0x80000000);
    values[4]=nh;values[5]=world_dot(view,geometry.half);values[6]=world_dot(outgoing,geometry.half);
    values[7]=geometry.raw_norm;values[8]=mtlx_vf_clamp_unit(mtlx_vf_mul(nh,nh).bits);values[9]=mixture;values[10]=glass;
}
MtlxVerifiedTransmissionEval mtlx_vf_transmission_eval(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta,uint32_t mixture,uint32_t glass) {
    MtlxVerifiedTransmissionEval out={0};
    MtlxVerifiedTransmissionGeometry geometry=mtlx_vf_transmission_geometry(normal,view,outgoing,alpha,eta);
    if(!geometry.valid)return out;
    uint32_t values[11];transmission_values(values,normal,view,outgoing,alpha,eta,mixture,glass,geometry);
    MtlxVerifiedTransmissionScalars scalar=mtlx_vf_transmission_scalars(values);
    if(!scalar.valid)return out;
    out.geometry=geometry;out.scalar=scalar;out.valid=1;return out;
}
MtlxVerifiedBsdfPowerPacket mtlx_vf_bsdf_power_packet(uint32_t budget,uint32_t pdf,uint32_t cosine,uint32_t eta,unsigned radiance) {
    MtlxVerifiedBsdfPowerPacket out={0};
    if(radiance>1 || !mtlx_verified_float_supported() || !transmission_nonnegative(budget) ||
       !outgoing_positive(pdf) || !outgoing_positive(cosine) || !outgoing_positive(eta))return out;
    const uint32_t env[4]={budget,pdf,cosine,eta};
    MtlxVerifiedFloatBounds box[4];for(unsigned i=0;i<4;++i)box[i]=mtlx_vf_range_point(env[i]);
    MtlxVerifiedFloatBounds candidate=radiance ? mtlx_verified_radiance_candidate_range(box) : mtlx_verified_power_candidate_range(box);
    if(!candidate.valid || !transmission_nonnegative(candidate.lower))return out;
    const uint32_t magnitude=candidate.lower&UINT32_C(0x7fffffff),bsdf=magnitude>8 ? magnitude-8 : 0;
    const uint32_t finalEnv[4]={bsdf,pdf,cosine,eta};
    for(unsigned i=0;i<4;++i)box[i]=mtlx_vf_range_point(finalEnv[i]);
    MtlxVerifiedFloatBounds power=radiance ? mtlx_verified_radiance_packet_range(box) : mtlx_verified_power_packet_range(box);
    if(!power.valid || !transmission_nonnegative(bsdf) || !transmission_nonnegative(power.lower) ||
       (power.upper&UINT32_C(0x7fffffff))>(budget&UINT32_C(0x7fffffff)))return out;
    out.bsdf=bsdf;out.power=radiance ? mtlx_verified_radiance_packet_power(finalEnv) : mtlx_verified_power_packet_power(finalEnv);
    out.valid=1;return out;
}
MtlxVerifiedWorldTransmissionPacket mtlx_vf_ndf24_world_transmission_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,
                                                                        uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance) {
    MtlxVerifiedWorldTransmissionPacket out={0};
    if(radiance>1)return out;
    MtlxVerifiedScatter scatter=mtlx_vf_ndf24_world_scatter(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word);
    if(!scatter.valid || scatter.kind!=1)return out;
    MtlxVerifiedBranchKernel kernel=mtlx_vf_ndf24_world_branch_kernel(factor,alpha,eta,normal,azimuth_word,radial_word);
    if(!kernel.valid || kernel.threshold>=UINT32_C(16777216))return out;
    const uint32_t probability=kernel.threshold ? mtlx_vf_pcg_uniform_bits((UINT32_C(16777216)-kernel.threshold)<<8) : UINT32_C(0x3f800000);
    MtlxVerifiedTransmissionEval evaluated=mtlx_vf_transmission_eval(normal,normal,scatter.direction,alpha,eta,probability,UINT32_C(0x3f800000));
    if(!evaluated.valid)return out;
    uint32_t values[11];transmission_values(values,normal,normal,scatter.direction,alpha,eta,probability,UINT32_C(0x3f800000),evaluated.geometry);
    MtlxVerifiedFloatBounds box[11];for(unsigned i=0;i<11;++i)box[i]=mtlx_vf_range_point(values[i]);
    MtlxVerifiedFloatBounds pdfBox=mtlx_verified_ndf_transmission_pdf_range(box);
    if(!pdfBox.valid || !outgoing_positive(pdfBox.lower))return out;
    const uint32_t pdf=mtlx_verified_ndf_transmission_pdf(values);
    MtlxVerifiedBsdfPowerPacket pair=mtlx_vf_bsdf_power_packet(scatter.weight,pdf,values[3],eta,radiance);
    if(!pair.valid)return out;
    out.scatter=scatter;out.pdf=pdf;out.cosine=values[3];out.pair=pair;out.valid=1;return out;
}

MtlxVerifiedReflectionGeometry mtlx_vf_reflection_geometry(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha) {
    MtlxVerifiedReflectionGeometry out={0};
    if(!mtlx_verified_float_supported() || !outgoing_positive(alpha) || alpha>UINT32_C(0x3f800000))return out;
    if(!world_norm_band(world_norm_box(normal),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(view),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(outgoing),UINT32_C(0x3f7fe000),UINT32_C(0x3f801000)))return out;
    MtlxVerifiedFloatBounds nv=outgoing_dot_box(view,normal),wn=outgoing_dot_box(outgoing,normal);
    if(!nv.valid || !wn.valid || !outgoing_positive(nv.lower) || !outgoing_positive(wn.lower))return out;
    const uint32_t env[11]={normal.x,normal.y,normal.z,view.x,view.y,view.z,outgoing.x,outgoing.y,outgoing.z,alpha,UINT32_C(0x3f800000)};
    MtlxVerifiedFloatBounds box[11];for(unsigned i=0;i<11;++i)box[i]=mtlx_vf_range_point(env[i]);
    MtlxVerifiedFloatBounds jac=mtlx_verified_reflection_jacobian_range(box);
    if(!jac.valid || !outgoing_positive(jac.lower))return out;
    MtlxVerifiedBitVec3 half=mtlx_verified_transmission_half(env);
    if(!world_norm_band(world_norm_box(half),UINT32_C(0x3f7ffe00),UINT32_C(0x3f800100)))return out;
    out.half=half;out.raw_norm=mtlx_verified_transmission_raw_norm(env);out.jacobian=mtlx_verified_reflection_jacobian(env);
    out.jacobian_bounds=jac;out.valid=1;return out;
}
MtlxVerifiedWorldReflectionPacket mtlx_vf_ndf24_world_reflection_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,
                                                                    uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word) {
    MtlxVerifiedWorldReflectionPacket out={0};
    MtlxVerifiedScatter scatter=mtlx_vf_ndf24_world_scatter(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word);
    if(!scatter.valid || scatter.kind!=0)return out;
    MtlxVerifiedBranchKernel kernel=mtlx_vf_ndf24_world_branch_kernel(factor,alpha,eta,normal,azimuth_word,radial_word);
    if(!kernel.valid || kernel.threshold==0 || kernel.threshold>UINT32_C(16777216))return out;
    uint32_t probability=kernel.threshold==UINT32_C(16777216) ? UINT32_C(0x3f800000) : mtlx_vf_pcg_uniform_bits(kernel.threshold<<8);
    MtlxVerifiedReflectionGeometry geometry=mtlx_vf_reflection_geometry(normal,normal,scatter.direction,alpha);
    if(!geometry.valid)return out;
    uint32_t values[11];transmission_values(values,normal,normal,scatter.direction,alpha,eta,probability,UINT32_C(0x3f800000),geometry);
    values[3]=world_dot(normal,scatter.direction);
    MtlxVerifiedFloatBounds box[11];for(unsigned i=0;i<11;++i)box[i]=mtlx_vf_range_point(values[i]);
    MtlxVerifiedFloatBounds pdfBox=mtlx_verified_reflection_ndf_pdf_range(box);
    if(!pdfBox.valid || !outgoing_positive(pdfBox.lower))return out;
    uint32_t pdf=mtlx_verified_reflection_ndf_pdf(values);
    MtlxVerifiedBsdfPowerPacket pair=mtlx_vf_bsdf_power_packet(scatter.weight,pdf,values[3],eta,0);
    if(!pair.valid)return out;
    out.scatter=scatter;out.pdf=pdf;out.cosine=values[3];out.pair=pair;out.valid=1;return out;
}
MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_world_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,
                                                              uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance) {
    MtlxVerifiedWorldSurfacePacket out={0};
    if(radiance>1)return out;
    MtlxVerifiedScatter scatter=mtlx_vf_ndf24_world_scatter(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word);
    if(!scatter.valid)return out;
    if(scatter.kind==0 || scatter.kind==1) {
        MtlxVerifiedWorldTransmissionPacket p=scatter.kind==0 ?
            mtlx_vf_ndf24_world_reflection_packet(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word) :
            mtlx_vf_ndf24_world_transmission_packet(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word,radiance);
        if(!p.valid)return out;
        out.scatter=p.scatter;out.density=p.pdf;out.cosine=p.cosine;out.coefficient=p.pair.bsdf;out.power=p.pair.power;out.valid=1;return out;
    }
    if(scatter.kind==2 && eta==UINT32_C(0x3f800000)) {
        MtlxVerifiedBranchKernel kernel=mtlx_vf_ndf24_world_branch_kernel(factor,alpha,eta,normal,azimuth_word,radial_word);
        if(!kernel.valid || kernel.threshold>=UINT32_C(16777216))return out;
        uint32_t mass=kernel.threshold ? mtlx_vf_pcg_uniform_bits((UINT32_C(16777216)-kernel.threshold)<<8) : UINT32_C(0x3f800000);
        MtlxVerifiedBsdfPowerPacket pair=mtlx_vf_bsdf_power_packet(scatter.weight,mass,UINT32_C(0x3f800000),UINT32_C(0x3f800000),0);
        if(!pair.valid)return out;
        out.scatter=scatter;out.density=mass;out.cosine=UINT32_C(0x3f800000);out.coefficient=pair.bsdf;out.power=pair.power;
        out.atomic=1;out.valid=1;return out;
    }
    return out;
}

MtlxVerifiedWorldSurfacePacket mtlx_vf_index_matched_packet(uint32_t factor,MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view) {
    MtlxVerifiedWorldSurfacePacket out={0};
    if(!mtlx_verified_float_supported() || !transmission_nonnegative(factor))return out;
    if(!world_norm_band(world_norm_box(normal),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(view),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)))return out;
    const uint32_t budget=mtlx_vf_clamp_unit(factor);
    out.scatter.direction=(MtlxVerifiedBitVec3){view.x^UINT32_C(0x80000000),view.y^UINT32_C(0x80000000),view.z^UINT32_C(0x80000000)};
    out.scatter.weight=budget;out.scatter.kind=2;out.scatter.valid=1;
    out.density=UINT32_C(0x3f800000);out.cosine=UINT32_C(0x3f800000);
    out.coefficient=budget;out.power=budget;out.atomic=1;out.valid=1;return out;
}

MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_index_aware_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,
                                                                   uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance) {
    MtlxVerifiedWorldSurfacePacket out={0};
    if(radiance>1)return out;
    if(eta==UINT32_C(0x3f800000)) {
        if(!transmission_nonnegative(alpha) || (alpha&UINT32_C(0x7fffffff))>UINT32_C(0x3f800000))return out;
        return mtlx_vf_index_matched_packet(factor,normal,normal);
    }
    return mtlx_vf_ndf24_world_surface_packet(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word,radiance);
}

MtlxVerifiedBsdfPowerPacket mtlx_vf_atomic_power_packet(uint32_t budget,uint32_t mass,uint32_t eta,unsigned radiance) {
    MtlxVerifiedBsdfPowerPacket out={0};
    if(radiance>1 || !mtlx_verified_float_supported() || !transmission_nonnegative(budget) ||
       !outgoing_positive(mass) || !outgoing_positive(eta))return out;
    if((budget&UINT32_C(0x7fffffff))==0){out.valid=1;return out;}
    return mtlx_vf_bsdf_power_packet(budget,mass,UINT32_C(0x3f800000),eta,radiance);
}
MtlxVerifiedAtomicBranchPacket mtlx_vf_atomic_branch_packet(uint32_t reflection,uint32_t transmission,uint32_t eta,uint32_t word,unsigned radiance) {
    MtlxVerifiedAtomicBranchPacket out={0};
    if(radiance>1)return out;
    MtlxVerifiedBranchKernel kernel=mtlx_vf_branch_kernel(reflection,transmission);
    if(!kernel.valid)return out;
    MtlxVerifiedBsdfPowerPacket rp={0,0,1},tp={0,0,1};
    if(kernel.threshold) {
        const uint32_t mass=kernel.threshold==UINT32_C(16777216) ? UINT32_C(0x3f800000) : mtlx_vf_pcg_uniform_bits(kernel.threshold<<8);
        rp=mtlx_vf_atomic_power_packet(kernel.reflection_weight,mass,eta,0);
    }
    if(kernel.threshold<UINT32_C(16777216)) {
        const uint32_t count=UINT32_C(16777216)-kernel.threshold;
        const uint32_t mass=count==UINT32_C(16777216) ? UINT32_C(0x3f800000) : mtlx_vf_pcg_uniform_bits(count<<8);
        tp=mtlx_vf_atomic_power_packet(kernel.transmission_weight,mass,eta,radiance);
    }
    if(!rp.valid || !tp.valid)return out;
    const unsigned reflected=(word>>8)<kernel.threshold;
    const uint32_t count=reflected ? kernel.threshold : UINT32_C(16777216)-kernel.threshold;
    const uint32_t mass=count==UINT32_C(16777216) ? UINT32_C(0x3f800000) : mtlx_vf_pcg_uniform_bits(count<<8);
    MtlxVerifiedBsdfPowerPacket pair=reflected ? rp : tp;
    if(!pair.valid)return out;
    out.kernel=kernel;out.reflected=reflected;out.mass=mass;out.pair=pair;out.valid=1;return out;
}
MtlxVerifiedSharedAllocation mtlx_vf_collapsed_allocation(uint32_t factor,uint32_t alpha,uint32_t eta) {
    const MtlxVerifiedSharedAllocation invalid={0,0,0,0};
    if(!mtlx_verified_float_supported() || !transmission_nonnegative(factor) || !outgoing_positive(eta) ||
       eta==UINT32_C(0x3f800000) || !mtlx_vf_ndf_alpha_collapsed(alpha))return invalid;
    const uint32_t env[4]={0,alpha,eta,UINT32_C(0x3f800000)};
    const uint32_t r=mtlx_verified_reflection_cos_power(env),t=mtlx_verified_transmission_cos_power(env);
    if(!transmission_nonnegative(r) || !transmission_nonnegative(t))return invalid;
    const uint32_t budget=mtlx_vf_scale_unit_power(mtlx_vf_clamp_unit(factor),mtlx_verified_sampled_cos_power(env));
    return mtlx_vf_allocate_shared_power(budget,mtlx_vf_clamp_unit(r),mtlx_vf_clamp_unit(t));
}
MtlxVerifiedWorldSurfacePacket mtlx_vf_collapsed_world_surface(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,uint32_t word,unsigned radiance) {
    MtlxVerifiedWorldSurfacePacket out={0};
    if(radiance>1 || !mtlx_verified_float_supported() ||
       !world_norm_band(world_norm_box(normal),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)))return out;
    MtlxVerifiedSharedAllocation allocation=mtlx_vf_collapsed_allocation(factor,alpha,eta);
    if(!allocation.valid)return out;
    MtlxVerifiedAtomicBranchPacket atom=mtlx_vf_atomic_branch_packet(allocation.reflection,allocation.transmission,eta,word,radiance);
    if(!atom.valid)return out;
    MtlxVerifiedBitVec3 direction=normal;
    if(!atom.reflected){direction.x^=UINT32_C(0x80000000);direction.y^=UINT32_C(0x80000000);direction.z^=UINT32_C(0x80000000);}
    out.scatter.direction=direction;out.scatter.weight=atom.reflected ? atom.kernel.reflection_weight : atom.kernel.transmission_weight;
    out.scatter.kind=atom.reflected ? 0u : 1u;out.scatter.valid=1;
    out.density=atom.mass;out.cosine=UINT32_C(0x3f800000);out.coefficient=atom.pair.bsdf;out.power=atom.pair.power;
    out.atomic=1;out.valid=1;return out;
}

MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_quantized_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,
                                                                 uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance) {
    if(eta==UINT32_C(0x3f800000))return mtlx_vf_ndf24_index_aware_surface_packet(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word,radiance);
    if(mtlx_vf_ndf_alpha_collapsed(alpha))return mtlx_vf_collapsed_world_surface(factor,alpha,eta,normal,branch_word,radiance);
    return mtlx_vf_ndf24_world_surface_packet(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word,radiance);
}

MtlxVerifiedTransmissionGeometry mtlx_vf_scaled_transmission_geometry(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta) {
    MtlxVerifiedTransmissionGeometry out={0};
    if(!mtlx_verified_float_supported() || !outgoing_positive(alpha) || alpha>UINT32_C(0x3f800000) ||
       !outgoing_positive(eta) || eta==UINT32_C(0x3f800000))return out;
    if(!world_norm_band(world_norm_box(normal),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(view),UINT32_C(0x3f7ffff0),UINT32_C(0x3f800008)) ||
       !world_norm_band(world_norm_box(outgoing),UINT32_C(0x3f7ffe00),UINT32_C(0x3f800100)))return out;
    MtlxVerifiedFloatBounds nv=outgoing_dot_box(view,normal),wn=outgoing_dot_box(outgoing,normal);
    if(!nv.valid || !wn.valid || !outgoing_positive(nv.lower) || !outgoing_negative(wn.upper))return out;
    const uint32_t env[11]={normal.x,normal.y,normal.z,view.x,view.y,view.z,outgoing.x,outgoing.y,outgoing.z,alpha,eta};
    MtlxVerifiedFloatBounds box[11];for(unsigned i=0;i<11;++i)box[i]=mtlx_vf_range_point(env[i]);
    MtlxVerifiedFloatBounds norm=mtlx_verified_scaled_transmission_norm_real_range(box);
    if(!norm.valid || !outgoing_positive(norm.lower))return out;
    MtlxVerifiedFloatBounds ideal=mtlx_verified_scaled_transmission_jacobian_real_range(box);
    const uint32_t actual=mtlx_verified_scaled_transmission_jacobian(env);
    MtlxVerifiedFloatBounds jac=mtlx_vf_range_hull(ideal,mtlx_vf_range_point(actual));
    if(!jac.valid || !outgoing_positive(jac.lower))return out;
    MtlxVerifiedBitVec3 half=mtlx_verified_scaled_transmission_half(env);
    if(!world_norm_band(world_norm_box(half),UINT32_C(0x3f7ffe00),UINT32_C(0x3f800100)))return out;
    out.half=half;out.raw_norm=mtlx_verified_scaled_transmission_raw_norm(env);out.jacobian=actual;
    out.jacobian_bounds=jac;out.valid=1;return out;
}
MtlxVerifiedTransmissionEval mtlx_vf_scaled_transmission_eval(MtlxVerifiedBitVec3 normal,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 outgoing,uint32_t alpha,uint32_t eta,uint32_t mixture,uint32_t glass) {
    MtlxVerifiedTransmissionEval out={0};
    MtlxVerifiedTransmissionGeometry geometry=mtlx_vf_scaled_transmission_geometry(normal,view,outgoing,alpha,eta);
    if(!geometry.valid)return out;
    uint32_t values[11];transmission_values(values,normal,view,outgoing,alpha,eta,mixture,glass,geometry);
    MtlxVerifiedTransmissionScalars scalar=mtlx_vf_transmission_scalars(values);
    if(!scalar.valid)return out;
    out.geometry=geometry;out.scalar=scalar;out.valid=1;return out;
}

MtlxVerifiedWorldTransmissionPacket mtlx_vf_ndf24_world_scaled_transmission_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,
                                                                        uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance) {
    MtlxVerifiedWorldTransmissionPacket out={0};
    if(radiance>1)return out;
    MtlxVerifiedScatter scatter=mtlx_vf_ndf24_world_scatter(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word);
    if(!scatter.valid || scatter.kind!=1)return out;
    MtlxVerifiedBranchKernel kernel=mtlx_vf_ndf24_world_branch_kernel(factor,alpha,eta,normal,azimuth_word,radial_word);
    if(!kernel.valid || kernel.threshold>=UINT32_C(16777216))return out;
    const uint32_t probability=kernel.threshold ? mtlx_vf_pcg_uniform_bits((UINT32_C(16777216)-kernel.threshold)<<8) : UINT32_C(0x3f800000);
    MtlxVerifiedTransmissionEval evaluated=mtlx_vf_scaled_transmission_eval(normal,normal,scatter.direction,alpha,eta,probability,UINT32_C(0x3f800000));
    if(!evaluated.valid)return out;
    uint32_t values[11];transmission_values(values,normal,normal,scatter.direction,alpha,eta,probability,UINT32_C(0x3f800000),evaluated.geometry);
    MtlxVerifiedFloatBounds box[11];for(unsigned i=0;i<11;++i)box[i]=mtlx_vf_range_point(values[i]);
    MtlxVerifiedFloatBounds pdfBox=mtlx_verified_ndf_transmission_pdf_range(box);
    if(!pdfBox.valid || !outgoing_positive(pdfBox.lower))return out;
    const uint32_t pdf=mtlx_verified_ndf_transmission_pdf(values);
    MtlxVerifiedBsdfPowerPacket pair=mtlx_vf_bsdf_power_packet(scatter.weight,pdf,values[3],eta,radiance);
    if(!pair.valid)return out;
    out.scatter=scatter;out.pdf=pdf;out.cosine=values[3];out.pair=pair;out.valid=1;return out;
}

MtlxVerifiedWorldSurfacePacket mtlx_vf_ndf24_stable_surface_packet(uint32_t factor,uint32_t alpha,uint32_t eta,MtlxVerifiedBitVec3 normal,
                                                               uint32_t azimuth_word,uint32_t radial_word,uint32_t branch_word,unsigned radiance) {
    MtlxVerifiedWorldSurfacePacket out=mtlx_vf_ndf24_quantized_surface_packet(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word,radiance);
    if(out.valid || eta==UINT32_C(0x3f800000) || mtlx_vf_ndf_alpha_collapsed(alpha))return out;
    MtlxVerifiedWorldTransmissionPacket p=mtlx_vf_ndf24_world_scaled_transmission_packet(factor,alpha,eta,normal,azimuth_word,radial_word,branch_word,radiance);
    if(!p.valid)return out;
    out.scatter=p.scatter;out.density=p.pdf;out.cosine=p.cosine;out.coefficient=p.pair.bsdf;out.power=p.pair.power;out.atomic=0;out.valid=1;
    return out;
}
