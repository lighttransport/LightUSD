#include "verified_math.h"
#include "verified_float.h"
#include "verified_trig_constants.h"
#include <fenv.h>
#include <string.h>

#ifdef __FAST_MATH__
#error "verified_math.c requires strict floating-point compilation"
#endif
#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif

static uint32_t add_bits(uint32_t a, uint32_t b) {
    float x,y;
    uint32_t bits;
    memcpy(&x,&a,sizeof(x)); memcpy(&y,&b,sizeof(y));
    volatile float result=x+y;
    float value=result;
    memcpy(&bits,&value,sizeof(bits));
    return bits;
}
static uint32_t mul_bits(uint32_t a, uint32_t b) {
    float x,y;
    uint32_t bits;
    memcpy(&x,&a,sizeof(x)); memcpy(&y,&b,sizeof(y));
    volatile float result=x*y;
    float value=result;
    memcpy(&bits,&value,sizeof(bits));
    return bits;
}

/* Exact nearest-even encoding of signed magnitude * 2^-64. All nonzero
 * inputs here are normal binary32; no host integer-to-float conversion. */
static uint32_t fixed64_bits(uint64_t magnitude, unsigned negative) {
    if (!magnitude) return 0;
    unsigned p=0;
    for (uint64_t v=magnitude; (v>>=1)!=0;) ++p;
    uint32_t mantissa;
    if (p<=23) mantissa=(uint32_t)(magnitude<<(23-p));
    else {
        unsigned shift=p-23;
        mantissa=(uint32_t)(magnitude>>shift);
        uint64_t remainder=magnitude&((UINT64_C(1)<<shift)-1);
        uint64_t halfway=UINT64_C(1)<<(shift-1);
        if (remainder>halfway || (remainder==halfway && (mantissa&1))) {
            ++mantissa;
            if (mantissa==UINT32_C(0x1000000)) { mantissa>>=1; ++p; }
        }
    }
    return (negative ? UINT32_C(0x80000000) : 0) |
        ((uint32_t)(p+63)<<23) | (mantissa&UINT32_C(0x7fffff));
}

static unsigned product_bit(const uint32_t p[9], unsigned bit) {
    return bit<288 ? ((p[bit/32]>>(bit%32))&1u) : 0u;
}

static uint32_t reduce(uint32_t magnitude, unsigned *quadrant) {
    unsigned exponent=magnitude>>23;
    uint32_t mantissa=(magnitude&UINT32_C(0x7fffff))|UINT32_C(0x800000);
    uint32_t product[9];
    uint64_t carry=0;
    for (unsigned i=0;i<8;++i) {
        uint64_t v=(uint64_t)mtlx_two_over_pi256[i]*mantissa+carry;
        product[i]=(uint32_t)v;
        carry=v>>32;
    }
    product[8]=(uint32_t)carry;
    /* x=(mantissa)*2^(exponent-150); coefficient has 256 fractional bits. */
    unsigned denominator_bits=406-exponent;
    unsigned up=product_bit(product,denominator_bits-1);
    *quadrant=(product_bit(product,denominator_bits)+
        2*product_bit(product,denominator_bits+1)+up)&3u;
    uint64_t fraction=0;
    for (unsigned i=0;i<64;++i)
        fraction|=(uint64_t)product_bit(product,denominator_bits-64+i)<<i;
    uint64_t absolute=up ? UINT64_C(0)-fraction : fraction;
    return mul_bits(fixed64_bits(absolute,up),MTLX_HALF_PI_BITS);
}

static uint32_t horner(const uint32_t *coefficients, unsigned count, uint32_t z) {
    uint32_t value=coefficients[count-1];
    while (--count) value=add_bits(coefficients[count-1],mul_bits(z,value));
    return value;
}

int mtlx_verified_sincos(uint32_t input, MtlxVerifiedSinCos *out) {
    if (!out) return 0;
    out->sin_bits=out->cos_bits=UINT32_C(0x7fc00000);
    out->invalid=1;
    if (!mtlx_verified_float_supported()) return 0;
    uint32_t magnitude=input&UINT32_C(0x7fffffff);
    if (magnitude>=UINT32_C(0x7f800000)) {
        out->invalid=magnitude==UINT32_C(0x7f800000) ||
            !(magnitude&UINT32_C(0x00400000));
        return 1;
    }
    if (magnitude<=UINT32_C(0x39800000)) {
        out->sin_bits=input; out->cos_bits=UINT32_C(0x3f800000); out->invalid=0;
        return 1;
    }
    fenv_t env;
    if (feholdexcept(&env)) return 0;
    unsigned q;
    uint32_t r=reduce(magnitude,&q), z=mul_bits(r,r);
    uint32_t s=mul_bits(r,horner(mtlx_sin_coefficients,7,z));
    uint32_t c=horner(mtlx_cos_coefficients,8,z);
    switch (q) {
        case 0: out->sin_bits=s; out->cos_bits=c; break;
        case 1: out->sin_bits=c; out->cos_bits=s^UINT32_C(0x80000000); break;
        case 2: out->sin_bits=s^UINT32_C(0x80000000); out->cos_bits=c^UINT32_C(0x80000000); break;
        default: out->sin_bits=c^UINT32_C(0x80000000); out->cos_bits=s; break;
    }
    if (input&UINT32_C(0x80000000)) out->sin_bits^=UINT32_C(0x80000000);
    out->invalid=0;
    return fesetenv(&env)==0;
}
