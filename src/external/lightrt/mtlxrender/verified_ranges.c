#include "verified_ranges.h"
#include "verified_float.h"
#include <fenv.h>
#include <string.h>

#ifdef __FAST_MATH__
#error "verified_ranges.c requires strict floating-point compilation"
#endif

static MtlxVerifiedFloatBounds invalid_range(void) {
    MtlxVerifiedFloatBounds r={UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),0};
    return r;
}
static float as_float(uint32_t bits) { float f; memcpy(&f,&bits,sizeof(f)); return f; }
static int finite_bits(uint32_t bits) { return (bits & UINT32_C(0x7fffffff))<UINT32_C(0x7f800000); }
static int well_formed(MtlxVerifiedFloatBounds a) {
    return a.valid==1 && finite_bits(a.lower) && finite_bits(a.upper) && as_float(a.lower)<=as_float(a.upper);
}
static MtlxVerifiedFloatBounds merge(MtlxVerifiedFloatBounds lower,MtlxVerifiedFloatBounds upper) {
    if (!lower.valid || !upper.valid) return invalid_range();
    MtlxVerifiedFloatBounds r={lower.lower,upper.upper,1};
    return r;
}
static MtlxVerifiedFloatBounds hull(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    MtlxVerifiedFloatBounds r={as_float(a.lower)<=as_float(b.lower)?a.lower:b.lower,
        as_float(a.upper)>=as_float(b.upper)?a.upper:b.upper,1};
    return r;
}
static MtlxVerifiedFloatBounds checked_impl(MtlxVerifiedFloatBounds a) {
    return well_formed(a)?a:invalid_range();
}
static MtlxVerifiedFloatBounds add_impl(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    if (!well_formed(a) || !well_formed(b)) return invalid_range();
    return merge(mtlx_vf_bound_add(a.lower,b.lower),mtlx_vf_bound_add(a.upper,b.upper));
}
static MtlxVerifiedFloatBounds sub_impl(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    if (!well_formed(a) || !well_formed(b)) return invalid_range();
    return merge(mtlx_vf_bound_sub(a.lower,b.upper),mtlx_vf_bound_sub(a.upper,b.lower));
}
static MtlxVerifiedFloatBounds mul_impl(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    if (!well_formed(a) || !well_formed(b)) return invalid_range();
    MtlxVerifiedFloatBounds p=mtlx_vf_bound_mul(a.lower,b.lower);
    if (!p.valid) return invalid_range();
    MtlxVerifiedFloatBounds q=mtlx_vf_bound_mul(a.lower,b.upper);
    if (!q.valid) return invalid_range();
    MtlxVerifiedFloatBounds r=mtlx_vf_bound_mul(a.upper,b.lower);
    if (!r.valid) return invalid_range();
    MtlxVerifiedFloatBounds s=mtlx_vf_bound_mul(a.upper,b.upper);
    if (!s.valid) return invalid_range();
    return hull(hull(p,q),hull(r,s));
}
static MtlxVerifiedFloatBounds inv_impl(MtlxVerifiedFloatBounds a) {
    if (!well_formed(a) || !(as_float(a.lower)>0.0f || as_float(a.upper)<0.0f)) return invalid_range();
    return merge(mtlx_vf_bound_div(UINT32_C(0x3f800000),a.upper),
                 mtlx_vf_bound_div(UINT32_C(0x3f800000),a.lower));
}
static MtlxVerifiedFloatBounds div_impl(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    MtlxVerifiedFloatBounds inverse=inv_impl(b);
    if (!inverse.valid) return invalid_range();
    return mul_impl(a,inverse);
}
static MtlxVerifiedFloatBounds neg_impl(MtlxVerifiedFloatBounds a) {
    if (!well_formed(a)) return invalid_range();
    MtlxVerifiedFloatBounds r={a.upper^UINT32_C(0x80000000),a.lower^UINT32_C(0x80000000),1};
    return r;
}
static MtlxVerifiedFloatBounds sqrt_impl(MtlxVerifiedFloatBounds a) {
    if (!well_formed(a) || as_float(a.lower)<0.0f) return invalid_range();
    return merge(mtlx_vf_bound_sqrt(a.lower),mtlx_vf_bound_sqrt(a.upper));
}

static MtlxVerifiedFloatBounds exact_add_impl(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    if(well_formed(a) && well_formed(b) && a.lower==a.upper && b.lower==b.upper) {
        MtlxVerifiedFloatResult result=mtlx_vf_add(a.lower,b.lower);
        if(!(result.flags&MTLX_VF_INEXACT) && finite_bits(result.bits)) {
            MtlxVerifiedFloatBounds out={result.bits,result.bits,1};return out;
        }
    }
    return add_impl(a,b);
}
static MtlxVerifiedFloatBounds exact_mul_impl(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    if(well_formed(a) && well_formed(b) && a.lower==a.upper && b.lower==b.upper) {
        MtlxVerifiedFloatResult result=mtlx_vf_mul(a.lower,b.lower);
        if(!(result.flags&MTLX_VF_INEXACT) && finite_bits(result.bits)) {
            MtlxVerifiedFloatBounds out={result.bits,result.bits,1};return out;
        }
    }
    return mul_impl(a,b);
}

static MtlxVerifiedFloatBounds hull_impl(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) {
    if(!well_formed(a) || !well_formed(b))return invalid_range();
    return hull(a,b);
}

static int begin(fenv_t *env) { return mtlx_verified_float_supported() && feholdexcept(env)==0; }
static MtlxVerifiedFloatBounds finish(MtlxVerifiedFloatBounds r,const fenv_t *env) {
    if (fesetenv(env)) return invalid_range();
    return r;
}
#define RANGE_BINARY(name,implementation) \
MtlxVerifiedFloatBounds name(MtlxVerifiedFloatBounds a,MtlxVerifiedFloatBounds b) { \
    fenv_t env; if (!begin(&env)) return invalid_range(); \
    return finish(implementation(a,b),&env); \
}
#define RANGE_UNARY(name,implementation) \
MtlxVerifiedFloatBounds name(MtlxVerifiedFloatBounds a) { \
    fenv_t env; if (!begin(&env)) return invalid_range(); \
    return finish(implementation(a),&env); \
}
RANGE_BINARY(mtlx_vf_range_hull,hull_impl)
RANGE_BINARY(mtlx_vf_range_exact_add,exact_add_impl)
RANGE_BINARY(mtlx_vf_range_exact_mul,exact_mul_impl)
RANGE_BINARY(mtlx_vf_range_add,add_impl)
RANGE_BINARY(mtlx_vf_range_sub,sub_impl)
RANGE_BINARY(mtlx_vf_range_mul,mul_impl)
RANGE_BINARY(mtlx_vf_range_div,div_impl)
RANGE_UNARY(mtlx_vf_range_checked,checked_impl)
RANGE_UNARY(mtlx_vf_range_neg,neg_impl)
RANGE_UNARY(mtlx_vf_range_inv,inv_impl)
RANGE_UNARY(mtlx_vf_range_sqrt,sqrt_impl)
#undef RANGE_BINARY
#undef RANGE_UNARY

MtlxVerifiedFloatBounds mtlx_vf_range_point(uint32_t x) {
    fenv_t env; if (!begin(&env)) return invalid_range();
    MtlxVerifiedFloatBounds r={x,x,1};
    return finish(finite_bits(x)?r:invalid_range(),&env);
}

int mtlx_vf_range_sign(MtlxVerifiedFloatBounds a) {
    fenv_t env; if (!begin(&env)) return 0;
    int sign=0;
    if (well_formed(a)) {
        if (as_float(a.upper)<=0.0f) sign=1;
        else if (as_float(a.lower)>0.0f) sign=2;
    }
    if (fesetenv(&env)) return 0;
    return sign;
}
