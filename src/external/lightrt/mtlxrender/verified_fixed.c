#include "verified_fixed.h"

static MtlxVerifiedFixedBounds invalid_fixed(void) {
    MtlxVerifiedFixedBounds r={0,0,0};return r;
}
static MtlxVerifiedFixedBounds pair(uint64_t lower,uint64_t upper) {
    MtlxVerifiedFixedBounds r={lower,upper,1};return r;
}
static uint64_t min64(uint64_t a,uint64_t b) {return a<b?a:b;}

MtlxVerifiedFixedBounds mtlx_vf_fixed_scaled(uint32_t bits,unsigned shift,uint64_t cap) {
    uint32_t magnitude=bits&UINT32_C(0x7fffffff),exponent=magnitude>>23;
    if(exponent==255 || shift>46 || cap>MTLX_VERIFIED_FIXED_SCALE)return invalid_fixed();
    if((bits&UINT32_C(0x80000000)) || magnitude==0 || cap==0)return pair(0,0);
    uint64_t mantissa=magnitude&UINT32_C(0x007fffff);
    int power;
    if(exponent==0)power=-101-(int)shift;
    else {mantissa|=UINT32_C(0x00800000);power=(int)exponent-102-(int)shift;}
    if(power>=0) {
        unsigned left=(unsigned)power;
        /* Compare before shifting, so neither overflow nor shift-by-64 occurs. */
        if(left>=64 || mantissa>(cap>>left))return pair(cap,cap);
        uint64_t exact=mantissa<<left;return pair(exact,exact);
    }
    unsigned right=(unsigned)(-power);
    if(right>=64)return pair(0,min64(cap,1));
    uint64_t floor=mantissa>>right;
    uint64_t remainder=mantissa&((UINT64_C(1)<<right)-1);
    return pair(min64(cap,floor),min64(cap,floor+(remainder!=0)));
}

MtlxVerifiedFixedBounds mtlx_vf_fixed_add(MtlxVerifiedFixedBounds a,MtlxVerifiedFixedBounds b) {
    if(a.valid!=1 || b.valid!=1 || a.lower>a.upper || b.lower>b.upper ||
       UINT64_MAX-a.upper<b.upper || UINT64_MAX-a.lower<b.lower)return invalid_fixed();
    return pair(a.lower+b.lower,a.upper+b.upper);
}
int mtlx_vf_fixed_width_accepted(MtlxVerifiedFixedBounds a) {
    return a.valid==1 && a.lower<=a.upper &&
        a.upper-a.lower<=MTLX_VERIFIED_FIXED_SCALE/UINT64_C(5000);
}

uint32_t mtlx_vf_fixed_upper(uint64_t upper) {
    if(upper>MTLX_VERIFIED_FIXED_SCALE)return UINT32_C(0x7fc00000);
    if(upper==0)return 0;
    unsigned leading=0;
    for(uint64_t n=upper;n>1;n>>=1)++leading;
    uint64_t significand;
    if(leading<=23)significand=upper<<(23u-leading);
    else {
        unsigned shift=leading-23u;
        uint64_t mask=(UINT64_C(1)<<shift)-1;
        significand=(upper>>shift)+((upper&mask)!=0);
    }
    /* Addition deliberately propagates a 24-bit significand carry into the
     * exponent. All intermediate values are bounded by the upper<=Q guard. */
    return ((79u+leading)<<23)+(uint32_t)(significand-UINT64_C(8388608));
}
