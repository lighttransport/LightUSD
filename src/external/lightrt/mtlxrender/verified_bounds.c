#include "verified_bounds.h"
#include "verified_float.h"

#ifdef __FAST_MATH__
#error "verified_bounds.c requires strict floating-point compilation"
#endif

static MtlxVerifiedFloatBounds unsupported(void) {
    MtlxVerifiedFloatBounds out={UINT32_C(0x7fc00000),UINT32_C(0x7fc00000),0};
    return out;
}

static MtlxVerifiedFloatBounds enclose(uint32_t bits) {
    uint32_t magnitude=bits & UINT32_C(0x7fffffff);
    if (magnitude>=UINT32_C(0x7f7fffff)) return unsupported();
    MtlxVerifiedFloatBounds out;
    if (bits & UINT32_C(0x80000000)) {
        out.lower=bits+1;
        out.upper=magnitude ? bits-1 : UINT32_C(1);
    } else {
        out.lower=magnitude ? bits-1 : UINT32_C(0x80000001);
        out.upper=bits+1;
    }
    out.valid=1;
    return out;
}

#define BOUND_OPERATION(name, operation) \
MtlxVerifiedFloatBounds name(uint32_t x, uint32_t y) { \
    if ((x & UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000) || \
        (y & UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000)) return unsupported(); \
    if (!mtlx_verified_float_supported()) return unsupported(); \
    return enclose(operation(x,y).bits); \
}
BOUND_OPERATION(mtlx_vf_bound_add,mtlx_vf_add)
BOUND_OPERATION(mtlx_vf_bound_sub,mtlx_vf_sub)
BOUND_OPERATION(mtlx_vf_bound_mul,mtlx_vf_mul)
BOUND_OPERATION(mtlx_vf_bound_div,mtlx_vf_div)
#undef BOUND_OPERATION

MtlxVerifiedFloatBounds mtlx_vf_bound_sqrt(uint32_t x) {
    if ((x & UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000)) return unsupported();
    if (!mtlx_verified_float_supported()) return unsupported();
    return enclose(mtlx_vf_sqrt(x).bits);
}
