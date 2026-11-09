// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// OpenUSD TsSpline binary blob codec. See spline-binary.hh.
//
// Layout (matches pxr/base/ts/binary.cpp), little-endian:
//   headerByte1: [0-3]=version [4-5]=typeDescriptor [6]=timeValued [7]=curveType
//   headerByte2: [0-2]=preExtrap [3-5]=postExtrap [6]=hasLoops
//   if preExtrap==Sloped(3):  double preSlope
//   if postExtrap==Sloped(3): double postSlope
//   if hasLoops: double protoStart, double protoEnd, int32 numPre, int32 numPost,
//                double valueOffset
//   uint32 knotCount
//   per knot:
//     uint8 flag: [0]=dualValued [1-2]=nextInterp [3]=curveType
//     double time
//     T value
//     if dualValued: T preValue
//     if !hermite: double preTanWidth, double postTanWidth
//     T preTanSlope, T postTanSlope
//     if version>1: uint8 algo (unused here; we always emit version 1)

#include "spline-binary.hh"

#include <cstring>
#include <limits>

namespace lightusd {

namespace {

using SplineData = primvar::PrimVar::SplineData;
using SplineKnotData = primvar::PrimVar::SplineKnotData;

template <typename T>
void Wr(std::vector<uint8_t> *buf, const T &v) {
  const size_t o = buf->size();
  buf->resize(o + sizeof(T));
  std::memcpy(buf->data() + o, &v, sizeof(T));
}

template <typename T>
bool Rd(const uint8_t **p, size_t *remain, T *out) {
  if (*remain < sizeof(T)) return false;
  std::memcpy(out, *p, sizeof(T));
  *p += sizeof(T);
  *remain -= sizeof(T);
  return true;
}

// typeDescriptor: 0=unspecified(double), 1=double, 2=float, 3=half.
int DescriptorForKnots(const SplineData &sd) {
  if (sd.knots.empty()) return 0;
  const uint32_t tid = sd.knots[0].val.type_id();
  if (tid == value::TypeTraits<float>::type_id()) return 2;
  if (tid == value::TypeTraits<value::half>::type_id()) return 3;
  return 1;  // double (and fallback)
}

// Extract a knot value::Value as a double regardless of stored scalar type.
double AsNum(const value::Value &v) {
  if (auto d = v.get_value<double>()) return d.value();
  if (auto f = v.get_value<float>()) return double(f.value());
  if (auto h = v.get_value<value::half>())
    return double(value::half_to_float(h.value()));
  return 0.0;
}

// Write a scalar of the descriptor's type from a double.
void WrTyped(std::vector<uint8_t> *buf, int desc, double v) {
  if (desc == 2) {
    Wr<float>(buf, static_cast<float>(v));
  } else if (desc == 3) {
    Wr<value::half>(buf, value::float_to_half_full(static_cast<float>(v)));
  } else {
    Wr<double>(buf, v);  // 0 (unspecified) and 1 (double)
  }
}

// Read a scalar of the descriptor's type as a double.
bool RdTyped(const uint8_t **p, size_t *remain, int desc, double *out) {
  if (desc == 2) {
    float f;
    if (!Rd<float>(p, remain, &f)) return false;
    *out = double(f);
  } else if (desc == 3) {
    value::half h;
    if (!Rd<value::half>(p, remain, &h)) return false;
    *out = double(value::half_to_float(h));
  } else {
    double d;
    if (!Rd<double>(p, remain, &d)) return false;
    *out = d;
  }
  return true;
}

// Make a value::Value of the descriptor's type from a double.
value::Value MakeTyped(int desc, double v) {
  if (desc == 2) return value::Value(static_cast<float>(v));
  if (desc == 3)
    return value::Value(value::float_to_half_full(static_cast<float>(v)));
  return value::Value(v);
}

}  // namespace

uint8_t SplineBinaryFormatVersion(const SplineData &sd) {
  // Version 2 is required as soon as any knot carries a non-None tangent
  // algorithm (mirrors pxr Ts_BinaryDataAccess::GetBinaryFormatVersion).
  for (const SplineKnotData &k : sd.knots) {
    if (k.preTangentAlgorithm != 0 || k.postTangentAlgorithm != 0) {
      return 2;
    }
  }
  return 1;
}

bool EncodeSplineToBinary(const SplineData &sd, std::vector<uint8_t> *out,
                          std::string *err) {
  if (!out) return false;
#if defined(__EMSCRIPTEN__) && !defined(__wasm64__)
  (void)err;
#endif
  out->clear();

  const int desc = DescriptorForKnots(sd);
  const bool hermite = (sd.curveType == 1);
  const uint8_t version = SplineBinaryFormatVersion(sd);

  // Header byte 1.
  uint8_t h1 = static_cast<uint8_t>(
      (version & 0x0f) | ((desc & 0x03) << 4) |
      (0 << 6) /* timeValued */ | ((sd.curveType & 0x01) << 7));
  Wr<uint8_t>(out, h1);

  // Header byte 2.
  const bool hasLoops = sd.hasLoop;
  uint8_t h2 = static_cast<uint8_t>((sd.preExtrapolation & 0x07) |
                                    ((sd.postExtrapolation & 0x07) << 3) |
                                    ((hasLoops ? 1 : 0) << 6));
  Wr<uint8_t>(out, h2);

  if (sd.preExtrapolation == 3 /*Sloped*/) {
    Wr<double>(out, sd.preExtrapolationSlope);
  }
  if (sd.postExtrapolation == 3 /*Sloped*/) {
    Wr<double>(out, sd.postExtrapolationSlope);
  }

  if (hasLoops) {
    Wr<double>(out, sd.loopProtoStart);
    Wr<double>(out, sd.loopProtoEnd);
    Wr<int32_t>(out, static_cast<int32_t>(sd.loopNumPreLoops));
    Wr<int32_t>(out, static_cast<int32_t>(sd.loopNumPostLoops));
    Wr<double>(out, sd.loopValueOffset);
  }

  // Knot section.
  const uint64_t knot_count = static_cast<uint64_t>(sd.knots.size());
  if (knot_count > uint64_t{std::numeric_limits<uint32_t>::max()}) {
    if (err) *err = "Too many spline knots to encode.";
    return false;
  }
  Wr<uint32_t>(out, static_cast<uint32_t>(knot_count));

  for (const SplineKnotData &k : sd.knots) {
    uint8_t flag = static_cast<uint8_t>(
        (k.hasDualValue ? 1 : 0) |
        ((k.interpolationMode & 0x03) << 1) |
        ((sd.curveType & 0x01) << 3));
    Wr<uint8_t>(out, flag);

    Wr<double>(out, k.time);
    WrTyped(out, desc, AsNum(k.val));

    if (k.hasDualValue) {
      WrTyped(out, desc, AsNum(k.preValue));
    }
    if (!hermite) {
      Wr<double>(out, k.preTangentWidth);
      Wr<double>(out, k.postTangentWidth);
    }
    WrTyped(out, desc, k.preTangentSlope);
    WrTyped(out, desc, k.postTangentSlope);

    if (version > 1) {
      // algorithmByte: low nibble = pre, high nibble = post (matches
      // pxr/base/ts/binary.cpp). Only present in spline binary version 2.
      uint8_t algo = static_cast<uint8_t>(
          (k.preTangentAlgorithm & 0x0f) |
          ((k.postTangentAlgorithm & 0x0f) << 4));
      Wr<uint8_t>(out, algo);
    }
  }

  return true;
}

bool DecodeSplineFromBinary(const uint8_t *data, size_t size, SplineData *out,
                            std::string *err) {
  if (!out) return false;
  *out = SplineData();

  const uint8_t *p = data;
  size_t remain = size;

  uint8_t h1 = 0;
  if (!Rd<uint8_t>(&p, &remain, &h1)) {
    if (err) *err = "Unexpected end of spline data (header byte 1).";
    return false;
  }
  const uint8_t version = h1 & 0x0f;
  const int desc = (h1 & 0x30) >> 4;
  const bool hermite = ((h1 & 0x80) >> 7) != 0;
  out->curveType = hermite ? 1 : 0;

  uint8_t h2 = 0;
  if (!Rd<uint8_t>(&p, &remain, &h2)) {
    if (err) *err = "Unexpected end of spline data (header byte 2).";
    return false;
  }
  out->preExtrapolation = h2 & 0x07;
  out->postExtrapolation = (h2 & 0x38) >> 3;
  const bool hasLoops = (h2 & 0x40) != 0;

  if (out->preExtrapolation == 3 /*Sloped*/) {
    if (!Rd<double>(&p, &remain, &out->preExtrapolationSlope)) return false;
  }
  if (out->postExtrapolation == 3 /*Sloped*/) {
    if (!Rd<double>(&p, &remain, &out->postExtrapolationSlope)) return false;
  }

  if (hasLoops) {
    int32_t npre = 0, npost = 0;
    if (!Rd<double>(&p, &remain, &out->loopProtoStart)) return false;
    if (!Rd<double>(&p, &remain, &out->loopProtoEnd)) return false;
    if (!Rd<int32_t>(&p, &remain, &npre)) return false;
    if (!Rd<int32_t>(&p, &remain, &npost)) return false;
    if (!Rd<double>(&p, &remain, &out->loopValueOffset)) return false;
    out->hasLoop = true;
    out->loopNumPreLoops = npre;
    out->loopNumPostLoops = npost;
  }

  uint32_t knotCount = 0;
  if (!Rd<uint32_t>(&p, &remain, &knotCount)) {
    if (err) *err = "Unexpected end of spline data (knot count).";
    return false;
  }

  const size_t valueSize = (desc == 2) ? sizeof(float)
                         : (desc == 3) ? sizeof(value::half)
                                       : sizeof(double);
  size_t minKnotBytes = sizeof(uint8_t) + sizeof(double) + valueSize +
                        valueSize + valueSize;
  if (!hermite) {
    minKnotBytes += sizeof(double) + sizeof(double);
  }
  if (version > 1) {
    minKnotBytes += sizeof(uint8_t);
  }
  if ((minKnotBytes > 0) &&
      (static_cast<uint64_t>(knotCount) >
       static_cast<uint64_t>(remain / minKnotBytes))) {
    if (err) *err = "Spline knot count exceeds remaining data.";
    return false;
  }
  if (static_cast<uint64_t>(knotCount) >
      static_cast<uint64_t>((std::numeric_limits<size_t>::max)() /
                            sizeof(SplineKnotData))) {
    if (err) *err = "Spline knot count exceeds addressable memory.";
    return false;
  }

  out->knots.reserve(knotCount);
  for (uint32_t i = 0; i < knotCount; i++) {
    SplineKnotData k;
    uint8_t flag = 0;
    if (!Rd<uint8_t>(&p, &remain, &flag)) return false;
    k.hasDualValue = (flag & 0x01) != 0;
    k.interpolationMode = (flag & 0x06) >> 1;

    if (!Rd<double>(&p, &remain, &k.time)) return false;

    double v = 0.0;
    if (!RdTyped(&p, &remain, desc, &v)) return false;
    k.val = MakeTyped(desc, v);

    if (k.hasDualValue) {
      double pv = 0.0;
      if (!RdTyped(&p, &remain, desc, &pv)) return false;
      k.preValue = MakeTyped(desc, pv);
    } else {
      k.preValue = k.val;
    }

    if (!hermite) {
      if (!Rd<double>(&p, &remain, &k.preTangentWidth)) return false;
      if (!Rd<double>(&p, &remain, &k.postTangentWidth)) return false;
    }

    if (!RdTyped(&p, &remain, desc, &k.preTangentSlope)) return false;
    if (!RdTyped(&p, &remain, desc, &k.postTangentSlope)) return false;

    if (version > 1) {
      uint8_t algo = 0;
      if (!Rd<uint8_t>(&p, &remain, &algo)) return false;
      k.preTangentAlgorithm = algo & 0x0f;
      k.postTangentAlgorithm = (algo >> 4) & 0x0f;
    }

    out->knots.push_back(k);
  }

  return true;
}

}  // namespace lightusd
