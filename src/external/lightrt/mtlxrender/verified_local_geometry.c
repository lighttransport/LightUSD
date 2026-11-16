#include "verified_direction.h"
#include "verified_float.h"
#include "verified_outgoing_program.h"
#include "verified_world_program.h"
#include "verified_reflection_horizon_program.h"
#include "verified_transmission_horizon_program.h"
#ifdef __FAST_MATH__
#error "verified_local_geometry.c requires strict floating-point compilation"
#endif
static uint32_t world_dot(MtlxVerifiedBitVec3 v,MtlxVerifiedBitVec3 w) {
 return mtlx_vf_add(mtlx_vf_add(mtlx_vf_mul(v.x,w.x).bits,mtlx_vf_mul(v.y,w.y).bits).bits,mtlx_vf_mul(v.z,w.z).bits).bits;
}

MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    MtlxVerifiedAnisotropicLocalProposal out={0};
    if(eta==UINT32_C(0x3f800000) || !mtlx_verified_float_supported())return out;
    const MtlxVerifiedBitVec3 normal={0,0,UINT32_C(0x3f800000)};
    const MtlxVerifiedOutgoing r=mtlx_vf_checked_outgoing(normal,view,half,eta,1);
    if(!r.valid)return out;
    const MtlxVerifiedOutgoing t=mtlx_vf_checked_outgoing(normal,view,half,eta,0);
    if(!t.valid)return out;
    const uint32_t values[13]={ax,ay,eta,mtlx_vf_clamp_unit(world_dot(half,view)),
        view.x,view.y,view.z,r.direction.x,r.direction.y,r.direction.z,
        t.direction.x,t.direction.y,t.direction.z^UINT32_C(0x80000000)};
    const MtlxVerifiedAnisotropicSingleProposal p=mtlx_vf_anisotropic_single_proposal(values);
    if(!p.valid)return out;
    out.reflection=r;out.transmission=t;out.power=p;
    for(unsigned i=0;i<13;++i)out.scalars[i]=values[i];
    out.valid=1;return out;
}
MtlxVerifiedScatter mtlx_vf_anisotropic_local_sample(MtlxVerifiedAnisotropicLocalProposal proposal,uint32_t word) {
    const MtlxVerifiedScatter invalid={{UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),UINT32_C(0x7fc00000)},UINT32_C(0x7fc00000),0,0};
    if(proposal.valid!=1 || proposal.transmission_absent>1 || proposal.reflection_absent>1 ||
       (proposal.transmission_absent && proposal.reflection_absent))return invalid;
    if(proposal.reflection_absent ? proposal.power.kernel.threshold!=0 : proposal.reflection.valid!=1)return invalid;
    if(proposal.transmission_absent ? proposal.power.kernel.threshold!=UINT32_C(16777216) : proposal.transmission.valid!=1)return invalid;
    const MtlxVerifiedBranchEvent event=mtlx_vf_anisotropic_single_event(proposal.power,word);
    if(!event.valid)return invalid;
    const MtlxVerifiedOutgoing selected=event.reflected ? proposal.reflection : proposal.transmission;
    const MtlxVerifiedScatter out={selected.direction,event.weight,selected.kind,1};return out;
}

MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_tir_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    MtlxVerifiedAnisotropicLocalProposal out={0};
    if(eta==0 || eta>=UINT32_C(0x7f800000) || eta==UINT32_C(0x3f800000) || !mtlx_verified_float_supported())return out;
    const uint32_t cosine=mtlx_vf_clamp_unit(world_dot(half,view));
    MtlxVerifiedFloatBounds env[8];
    for(unsigned i=0;i<8;++i)env[i]=mtlx_vf_range_point(i==6 ? eta : cosine);
    const MtlxVerifiedFloatBounds box=mtlx_verified_outgoing_discriminant_range(env);
    if(!box.valid || (!(box.upper&UINT32_C(0x80000000)) && (box.upper&UINT32_C(0x7fffffff))!=0))return out;
    const MtlxVerifiedBitVec3 normal={0,0,UINT32_C(0x3f800000)};
    const MtlxVerifiedOutgoing r=mtlx_vf_checked_outgoing(normal,view,half,eta,1);
    if(!r.valid)return out;
    const uint32_t values[13]={ax,ay,eta,cosine,view.x,view.y,view.z,
        r.direction.x,r.direction.y,r.direction.z,0,0,UINT32_C(0x80000000)};
    const MtlxVerifiedAnisotropicSingleProposal p=mtlx_vf_anisotropic_single_proposal(values);
    if(!p.valid || p.kernel.threshold!=UINT32_C(16777216))return out;
    out.reflection=r;out.power=p;out.tir_discriminant=box;out.transmission_absent=1;
    for(unsigned i=0;i<13;++i)out.scalars[i]=values[i];
    out.valid=1;return out;
}
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_typed_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    const MtlxVerifiedAnisotropicLocalProposal regular=mtlx_vf_anisotropic_local_proposal(ax,ay,eta,view,half);
    if(regular.valid)return regular;
    return mtlx_vf_anisotropic_local_tir_proposal(ax,ay,eta,view,half);
}

MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_horizon_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    MtlxVerifiedAnisotropicLocalProposal out={0};
    if(eta==UINT32_C(0x3f800000) || !mtlx_verified_float_supported())return out;
    const MtlxVerifiedBitVec3 normal={0,0,UINT32_C(0x3f800000)};
    const MtlxVerifiedOutgoing t=mtlx_vf_checked_outgoing(normal,view,half,eta,0);
    if(!t.valid)return out;
    const uint32_t values_r[8]={half.x,half.y,half.z,view.x,view.y,view.z,0,0};
    const MtlxVerifiedBitVec3 raw=mtlx_verified_reflected_vector(values_r);
    const MtlxVerifiedFloatBounds norm_env[3]={{raw.x,raw.x,1},{raw.y,raw.y,1},{raw.z,raw.z,1}};
    const MtlxVerifiedFloatBounds norm=mtlx_verified_vector_norm(norm_env);
    if(!norm.valid || norm.lower>norm.upper || norm.lower<UINT32_C(0x3f7fe000) || norm.upper>UINT32_C(0x3f801000))return out;
    MtlxVerifiedFloatBounds env[8];for(unsigned i=0;i<8;++i)env[i]=mtlx_vf_range_point(values_r[i]);
    const MtlxVerifiedFloatBounds box=mtlx_vf_range_hull(mtlx_verified_reflection_horizon_real_range(env),mtlx_vf_range_point(raw.z));
    if(!box.valid || (!(box.upper&UINT32_C(0x80000000)) && (box.upper&UINT32_C(0x7fffffff))!=0))return out;
    const uint32_t values[13]={ax,ay,eta,mtlx_vf_clamp_unit(world_dot(half,view)),view.x,view.y,view.z,
        0,0,0,t.direction.x,t.direction.y,t.direction.z^UINT32_C(0x80000000)};
    const MtlxVerifiedAnisotropicSingleProposal p=mtlx_vf_anisotropic_single_proposal(values);
    if(!p.valid || p.kernel.threshold!=0)return out;
    out.transmission=t;out.power=p;out.reflection_norm=norm;out.reflection_z=box;out.reflection_absent=1;
    for(unsigned i=0;i<13;++i)out.scalars[i]=values[i];
    out.valid=1;return out;
}
MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_routed_proposal(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    const MtlxVerifiedAnisotropicLocalProposal prior=mtlx_vf_anisotropic_local_typed_proposal(ax,ay,eta,view,half);
    if(prior.valid)return prior;
    return mtlx_vf_anisotropic_local_horizon_proposal(ax,ay,eta,view,half);
}

MtlxVerifiedAnisotropicLocalNull mtlx_vf_anisotropic_local_null(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    MtlxVerifiedAnisotropicLocalNull out={0};
    if(ax==0 || ax>UINT32_C(0x3f800000) || ay==0 || ay>UINT32_C(0x3f800000) || eta==UINT32_C(0x3f800000))return out;
    const MtlxVerifiedBitVec3 normal={0,0,UINT32_C(0x3f800000)};
    if(!mtlx_vf_outgoing_inputs_valid(normal,view,half,eta))return out;
    const uint32_t values[8]={half.x,half.y,half.z,view.x,view.y,view.z,0,0};
    const MtlxVerifiedBitVec3 raw=mtlx_verified_reflected_vector(values);
    const MtlxVerifiedFloatBounds norm_env[3]={{raw.x,raw.x,1},{raw.y,raw.y,1},{raw.z,raw.z,1}};
    const MtlxVerifiedFloatBounds norm=mtlx_verified_vector_norm(norm_env);
    if(!norm.valid || norm.lower>norm.upper || norm.lower<UINT32_C(0x3f7fe000) || norm.upper>UINT32_C(0x3f801000))return out;
    MtlxVerifiedFloatBounds env[8];for(unsigned i=0;i<8;++i)env[i]=mtlx_vf_range_point(values[i]);
    const MtlxVerifiedFloatBounds z=mtlx_vf_range_hull(mtlx_verified_reflection_horizon_real_range(env),mtlx_vf_range_point(raw.z));
    if(!z.valid || (!(z.upper&UINT32_C(0x80000000)) && (z.upper&UINT32_C(0x7fffffff))!=0))return out;
    const uint32_t cosine=mtlx_vf_clamp_unit(world_dot(half,view));
    for(unsigned i=0;i<8;++i)env[i]=mtlx_vf_range_point(i==6 ? eta : cosine);
    const MtlxVerifiedFloatBounds disc=mtlx_verified_outgoing_discriminant_range(env);
    if(!disc.valid || (!(disc.upper&UINT32_C(0x80000000)) && (disc.upper&UINT32_C(0x7fffffff))!=0))return out;
    out.reflection_norm=norm;out.reflection_z=z;out.discriminant=disc;out.valid=1;return out;
}
MtlxVerifiedAnisotropicLocalPrepared mtlx_vf_anisotropic_local_prepare(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    MtlxVerifiedAnisotropicLocalPrepared out={0};
    const MtlxVerifiedAnisotropicLocalProposal p=mtlx_vf_anisotropic_local_routed_proposal(ax,ay,eta,view,half);
    if(p.valid){out.proposal=p;out.valid=1;return out;}
    const MtlxVerifiedAnisotropicLocalNull n=mtlx_vf_anisotropic_local_null(ax,ay,eta,view,half);
    if(n.valid){out.null_certificate=n;out.terminated=1;out.valid=1;}
    return out;
}
MtlxVerifiedAnisotropicLocalEvent mtlx_vf_anisotropic_local_event(MtlxVerifiedAnisotropicLocalPrepared prepared,uint32_t word) {
    MtlxVerifiedAnisotropicLocalEvent out={0};
    if(prepared.valid!=1 || prepared.terminated>1 || prepared.index_matched>1 || !mtlx_verified_float_supported())return out;
    if(prepared.index_matched){
        if(prepared.terminated || prepared.proposal.valid || prepared.null_certificate.valid || prepared.matched.valid!=1 ||
           prepared.matched.kind!=2 || prepared.matched.weight!=UINT32_C(0x3f800000))return out;
        out.scatter=prepared.matched;out.mass=UINT32_C(0x3f800000);out.atomic=1;out.valid=1;return out;
    }
    if(prepared.matched.valid)return out;
    if(prepared.terminated){
        if(prepared.null_certificate.valid!=1 || prepared.proposal.valid)return out;
        out.terminated=1;out.valid=1;return out;
    }
    if(prepared.null_certificate.valid)return out;
    out.scatter=mtlx_vf_anisotropic_local_sample(prepared.proposal,word);
    if(out.scatter.valid)out.valid=1;
    return out;
}
MtlxVerifiedAnisotropicLocalPrepared mtlx_vf_anisotropic_local_prepare_indexed(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    if(eta==UINT32_C(0x3f800000)){
        MtlxVerifiedAnisotropicLocalPrepared out={0};
        const MtlxVerifiedBitVec3 normal={0,0,UINT32_C(0x3f800000)};
        const MtlxVerifiedWorldSurfacePacket p=mtlx_vf_index_matched_packet(UINT32_C(0x3f800000),normal,view);
        if(p.valid){out.matched=p.scatter;out.index_matched=1;out.valid=1;}
        return out;
    }
    return mtlx_vf_anisotropic_local_prepare(ax,ay,eta,view,half);
}

MtlxVerifiedAnisotropicLocalProposal mtlx_vf_anisotropic_local_transmission_horizon(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    MtlxVerifiedAnisotropicLocalProposal out={0};
    if(eta==UINT32_C(0x3f800000) || !mtlx_verified_float_supported())return out;
    const MtlxVerifiedBitVec3 normal={0,0,UINT32_C(0x3f800000)};
    const MtlxVerifiedOutgoing r=mtlx_vf_checked_outgoing(normal,view,half,eta,1);
    if(!r.valid)return out;
    const uint32_t cosine=mtlx_vf_clamp_unit(world_dot(half,view));
    const uint32_t values_t[8]={half.x,half.y,half.z,view.x,view.y,view.z,eta,cosine};
    MtlxVerifiedFloatBounds env[8];for(unsigned i=0;i<8;++i)env[i]=mtlx_vf_range_point(values_t[i]);
    const MtlxVerifiedFloatBounds disc=mtlx_verified_outgoing_discriminant_range(env);
    const uint32_t sin_squared=mtlx_verified_outgoing_sin_squared(values_t);
    if(!disc.valid || disc.lower==0 || disc.lower>=UINT32_C(0x7f800000) ||
       (sin_squared&UINT32_C(0x7fffffff))>=UINT32_C(0x3f800000) ||
       ((sin_squared&UINT32_C(0x80000000)) && (sin_squared&UINT32_C(0x7fffffff))!=0))return out;
    const MtlxVerifiedBitVec3 raw=mtlx_verified_transmitted_raw(values_t);
    const uint32_t q=world_dot(raw,raw);
    const MtlxVerifiedFloatBounds raw_norm=mtlx_vf_range_hull(mtlx_verified_transmission_horizon_norm_range(env),mtlx_vf_range_point(q));
    if(!raw_norm.valid || raw_norm.lower==0 || raw_norm.lower>=UINT32_C(0x7f800000))return out;
    const uint32_t inv=mtlx_vf_div(UINT32_C(0x3f800000),mtlx_vf_sqrt(q).bits).bits;
    const MtlxVerifiedBitVec3 direction={mtlx_vf_mul(raw.x,inv).bits,mtlx_vf_mul(raw.y,inv).bits,mtlx_vf_mul(raw.z,inv).bits};
    const MtlxVerifiedFloatBounds ne[3]={{direction.x,direction.x,1},{direction.y,direction.y,1},{direction.z,direction.z,1}};
    const MtlxVerifiedFloatBounds norm=mtlx_verified_vector_norm(ne);
    if(!norm.valid || norm.lower>norm.upper || norm.lower<UINT32_C(0x3f7ffe00) || norm.upper>UINT32_C(0x3f800100) ||
       ((direction.z&UINT32_C(0x80000000)) && (direction.z&UINT32_C(0x7fffffff))!=0))return out;
    const MtlxVerifiedFloatBounds z=mtlx_vf_range_hull(mtlx_verified_transmission_horizon_z_range(env),mtlx_vf_range_point(raw.z));
    if(!z.valid || ((z.lower&UINT32_C(0x80000000)) && (z.lower&UINT32_C(0x7fffffff))!=0))return out;
    const uint32_t values[13]={ax,ay,eta,cosine,view.x,view.y,view.z,r.direction.x,r.direction.y,r.direction.z,0,0,UINT32_C(0x80000000)};
    const MtlxVerifiedAnisotropicSingleProposal p=mtlx_vf_anisotropic_single_proposal(values);
    if(!p.valid || p.kernel.threshold!=UINT32_C(16777216))return out;
    out.reflection=r;out.transmission.direction=direction;out.transmission.kind=1;
    out.transmission_absent=1;out.power=p;out.transmission_raw_norm=raw_norm;out.transmission_norm=norm;
    out.transmission_z=z;out.transmission_discriminant=disc;
    for(unsigned i=0;i<13;++i)out.scalars[i]=values[i];
    out.valid=1;return out;
}
MtlxVerifiedAnisotropicLocalPrepared mtlx_vf_anisotropic_local_prepare_extended(uint32_t ax,uint32_t ay,uint32_t eta,MtlxVerifiedBitVec3 view,MtlxVerifiedBitVec3 half) {
    MtlxVerifiedAnisotropicLocalPrepared out=mtlx_vf_anisotropic_local_prepare_indexed(ax,ay,eta,view,half);
    if(out.valid)return out;
    const MtlxVerifiedAnisotropicLocalProposal p=mtlx_vf_anisotropic_local_transmission_horizon(ax,ay,eta,view,half);
    if(p.valid){out.proposal=p;out.valid=1;}
    return out;
}
