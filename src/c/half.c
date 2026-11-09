/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2024-Present Light Transport Entertainment Inc.
 *
 * half.c -- C11 IEEE 754 half (float16) <-> float32 conversion.
 *
 * Crate stores half/half2/half3/half4/quath as packed uint16 components. The
 * next Value model has no 16-bit storage, so half arrays materialize into a
 * float buffer (widening is exact) and re-encode by narrowing back to half
 * (round-to-nearest-even). Half-origin values round-trip byte-exact: a float
 * widened from a half has zero low mantissa bits, so narrowing reproduces it.
 *
 * This is the C core of src/next/crate/crate-format.hh's HalfToFloat /
 * FloatToHalf; the C++ header forwards here so the bit twiddling lives once and
 * is available to C consumers (e.g. the C ABI).
 */

#include "lightusd-core-c.h"

#include <string.h>

float lu_half_to_float(uint16_t h) {
  uint32_t sign = (uint32_t)(h >> 15) & 1u;
  uint32_t exp = (uint32_t)(h >> 10) & 0x1Fu;
  uint32_t mant = (uint32_t)h & 0x3FFu;
  uint32_t f;
  if (exp == 0) {
    if (mant == 0) {
      f = sign << 31; /* +/- zero */
    } else {
      /* Subnormal half -> normalized float. */
      exp = 1;
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3FFu;
      f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
  } else if (exp == 0x1Fu) {
    f = (sign << 31) | (0xFFu << 23) | (mant << 13); /* inf / nan */
  } else {
    f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  {
    float out;
    memcpy(&out, &f, sizeof(out));
    return out;
  }
}

uint16_t lu_float_to_half(float value) {
  uint32_t f;
  memcpy(&f, &value, sizeof(f));
  uint16_t sign = (uint16_t)((f >> 16) & 0x8000u);
  uint32_t fexp = (f >> 23) & 0xFFu;
  uint32_t mant = f & 0x7FFFFFu;

  if (fexp == 0xFFu) { /* inf / nan */
    return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
  }
  int32_t exp = (int32_t)fexp - 127 + 15;
  if (exp >= 0x1F) {
    return (uint16_t)(sign | 0x7C00u); /* overflow -> inf */
  }
  if (exp <= 0) {
    if (exp < -10) return sign; /* underflow -> zero */
    mant |= 0x800000u;
    uint32_t shift = (uint32_t)(14 - exp);
    uint32_t hm = mant >> shift;
    uint32_t rem = mant & ((1u << shift) - 1u);
    uint32_t half = 1u << (shift - 1);
    if (rem > half || (rem == half && (hm & 1u))) hm++; /* round to nearest even */
    return (uint16_t)(sign | hm);
  }
  {
    uint16_t h = (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
    uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++; /* round to nearest even */
    return h;
  }
}
