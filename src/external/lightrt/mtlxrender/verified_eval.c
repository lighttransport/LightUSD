#include "verified_math.h"
#include "verified_float.h"
#include "mtlx_eval.h"
#include <string.h>

static int graph_verified_sincos(float input, float *sine, float *cosine) {
    uint32_t bits;
    MtlxVerifiedSinCos result;
    memcpy(&bits, &input, sizeof(bits));
    if (!mtlx_verified_sincos(bits, &result)) return 0;
    memcpy(sine, &result.sin_bits, sizeof(*sine));
    memcpy(cosine, &result.cos_bits, sizeof(*cosine));
    return 1;
}

int mtlx_verified_math_enable(int enabled) {
    if (enabled && !mtlx_verified_float_supported()) return 0;
    mtlx_eval_set_sincos_backend(enabled ? graph_verified_sincos : NULL);
    return 1;
}
