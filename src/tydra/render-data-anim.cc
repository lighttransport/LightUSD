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
static constexpr size_t kMaxValueClipCacheEntries = 16;

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

}  // namespace

bool RenderSceneConverter::LoadValueClipLayer(
    const RenderSceneConverterEnv &env, const std::string &assetPath,
    std::shared_ptr<Layer> *layer_out) {
  if (!layer_out) {
    return false;
  }

  if (assetPath.empty()) {
    return false;
  }

  const std::string sanitized = utils::SanitizeAssetPath(
      assetPath, env.asset_resolver.get_allow_parent_relative_paths());
  if (sanitized.empty()) {
    PUSH_WARN(fmt::format("Unsafe clip layer asset path: {}", assetPath));
    return false;
  }

  const std::string resolved_asset_path = env.asset_resolver.resolve(sanitized);
  if (resolved_asset_path.empty()) {
    PUSH_WARN(fmt::format("Failed to resolve clip layer asset path: {}", sanitized));
    return false;
  }

  auto it = _value_clip_layer_cache.find(resolved_asset_path);
  if (it != _value_clip_layer_cache.end()) {
    *layer_out = it->second;
    return true;
  }

  Layer layer;
  std::string warn, err;
  if (!LoadLayerFromAsset(const_cast<AssetResolutionResolver &>(env.asset_resolver),
                         resolved_asset_path, &layer, &warn, &err)) {
    if (!warn.empty()) {
      PUSH_WARN(fmt::format("Failed to load clip layer: {}", warn));
    }
    if (!err.empty()) {
      PUSH_WARN(fmt::format("Failed to load clip layer: {}", err));
    }
    return false;
  }

  auto stage_cache = std::make_shared<Layer>(std::move(layer));

  if (_value_clip_layer_cache.size() >= kMaxValueClipCacheEntries) {
    _value_clip_layer_cache.erase(_value_clip_layer_cache.begin());
  }
  _value_clip_layer_cache[resolved_asset_path] = stage_cache;
  *layer_out = stage_cache;

  return true;
}

bool RenderSceneConverter::LoadValueClipStage(
    const RenderSceneConverterEnv &env, const std::string &assetPath,
    std::shared_ptr<Stage> *stage_out) {
  if (!stage_out) {
    return false;
  }

  if (assetPath.empty()) {
    return false;
  }

  const std::string sanitized = utils::SanitizeAssetPath(
      assetPath, env.asset_resolver.get_allow_parent_relative_paths());
  if (sanitized.empty()) {
    PUSH_WARN(fmt::format("Unsafe clip stage asset path: {}", assetPath));
    return false;
  }

  const std::string resolved_asset_path = env.asset_resolver.resolve(sanitized);
  if (resolved_asset_path.empty()) {
    PUSH_WARN(fmt::format("Failed to resolve clip stage asset path: {}", sanitized));
    return false;
  }

  auto stage_it = _value_clip_stage_cache.find(resolved_asset_path);
  if (stage_it != _value_clip_stage_cache.end()) {
    *stage_out = stage_it->second;
    return true;
  }

  std::shared_ptr<Layer> layer;
  if (!LoadValueClipLayer(env, resolved_asset_path, &layer) || !layer) {
    return false;
  }

  Layer layer_copy = *layer;

  Stage clip_stage;
  std::string warn, err;
  if (!LayerToStage(std::move(layer_copy), &clip_stage, &warn, &err)) {
    if (!warn.empty()) {
      PUSH_WARN(fmt::format("Failed to convert clip layer to stage ({}): {}", assetPath, warn));
    }
    if (!err.empty()) {
      PUSH_WARN(fmt::format("Failed to convert clip layer to stage ({}): {}", assetPath, err));
    }
    return false;
  }

  auto stage_cache = std::make_shared<Stage>(std::move(clip_stage));
  if (_value_clip_stage_cache.size() >= kMaxValueClipCacheEntries) {
    _value_clip_stage_cache.erase(_value_clip_stage_cache.begin());
  }
  _value_clip_stage_cache[resolved_asset_path] = stage_cache;
  *stage_out = stage_cache;

  return true;
}

bool RenderSceneConverter::ConvertSkelAnimation(const RenderSceneConverterEnv &env,
                                            const Path &abs_path,
                                            const SkelAnimation &skelAnim,
                                            int32_t skeleton_id,
                                            AnimationClip *anim_out) {
  // The spec says:
  // "An animation source is only valid if its translation, rotation, and scale components
  //  are all authored, storing arrays sized to the same size as the authored joints array."
  //
  // Convert USD SkelAnimation to glTF/Three.js compatible AnimationClip structure
  // with flat sampler arrays and channel bindings

  std::vector<value::token> joints;

  if (skelAnim.joints.authored()) {
    if (!EvaluateTypedAttribute(env.stage, skelAnim.joints, "joints", &joints, &_err)) {
      PUSH_ERROR_AND_RETURN(fmt::format("Failed to evaluate `joints` in SkelAnimation Prim : {}", abs_path));
    }

    if (!skelAnim.rotations.authored() ||
        !skelAnim.translations.authored() ||
        !skelAnim.scales.authored()) {

      PUSH_ERROR_AND_RETURN(fmt::format("`translations`, `rotations` and `scales` must be all authored for SkelAnimation Prim {}. authored flags: translations {}, rotations {}, scales {}", abs_path, skelAnim.translations.authored() ? "yes" : "no",
      skelAnim.rotations.authored() ? "yes" : "no",
      skelAnim.scales.authored() ? "yes" : "no"));
    }
  }

  std::vector<value::token> blendShapes;
  if (skelAnim.blendShapes.authored()) {
    std::string blendShapeErr;
    if (!EvaluateTypedAttribute(env.stage, skelAnim.blendShapes, "blendShapes", &blendShapes, &blendShapeErr)) {
      if (skelAnim.blendShapes.is_value_empty()) {
        PUSH_WARN(fmt::format(
            "Skipping empty `blendShapes` declaration in SkelAnimation Prim : {}",
            abs_path));
      } else {
        _err += blendShapeErr;
        PUSH_ERROR_AND_RETURN(fmt::format("Failed to evaluate `blendShapes` in SkelAnimation Prim : {}", abs_path));
      }
    }

    if (blendShapes.size() && !skelAnim.blendShapeWeights.authored()) {
      PUSH_ERROR_AND_RETURN(fmt::format("`blendShapeWeights` must be authored for SkelAnimation Prim {}", abs_path));
    }
  }

  // Setup basic metadata
  anim_out->abs_path = abs_path.full_path_name();
  anim_out->prim_name = skelAnim.name;
  anim_out->name = skelAnim.name;
  anim_out->display_name = skelAnim.metas().has_displayName() ? skelAnim.metas().get_displayName() : "";
  anim_out->duration = 0.0f;  // Will be computed below
  anim_out->source_type = AnimationSourceType::SkelAnimation;
  anim_out->num_animated_joints = int32_t(joints.size());

  // Joint animations - convert to glTF-style flat arrays
  // Strategy: Pre-allocate output samplers, then scatter data directly from
  // TypedTimeSamples into per-joint samplers. Avoids copying all frame data
  // into intermediate vectors.
  if (joints.size()) {
    if (skeleton_id < 0 || skeleton_id >= int32_t(skeletons.size())) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Invalid skeleton_id {} for SkelAnimation {}",
          skeleton_id, abs_path.full_path_name()));
    }

    // SkelAnimation::joints ordering may differ from Skeleton::joints ordering.
    // Build an explicit animation-joint -> skeleton-joint remap once, then use
    // canonical skeleton joint IDs in all emitted channels.
    auto cache_it = _skelNameToIndexCache.find(skeleton_id);
    if (cache_it == _skelNameToIndexCache.end()) {
      cache_it = _skelNameToIndexCache
                     .emplace(skeleton_id,
                              BuildSkelNameToIndexMap(skeletons[size_t(skeleton_id)]))
                     .first;
    }
    const auto &token_to_index_map = cache_it->second;

    auto normalize_joint_token = [](const std::string &token,
                                    std::string *out) -> bool {
      if (!token.empty() && token[0] == '/') {
        *out = token.substr(1);
        return true;
      }
      return false;
    };

    Animatable<std::vector<value::float3>> translations;
    if (!skelAnim.translations.get_value(&translations)) {
      PUSH_ERROR_AND_RETURN(fmt::format("Failed to get `translations` attribute of SkelAnimation: {}", abs_path));
    }

    Animatable<std::vector<value::quatf>> rotations;
    if (!skelAnim.rotations.get_value(&rotations)) {
      PUSH_ERROR_AND_RETURN(fmt::format("Failed to get `rotations` attribute of SkelAnimation: {}", abs_path));
    }

    Animatable<std::vector<value::half3>> scales;
    if (!skelAnim.scales.get_value(&scales)) {
      PUSH_ERROR_AND_RETURN(fmt::format("Failed to get `scales` attribute of SkelAnimation: {}", abs_path));
    }

    size_t nJoints = joints.size();
    std::vector<int32_t> anim_joint_to_skel_joint(nJoints, -1);
    for (size_t j = 0; j < nJoints; j++) {
      const std::string joint_token = joints[j].str();

      auto token_it = token_to_index_map.find(joint_token);
      if (token_it != token_to_index_map.end()) {
        anim_joint_to_skel_joint[j] = int32_t(token_it->second);
        continue;
      }

      std::string normalized_joint_token;
      if (normalize_joint_token(joint_token, &normalized_joint_token)) {
        auto norm_it = token_to_index_map.find(normalized_joint_token);
        if (norm_it != token_to_index_map.end()) {
          anim_joint_to_skel_joint[j] = int32_t(norm_it->second);
          continue;
        }
      }

      PUSH_ERROR_AND_RETURN(fmt::format(
          "SkelAnimation joint token '{}' is not found in Skeleton {} (id={})",
          joint_token, skeletons[size_t(skeleton_id)].abs_path, skeleton_id));
    }

    // Count frames for each property (first pass - no data copy)
    size_t nTransTimes = 0, nRotTimes = 0, nScaleTimes = 0;
    if (translations.has_timesamples()) {
      nTransTimes = translations.get_timesamples_ptr()->size();
    } else if (translations.has_value()) {
      nTransTimes = 1;
    }
    if (rotations.has_timesamples()) {
      nRotTimes = rotations.get_timesamples_ptr()->size();
    } else if (rotations.has_value()) {
      nRotTimes = 1;
    }
    if (scales.has_timesamples()) {
      nScaleTimes = scales.get_timesamples_ptr()->size();
    } else if (scales.has_value()) {
      nScaleTimes = 1;
    }

    // Pre-allocate all output samplers and channels
    size_t nProps = (nTransTimes ? 1 : 0) + (nRotTimes ? 1 : 0) + (nScaleTimes ? 1 : 0);
    size_t totalSamplers = nJoints * nProps;
    size_t baseSamplerIdx = anim_out->samplers.size();
    anim_out->samplers.resize(baseSamplerIdx + totalSamplers);
    size_t baseChannelIdx = anim_out->channels.size();
    anim_out->channels.resize(baseChannelIdx + totalSamplers);

    // Allocate flat bulk buffers for scatter writes (avoids per-sampler resize zero-fill).
    // Layout: [joint0_frame0, joint0_frame1, ..., joint1_frame0, ...] (joint-major)
    // Scatter writes in frame-major order; final copy to per-sampler vectors is joint-major memcpy.
    size_t transBufTimesSize = 0, transBufValsSize = 0;
    size_t rotBufTimesSize = 0, rotBufValsSize = 0;
    size_t scaleBufTimesSize = 0, scaleBufValsSize = 0;

    if (nTransTimes) {
      if (!safe::mul(nJoints, nTransTimes, &transBufTimesSize)) {
        PUSH_ERROR_AND_RETURN("Integer overflow: nJoints * nTransTimes");
      }
      if (!safe::mul3(nJoints, nTransTimes, size_t(3), &transBufValsSize)) {
        PUSH_ERROR_AND_RETURN("Integer overflow: nJoints * nTransTimes * 3");
      }
    }
    if (nRotTimes) {
      if (!safe::mul(nJoints, nRotTimes, &rotBufTimesSize)) {
        PUSH_ERROR_AND_RETURN("Integer overflow: nJoints * nRotTimes");
      }
      if (!safe::mul3(nJoints, nRotTimes, size_t(4), &rotBufValsSize)) {
        PUSH_ERROR_AND_RETURN("Integer overflow: nJoints * nRotTimes * 4");
      }
    }
    if (nScaleTimes) {
      if (!safe::mul(nJoints, nScaleTimes, &scaleBufTimesSize)) {
        PUSH_ERROR_AND_RETURN("Integer overflow: nJoints * nScaleTimes");
      }
      if (!safe::mul3(nJoints, nScaleTimes, size_t(3), &scaleBufValsSize)) {
        PUSH_ERROR_AND_RETURN("Integer overflow: nJoints * nScaleTimes * 3");
      }
    }

    // Single allocation for all bulk data — compute total with overflow checks
    size_t totalFloats = 0;
    if (!safe::add(transBufTimesSize, transBufValsSize, &totalFloats) ||
        !safe::add(totalFloats, rotBufTimesSize, &totalFloats) ||
        !safe::add(totalFloats, rotBufValsSize, &totalFloats) ||
        !safe::add(totalFloats, scaleBufTimesSize, &totalFloats) ||
        !safe::add(totalFloats, scaleBufValsSize, &totalFloats)) {
      PUSH_ERROR_AND_RETURN("Integer overflow in totalFloats computation");
    }
    std::unique_ptr<float[]> bulkBuf(new float[totalFloats]);
    float *ptr = bulkBuf.get();

    float *transTimesBuf = ptr; ptr += transBufTimesSize;
    float *transValsBuf  = ptr; ptr += transBufValsSize;
    float *rotTimesBuf   = ptr; ptr += rotBufTimesSize;
    float *rotValsBuf    = ptr; ptr += rotBufValsSize;
    float *scaleTimesBuf = ptr; ptr += scaleBufTimesSize;
    float *scaleValsBuf  = ptr; ptr += scaleBufValsSize;

    // Setup channels (no value arrays yet — will be assigned after scatter)
    for (size_t j = 0; j < nJoints; j++) {
      const int32_t resolved_joint_id = anim_joint_to_skel_joint[j];
      size_t samplerOff = baseSamplerIdx + j * nProps;
      size_t channelOff = baseChannelIdx + j * nProps;
      size_t pi = 0;
      if (nTransTimes) {
        anim_out->samplers[samplerOff + pi].interpolation = AnimationInterpolation::Linear;
        auto &ch = anim_out->channels[channelOff + pi];
        ch.target_type = ChannelTargetType::SkeletonJoint;
        ch.path = AnimationPath::Translation;
        ch.skeleton_id = skeleton_id;
        ch.joint_id = resolved_joint_id;
        ch.sampler = int32_t(samplerOff + pi);
        pi++;
      }
      if (nRotTimes) {
        anim_out->samplers[samplerOff + pi].interpolation = AnimationInterpolation::Linear;
        auto &ch = anim_out->channels[channelOff + pi];
        ch.target_type = ChannelTargetType::SkeletonJoint;
        ch.path = AnimationPath::Rotation;
        ch.skeleton_id = skeleton_id;
        ch.joint_id = resolved_joint_id;
        ch.sampler = int32_t(samplerOff + pi);
        pi++;
      }
      if (nScaleTimes) {
        anim_out->samplers[samplerOff + pi].interpolation = AnimationInterpolation::Linear;
        auto &ch = anim_out->channels[channelOff + pi];
        ch.target_type = ChannelTargetType::SkeletonJoint;
        ch.path = AnimationPath::Scale;
        ch.skeleton_id = skeleton_id;
        ch.joint_id = resolved_joint_id;
        ch.sampler = int32_t(samplerOff + pi);
        pi++;
      }
    }

    // Scatter into bulk buffers (joint-major layout: [j0_f0, j0_f1, ..., j1_f0, ...])
    // This avoids per-sampler vector::resize() zero-fill overhead.

    // Scatter translations into bulk buffer
    if (nTransTimes) {
      auto scatterTransFrame = [&](size_t frameIdx, float time, const std::vector<value::float3> &frameData) {
        if (frameData.size() != nJoints) {
          _err = fmt::format("Array length mismatch: translations.size {} != joints.size {} at frame {}",
            frameData.size(), nJoints, frameIdx);
          return false;
        }
        if (time > anim_out->duration) anim_out->duration = time;
        for (size_t j = 0; j < nJoints; j++) {
          size_t idx;
          if (!safe::mul(j, nTransTimes, &idx)) { return false; }
          if (!safe::add(idx, frameIdx, &idx)) { return false; }
          transTimesBuf[idx] = time;

          size_t dest_idx;
          if (!safe::mul(idx, 3, &dest_idx)) { return false; }
          memcpy(&transValsBuf[dest_idx], frameData[j].data(), 3 * sizeof(float));
        }
        return true;
      };

      if (translations.has_timesamples()) {
        size_t frameIdx = 0;
        if (const value::TimeSamples *_tsp = translations.get_timesamples_ptr()) {
          for (const auto &_s : _tsp->get_samples()) {
            if (_s.blocked) continue;
            const std::vector<value::float3> *_pv =
                _s.value.as<std::vector<value::float3>>();
            if (!_pv) continue;
            if (!scatterTransFrame(frameIdx, float(_s.t), *_pv)) {
              PUSH_ERROR_AND_RETURN(_err);
            }
            frameIdx++;
          }
        }
      } else {
        std::vector<value::float3> default_value;
        if (!translations.get_scalar(&default_value)) {
          PUSH_ERROR_AND_RETURN(fmt::format("Failed to get default translations: {}", abs_path));
        }
        if (!scatterTransFrame(0, 0.0f, default_value)) {
          PUSH_ERROR_AND_RETURN(_err);
        }
      }
    }

    // Scatter rotations into bulk buffer
    // quatf layout: { float3 imag; float real; } = 4 contiguous floats
    if (nRotTimes) {
      auto scatterRotFrame = [&](size_t frameIdx, float time, const std::vector<value::quatf> &frameData) {
        if (frameData.size() != nJoints) {
          _err = fmt::format("Array length mismatch: rotations.size {} != joints.size {} at frame {}",
            frameData.size(), nJoints, frameIdx);
          return false;
        }
        if (time > anim_out->duration) anim_out->duration = time;
        for (size_t j = 0; j < nJoints; j++) {
          size_t idx;
          if (!safe::mul(j, nRotTimes, &idx)) { return false; }
          if (!safe::add(idx, frameIdx, &idx)) { return false; }
          rotTimesBuf[idx] = time;

          size_t dest_idx;
          if (!safe::mul(idx, 4, &dest_idx)) { return false; }
          memcpy(&rotValsBuf[dest_idx], &frameData[j], 4 * sizeof(float));
        }
        return true;
      };

      if (rotations.has_timesamples()) {
        size_t frameIdx = 0;
        if (const value::TimeSamples *_tsp = rotations.get_timesamples_ptr()) {
          for (const auto &_s : _tsp->get_samples()) {
            if (_s.blocked) continue;
            const std::vector<value::quatf> *_pv =
                _s.value.as<std::vector<value::quatf>>();
            if (!_pv) continue;
            if (!scatterRotFrame(frameIdx, float(_s.t), *_pv)) {
              PUSH_ERROR_AND_RETURN(_err);
            }
            frameIdx++;
          }
        }
      } else {
        std::vector<value::quatf> default_value;
        if (!rotations.get_scalar(&default_value)) {
          PUSH_ERROR_AND_RETURN(fmt::format("Failed to get default rotations: {}", abs_path));
        }
        if (!scatterRotFrame(0, 0.0f, default_value)) {
          PUSH_ERROR_AND_RETURN(_err);
        }
      }
    }

    // Scatter scales into bulk buffer (with half->float conversion)
    if (nScaleTimes) {
      auto scatterScaleFrame = [&](size_t frameIdx, float time, const std::vector<value::half3> &frameData) {
        if (frameData.size() != nJoints) {
          _err = fmt::format("Array length mismatch: scales.size {} != joints.size {} at frame {}",
            frameData.size(), nJoints, frameIdx);
          return false;
        }
        if (time > anim_out->duration) anim_out->duration = time;
        for (size_t j = 0; j < nJoints; j++) {
          scaleTimesBuf[j * nScaleTimes + frameIdx] = time;
          float *dst = &scaleValsBuf[(j * nScaleTimes + frameIdx) * 3];
          const auto &v = frameData[j];
          dst[0] = value::half_to_float(v[0]);
          dst[1] = value::half_to_float(v[1]);
          dst[2] = value::half_to_float(v[2]);
        }
        return true;
      };

      if (scales.has_timesamples()) {
        size_t frameIdx = 0;
        if (const value::TimeSamples *_tsp = scales.get_timesamples_ptr()) {
          for (const auto &_s : _tsp->get_samples()) {
            if (_s.blocked) continue;
            const std::vector<value::half3> *_pv =
                _s.value.as<std::vector<value::half3>>();
            if (!_pv) continue;
            if (!scatterScaleFrame(frameIdx, float(_s.t), *_pv)) {
              PUSH_ERROR_AND_RETURN(_err);
            }
            frameIdx++;
          }
        }
      } else {
        std::vector<value::half3> default_value;
        if (!scales.get_scalar(&default_value)) {
          PUSH_ERROR_AND_RETURN(fmt::format("Failed to get default scales: {}", abs_path));
        }
        if (!scatterScaleFrame(0, 0.0f, default_value)) {
          PUSH_ERROR_AND_RETURN(_err);
        }
      }
    }

    // Copy from bulk buffers into per-sampler vectors (single memcpy per sampler,
    // using assign() which allocates + copies without zero-fill overhead)
    for (size_t j = 0; j < nJoints; j++) {
      size_t samplerOff = baseSamplerIdx + j * nProps;
      size_t pi = 0;
      if (nTransTimes) {
        auto &s = anim_out->samplers[samplerOff + pi];
        const float *tBase = &transTimesBuf[j * nTransTimes];
        const float *vBase = &transValsBuf[j * nTransTimes * 3];
        s.times.assign(tBase, tBase + nTransTimes);
        s.values.assign(vBase, vBase + nTransTimes * 3);
        pi++;
      }
      if (nRotTimes) {
        auto &s = anim_out->samplers[samplerOff + pi];
        const float *tBase = &rotTimesBuf[j * nRotTimes];
        const float *vBase = &rotValsBuf[j * nRotTimes * 4];
        s.times.assign(tBase, tBase + nRotTimes);
        s.values.assign(vBase, vBase + nRotTimes * 4);
        pi++;
      }
      if (nScaleTimes) {
        auto &s = anim_out->samplers[samplerOff + pi];
        const float *tBase = &scaleTimesBuf[j * nScaleTimes];
        const float *vBase = &scaleValsBuf[j * nScaleTimes * 3];
        s.times.assign(tBase, tBase + nScaleTimes);
        s.values.assign(vBase, vBase + nScaleTimes * 3);
        pi++;
      }
    }

    if (!CompactNewSamplers(anim_out, baseSamplerIdx, baseChannelIdx)) {
      PUSH_ERROR_AND_RETURN(
          "Too many animation samplers after SkelAnimation deduplication");
    }
  }

  if (blendShapes.size()) {
    Animatable<std::vector<float>> weights;
    if (!skelAnim.blendShapeWeights.get_value(&weights)) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Failed to get `blendShapeWeights` attribute of SkelAnimation: {}",
          abs_path));
    }

    std::vector<std::string> blendshape_names;
    blendshape_names.reserve(blendShapes.size());
    for (const value::token &tok : blendShapes) {
      blendshape_names.push_back(tok.str());
    }

    KeyframeSampler sampler;
    sampler.interpolation = AnimationInterpolation::Linear;

    auto appendWeightsFrame = [&](size_t frameIdx, float time,
                                  const std::vector<float> &frameData) {
      (void)frameIdx;
      if (frameData.size() != blendShapes.size()) {
        _err = fmt::format(
            "Array length mismatch: blendShapeWeights.size {} != "
            "blendShapes.size {} at frame {}",
            frameData.size(), blendShapes.size(), frameIdx);
        return false;
      }
      sampler.times.push_back(time);
      sampler.values.insert(sampler.values.end(), frameData.begin(),
                            frameData.end());
      if (time > anim_out->duration) {
        anim_out->duration = time;
      }
      return true;
    };

    if (weights.has_timesamples()) {
      size_t frameIdx = 0;
      if (const value::TimeSamples *tsp = weights.get_timesamples_ptr()) {
        sampler.times.reserve(tsp->size());
        size_t value_count = 0;
        if (!safe::mul(tsp->size(), blendShapes.size(), &value_count)) {
          PUSH_ERROR_AND_RETURN(
              "Integer overflow in blendShapeWeights sampler size.");
        }
        sampler.values.reserve(value_count);
        for (const auto &sample : tsp->get_samples()) {
          if (sample.blocked) {
            continue;
          }
          const std::vector<float> *pv =
              sample.value.as<std::vector<float>>();
          if (!pv) {
            continue;
          }
          if (!appendWeightsFrame(frameIdx, float(sample.t), *pv)) {
            PUSH_ERROR_AND_RETURN(_err);
          }
          frameIdx++;
        }
      }
    } else if (weights.has_value()) {
      std::vector<float> default_value;
      if (!weights.get_scalar(&default_value)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Failed to get default blendShapeWeights: {}", abs_path));
      }
      if (!appendWeightsFrame(0, 0.0f, default_value)) {
        PUSH_ERROR_AND_RETURN(_err);
      }
    }

    if (sampler.times.empty()) {
      PUSH_WARN(fmt::format(
          "Skipping empty blendShapeWeights animation for SkelAnimation {}",
          abs_path.full_path_name()));
    } else {
      const int32_t sampler_index =
          static_cast<int32_t>(anim_out->samplers.size());
      anim_out->samplers.push_back(std::move(sampler));

      AnimationChannel channel;
      channel.target_type = ChannelTargetType::SceneNode;
      channel.path = AnimationPath::Weights;
      channel.target_node = -1;
      channel.skeleton_id = skeleton_id;
      channel.sampler = sampler_index;
      channel.property_name = "blendShapeWeights";
      channel.target_prim_path.clear();
      channel.blendshape_target_names = std::move(blendshape_names);
      anim_out->channels.push_back(std::move(channel));
    }
  }

  return true;
}

//
}  // namespace tydra
}  // namespace lightusd
