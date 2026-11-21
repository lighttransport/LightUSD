#include "verified_float.h"

#include <fenv.h>
#include <float.h>
#include <math.h>
#include <string.h>

#ifdef __FAST_MATH__
#error "verified_float.c requires strict floating-point compilation"
#endif
#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif

uint32_t mtlx_vf_clamp_unit(uint32_t x) {
    uint32_t magnitude=x&UINT32_C(0x7fffffff);
    if(magnitude>=UINT32_C(0x7f800000) || (x&UINT32_C(0x80000000)))return 0;
    return x>=UINT32_C(0x3f800000)?UINT32_C(0x3f800000):x;
}

uint32_t mtlx_vf_scale_unit_power(uint32_t factor,uint32_t power) {
    uint32_t magnitude=factor&UINT32_C(0x7fffffff);
    if(magnitude>=UINT32_C(0x7f800000) ||
       ((factor&UINT32_C(0x80000000)) && magnitude!=0) ||
       !mtlx_verified_float_supported())return UINT32_C(0x7fc00000);
    return mtlx_vf_mul(factor,mtlx_vf_clamp_unit(power)).bits;
}
static float from_bits(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
static uint32_t to_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

int mtlx_verified_float_supported(void) {
#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128 || FLT_MIN_EXP != -125 || FLT_EVAL_METHOD != 0
    return 0;
#else
    if (sizeof(float) != sizeof(uint32_t) || fegetround() != FE_TONEAREST) return 0;
    fenv_t env;
    if (feholdexcept(&env)) return 0;
    volatile float tiny = from_bits(UINT32_C(1));
    volatile float one = 1.0f;
    volatile float gradual = tiny * one;
    volatile float tie = from_bits(UINT32_C(0x3f800000)) + from_bits(UINT32_C(0x33800000));
    int valid = to_bits(gradual) == 1 && to_bits(tie) == UINT32_C(0x3f800000);
    if (fesetenv(&env)) return 0;
    return valid;
#endif
}

static MtlxVerifiedFloatResult finish(float value, const fenv_t *env) {
    int raised = fetestexcept(FE_ALL_EXCEPT);
    MtlxVerifiedFloatResult out;
    out.bits = to_bits(value);
    /* Canonical quiet NaN, as in the executable Lean model. */
    if ((out.bits & UINT32_C(0x7fffffff)) > UINT32_C(0x7f800000))
        out.bits = UINT32_C(0x7fc00000);
    out.flags = ((raised & FE_INVALID) ? MTLX_VF_INVALID : 0u) |
        ((raised & FE_DIVBYZERO) ? MTLX_VF_DIVIDE_BY_ZERO : 0u) |
        ((raised & FE_OVERFLOW) ? MTLX_VF_OVERFLOW : 0u) |
        ((raised & FE_UNDERFLOW) ? MTLX_VF_UNDERFLOW : 0u) |
        ((raised & FE_INEXACT) ? MTLX_VF_INEXACT : 0u);
    /* The model specifies after-rounding tininess, irrespective of the host's
     * hardware convention for the underflow flag at a normal boundary. */
    if (out.flags & MTLX_VF_INEXACT) {
        if ((out.bits & UINT32_C(0x7f800000)) == 0) out.flags |= MTLX_VF_UNDERFLOW;
        else out.flags &= ~MTLX_VF_UNDERFLOW;
    }
    if (fesetenv(env)) {
        out.bits = UINT32_C(0x7fc00000);
        out.flags = MTLX_VF_INVALID;
    }
    return out;
}

#define BINARY_OPERATION(name, op) \
MtlxVerifiedFloatResult name(uint32_t x, uint32_t y) { \
    fenv_t env; \
    if (feholdexcept(&env)) { \
        MtlxVerifiedFloatResult error = {UINT32_C(0x7fc00000), MTLX_VF_INVALID}; \
        return error; \
    } \
    volatile float a = from_bits(x), b = from_bits(y); \
    volatile float value = a op b; \
    return finish(value, &env); \
}
BINARY_OPERATION(mtlx_vf_add, +)
BINARY_OPERATION(mtlx_vf_sub, -)
BINARY_OPERATION(mtlx_vf_mul, *)
BINARY_OPERATION(mtlx_vf_div, /)
#undef BINARY_OPERATION

MtlxVerifiedFloatResult mtlx_vf_sqrt(uint32_t x) {
    fenv_t env;
    if (feholdexcept(&env)) {
        MtlxVerifiedFloatResult error = {UINT32_C(0x7fc00000), MTLX_VF_INVALID};
        return error;
    }
    volatile float a = from_bits(x);
    volatile float value = sqrtf(a);
    return finish(value, &env);
}
