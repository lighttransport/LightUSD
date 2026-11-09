// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
#include "value-types.hh"

#include <type_traits>


//
#include "common-macros.inc"

namespace lightusd {
namespace value {

bool IsReasonableValueVectorSize(size_t size) {
  constexpr size_t kMaxReasonableSize = 100000000;  // 100M
  if (size > kMaxReasonableSize) {
    DCOUT("ERROR: Vector size " << size << " exceeds reasonable limit ("
                                << kMaxReasonableSize
                                << "). Data is likely corrupted!");
    return false;
  }
  return true;
}

// Static member definition for ValueView
// This is a placeholder value used for type checking - the warnings are acceptable here
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"

Value ValueView::value_placeholder_;
#pragma clang diagnostic pop
#endif // __clang__

//
// Supported type for `Linear` interpolation
//
// half, float, double, TimeCode(double)
// matrix2d, matrix3d, matrix4d,
// float2h, float3h, float4h
// float2f, float3f, float4f
// float2d, float3d, float4d
// quath, quatf, quatd
// (use slerp for quaternion type)

namespace {

// https://www.realtime.bc.ca/articles/endian-safe.html
union HostEndianness {
  int i;
  char c[sizeof(int)];

  HostEndianness() : i(1) {}

  bool isBig() const { return c[0] == 0; }
  bool isLittle() const { return c[0] != 0; }
};

// https://gist.github.com/rygorous/2156668
// Little endian
union FP32le {
  unsigned int u;
  float f;
  struct {
    unsigned int Mantissa : 23;
    unsigned int Exponent : 8;
    unsigned int Sign : 1;
  } s;
};

// Big endian
union FP32be {
  unsigned int u;
  float f;
  struct {
    unsigned int Sign : 1;
    unsigned int Exponent : 8;
    unsigned int Mantissa : 23;
  } s;
};

// Little endian
union float16le {
  unsigned short u;
  struct {
    unsigned int Mantissa : 10;
    unsigned int Exponent : 5;
    unsigned int Sign : 1;
  } s;
};

// Big endian
union float16be {
  unsigned short u;
  struct {
    unsigned int Sign : 1;
    unsigned int Exponent : 5;
    unsigned int Mantissa : 10;
  } s;
};

// half_to_float_le/be moved to inline in value-types.hh

half float_to_half_full_be(float _f) {
  FP32be f;
  f.f = _f;
  float16be o = {0};

  // Based on ISPC reference code (with minor modifications)
  if (f.s.Exponent == 0)  // Signed zero/denormal (which will underflow)
    o.s.Exponent = 0;
  else if (f.s.Exponent == 255)  // Inf or NaN (all exponent bits set)
  {
    o.s.Exponent = 31;
    o.s.Mantissa = f.s.Mantissa ? 0x200 : 0;  // NaN->qNaN and Inf->Inf
  } else                                      // Normalized number
  {
    // Exponent unbias the single, then bias the halfp
    int newexp = f.s.Exponent - 127 + 15;
    if (newexp >= 31)  // Overflow, return signed infinity
      o.s.Exponent = 31;
    else if (newexp <= 0)  // Underflow
    {
      if ((14 - newexp) <= 24)  // Mantissa might be non-zero
      {
        unsigned int mant = f.s.Mantissa | 0x800000;  // Hidden 1 bit
        o.s.Mantissa = mant >> (14 - newexp);
        if ((mant >> (13 - newexp)) & 1)  // Check for rounding
          o.u++;  // Round, might overflow into exp bit, but this is OK
      }
    } else {
      o.s.Exponent = static_cast<unsigned int>(newexp);
      o.s.Mantissa = f.s.Mantissa >> 13;
      if (f.s.Mantissa & 0x1000)  // Check for rounding
        o.u++;                    // Round, might overflow to inf, this is OK
    }
  }

  o.s.Sign = f.s.Sign;

  half ret;
  ret.value = (*reinterpret_cast<const uint16_t *>(&o));

  return ret;
}

half float_to_half_full_le(float _f) {
  FP32le f;
  f.f = _f;
  float16le o = {0};

  // Based on ISPC reference code (with minor modifications)
  if (f.s.Exponent == 0)  // Signed zero/denormal (which will underflow)
    o.s.Exponent = 0;
  else if (f.s.Exponent == 255)  // Inf or NaN (all exponent bits set)
  {
    o.s.Exponent = 31;
    o.s.Mantissa = f.s.Mantissa ? 0x200 : 0;  // NaN->qNaN and Inf->Inf
  } else                                      // Normalized number
  {
    // Exponent unbias the single, then bias the halfp
    int newexp = f.s.Exponent - 127 + 15;
    if (newexp >= 31)  // Overflow, return signed infinity
      o.s.Exponent = 31;
    else if (newexp <= 0)  // Underflow
    {
      if ((14 - newexp) <= 24)  // Mantissa might be non-zero
      {
        unsigned int mant = f.s.Mantissa | 0x800000;  // Hidden 1 bit
        o.s.Mantissa = mant >> (14 - newexp);
        if ((mant >> (13 - newexp)) & 1)  // Check for rounding
          o.u++;  // Round, might overflow into exp bit, but this is OK
      }
    } else {
      o.s.Exponent = static_cast<unsigned int>(newexp);
      o.s.Mantissa = f.s.Mantissa >> 13;
      if (f.s.Mantissa & 0x1000)  // Check for rounding
        o.u++;                    // Round, might overflow to inf, this is OK
    }
  }

  o.s.Sign = f.s.Sign;

  half ret;
  ret.value = (*reinterpret_cast<const uint16_t *>(&o));
  return ret;
}

}  // namespace

float half_to_float(value::half h) {
  uint16_t hu = h.value;
  uint32_t o;

  o = (hu & 0x7fffU) << 13U;              // exponent/mantissa bits
  uint32_t exp_ = (0x7c00U << 13U) & o;   // just the exponent
  o += (127 - 15) << 23;                   // exponent adjust

  if (exp_ == (0x7c00U << 13U))            // Inf/NaN?
    o += (128 - 16) << 23;                 // extra exp adjust
  else if (exp_ == 0) {                    // Zero/Denormal?
    o += 1 << 23;                          // extra exp adjust
    float of;
    memcpy(&of, &o, sizeof(float));
    uint32_t magic = 113 << 23;
    float magicf;
    memcpy(&magicf, &magic, sizeof(float));
    of -= magicf;                          // renormalize
    memcpy(&o, &of, sizeof(uint32_t));
  }

  o |= (hu & 0x8000U) << 16U;             // sign bit
  float result;
  memcpy(&result, &o, sizeof(float));
  return result;
}

half float_to_half_full(float _f) {
  // TODO: Compile time detection of endianness
  HostEndianness endian;

  if (endian.isBig()) {
    return float_to_half_full_be(_f);
  } else if (endian.isLittle()) {
    return float_to_half_full_le(_f);
  }

  ///???
  half fp16{0};  // TODO: Raise exception or return NaN
  return fp16;
}

// half arithmetic operators

half operator+(const half &a, const half &b) {
  return float_to_half_full(half_to_float(a) + half_to_float(b));
}

half operator-(const half &a, const half &b) {
  return float_to_half_full(half_to_float(a) - half_to_float(b));
}

half operator*(const half &a, const half &b) {
  return float_to_half_full(half_to_float(a) * half_to_float(b));
}

half operator/(const half &a, const half &b) {
  return float_to_half_full(half_to_float(a) / half_to_float(b));
}

half& operator+=(half &a, const half &b) {
  a = float_to_half_full(half_to_float(a) + half_to_float(b));
  return a;
}

half& operator-=(half &a, const half &b) {
  a = float_to_half_full(half_to_float(a) - half_to_float(b));
  return a;
}

half& operator*=(half &a, const half &b) {
  a = float_to_half_full(half_to_float(a) * half_to_float(b));
  return a;
}

half& operator/=(half &a, const half &b) {
  a = float_to_half_full(half_to_float(a) / half_to_float(b));
  return a;
}

half operator+(const half &a, float b) {
  return float_to_half_full(half_to_float(a) + b);
}

half operator-(const half &a, float b) {
  return float_to_half_full(half_to_float(a) - b);
}

half operator*(const half &a, float b) {
  return float_to_half_full(half_to_float(a) * b);
}

half operator/(const half &a, float b) {
  return float_to_half_full(half_to_float(a) / b);
}

half operator+(float a, const half &b) {
  return float_to_half_full(a + half_to_float(b));
}

half operator-(float a, const half &b) {
  return float_to_half_full(a - half_to_float(b));
}

half operator*(float a, const half &b) {
  return float_to_half_full(a * half_to_float(b));
}

half operator/(float a, const half &b) {
  return float_to_half_full(a / half_to_float(b));
}


}  // namespace value
}  // namespace lightusd
