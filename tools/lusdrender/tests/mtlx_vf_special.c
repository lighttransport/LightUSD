// Native verified binary32 ops versus the special-value theorems of
// verif/lean/MtlxSpec/Binary32Special.lean (exact bits and per-op flags).
#include <stdint.h>
#include <stdio.h>

#include "verified_float.h"

typedef MtlxVerifiedFloatResult (*Op)(uint32_t, uint32_t);
static int fails = 0;
static void check(const char *name, Op op, uint32_t x, uint32_t y, uint32_t bits, unsigned flags) {
  MtlxVerifiedFloatResult r = op(x, y);
  if (r.bits != bits || r.flags != flags) {
    printf("FAIL %s(%08x,%08x) = %08x flags %u, model %08x flags %u\n", name, x, y, r.bits, r.flags, bits, flags);
    fails++;
  }
}
#define QNAN 0x7fc00000u
#define SNAN 0x7f800001u
#define PNAN 0x7fc12345u /* quiet NaN with payload */
int main(void) {
  if (!mtlx_verified_float_supported()) { printf("unsupported host\n"); return 77; }
  /* NaN propagation: model returns the canonical quiet NaN; invalid only for signalling */
  check("add", mtlx_vf_add, PNAN, 0x3f800000, QNAN, 0);
  check("add", mtlx_vf_add, 0x3f800000, PNAN, QNAN, 0);
  check("mul", mtlx_vf_mul, PNAN, 0x40000000, QNAN, 0);
  check("div", mtlx_vf_div, 0x40000000, PNAN, QNAN, 0);
  check("add", mtlx_vf_add, SNAN, 0x3f800000, QNAN, MTLX_VF_INVALID);
  /* invalid operations */
  check("mul", mtlx_vf_mul, 0x7f800000, 0x00000000, QNAN, MTLX_VF_INVALID);
  check("mul", mtlx_vf_mul, 0x80000000, 0xff800000, QNAN, MTLX_VF_INVALID);
  check("div", mtlx_vf_div, 0x00000000, 0x80000000, QNAN, MTLX_VF_INVALID);
  check("div", mtlx_vf_div, 0x7f800000, 0xff800000, QNAN, MTLX_VF_INVALID);
  check("add", mtlx_vf_add, 0x7f800000, 0xff800000, QNAN, MTLX_VF_INVALID);
  check("sub", mtlx_vf_sub, 0x7f800000, 0x7f800000, QNAN, MTLX_VF_INVALID);
  /* exact infinities */
  check("add", mtlx_vf_add, 0x7f800000, 0x7f800000, 0x7f800000, 0);
  check("mul", mtlx_vf_mul, 0xff800000, 0x40000000, 0xff800000, 0);
  check("div", mtlx_vf_div, 0x3f800000, 0x80000000, 0xff800000, MTLX_VF_DIVIDE_BY_ZERO);
  check("div", mtlx_vf_div, 0xbf800000, 0x00000000, 0xff800000, MTLX_VF_DIVIDE_BY_ZERO);
  check("div", mtlx_vf_div, 0x3f800000, 0xff800000, 0x80000000, 0);
  /* signed zeros under round-to-nearest */
  check("add", mtlx_vf_add, 0x00000000, 0x80000000, 0x00000000, 0);
  check("add", mtlx_vf_add, 0x80000000, 0x80000000, 0x80000000, 0);
  check("sub", mtlx_vf_sub, 0x3f800000, 0x3f800000, 0x00000000, 0);
  check("sub", mtlx_vf_sub, 0x00000001, 0x00000001, 0x00000000, 0);
  check("mul", mtlx_vf_mul, 0x80000000, 0x3f800000, 0x80000000, 0);
  check("mul", mtlx_vf_mul, 0x80000000, 0xbf800000, 0x00000000, 0);
  printf("%s: %d mismatches\n", fails ? "FAIL" : "ok", fails);
  return fails != 0;
}
