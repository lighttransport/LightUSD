// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// TODO:
//   - [ ] Subdivision surface to polygon mesh conversion.
//     - [ ] Correctly handle primvar with 'vertex' interpolation(Use the basis
//     function of subd surface)
//   - [ ] Support material binding collection(Collection API)
//   - [ ] Support multiple skel animation
//   https://github.com/PixarAnimationStudios/OpenUSD/issues/2246
//   - [ ] Adjust normal vector computation with handness?
//   - [ ] Node xform animation
//   - [ ] Better build of index buffer
//     - [ ] Preserve the order of 'points' variable(mesh.points, Skin
//     indices/weights, BlendShape points, ...) as much as possible.
//     - Implement spatial hash
//
//
// Animation, skeleton, and light conversion routines split from render-data.cc
//
#include <algorithm>
#include <numeric>
#include <set>
#include <limits>
#include <unordered_map>

#include "common-utils.hh"
#include "common-types.hh"
#include "image-loader.hh"
#include "image-util.hh"
#include "image-types.hh"
#include "safe-arithmetic.hh"
#include "linear-algebra.hh"
#include "math-util.inc"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "usdMtlx.hh"
#include "value-clip-utils.hh"
#include "value-pprint.hh"
#include "bone-util.hh"
#include "shape-to-mesh.hh"
#include "materialx-to-json.hh"
#include "mmap-array-ref.hh"

// Helper macros for iterating over TypedTimeSamples
#define FOREACH_TIMESAMPLES_BEGIN(ts, var_t, var_value, var_blocked) \
  for (const auto &_sample : (ts).get_samples()) { \
    const double var_t = _sample.t; \
    const auto &var_value = _sample.value; \
    const bool var_blocked = _sample.blocked; \
    if (!var_blocked) {

#define FOREACH_TIMESAMPLES_END() \
    } \
  }

//
#include "common-macros.inc"
#include "math-util.inc"


//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"

namespace lightusd {
namespace tydra {
namespace {
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
static bool IsClipTransformAttribute(const std::string &name) {
  return ((name == "xformOpOrder") || (name.rfind("xformOp:", 0) == 0));
}

static bool SetOrCheckComponentCount(size_t *out_count, size_t this_count) {
  if (!out_count) {
    return true;
  }

  if (*out_count == 0) {
    *out_count = this_count;
    return true;
  }

  return *out_count == this_count;
}

static bool KeyframeSamplerEqual(const KeyframeSampler &a,
                                 const KeyframeSampler &b) {
  return a.interpolation == b.interpolation && a.times == b.times &&
         a.values == b.values;
}

static size_t HashCombine(size_t seed, const size_t value) {
  return seed ^ (value + size_t(0x9e3779b97f4a7c15ull) + (seed << 6) +
                 (seed >> 2));
}

static size_t HashFloatArray(const std::vector<float> &values) {
  size_t h = values.size();
  for (const float v : values) {
    h = HashCombine(h, std::hash<float>{}(v));
  }
  return h;
}

static size_t HashKeyframeSampler(const KeyframeSampler &sampler) {
  size_t h = std::hash<int>{}(int(sampler.interpolation));
  h = HashCombine(h, HashFloatArray(sampler.times));
  h = HashCombine(h, HashFloatArray(sampler.values));
  return h;
}

static bool CompactNewSamplers(AnimationClip *clip, const size_t first_sampler,
                               const size_t first_channel) {
  if (!clip || first_sampler >= clip->samplers.size()) {
    return true;
  }

  std::vector<KeyframeSampler> compacted;
  compacted.reserve(clip->samplers.size());
  std::vector<int32_t> remap(clip->samplers.size(), -1);
  std::unordered_map<size_t, std::vector<size_t>> hash_buckets;

  for (size_t i = 0; i < first_sampler; ++i) {
    remap[i] = static_cast<int32_t>(i);
    compacted.push_back(std::move(clip->samplers[i]));
    hash_buckets[HashKeyframeSampler(compacted.back())].push_back(i);
  }

  for (size_t i = first_sampler; i < clip->samplers.size(); ++i) {
    int32_t mapped = -1;
    const size_t sampler_hash = HashKeyframeSampler(clip->samplers[i]);
    if (auto it = hash_buckets.find(sampler_hash); it != hash_buckets.end()) {
      for (const size_t candidate_idx : it->second) {
        if (KeyframeSamplerEqual(compacted[candidate_idx],
                                 clip->samplers[i])) {
          mapped = static_cast<int32_t>(candidate_idx);
          break;
        }
      }
    }

    if (mapped < 0) {
      if (compacted.size() >
          size_t(std::numeric_limits<int32_t>::max())) {
        return false;
      }
      mapped = static_cast<int32_t>(compacted.size());
      compacted.push_back(std::move(clip->samplers[i]));
      hash_buckets[sampler_hash].push_back(size_t(mapped));
    }
    remap[i] = mapped;
  }

  for (size_t i = first_channel; i < clip->channels.size(); ++i) {
    const int32_t old_sampler = clip->channels[i].sampler;
    if (old_sampler >= 0 && size_t(old_sampler) < remap.size() &&
        remap[size_t(old_sampler)] >= 0) {
      clip->channels[i].sampler = remap[size_t(old_sampler)];
    }
  }

  clip->samplers = std::move(compacted);
  return true;
}

template <typename T>
static void AppendNumericToFloatArray(std::vector<float> *dst, const T v) {
  dst->push_back(static_cast<float>(v));
}

template <typename Vec>
static void AppendVectorLikeToFloatArray(const Vec &value,
                                        std::vector<float> *dst) {
  for (const auto &elem : value) {
    dst->push_back(static_cast<float>(elem));
  }
}

template <typename Quat>
static void AppendQuatToFloatArray(const Quat &value, std::vector<float> *dst) {
  dst->push_back(static_cast<float>(value.imag[0]));
  dst->push_back(static_cast<float>(value.imag[1]));
  dst->push_back(static_cast<float>(value.imag[2]));
  dst->push_back(static_cast<float>(value.real));
}

static void AppendQuatToFloatArray(const value::quath &value,
                                   std::vector<float> *dst) {
  dst->push_back(value::half_to_float(value.imag[0]));
  dst->push_back(value::half_to_float(value.imag[1]));
  dst->push_back(value::half_to_float(value.imag[2]));
  dst->push_back(value::half_to_float(value.real));
}

template <typename Scalar, size_t N>
static void AppendMatrixToFloatArray(const Scalar (&m)[N][N],
                                     std::vector<float> *dst) {
  for (size_t row = 0; row < N; ++row) {
    for (size_t col = 0; col < N; ++col) {
      dst->push_back(static_cast<float>(m[row][col]));
    }
  }
}

static bool AppendValueToFloatArray(const TerminalAttributeValue &value,
                                  std::vector<float> *dst,
                                  size_t *component_count) {
  size_t this_count = 0;

  if (auto *v = value.as<bool>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v ? 1.0f : 0.0f);
    return true;
  }

  if (auto *v = value.as<char>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<uint8_t>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<int16_t>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<uint16_t>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<int>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<uint32_t>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<int64_t>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<uint64_t>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<float>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<double>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, *v);
    return true;
  }

  if (auto *v = value.as<value::half>()) {
    this_count = 1;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendNumericToFloatArray(dst, value::half_to_float(*v));
    return true;
  }

  if (auto *v = value.as<value::float2>()) {
    this_count = 2;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendVectorLikeToFloatArray(*v, dst);
    return true;
  }
  if (auto *v = value.as<value::float3>()) {
    this_count = 3;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendVectorLikeToFloatArray(*v, dst);
    return true;
  }
  if (auto *v = value.as<value::float4>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendVectorLikeToFloatArray(*v, dst);
    return true;
  }

  if (auto *v = value.as<value::double2>()) {
    this_count = 2;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendVectorLikeToFloatArray(*v, dst);
    return true;
  }
  if (auto *v = value.as<value::double3>()) {
    this_count = 3;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendVectorLikeToFloatArray(*v, dst);
    return true;
  }
  if (auto *v = value.as<value::double4>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendVectorLikeToFloatArray(*v, dst);
    return true;
  }

  if (auto *v = value.as<value::half2>()) {
    this_count = 2;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    dst->push_back(value::half_to_float((*v)[0]));
    dst->push_back(value::half_to_float((*v)[1]));
    return true;
  }
  if (auto *v = value.as<value::half3>()) {
    this_count = 3;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    dst->push_back(value::half_to_float((*v)[0]));
    dst->push_back(value::half_to_float((*v)[1]));
    dst->push_back(value::half_to_float((*v)[2]));
    return true;
  }
  if (auto *v = value.as<value::half4>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    dst->push_back(value::half_to_float((*v)[0]));
    dst->push_back(value::half_to_float((*v)[1]));
    dst->push_back(value::half_to_float((*v)[2]));
    dst->push_back(value::half_to_float((*v)[3]));
    return true;
  }

  if (auto *v = value.as<value::quath>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendQuatToFloatArray(*v, dst);
    return true;
  }
  if (auto *v = value.as<value::quatf>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendQuatToFloatArray(*v, dst);
    return true;
  }
  if (auto *v = value.as<value::quatd>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendQuatToFloatArray(*v, dst);
    return true;
  }

  if (auto *v = value.as<value::matrix2f>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendMatrixToFloatArray(v->m, dst);
    return true;
  }
  if (auto *v = value.as<value::matrix3f>()) {
    this_count = 9;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendMatrixToFloatArray(v->m, dst);
    return true;
  }
  if (auto *v = value.as<value::matrix4f>()) {
    this_count = 16;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendMatrixToFloatArray(v->m, dst);
    return true;
  }
  if (auto *v = value.as<value::matrix2d>()) {
    this_count = 4;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendMatrixToFloatArray(v->m, dst);
    return true;
  }
  if (auto *v = value.as<value::matrix3d>()) {
    this_count = 9;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendMatrixToFloatArray(v->m, dst);
    return true;
  }
  if (auto *v = value.as<value::matrix4d>()) {
    this_count = 16;
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    AppendMatrixToFloatArray(v->m, dst);
    return true;
  }

  if (auto *v = value.as<std::vector<float>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(e);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<double>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::half>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(value::half_to_float(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<char>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<uint8_t>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<int16_t>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<uint16_t>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<int>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<int64_t>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<uint32_t>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<uint64_t>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(static_cast<float>(e));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<bool>>()) {
    this_count = v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto e : *v) {
      dst->push_back(e ? 1.0f : 0.0f);
    }
    return true;
  }

  if (auto *v = value.as<std::vector<value::float2>>()) {
    this_count = 2 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendVectorLikeToFloatArray(e, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::float3>>()) {
    this_count = 3 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendVectorLikeToFloatArray(e, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::float4>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendVectorLikeToFloatArray(e, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::double2>>()) {
    this_count = 2 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendVectorLikeToFloatArray(e, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::double3>>()) {
    this_count = 3 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendVectorLikeToFloatArray(e, dst);
    }
    return true;
  }

  if (auto *v = value.as<std::vector<value::double4>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendVectorLikeToFloatArray(e, dst);
    }
    return true;
  }

  if (auto *v = value.as<std::vector<value::half2>>()) {
    this_count = 2 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(value::half_to_float(e[0]));
      dst->push_back(value::half_to_float(e[1]));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::half3>>()) {
    this_count = 3 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(value::half_to_float(e[0]));
      dst->push_back(value::half_to_float(e[1]));
      dst->push_back(value::half_to_float(e[2]));
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::half4>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      dst->push_back(value::half_to_float(e[0]));
      dst->push_back(value::half_to_float(e[1]));
      dst->push_back(value::half_to_float(e[2]));
      dst->push_back(value::half_to_float(e[3]));
    }
    return true;
  }

  if (auto *v = value.as<std::vector<value::quath>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendQuatToFloatArray(e, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::quatf>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendQuatToFloatArray(e, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::quatd>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendQuatToFloatArray(e, dst);
    }
    return true;
  }

  if (auto *v = value.as<std::vector<value::matrix2f>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendMatrixToFloatArray(e.m, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::matrix3f>>()) {
    this_count = 9 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendMatrixToFloatArray(e.m, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::matrix4f>>()) {
    this_count = 16 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendMatrixToFloatArray(e.m, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::matrix2d>>()) {
    this_count = 4 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendMatrixToFloatArray(e.m, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::matrix3d>>()) {
    this_count = 9 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendMatrixToFloatArray(e.m, dst);
    }
    return true;
  }
  if (auto *v = value.as<std::vector<value::matrix4d>>()) {
    this_count = 16 * v->size();
    if (!SetOrCheckComponentCount(component_count, this_count)) return false;
    for (const auto &e : *v) {
      AppendMatrixToFloatArray(e.m, dst);
    }
    return true;
  }

  return false;
}
}


// Helper function: Quaternion multiplication using direct member access
// (avoids operator[] pointer arithmetic overhead)
// q1 * q2, Hamilton convention
[[maybe_unused]] static inline value::quatf quat_mul(const value::quatf &q1, const value::quatf &q2) {
  const float x1 = q1.imag[0], y1 = q1.imag[1], z1 = q1.imag[2], w1 = q1.real;
  const float x2 = q2.imag[0], y2 = q2.imag[1], z2 = q2.imag[2], w2 = q2.real;
  value::quatf r;
  r.imag[0] = w1*x2 + x1*w2 + y1*z2 - z1*y2;
  r.imag[1] = w1*y2 - x1*z2 + y1*w2 + z1*x2;
  r.imag[2] = w1*z2 + x1*y2 - y1*x2 + z1*w2;
  r.real    = w1*w2 - x1*x2 - y1*y2 - z1*z2;
  return r;
}

// Specialized single-axis angle-to-quaternion (avoids multiply-by-zero for the
// two unused axis components). Keeps sin_pi/cos_pi for accuracy.
static inline value::quatf to_quaternion_x(float angle) {
  float s = float(math::sin_pi(double(angle) / 360.0));
  float c = float(math::cos_pi(double(angle) / 360.0));
  value::quatf q;
  q.imag[0] = s;  q.imag[1] = 0.0f;  q.imag[2] = 0.0f;  q.real = c;
  return q;
}

static inline value::quatf to_quaternion_y(float angle) {
  float s = float(math::sin_pi(double(angle) / 360.0));
  float c = float(math::cos_pi(double(angle) / 360.0));
  value::quatf q;
  q.imag[0] = 0.0f;  q.imag[1] = s;  q.imag[2] = 0.0f;  q.real = c;
  return q;
}

static inline value::quatf to_quaternion_z(float angle) {
  float s = float(math::sin_pi(double(angle) / 360.0));
  float c = float(math::cos_pi(double(angle) / 360.0));
  value::quatf q;
  q.imag[0] = 0.0f;  q.imag[1] = 0.0f;  q.imag[2] = s;  q.real = c;
  return q;
}

// Direct Euler-to-quaternion conversion using closed-form formulas.
// Computes the combined quaternion from 3 axis-aligned rotations in one step,
// avoiding intermediate quaternion objects and 2 quaternion multiplications.
// All 6 rotation orders are supported.
// angles[0] = X angle, angles[1] = Y angle, angles[2] = Z angle (degrees)
static inline value::quatf euler_to_quatf(
    const value::double3 &angles, XformOp::OpType rot_order) {
  // Half-angle trig values (using sin_pi/cos_pi for accuracy)
  const float sx = float(math::sin_pi(angles[0] / 360.0));
  const float cx = float(math::cos_pi(angles[0] / 360.0));
  const float sy = float(math::sin_pi(angles[1] / 360.0));
  const float cy = float(math::cos_pi(angles[1] / 360.0));
  const float sz = float(math::sin_pi(angles[2] / 360.0));
  const float cz = float(math::cos_pi(angles[2] / 360.0));

  value::quatf q;

  switch (rot_order) {
    case XformOp::OpType::RotateXYZ:
      // Q = Qz * Qy * Qx
      q.imag[0] = cz*cy*sx - sz*sy*cx;
      q.imag[1] = cz*sy*cx + sz*cy*sx;
      q.imag[2] = sz*cy*cx - cz*sy*sx;
      q.real    = cz*cy*cx + sz*sy*sx;
      break;
    case XformOp::OpType::RotateXZY:
      // Q = Qy * Qz * Qx
      q.imag[0] = cy*cz*sx + sy*sz*cx;
      q.imag[1] = cy*sz*sx + sy*cz*cx;
      q.imag[2] = cy*sz*cx - sy*cz*sx;
      q.real    = cy*cz*cx - sy*sz*sx;
      break;
    case XformOp::OpType::RotateYXZ:
      // Q = Qz * Qx * Qy
      q.imag[0] = cz*sx*cy - sz*cx*sy;
      q.imag[1] = cz*cx*sy + sz*sx*cy;
      q.imag[2] = cz*sx*sy + sz*cx*cy;
      q.real    = cz*cx*cy - sz*sx*sy;
      break;
    case XformOp::OpType::RotateYZX:
      // Q = Qx * Qz * Qy
      q.imag[0] = sx*cz*cy - cx*sz*sy;
      q.imag[1] = cx*cz*sy - sx*sz*cy;
      q.imag[2] = cx*sz*cy + sx*cz*sy;
      q.real    = cx*cz*cy + sx*sz*sy;
      break;
    case XformOp::OpType::RotateZXY:
      // Q = Qy * Qx * Qz
      q.imag[0] = cy*sx*cz + sy*cx*sz;
      q.imag[1] = sy*cx*cz - cy*sx*sz;
      q.imag[2] = cy*cx*sz - sy*sx*cz;
      q.real    = cy*cx*cz + sy*sx*sz;
      break;
    case XformOp::OpType::RotateZYX:
      // Q = Qx * Qy * Qz
      q.imag[0] = cx*sy*sz + sx*cy*cz;
      q.imag[1] = cx*sy*cz - sx*cy*sz;
      q.imag[2] = cx*cy*sz + sx*sy*cz;
      q.real    = cx*cy*cz - sx*sy*sz;
      break;
    default:
      // Fallback: treat as XYZ
      q.imag[0] = cz*cy*sx - sz*sy*cx;
      q.imag[1] = cz*sy*cx + sz*cy*sx;
      q.imag[2] = sz*cy*cx - cz*sy*sx;
      q.real    = cz*cy*cx + sz*sy*sx;
      break;
  }

  return q;
}

bool RenderSceneConverter::ExtractPrimPropertyAnimation(
    const RenderSceneConverterEnv &env,
    const Prim &prim,
    const Path &abs_path,
    int32_t target_node_index,
    AnimationClip *anim_out) {
  (void)env;

  if (!anim_out) {
    PUSH_ERROR_AND_RETURN("anim_out is nullptr");
  }

  if (prim.is<SkelAnimation>() || prim.is<Skeleton>() ||
      prim.is<BlendShape>()) {
    return false;
  }

  std::set<std::string> appended_property_names;

  auto append_time_samples = [&](const std::string &attr_name,
                                 const value::TimeSamples &ts) {
    if (ts.size() == 0) {
      return;
    }

    KeyframeSampler sampler;
    sampler.interpolation = AnimationInterpolation::Linear;
    sampler.times.reserve(ts.size());

    size_t component_count = 0;
    bool supported = true;
    FOREACH_TIMESAMPLES_BEGIN(ts, sample_t, sample_value, sample_blocked)
      if (sample_blocked) {
        continue;
      }
      const size_t prev_values = sampler.values.size();
      size_t expected_count = component_count;
      if (!AppendValueToFloatArray(sample_value, &sampler.values,
                                   &expected_count)) {
        sampler.values.resize(prev_values);
        supported = false;
        break;
      }
      sampler.times.push_back(float(sample_t));
      component_count = expected_count;
      if (float(sample_t) > anim_out->duration) {
        anim_out->duration = float(sample_t);
      }
    FOREACH_TIMESAMPLES_END()

    if (!supported) {
      PUSH_WARN(fmt::format(
          "Skipping animated attribute '{}' for {} due to unsupported or "
          "inconsistent sample type.",
          attr_name, abs_path.full_path_name()));
      return;
    }
    if (sampler.times.empty() || component_count == 0) {
      return;
    }

    const int32_t sampler_index = int32_t(anim_out->samplers.size());
    anim_out->samplers.push_back(std::move(sampler));

    AnimationChannel channel;
    channel.target_type = ChannelTargetType::SceneNode;
    channel.path = AnimationPath::CustomProperty;
    channel.target_node = target_node_index;
    channel.target_prim_path = abs_path.full_path_name();
    channel.sampler = sampler_index;
    channel.is_custom_property = true;
    channel.property_name = attr_name;
    anim_out->channels.push_back(std::move(channel));
    appended_property_names.insert(attr_name);
  };

  auto append_fallback_anim = [&](const std::string &name,
                                  const auto &attr) {
    if (!attr.authored()) {
      return;
    }
    const auto &anim = attr.get_value();
    if (anim.has_timesamples()) {
      append_time_samples(name, *anim.get_timesamples_ptr());
    }
  };

  auto append_optional_anim = [&](const std::string &name,
                                  const auto &attr) {
    if (!attr.authored()) {
      return;
    }
    const auto anim = attr.get_value();
    if (anim && anim.value().has_timesamples()) {
      append_time_samples(name, *anim.value().get_timesamples_ptr());
    }
  };

  if (const auto *camera = prim.as<GeomCamera>()) {
    append_optional_anim("clippingPlanes", camera->clippingPlanes);
    append_fallback_anim("clippingRange", camera->clippingRange);
    append_fallback_anim("exposure", camera->exposure);
    append_fallback_anim("focalLength", camera->focalLength);
    append_fallback_anim("focusDistance", camera->focusDistance);
    append_fallback_anim("horizontalAperture", camera->horizontalAperture);
    append_fallback_anim("horizontalApertureOffset",
                         camera->horizontalApertureOffset);
    append_fallback_anim("verticalAperture", camera->verticalAperture);
    append_fallback_anim("verticalApertureOffset",
                         camera->verticalApertureOffset);
    append_fallback_anim("fStop", camera->fStop);
    append_fallback_anim("shutterClose", camera->shutterClose);
    append_fallback_anim("shutterOpen", camera->shutterOpen);
  }

  auto append_light_api = [&](const LightAPI &light) {
    append_fallback_anim("inputs:color", light.color);
    append_fallback_anim("inputs:colorTemperature", light.colorTemperature);
    append_fallback_anim("inputs:diffuse", light.diffuse);
    append_fallback_anim("inputs:enableColorTemperature",
                         light.enableColorTemperature);
    append_fallback_anim("inputs:exposure", light.exposure);
    append_fallback_anim("inputs:intensity", light.intensity);
    append_fallback_anim("inputs:normalize", light.normalize);
    append_fallback_anim("inputs:specular", light.specular);
    append_fallback_anim("inputs:shadow:enable", light.shadowEnable);
    append_fallback_anim("inputs:shadow:color", light.shadowColor);
    append_fallback_anim("inputs:shadow:distance", light.shadowDistance);
    append_fallback_anim("inputs:shadow:falloff", light.shadowFalloff);
    append_fallback_anim("inputs:shadow:falloffGamma",
                         light.shadowFalloffGamma);
    append_fallback_anim("inputs:shaping:focus", light.shapingFocus);
    append_fallback_anim("inputs:shaping:focusTint", light.shapingFocusTint);
    append_fallback_anim("inputs:shaping:cone:angle",
                         light.shapingConeAngle);
    append_fallback_anim("inputs:shaping:cone:softness",
                         light.shapingConeSoftness);
    append_fallback_anim("inputs:shaping:ies:angleScale",
                         light.shapingIesAngleScale);
    append_fallback_anim("inputs:shaping:ies:normalize",
                         light.shapingIesNormalize);
  };

  if (const auto *sphere_light = prim.as<SphereLight>()) {
    append_light_api(*sphere_light);
    append_fallback_anim("inputs:radius", sphere_light->radius);
  } else if (const auto *cylinder_light = prim.as<CylinderLight>()) {
    append_light_api(*cylinder_light);
    append_fallback_anim("inputs:length", cylinder_light->length);
    append_fallback_anim("inputs:radius", cylinder_light->radius);
  } else if (const auto *rect_light = prim.as<RectLight>()) {
    append_light_api(*rect_light);
    append_fallback_anim("inputs:height", rect_light->height);
    append_fallback_anim("inputs:width", rect_light->width);
  } else if (const auto *disk_light = prim.as<DiskLight>()) {
    append_light_api(*disk_light);
    append_fallback_anim("inputs:radius", disk_light->radius);
  } else if (const auto *distant_light = prim.as<DistantLight>()) {
    append_light_api(*distant_light);
    append_fallback_anim("inputs:angle", distant_light->angle);
  } else if (const auto *dome_light = prim.as<DomeLight>()) {
    append_light_api(*dome_light);
  } else if (const auto *dome_light_1 = prim.as<DomeLight_1>()) {
    append_light_api(*dome_light_1);
  } else if (const auto *geometry_light = prim.as<GeometryLight>()) {
    append_light_api(*geometry_light);
  } else if (const auto *portal_light = prim.as<PortalLight>()) {
    append_light_api(*portal_light);
  }

  auto is_xform_attribute = [](const std::string &name) {
    return name == "xformOpOrder" || name.rfind("xformOp:", 0) == 0;
  };

  std::vector<std::string> attr_names;
  std::string attr_err;
  if (!GetAttributeNames(prim, &attr_names, &attr_err)) {
    attr_names.clear();
  }

  for (const std::string &attr_name : attr_names) {
    if (is_xform_attribute(attr_name)) {
      continue;
    }
    if (appended_property_names.count(attr_name)) {
      continue;
    }

    Property prop;
    std::string prop_err;
    if (!GetProperty(prim, attr_name, &prop, &prop_err)) {
      if (!prop_err.empty()) {
        PUSH_WARN(fmt::format("Failed to read property '{}' for {}: {}",
                              attr_name, abs_path.full_path_name(),
                              prop_err));
      }
      continue;
    }

    const Attribute *attr = prop.get_attribute_or_null();
    if (!attr || !attr->has_timesamples()) {
      continue;
    }

    append_time_samples(attr_name, attr->get_var().ts_raw());
  }

  if (anim_out->channels.empty()) {
    return false;
  }

  anim_out->abs_path = abs_path.full_path_name();
  anim_out->prim_name = prim.element_name();
  anim_out->name = prim.element_name() + "_properties";
  anim_out->source_type = AnimationSourceType::Unknown;
  anim_out->num_animated_nodes = 1;

  return true;
}

bool RenderSceneConverter::ExtractXformOpAnimation(
    const RenderSceneConverterEnv &env,
    const Path &abs_path,
    const std::string &prim_name,
    const Xformable &xformable,
    int32_t target_node_index,
    AnimationClip *anim_out) {

  (void)env;  // Unused parameter

  if (!anim_out) {
    PUSH_ERROR_AND_RETURN("anim_out is nullptr");
  }

  // Check if xformable has any animated xformOps
  if (!xformable.has_timesamples()) {
    return false;  // No animation data
  }

  // Setup basic metadata
  anim_out->abs_path = abs_path.full_path_name();
  anim_out->prim_name = prim_name;
  anim_out->name = prim_name + "_xform";
  anim_out->duration = 0.0f;  // Will be computed below
  anim_out->source_type = AnimationSourceType::XformOp;
  anim_out->num_animated_nodes = 1;

  // Process each xformOp that has time samples
  for (size_t xform_idx = 0; xform_idx < xformable.xformOps.size(); xform_idx++) {
    const XformOp &xformOp = xformable.xformOps[xform_idx];

    if (xformOp.op_type == XformOp::OpType::ResetXformStack) {
      continue;  // Skip reset operations
    }

    if (!xformOp.has_timesamples()) {
      continue;  // Skip non-animated ops
    }

    // Get the time samples
    auto ts_opt = xformOp.get_timesamples();
    if (!ts_opt) {
      continue;
    }

    const value::TimeSamples &ts = ts_opt.value();
    if (ts.size() == 0) {
      continue;
    }

    // Determine the animation path based on xformOp type
    AnimationPath anim_path = AnimationPath::Translation;  // Default initialization
    bool is_supported = false;

    switch (xformOp.op_type) {
      case XformOp::OpType::Translate:
        anim_path = AnimationPath::Translation;
        is_supported = true;
        break;

      case XformOp::OpType::Scale:
        anim_path = AnimationPath::Scale;
        is_supported = true;
        break;

      case XformOp::OpType::Orient:
        anim_path = AnimationPath::Rotation;
        is_supported = true;
        break;

      case XformOp::OpType::RotateX:
      case XformOp::OpType::RotateY:
      case XformOp::OpType::RotateZ:
      case XformOp::OpType::RotateXYZ:
      case XformOp::OpType::RotateXZY:
      case XformOp::OpType::RotateYXZ:
      case XformOp::OpType::RotateYZX:
      case XformOp::OpType::RotateZXY:
      case XformOp::OpType::RotateZYX:
        anim_path = AnimationPath::Rotation;
        is_supported = true;
        break;

      case XformOp::OpType::Transform:
        // Full matrix transform - decompose into TRS
        // We'll handle this specially below since it produces multiple animation channels
        is_supported = true;
        break;

      case XformOp::OpType::ResetXformStack:
        // Not animatable - skip
        is_supported = false;
        break;
    }

    if (!is_supported) {
      continue;
    }

    // Special handling for Transform (matrix) - decompose into TRS
    if (xformOp.op_type == XformOp::OpType::Transform) {
      std::vector<double> times;
      std::vector<value::double3> translations;
      std::vector<value::quatd> rotations;
      std::vector<value::double3> scales;

      // Extract and decompose matrix time samples
      FOREACH_TIMESAMPLES_BEGIN(ts, sample_t, sample_value, sample_blocked)
        if (sample_blocked) {
          continue;
        }

        value::matrix4d mat;
        bool got_value = false;

        if (auto v = sample_value.as<value::matrix4d>()) {
          mat = *v;
          got_value = true;
        } else if (auto vf = sample_value.as<value::matrix4f>()) {
          // Convert float matrix to double
          const auto &m = *vf;
          for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
              mat.m[i][j] = double(m.m[i][j]);
            }
          }
          got_value = true;
        }

        if (got_value) {
          value::double3 translation, scale;
          value::quatd rotation;

          // Decompose the matrix
          if (decompose(mat, &translation, &rotation, &scale)) {
            times.push_back(sample_t);
            translations.push_back(translation);
            rotations.push_back(rotation);
            scales.push_back(scale);

            if (float(sample_t) > anim_out->duration) {
              anim_out->duration = float(sample_t);
            }
          } else {
            PUSH_WARN(fmt::format("Failed to decompose matrix at time {} for xformOp:transform at {}",
                                 sample_t, abs_path.full_path_name()));
          }
        }
      FOREACH_TIMESAMPLES_END()

      // Create three separate animation channels for T, R, S
      if (!times.empty()) {
        // Translation channel
        {
          KeyframeSampler sampler;
          sampler.interpolation = AnimationInterpolation::Linear;
          sampler.times.reserve(times.size());
          sampler.values.reserve(times.size() * 3);

          for (size_t i = 0; i < times.size(); i++) {
            sampler.times.push_back(float(times[i]));
            sampler.values.push_back(float(translations[i][0]));
            sampler.values.push_back(float(translations[i][1]));
            sampler.values.push_back(float(translations[i][2]));
          }

          int32_t sampler_idx = int32_t(anim_out->samplers.size());
          anim_out->samplers.push_back(sampler);

          AnimationChannel channel;
          channel.target_type = ChannelTargetType::SceneNode;
          channel.path = AnimationPath::Translation;
          channel.target_node = target_node_index;
          channel.target_prim_path = abs_path.full_path_name();
          channel.sampler = sampler_idx;
          anim_out->channels.push_back(channel);
        }

        // Rotation channel
        {
          KeyframeSampler sampler;
          sampler.interpolation = AnimationInterpolation::Linear;
          sampler.times.reserve(times.size());
          sampler.values.reserve(times.size() * 4);

          for (size_t i = 0; i < times.size(); i++) {
            sampler.times.push_back(float(times[i]));
            sampler.values.push_back(float(rotations[i].imag[0]));
            sampler.values.push_back(float(rotations[i].imag[1]));
            sampler.values.push_back(float(rotations[i].imag[2]));
            sampler.values.push_back(float(rotations[i].real));
          }

          int32_t sampler_idx = int32_t(anim_out->samplers.size());
          anim_out->samplers.push_back(sampler);

          AnimationChannel channel;
          channel.target_type = ChannelTargetType::SceneNode;
          channel.path = AnimationPath::Rotation;
          channel.target_node = target_node_index;
          channel.target_prim_path = abs_path.full_path_name();
          channel.sampler = sampler_idx;
          anim_out->channels.push_back(channel);
        }

        // Scale channel
        {
          KeyframeSampler sampler;
          sampler.interpolation = AnimationInterpolation::Linear;
          sampler.times.reserve(times.size());
          sampler.values.reserve(times.size() * 3);

          for (size_t i = 0; i < times.size(); i++) {
            sampler.times.push_back(float(times[i]));
            sampler.values.push_back(float(scales[i][0]));
            sampler.values.push_back(float(scales[i][1]));
            sampler.values.push_back(float(scales[i][2]));
          }

          int32_t sampler_idx = int32_t(anim_out->samplers.size());
          anim_out->samplers.push_back(sampler);

          AnimationChannel channel;
          channel.target_type = ChannelTargetType::SceneNode;
          channel.path = AnimationPath::Scale;
          channel.target_node = target_node_index;
          channel.target_prim_path = abs_path.full_path_name();
          channel.sampler = sampler_idx;
          anim_out->channels.push_back(channel);
        }
      }

      // Skip the regular processing below
      continue;
    }

    // Create a keyframe sampler
    KeyframeSampler sampler;
    sampler.interpolation = AnimationInterpolation::Linear;

    // Extract time samples based on the operation type
    if (anim_path == AnimationPath::Translation || anim_path == AnimationPath::Scale) {
      // Handle vec3 types (translation, scale)
      std::vector<double> times;
      std::vector<value::float3> values;

      FOREACH_TIMESAMPLES_BEGIN(ts, sample_t, sample_value, sample_blocked)
        if (sample_blocked) {
          continue;
        }

        // Try to get value as various vec3 types
        value::float3 vec;
        bool got_value = false;

        if (auto v = sample_value.as<value::float3>()) {
          vec = *v;
          got_value = true;
        } else if (auto vd = sample_value.as<value::double3>()) {
          vec[0] = float((*vd)[0]);
          vec[1] = float((*vd)[1]);
          vec[2] = float((*vd)[2]);
          got_value = true;
        } else if (auto vh = sample_value.as<value::half3>()) {
          vec[0] = value::half_to_float((*vh)[0]);
          vec[1] = value::half_to_float((*vh)[1]);
          vec[2] = value::half_to_float((*vh)[2]);
          got_value = true;
        }

        if (got_value) {
          times.push_back(sample_t);
          values.push_back(vec);
          if (float(sample_t) > anim_out->duration) {
            anim_out->duration = float(sample_t);
          }
        }
      FOREACH_TIMESAMPLES_END()

      // Build sampler data
      if (!times.empty()) {
        sampler.times.reserve(times.size());
        sampler.values.reserve(times.size() * 3);

        for (size_t i = 0; i < times.size(); i++) {
          sampler.times.push_back(float(times[i]));
          sampler.values.push_back(values[i][0]);
          sampler.values.push_back(values[i][1]);
          sampler.values.push_back(values[i][2]);
        }
      }

    } else if (anim_path == AnimationPath::Rotation) {
      // Handle rotation types
      std::vector<double> times;
      std::vector<value::quatf> values;

      // For Orient operations, we have quaternions
      if (xformOp.op_type == XformOp::OpType::Orient) {
        FOREACH_TIMESAMPLES_BEGIN(ts, sample_t, sample_value, sample_blocked)
          if (sample_blocked) {
            continue;
          }

          value::quatf quat;
          bool got_value = false;

          if (auto v = sample_value.as<value::quatf>()) {
            quat = *v;
            got_value = true;
          } else if (auto vd = sample_value.as<value::quatd>()) {
            quat.imag[0] = float(vd->imag[0]);
            quat.imag[1] = float(vd->imag[1]);
            quat.imag[2] = float(vd->imag[2]);
            quat.real     = float(vd->real);
            got_value = true;
          } else if (auto vh = sample_value.as<value::quath>()) {
            quat.imag[0] = value::half_to_float(vh->imag[0]);
            quat.imag[1] = value::half_to_float(vh->imag[1]);
            quat.imag[2] = value::half_to_float(vh->imag[2]);
            quat.real     = value::half_to_float(vh->real);
            got_value = true;
          }

          if (got_value) {
            times.push_back(sample_t);
            values.push_back(quat);
            if (float(sample_t) > anim_out->duration) {
              anim_out->duration = float(sample_t);
            }
          }
        FOREACH_TIMESAMPLES_END()

      } else {
        // For Rotate operations, we have angles that need to be converted to quaternions
        // We'll extract the angle values and convert them to quaternions
        std::vector<double> angle_times;
        std::vector<double> angle_values;

        if (xformOp.op_type == XformOp::OpType::RotateX ||
            xformOp.op_type == XformOp::OpType::RotateY ||
            xformOp.op_type == XformOp::OpType::RotateZ) {
          // Single-axis rotation (scalar angle)
          FOREACH_TIMESAMPLES_BEGIN(ts, sample_t, sample_value, sample_blocked)
            if (sample_blocked) {
              continue;
            }

            double angle = 0.0;
            bool got_value = false;

            if (auto v = sample_value.as<double>()) {
              angle = *v;
              got_value = true;
            } else if (auto vf = sample_value.as<float>()) {
              angle = double(*vf);
              got_value = true;
            }

            if (got_value) {
              angle_times.push_back(sample_t);
              angle_values.push_back(angle);
              if (float(sample_t) > anim_out->duration) {
                anim_out->duration = float(sample_t);
              }
            }
          FOREACH_TIMESAMPLES_END()

          // Convert angles to quaternions using specialized single-axis functions
          for (size_t i = 0; i < angle_times.size(); i++) {
            times.push_back(angle_times[i]);
            if (xformOp.op_type == XformOp::OpType::RotateX) {
              values.push_back(to_quaternion_x(float(angle_values[i])));
            } else if (xformOp.op_type == XformOp::OpType::RotateY) {
              values.push_back(to_quaternion_y(float(angle_values[i])));
            } else {  // RotateZ
              values.push_back(to_quaternion_z(float(angle_values[i])));
            }
          }

        } else {
          // Multi-axis rotation (vec3 of angles)
          // For RotateXYZ and similar, we need to compute the combined quaternion
          std::vector<value::double3> euler_angles;

          FOREACH_TIMESAMPLES_BEGIN(ts, sample_t, sample_value, sample_blocked)
            if (sample_blocked) {
              continue;
            }

            value::double3 angles;
            bool got_value = false;

            if (auto v = sample_value.as<value::float3>()) {
              angles[0] = double((*v)[0]);
              angles[1] = double((*v)[1]);
              angles[2] = double((*v)[2]);
              got_value = true;
            } else if (auto vd = sample_value.as<value::double3>()) {
              angles = *vd;
              got_value = true;
            } else if (auto vh = sample_value.as<value::half3>()) {
              angles[0] = double(value::half_to_float((*vh)[0]));
              angles[1] = double(value::half_to_float((*vh)[1]));
              angles[2] = double(value::half_to_float((*vh)[2]));
              got_value = true;
            }

            if (got_value) {
              angle_times.push_back(sample_t);
              euler_angles.push_back(angles);
              if (float(sample_t) > anim_out->duration) {
                anim_out->duration = float(sample_t);
              }
            }
          FOREACH_TIMESAMPLES_END()

          // Convert Euler angles to quaternions using direct closed-form formula
          // (handles all rotation orders correctly)
          for (size_t i = 0; i < angle_times.size(); i++) {
            times.push_back(angle_times[i]);
            values.push_back(euler_to_quatf(euler_angles[i], xformOp.op_type));
          }
        }
      }

      // Build sampler data for rotations (quaternions)
      if (!times.empty()) {
        sampler.times.reserve(times.size());
        sampler.values.reserve(times.size() * 4);

        for (size_t i = 0; i < times.size(); i++) {
          sampler.times.push_back(float(times[i]));
          sampler.values.push_back(values[i].imag[0]);
          sampler.values.push_back(values[i].imag[1]);
          sampler.values.push_back(values[i].imag[2]);
          sampler.values.push_back(values[i].real);
        }
      }
    }

    // Only add if we have valid sampler data
    if (!sampler.times.empty()) {
      int32_t sampler_idx = int32_t(anim_out->samplers.size());
      anim_out->samplers.push_back(sampler);

      AnimationChannel channel;
      channel.target_type = ChannelTargetType::SceneNode;
      channel.path = anim_path;
      channel.target_node = target_node_index;
      channel.target_prim_path = abs_path.full_path_name();
      channel.sampler = sampler_idx;
      anim_out->channels.push_back(channel);
    }
  }

  // Return true if we extracted any animation data
  return !anim_out->channels.empty();
}


}  // namespace tydra
}  // namespace lightusd
