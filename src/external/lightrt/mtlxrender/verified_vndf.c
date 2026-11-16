#include "verified_vndf.h"
#include "verified_float.h"
#include "verified_math.h"
#ifdef __FAST_MATH__
#error "verified_vndf.c requires strict floating-point compilation"
#endif

static int finite_bits(uint32_t x) { return (x&UINT32_C(0x7fffffff))<UINT32_C(0x7f800000); }
static int positive_bits(uint32_t x) { return x>0 && x<UINT32_C(0x7f800000); }
static int finite_vec(MtlxVerifiedBitVec3 v) { return finite_bits(v.x)&&finite_bits(v.y)&&finite_bits(v.z); }
static uint32_t nonnegative(uint32_t x) { return (x&UINT32_C(0x80000000)) || !(x&UINT32_C(0x7fffffff)) ? 0 : x; }
static uint32_t length_bits(MtlxVerifiedBitVec3 v) {
    uint32_t xy=mtlx_vf_add(mtlx_vf_mul(v.x,v.x).bits,mtlx_vf_mul(v.y,v.y).bits).bits;
    return mtlx_vf_sqrt(mtlx_vf_add(xy,mtlx_vf_mul(v.z,v.z).bits).bits).bits;
}
static MtlxVerifiedBitVec3 scale(MtlxVerifiedBitVec3 v,uint32_t f) {
    const MtlxVerifiedBitVec3 out={mtlx_vf_mul(v.x,f).bits,mtlx_vf_mul(v.y,f).bits,mtlx_vf_mul(v.z,f).bits};
    return out;
}
static MtlxVerifiedVndfTrace invalid_trace(void) {
    const uint32_t n=UINT32_C(0x7fc00000);
    const MtlxVerifiedBitVec3 v={n,n,n};
    const MtlxVerifiedVndfTrace out={v,v,v,v,n,n,n,n,n,n,0};
    return out;
}

MtlxVerifiedVndfTrace mtlx_vf_vndf24_trace(uint32_t ax,uint32_t ay,
    MtlxVerifiedBitVec3 view,uint32_t azimuth_word,uint32_t height_word) {
    const uint32_t one=UINT32_C(0x3f800000);
    if(!positive_bits(ax)||ax>one||!positive_bits(ay)||ay>one||
       !finite_vec(view)||!positive_bits(view.z)||!mtlx_verified_float_supported())return invalid_trace();
    MtlxVerifiedVndfTrace out={0};
    const MtlxVerifiedBitVec3 av={mtlx_vf_mul(ax,view.x).bits,mtlx_vf_mul(ay,view.y).bits,view.z};
    out.stretched_length=length_bits(av);
    if(!finite_vec(av)||!positive_bits(out.stretched_length))return invalid_trace();
    out.stretched=scale(av,mtlx_vf_div(one,out.stretched_length).bits);
    if(!finite_vec(out.stretched))return invalid_trace();
    out.u=mtlx_vf_pcg_uniform_bits(azimuth_word);
    out.v=mtlx_vf_pcg_uniform_bits(height_word);
    out.phase=mtlx_vf_mul(UINT32_C(0x40c90fdb),out.u).bits;
    out.cap.z=mtlx_vf_sub(mtlx_vf_mul(mtlx_vf_sub(one,out.v).bits,
        mtlx_vf_add(one,out.stretched.z).bits).bits,out.stretched.z).bits;
    const uint32_t q=mtlx_vf_sub(one,mtlx_vf_mul(out.cap.z,out.cap.z).bits).bits;
    if(!finite_bits(q))return invalid_trace();
    out.radial=mtlx_vf_sqrt(nonnegative(q)).bits;
    MtlxVerifiedSinCos trig;
    if(!mtlx_verified_sincos(out.phase,&trig)||trig.invalid)return invalid_trace();
    out.cap.x=mtlx_vf_mul(out.radial,trig.cos_bits).bits;
    out.cap.y=mtlx_vf_mul(out.radial,trig.sin_bits).bits;
    const MtlxVerifiedBitVec3 sum={mtlx_vf_add(out.cap.x,out.stretched.x).bits,
        mtlx_vf_add(out.cap.y,out.stretched.y).bits,mtlx_vf_add(out.cap.z,out.stretched.z).bits};
    if(!finite_vec(out.cap)||!finite_vec(sum))return invalid_trace();
    out.raw.x=mtlx_vf_mul(ax,sum.x).bits;
    out.raw.y=mtlx_vf_mul(ay,sum.y).bits;
    out.raw.z=nonnegative(sum.z);
    out.raw_length=length_bits(out.raw);
    if(!finite_vec(out.raw)||!positive_bits(out.raw_length))return invalid_trace();
    out.half=scale(out.raw,mtlx_vf_div(one,out.raw_length).bits);
    if(!finite_vec(out.half)||!finite_bits(out.u)||!finite_bits(out.v)||
       !finite_bits(out.phase)||!finite_bits(out.radial))return invalid_trace();
    out.valid=1;
    return out;
}
