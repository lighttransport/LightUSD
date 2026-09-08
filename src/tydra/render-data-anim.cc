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
}

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

bool RenderSceneConverter::ConvertValueClipAnimation(
    const RenderSceneConverterEnv &env,
    const Prim &prim,
    const Path &abs_path,
    int32_t target_node_index,
    AnimationClip *anim_out) {

  if (!anim_out) {
    PUSH_ERROR_AND_RETURN("anim_out is nullptr");
  }

  if (!prim.metas().has_clips()) {
    return false;
  }

  const auto &clips_dict = prim.metas().get_clips();
  if (clips_dict.empty()) {
    return false;
  }

  ClipSetMetadata clip_meta;
  std::string err;
  if (!ParseClipSetMetadataFull(clips_dict, &clip_meta, &err)) {
    if (!err.empty()) {
      PUSH_WARN(fmt::format("Failed to parse clip metadata for {}: {}", abs_path.full_path_name(), err));
    }
    return false;
  }

  if (clip_meta.assetPaths.empty()) {
    return false;
  }

  if (clip_meta.active.empty() || clip_meta.times.empty()) {
    PUSH_WARN(fmt::format("Clip metadata for {} is incomplete (active={}, times={})",
                         abs_path.full_path_name(),
                         clip_meta.active.empty() ? "empty" : "ok",
                         clip_meta.times.empty() ? "empty" : "ok"));
  }

  std::vector<std::pair<double, double>> times = clip_meta.times;
  std::vector<std::pair<double, int>> active = clip_meta.active;
  // static storage duration so the comparison lambdas below can use it
  // without capturing: clang's -Wunused-lambda-capture rejects capturing a
  // constant expression, while MSVC's C3493 rejects using a local one without
  // a capture. A static constexpr sidesteps both.
  static constexpr double kTimeKeyEpsilon = std::numeric_limits<double>::epsilon();

  if (times.size() >= 2) {
    std::sort(times.begin(), times.end(),
              [](const std::pair<double, double> &a, const std::pair<double, double> &b) {
                return a.first < b.first;
              });
    times.erase(std::unique(times.begin(), times.end(),
                           [](const std::pair<double, double> &a,
                             const std::pair<double, double> &b) {
                             return std::fabs(a.first - b.first) <= kTimeKeyEpsilon;
                           }),
               times.end());
  }

  if (active.size() >= 2) {
    std::sort(active.begin(), active.end(),
              [](const std::pair<double, int> &a, const std::pair<double, int> &b) {
                return a.first < b.first;
              });
    active.erase(std::unique(active.begin(), active.end(),
                            [](const std::pair<double, int> &a,
                              const std::pair<double, int> &b) {
                              return std::fabs(a.first - b.first) <= kTimeKeyEpsilon;
                            }),
                active.end());
  }

  std::vector<double> sample_times;
  {
    double start_t = 0.0;
    double end_t = 0.0;
    bool has_range = false;
    if (env.scene_config.value_clip_use_time_range) {
      start_t = env.scene_config.value_clip_start_time;
      end_t = env.scene_config.value_clip_end_time;
      has_range = true;
      if (end_t < start_t) {
        std::swap(start_t, end_t);
      }
    } else if (!times.empty()) {
      start_t = times.front().first;
      end_t = times.back().first;
      has_range = true;
    } else if (!active.empty()) {
      start_t = active.front().first;
      end_t = active.back().first;
      has_range = true;
    } else if (env.stage.metas().startTimeCode.authored() &&
               env.stage.metas().endTimeCode.authored()) {
      start_t = env.stage.metas().startTimeCode.get_value();
      end_t = env.stage.metas().endTimeCode.get_value();
      has_range = true;
      if (end_t < start_t) {
        std::swap(start_t, end_t);
      }
    }

    if (env.scene_config.value_clip_sample_rate > 0.0f && has_range) {
      double sample_rate = double(env.scene_config.value_clip_sample_rate);
      if (sample_rate <= 0.0) {
        return false;
      }
      double dt = 1.0 / sample_rate;
      if (dt > 0.0) {
        for (double t = start_t; t <= end_t + dt * 0.5; t += dt) {
          double clamped_t = (t > end_t) ? end_t : t;
          sample_times.push_back(clamped_t);
          if (clamped_t >= end_t) {
            break;
          }
        }
      }
    } else if (!times.empty()) {
      for (const auto &kv : times) {
        sample_times.push_back(kv.first);
      }
    } else if (!active.empty()) {
      for (const auto &kv : active) {
        sample_times.push_back(kv.first);
      }
    } else if (has_range) {
      sample_times.push_back(start_t);
      sample_times.push_back(end_t);
    }
  }

  if (sample_times.empty()) {
    // Fallback single sample to avoid false negatives on non-animated metadata.
    sample_times.push_back(0.0);
  }

  std::sort(sample_times.begin(), sample_times.end());
  sample_times.erase(std::unique(sample_times.begin(), sample_times.end()),
                    sample_times.end());

  std::vector<std::string> clip_prim_path_candidates;
  {
    std::string p = clip_meta.primPath.empty() ? abs_path.full_path_name()
                                               : clip_meta.primPath;
    clip_prim_path_candidates.push_back(p);

    if (!p.empty() && p[0] != '/') {
      std::string rel = p;
      if (rel == ".") {
        rel.clear();
      } else if (rel.size() >= 2 && rel[0] == '.' && rel[1] == '/') {
        rel = rel.substr(2);
      }

      Path parent = abs_path.get_parent_prim_path();
      if (parent.is_valid()) {
        std::string parent_path = parent.full_path_name();
        if (!parent_path.empty() && parent_path != "/") {
          if (parent_path.back() != '/') {
            parent_path.push_back('/');
          }
          if (!rel.empty()) {
            clip_prim_path_candidates.push_back(parent_path + rel);
          } else {
            clip_prim_path_candidates.push_back(parent_path);
          }
        } else if (!rel.empty()) {
          clip_prim_path_candidates.push_back(std::string("/") + rel);
        }
      }
    }

    if (clip_prim_path_candidates.empty()) {
      clip_prim_path_candidates.push_back(abs_path.full_path_name());
    }

    std::sort(clip_prim_path_candidates.begin(),
              clip_prim_path_candidates.end());
    clip_prim_path_candidates.erase(std::unique(clip_prim_path_candidates.begin(),
                                                clip_prim_path_candidates.end()),
                                    clip_prim_path_candidates.end());
  }

  auto resolve_clip_matrix = [this, &clip_prim_path_candidates, &env](
      const std::string &clip_asset_path,
      double clip_time, value::matrix4d *matrix_out) -> bool {

    std::shared_ptr<Stage> clip_stage;
    if (!LoadValueClipStage(env, clip_asset_path, &clip_stage) || !clip_stage) {
      return false;
    }

    const Prim *clip_prim = nullptr;
    std::string find_err;
    for (const auto &candidate : clip_prim_path_candidates) {
      if (candidate.empty()) {
        continue;
      }
      Path clip_path(candidate, "");
      if (clip_path.is_valid() &&
          clip_stage->find_prim_at_path(clip_path, clip_prim, &find_err)) {
        break;
      }
    }

    if (!clip_prim) {
      if (!find_err.empty()) {
        PUSH_WARN(fmt::format("Failed to resolve clip prim in '{}': {}", clip_asset_path, find_err));
      }
      return false;
    }

    const Xformable *clip_xformable = nullptr;
    if (!CastToXformable(*clip_prim, &clip_xformable) || !clip_xformable) {
      return false;
    }

    bool resetXformStack = false;
    auto clip_matrix = clip_xformable->GetLocalMatrix(
        clip_time, value::TimeSampleInterpolationType::Linear, &resetXformStack);
    if (!clip_matrix) {
      if (!find_err.empty()) {
        PUSH_WARN(fmt::format("Failed to evaluate matrix from clip '{}' at t {}: {}",
                             clip_asset_path, clip_time, clip_matrix.error()));
      }
      return false;
    }

    *matrix_out = clip_matrix.value();
    return true;
  };

  auto collect_matrix_timeseries =
      [&](std::vector<float> *times_out,
          std::vector<float> *trans_values_out,
          std::vector<float> *rot_values_out,
          std::vector<float> *scale_values_out) -> bool {
    bool has_any = false;
    for (const double stage_time : sample_times) {
      std::string clip_asset_path;
      double clip_time = 0.0;
      bool has_query = ResolveValueClipQuery(active, times, clip_meta.assetPaths,
                                            stage_time, &clip_asset_path, &clip_time);

      value::matrix4d mat;
      bool got_value = false;

      if (has_query) {
        if (resolve_clip_matrix(clip_asset_path, clip_time, &mat)) {
          got_value = true;
        }
      }

      if (!got_value &&
          clip_meta.interpolateMissingClipValues &&
          (active.size() >= 2)) {
        int active_entry_idx = -1;
        for (size_t i = 0; i < active.size(); i++) {
          if (active[i].first <= stage_time) {
            active_entry_idx = static_cast<int>(i);
          } else {
            break;
          }
        }
        if (active_entry_idx < 0) {
          active_entry_idx = 0;
        }

        value::matrix4d prev_mat, next_mat;
        bool has_prev = false;
        bool has_next = false;

        for (int i = static_cast<int>(active_entry_idx) - 1; i >= 0; i--) {
          int asset_idx = active[static_cast<size_t>(i)].second;
          if (asset_idx >= 0 &&
              asset_idx < static_cast<int>(clip_meta.assetPaths.size()) &&
              resolve_clip_matrix(clip_meta.assetPaths[static_cast<size_t>(asset_idx)],
                                 RemapStageTimeToClipTime(times,
                                                          active[static_cast<size_t>(i)].first),
                                 &prev_mat)) {
            has_prev = true;
            break;
          }
        }

        for (size_t i = static_cast<size_t>(active_entry_idx + 1);
             i < active.size(); i++) {
          int asset_idx = active[i].second;
          if (asset_idx >= 0 &&
              asset_idx < static_cast<int>(clip_meta.assetPaths.size()) &&
              resolve_clip_matrix(clip_meta.assetPaths[static_cast<size_t>(asset_idx)],
                                 RemapStageTimeToClipTime(times, active[i].first),
                                 &next_mat)) {
            has_next = true;
            break;
          }
        }

        if (has_prev) {
          mat = prev_mat;
          got_value = true;
        } else if (has_next) {
          mat = next_mat;
          got_value = true;
        }
      }

      if (!got_value) {
        continue;
      }

      value::double3 translation;
      value::quatd rotation;
      value::double3 scale;

      if (!decompose(mat, &translation, &rotation, &scale)) {
        PUSH_WARN(fmt::format(
            "Failed to decompose value clip matrix at stage time {} for {}",
            stage_time, abs_path.full_path_name()));
        continue;
      }

      times_out->push_back(static_cast<float>(stage_time));
      trans_values_out->push_back(static_cast<float>(translation[0]));
      trans_values_out->push_back(static_cast<float>(translation[1]));
      trans_values_out->push_back(static_cast<float>(translation[2]));

      rot_values_out->push_back(static_cast<float>(rotation.imag[0]));
      rot_values_out->push_back(static_cast<float>(rotation.imag[1]));
      rot_values_out->push_back(static_cast<float>(rotation.imag[2]));
      rot_values_out->push_back(static_cast<float>(rotation.real));

      scale_values_out->push_back(static_cast<float>(scale[0]));
      scale_values_out->push_back(static_cast<float>(scale[1]));
      scale_values_out->push_back(static_cast<float>(scale[2]));

      has_any = true;
    }
    return has_any;
  };

  auto collect_custom_attr_timeseries =
      [&](const std::string &attr_name,
          std::vector<float> *times_out, std::vector<float> *values_out,
          size_t *component_count_out) -> bool {
    if (!times_out || !values_out || !component_count_out) {
      return false;
    }

    size_t local_component_count = 0;
    for (const double stage_time : sample_times) {
      TerminalAttributeValue value;
      std::string eval_err;
      if (!EvaluateAttributeFromClips(prim, attr_name, &value, &eval_err,
                                     stage_time,
                                     value::TimeSampleInterpolationType::Linear)) {
        if (!eval_err.empty()) {
          PUSH_WARN(fmt::format("Failed to evaluate attribute '{}' at t={} for {}: {}",
                               attr_name, stage_time, abs_path.full_path_name(),
                               eval_err));
        } else {
          PUSH_WARN(fmt::format("Failed to evaluate attribute '{}' at t={} for {}",
                               attr_name, stage_time, abs_path.full_path_name()));
        }
        continue;
      }

      size_t expected_count = local_component_count;
      size_t prev_values = values_out->size();
      if (!AppendValueToFloatArray(value, values_out, &expected_count)) {
        if (!eval_err.empty()) {
          PUSH_WARN(fmt::format("Skipping clip attribute '{}' for {}: {}",
                               attr_name, abs_path.full_path_name(),
                               eval_err));
        } else {
          PUSH_WARN(fmt::format("Skipping clip attribute '{}' for {} due to unsupported type.",
                               attr_name, abs_path.full_path_name()));
        }
        values_out->resize(prev_values);
        return false;
      }

      times_out->push_back(static_cast<float>(stage_time));
      local_component_count = expected_count;
    }

    if (!times_out->empty()) {
      *component_count_out = local_component_count;
      return true;
    }
    return false;
  };

  std::vector<float> times_f;
  std::vector<float> trans_values;
  std::vector<float> rot_values;
  std::vector<float> scale_values;
  bool has_matrix_data = collect_matrix_timeseries(&times_f, &trans_values,
                                                  &rot_values, &scale_values);

  std::vector<std::string> attr_names;
  {
    std::string attr_err;
    if (!GetAttributeNames(prim, &attr_names, &attr_err)) {
      if (!attr_err.empty()) {
        PUSH_WARN(fmt::format("Failed to collect attributes for value clip on {}: {}",
                             abs_path.full_path_name(), attr_err));
      }
    }
  }

  std::set<std::string> attr_name_set;
  for (const auto &name : attr_names) {
    attr_name_set.insert(name);
  }

  auto collect_clip_attr_names = [&](const std::string &clip_asset_path) -> void {
    std::shared_ptr<Stage> clip_stage;
      if (!LoadValueClipStage(env, clip_asset_path, &clip_stage) || !clip_stage) {
      PUSH_WARN(fmt::format(
          "Failed to load value clip stage for attribute discovery: {}",
          clip_asset_path));
      return;
    }

    const Prim *clip_ps = nullptr;
    std::string find_err;
    for (const auto &candidate : clip_prim_path_candidates) {
      if (candidate.empty()) {
        continue;
      }
      Path clip_path(candidate, "");
      if (clip_path.is_valid() &&
          clip_stage->find_prim_at_path(clip_path, clip_ps, &find_err)) {
        break;
      }
    }

    if (!clip_ps) {
      if (!find_err.empty()) {
        PUSH_WARN(fmt::format("Failed to find clip prim {} in clip stage {}: {}",
                              clip_meta.primPath, clip_asset_path, find_err));
      }
      return;
    }

    std::vector<std::string> clip_attr_names;
    std::string clip_attr_err;
    if (!GetAttributeNames(*clip_ps, &clip_attr_names, &clip_attr_err)) {
      if (!clip_attr_err.empty()) {
        PUSH_WARN(fmt::format("Failed to collect clip attributes for {} in {}: {}",
                              clip_meta.primPath, clip_asset_path,
                              clip_attr_err));
      }
      return;
    }

    for (const auto &name : clip_attr_names) {
      attr_name_set.insert(name);
    }
  };

  for (const auto &clip_asset_path : clip_meta.assetPaths) {
    collect_clip_attr_names(clip_asset_path);
  }

  if (attr_name_set.empty()) {
    return false;
  }

  attr_names.assign(attr_name_set.begin(), attr_name_set.end());

  struct CustomPropertyData {
    std::string name;
    std::vector<float> times;
    std::vector<float> values;
    size_t component_count{0};
  };
  std::vector<CustomPropertyData> custom_properties;

  for (size_t i = 0; i < attr_names.size(); i++) {
    const auto &attr_name = attr_names[i];
    if (IsClipTransformAttribute(attr_name)) {
      continue;
    }
    CustomPropertyData data;
    data.name = attr_name;
    size_t component_count = 0;
    if (!collect_custom_attr_timeseries(attr_name, &data.times, &data.values,
                                       &component_count)) {
      continue;
    }
    data.component_count = component_count;
    custom_properties.push_back(std::move(data));
  }

  const bool has_custom_data = !custom_properties.empty();
  if (!has_matrix_data && !has_custom_data) {
    return false;
  }

  auto get_time_range = [](const std::vector<float> &values) -> std::pair<float, float> {
    if (values.empty()) {
      return {0.0f, 0.0f};
    }
    return {values.front(), values.back()};
  };

  auto get_property_component_count =
      [](const CustomPropertyData &data) -> size_t {
    if (data.times.empty()) {
      return 0;
    }
    if (data.values.empty()) {
      return 0;
    }
    size_t count = data.values.size() / data.times.size();
    return count;
  };

  if (has_matrix_data) {
    auto time_range = get_time_range(times_f);
    if (time_range.first < std::numeric_limits<float>::infinity()) {
      anim_out->value_clip_start_time = time_range.first;
      anim_out->value_clip_end_time = time_range.second;
    }
  }

  float clip_start_time = anim_out->value_clip_start_time;
  float clip_end_time = anim_out->value_clip_end_time;
  if (!has_matrix_data) {
    clip_start_time = std::numeric_limits<float>::infinity();
    clip_end_time = -std::numeric_limits<float>::infinity();
  }

  for (const auto &data : custom_properties) {
    const size_t comp_count = get_property_component_count(data);
    if (comp_count == 0 || data.values.size() != data.times.size() * comp_count) {
      continue;
    }

    auto range = get_time_range(data.times);
    if (range.first < clip_start_time) {
      clip_start_time = range.first;
    }
    if (range.second > clip_end_time) {
      clip_end_time = range.second;
    }
  }

  if (!std::isfinite(clip_start_time) || !std::isfinite(clip_end_time)) {
    return false;
  }

  anim_out->abs_path = abs_path.full_path_name();
  anim_out->prim_name = abs_path.element_name();
  anim_out->name = abs_path.element_name() + "_xform_valueclip";
  anim_out->duration = clip_end_time;
  anim_out->source_type = AnimationSourceType::XformOp;
  anim_out->num_animated_nodes = 1;
  anim_out->has_value_clip = true;
  anim_out->value_clip_baked = true;
  anim_out->value_clip_start_time = clip_start_time;
  anim_out->value_clip_end_time = clip_end_time;
  anim_out->value_clip_sample_rate = env.scene_config.value_clip_sample_rate;
  anim_out->clip_asset_paths = clip_meta.assetPaths;

  if (has_matrix_data) {
    KeyframeSampler trans_sampler;
    trans_sampler.interpolation = AnimationInterpolation::Linear;
    trans_sampler.times = times_f;
    trans_sampler.values = std::move(trans_values);
    anim_out->samplers.push_back(std::move(trans_sampler));

    KeyframeSampler rot_sampler;
    rot_sampler.interpolation = AnimationInterpolation::Linear;
    rot_sampler.times = times_f;
    rot_sampler.values = std::move(rot_values);
    anim_out->samplers.push_back(std::move(rot_sampler));

    KeyframeSampler scale_sampler;
    scale_sampler.interpolation = AnimationInterpolation::Linear;
    scale_sampler.times = times_f;
    scale_sampler.values = std::move(scale_values);
    anim_out->samplers.push_back(std::move(scale_sampler));

    {
      AnimationChannel channel;
      channel.target_type = ChannelTargetType::SceneNode;
      channel.path = AnimationPath::Translation;
      channel.target_node = target_node_index;
      channel.target_prim_path = abs_path.full_path_name();
      channel.sampler = 0;
      anim_out->channels.push_back(channel);
    }
    {
      AnimationChannel channel;
      channel.target_type = ChannelTargetType::SceneNode;
      channel.path = AnimationPath::Rotation;
      channel.target_node = target_node_index;
      channel.target_prim_path = abs_path.full_path_name();
      channel.sampler = 1;
      anim_out->channels.push_back(channel);
    }
    {
      AnimationChannel channel;
      channel.target_type = ChannelTargetType::SceneNode;
      channel.path = AnimationPath::Scale;
      channel.target_node = target_node_index;
      channel.target_prim_path = abs_path.full_path_name();
      channel.sampler = 2;
      anim_out->channels.push_back(channel);
    }
  }

  for (const auto &data : custom_properties) {
    const size_t comp_count = get_property_component_count(data);
    if (comp_count == 0) {
      continue;
    }

    KeyframeSampler sampler;
    sampler.interpolation = AnimationInterpolation::Linear;
    sampler.times = data.times;
    sampler.values = data.values;
    const int sampler_index = static_cast<int32_t>(anim_out->samplers.size());
    anim_out->samplers.push_back(std::move(sampler));

    AnimationChannel channel;
    channel.target_type = ChannelTargetType::SceneNode;
    channel.path = AnimationPath::CustomProperty;
    channel.target_node = target_node_index;
    channel.target_prim_path = abs_path.full_path_name();
    channel.sampler = sampler_index;
    channel.is_custom_property = true;
    channel.property_name = data.name;
    anim_out->channels.push_back(std::move(channel));
  }

  return true;
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

//
// Light conversion implementations
//

// Helper to extract common light properties
template<typename LightType>
static bool ExtractCommonLightProperties(
    const RenderSceneConverterEnv &env,
    const LightType &light,  // BoundableLight or NonboundableLight
    RenderLight *rlight) {

  // Extract color
  if (light.color.authored() && !light.color.is_blocked()) {
    value::color3f col;
    if (light.color.get_value().get(env.timecode, &col)) {
      rlight->color[0] = col[0];
      rlight->color[1] = col[1];
      rlight->color[2] = col[2];
    }
  }

  // Extract intensity
  if (light.intensity.authored() && !light.intensity.is_blocked()) {
    float val;
    if (light.intensity.get_value().get(env.timecode, &val)) {
      rlight->intensity = val;
    }
  }

  // Extract exposure
  if (light.exposure.authored() && !light.exposure.is_blocked()) {
    float val;
    if (light.exposure.get_value().get(env.timecode, &val)) {
      rlight->exposure = val;
    }
  }

  // Extract diffuse multiplier
  if (light.diffuse.authored() && !light.diffuse.is_blocked()) {
    float val;
    if (light.diffuse.get_value().get(env.timecode, &val)) {
      rlight->diffuse = val;
    }
  }

  // Extract specular multiplier
  if (light.specular.authored() && !light.specular.is_blocked()) {
    float val;
    if (light.specular.get_value().get(env.timecode, &val)) {
      rlight->specular = val;
    }
  }

  // Extract normalize flag
  if (light.normalize.authored() && !light.normalize.is_blocked()) {
    bool val;
    if (light.normalize.get_value().get(env.timecode, &val)) {
      rlight->normalize = val;
    }
  }

  // Extract color temperature
  if (light.enableColorTemperature.authored() && !light.enableColorTemperature.is_blocked()) {
    bool val;
    if (light.enableColorTemperature.get_value().get(env.timecode, &val)) {
      rlight->enableColorTemperature = val;
    }
  }

  if (light.colorTemperature.authored() && !light.colorTemperature.is_blocked()) {
    float val;
    if (light.colorTemperature.get_value().get(env.timecode, &val)) {
      rlight->colorTemperature = val;
    }
  }

  // Extract shadow properties
  if (light.shadowEnable.authored() && !light.shadowEnable.is_blocked()) {
    bool val;
    if (light.shadowEnable.get_value().get(env.timecode, &val)) {
      rlight->shadowEnable = val;
    }
  }

  if (light.shadowColor.authored() && !light.shadowColor.is_blocked()) {
    value::color3f col;
    if (light.shadowColor.get_value().get(env.timecode, &col)) {
      rlight->shadowColor[0] = col[0];
      rlight->shadowColor[1] = col[1];
      rlight->shadowColor[2] = col[2];
    }
  }

  if (light.shadowDistance.authored() && !light.shadowDistance.is_blocked()) {
    float val;
    if (light.shadowDistance.get_value().get(env.timecode, &val)) {
      rlight->shadowDistance = val;
    }
  }

  if (light.shadowFalloff.authored() && !light.shadowFalloff.is_blocked()) {
    float val;
    if (light.shadowFalloff.get_value().get(env.timecode, &val)) {
      rlight->shadowFalloff = val;
    }
  }

  if (light.shadowFalloffGamma.authored() && !light.shadowFalloffGamma.is_blocked()) {
    float val;
    if (light.shadowFalloffGamma.get_value().get(env.timecode, &val)) {
      rlight->shadowFalloffGamma = val;
    }
  }

  if (light.spectralEmission.authored() &&
      !light.spectralEmission.is_blocked()) {
    std::vector<value::float2> samples;
    if (light.spectralEmission.get_value(&samples) && !samples.empty()) {
      SpectralEmission spd;
      spd.samples = std::move(samples);
      rlight->spd_emission = std::move(spd);
    }
  }

  return true;
}

// Helper to extract shaping properties (for SphereLight and RectLight)
template<typename LightType>
static bool ExtractShapingProperties(
    const RenderSceneConverterEnv &env,
    const LightType &light,  // BoundableLight with shapingFocus, etc.
    RenderLight *rlight) {

  if (light.shapingFocus.authored() && !light.shapingFocus.is_blocked()) {
    float val;
    if (light.shapingFocus.get_value().get(env.timecode, &val)) {
      rlight->shapingFocus = val;
    }
  }

  if (light.shapingFocusTint.authored() && !light.shapingFocusTint.is_blocked()) {
    value::color3f col;
    if (light.shapingFocusTint.get_value().get(env.timecode, &col)) {
      rlight->shapingFocusTint[0] = col[0];
      rlight->shapingFocusTint[1] = col[1];
      rlight->shapingFocusTint[2] = col[2];
    }
  }

  if (light.shapingConeAngle.authored() && !light.shapingConeAngle.is_blocked()) {
    float val;
    if (light.shapingConeAngle.get_value().get(env.timecode, &val)) {
      rlight->shapingConeAngle = val;
    }
  }

  if (light.shapingConeSoftness.authored() && !light.shapingConeSoftness.is_blocked()) {
    float val;
    if (light.shapingConeSoftness.get_value().get(env.timecode, &val)) {
      rlight->shapingConeSoftness = val;
    }
  }

  if (light.shapingIesFile.authored() && !light.shapingIesFile.is_blocked()) {
    value::AssetPath asset;
    std::string eval_err;
    if (EvaluateTypedAnimatableAttribute(
            env.stage, light.shapingIesFile,
            "inputs:shaping:ies:file", &asset, &eval_err, env.timecode,
            env.tinterp)) {
      rlight->shapingIesFile = asset.GetAssetPath();
    }
  }

  if (light.shapingIesAngleScale.authored() &&
      !light.shapingIesAngleScale.is_blocked()) {
    float val;
    if (light.shapingIesAngleScale.get_value().get(env.timecode, &val)) {
      rlight->shapingIesAngleScale = val;
    }
  }

  if (light.shapingIesNormalize.authored() &&
      !light.shapingIesNormalize.is_blocked()) {
    bool val;
    if (light.shapingIesNormalize.get_value().get(env.timecode, &val)) {
      rlight->shapingIesNormalize = val;
    }
  }

  return true;
}

bool RenderSceneConverter::ConvertSphereLight(
    const RenderSceneConverterEnv &env,
    const Path &light_abs_path,
    const SphereLight &light,
    RenderLight *rlight_out) {

  if (!rlight_out) {
    PUSH_ERROR_AND_RETURN("rlight_out arg is nullptr.");
  }

  RenderLight rlight;
  rlight.name = light.name;
  rlight.abs_path = light_abs_path.full_path_name();
  rlight.type = RenderLight::Type::Sphere;

  // Extract common properties
  if (!ExtractCommonLightProperties(env, light, &rlight)) {
    return false;
  }

  // Extract shaping properties
  if (!ExtractShapingProperties(env, light, &rlight)) {
    return false;
  }

  // Extract radius
  if (light.radius.authored() && !light.radius.is_blocked()) {
    float val;
    if (light.radius.get_value().get(env.timecode, &val)) {
      rlight.radius = val;
    }
  }

  (*rlight_out) = std::move(rlight);
  return true;
}

bool RenderSceneConverter::ConvertDistantLight(
    const RenderSceneConverterEnv &env,
    const Path &light_abs_path,
    const DistantLight &light,
    RenderLight *rlight_out) {

  if (!rlight_out) {
    PUSH_ERROR_AND_RETURN("rlight_out arg is nullptr.");
  }

  RenderLight rlight;
  rlight.name = light.name;
  rlight.abs_path = light_abs_path.full_path_name();
  rlight.type = RenderLight::Type::Distant;

  // Extract common properties
  if (!ExtractCommonLightProperties(env, light, &rlight)) {
    return false;
  }

  if (!ExtractShapingProperties(env, light, &rlight)) {
    return false;
  }

  // Extract angle (angular diameter in degrees)
  if (light.angle.authored() && !light.angle.is_blocked()) {
    float val;
    if (light.angle.get_value().get(env.timecode, &val)) {
      rlight.angle = val;
    }
  }

  (*rlight_out) = std::move(rlight);
  return true;
}

bool RenderSceneConverter::ConvertDomeLight(
    const RenderSceneConverterEnv &env,
    const Path &light_abs_path,
    const DomeLight &light,
    RenderLight *rlight_out) {

  if (!rlight_out) {
    PUSH_ERROR_AND_RETURN("rlight_out arg is nullptr.");
  }

  RenderLight rlight;
  rlight.name = light.name;
  rlight.abs_path = light_abs_path.full_path_name();
  rlight.type = RenderLight::Type::Dome;

  // Extract common properties
  if (!ExtractCommonLightProperties(env, light, &rlight)) {
    return false;
  }

  if (!ExtractShapingProperties(env, light, &rlight)) {
    return false;
  }

  // Extract texture file and load envmap image
  if (light.file.authored() && !light.file.is_blocked()) {
    value::AssetPath assetPath;
    std::string eval_err;
    if (EvaluateTypedAnimatableAttribute(
            env.stage, light.file, "inputs:texture:file", &assetPath,
            &eval_err, env.timecode, env.tinterp)) {
      rlight.textureFile = assetPath.GetAssetPath();

      // Load the envmap texture if scene config allows
      if (env.scene_config.load_texture_assets && !assetPath.GetAssetPath().empty()) {
        TextureImage texImage;
        BufferData imageBuffer;
        imageBuffer.componentType = ComponentType::UInt8;

        std::string warn, err;

        TextureImageLoaderFunction tex_loader_fun =
            env.material_config.texture_image_loader_function;
        if (!tex_loader_fun) {
          tex_loader_fun = DefaultTextureImageLoaderFunction;
        }

        AssetInfo assetInfo;  // Empty asset info for now
        bool tex_loaded = tex_loader_fun(
            assetPath, assetInfo, env.asset_resolver, &texImage,
            &imageBuffer.data,
            env.material_config.texture_image_loader_function_userdata,
            &warn, &err);

        if (warn.size()) {
          PushWarn(warn);
        }

        if (!tex_loaded) {
          if (!env.material_config.allow_texture_load_failure) {
            PUSH_ERROR_AND_RETURN(fmt::format(
                "Failed to load envmap texture: `{}` err = {}",
                assetPath.GetAssetPath(), err));
          }

          const std::string load_err =
              err.empty() ? std::string("loader returned failure") : err;
          PushWarn(fmt::format(
              "Failed to decode envmap texture: `{}`. reason = {}. "
              "Falling back to raw asset storage.",
              assetPath.GetAssetPath(), load_err));
        }

        if (tex_loaded) {
          texImage.asset_identifier = assetPath.GetAssetPath();
          texImage.decoded = true;

          // HDR images (like EXR) should be treated as linear/Raw colorspace
          // Most envmaps are HDR and should not have sRGB gamma
          texImage.usdColorSpace = ColorSpace::Raw;
          texImage.colorSpace = ColorSpace::Lin_sRGB;

          // Add buffer
          texImage.buffer_id = int64_t(buffers.size());
          buffers.emplace_back(std::move(imageBuffer));

          // Add image and set envmap_texture_id
          rlight.envmap_texture_id = int32_t(images.size());
          images.emplace_back(texImage);

          DCOUT("Loaded envmap texture: " << assetPath.GetAssetPath()
                << " width=" << texImage.width
                << " height=" << texImage.height
                << " channels=" << texImage.channels);
        } else {
          // Fallback: store raw asset when decoding fails (e.g., EXR/HDR not supported)
          // Try to store the raw asset for later decoding (e.g., in JS layer)
          Asset asset;
          std::string resolvedPath;
          std::string readErr;
          AssetInfo fallbackAssetInfo;

          if (RawAssetRead(assetPath, fallbackAssetInfo, env.asset_resolver, &asset,
                           resolvedPath, nullptr, nullptr, &readErr)) {
            TextureImage fallbackTexImage;
            BufferData fallbackImageBuffer;
            fallbackImageBuffer.componentType = ComponentType::UInt8;

            fallbackTexImage.asset_identifier = resolvedPath;

            // Steal the asset's bytes (no copy); `asset` is not used afterward.
            SetBufferDataBytes(fallbackImageBuffer, asset.release_buffer());

            fallbackTexImage.buffer_id = int64_t(buffers.size());
            buffers.emplace_back(std::move(fallbackImageBuffer));

            fallbackTexImage.decoded = false;
            fallbackTexImage.usdColorSpace = ColorSpace::Raw;

            rlight.envmap_texture_id = int32_t(images.size());
            images.emplace_back(fallbackTexImage);

            DCOUT("Stored envmap asset (fallback): " << resolvedPath);
          } else {
            PushWarn(fmt::format("Failed to read envmap asset: `{}`. reason = {}",
                                 assetPath.GetAssetPath(), readErr));
          }
        }
      } else if (!env.scene_config.load_texture_assets) {
        // Store asset path only without decoding
        Asset asset;
        std::string resolvedPath;
        std::string err;
        AssetInfo assetInfo;

        if (RawAssetRead(assetPath, assetInfo, env.asset_resolver, &asset,
                         resolvedPath, nullptr, nullptr, &err)) {
          TextureImage texImage;
          BufferData imageBuffer;
          imageBuffer.componentType = ComponentType::UInt8;

          texImage.asset_identifier = resolvedPath;

          // Steal the asset's bytes (no copy); `asset` is not used afterward.
          SetBufferDataBytes(imageBuffer, asset.release_buffer());

          texImage.buffer_id = int64_t(buffers.size());
          buffers.emplace_back(std::move(imageBuffer));

          texImage.decoded = false;
          texImage.usdColorSpace = ColorSpace::Raw;

          rlight.envmap_texture_id = int32_t(images.size());
          images.emplace_back(texImage);

          DCOUT("Stored envmap asset: " << resolvedPath);
        } else {
          PushWarn(fmt::format("Failed to read envmap asset (load_texture_assets=false): `{}`. reason = {}",
                               assetPath.GetAssetPath(), err));
        }
      }
    } else if (!eval_err.empty()) {
      PUSH_WARN(fmt::format(
          "Failed to resolve DomeLight `inputs:texture:file`: {}", eval_err));
    }
  }

  // Extract texture format
  // Note: textureFormat is typically not time-sampled, use fallback/default
  if (light.textureFormat.authored() && !light.textureFormat.is_blocked()) {
    const auto& fmt_animatable = light.textureFormat.get_value();
    // Get default value directly from Animatable (not time-sampled)
    if (fmt_animatable.is_scalar()) {
      DomeLight::TextureFormat fmt;
      if (fmt_animatable.get_scalar(&fmt)) {
        switch (fmt) {
          case DomeLight::TextureFormat::Automatic:
            rlight.domeTextureFormat = RenderLight::DomeTextureFormat::Automatic;
            break;
          case DomeLight::TextureFormat::Latlong:
            rlight.domeTextureFormat = RenderLight::DomeTextureFormat::Latlong;
            break;
          case DomeLight::TextureFormat::MirroredBall:
            rlight.domeTextureFormat = RenderLight::DomeTextureFormat::MirroredBall;
            break;
          case DomeLight::TextureFormat::Angular:
            rlight.domeTextureFormat = RenderLight::DomeTextureFormat::Angular;
            break;
        }
      }
    }
  }

  // Extract guide radius
  if (light.guideRadius.authored() && !light.guideRadius.is_blocked()) {
    float val;
    if (light.guideRadius.get_value().get(env.timecode, &val)) {
      rlight.guideRadius = val;
    }
  }

  (*rlight_out) = std::move(rlight);
  return true;
}

bool RenderSceneConverter::ConvertRectLight(
    const RenderSceneConverterEnv &env,
    const Path &light_abs_path,
    const RectLight &light,
    RenderLight *rlight_out) {

  if (!rlight_out) {
    PUSH_ERROR_AND_RETURN("rlight_out arg is nullptr.");
  }

  RenderLight rlight;
  rlight.name = light.name;
  rlight.abs_path = light_abs_path.full_path_name();
  rlight.type = RenderLight::Type::Rect;

  // Extract common properties
  if (!ExtractCommonLightProperties(env, light, &rlight)) {
    return false;
  }

  // Extract shaping properties
  if (!ExtractShapingProperties(env, light, &rlight)) {
    return false;
  }

  // Extract width
  if (light.width.authored() && !light.width.is_blocked()) {
    float val;
    if (light.width.get_value().get(env.timecode, &val)) {
      rlight.width = val;
    }
  }

  // Extract height
  if (light.height.authored() && !light.height.is_blocked()) {
    float val;
    if (light.height.get_value().get(env.timecode, &val)) {
      rlight.height = val;
    }
  }

  // Extract texture file (optional)
  if (light.file.authored() && !light.file.is_blocked()) {
    value::AssetPath asset;
    std::string eval_err;
    if (EvaluateTypedAnimatableAttribute(
            env.stage, light.file, "inputs:texture:file", &asset, &eval_err,
            env.timecode, env.tinterp)) {
      rlight.textureFile = asset.GetAssetPath();
    } else if (!eval_err.empty()) {
      PUSH_WARN(fmt::format(
          "Failed to resolve RectLight `inputs:texture:file`: {}", eval_err));
    }
  }

  (*rlight_out) = std::move(rlight);
  return true;
}

bool RenderSceneConverter::ConvertDiskLight(
    const RenderSceneConverterEnv &env,
    const Path &light_abs_path,
    const DiskLight &light,
    RenderLight *rlight_out) {

  if (!rlight_out) {
    PUSH_ERROR_AND_RETURN("rlight_out arg is nullptr.");
  }

  RenderLight rlight;
  rlight.name = light.name;
  rlight.abs_path = light_abs_path.full_path_name();
  rlight.type = RenderLight::Type::Disk;

  // Extract common properties
  if (!ExtractCommonLightProperties(env, light, &rlight)) {
    return false;
  }

  if (!ExtractShapingProperties(env, light, &rlight)) {
    return false;
  }

  // Extract radius
  if (light.radius.authored() && !light.radius.is_blocked()) {
    float val;
    if (light.radius.get_value().get(env.timecode, &val)) {
      rlight.radius = val;
    }
  }

  (*rlight_out) = std::move(rlight);
  return true;
}

bool RenderSceneConverter::ConvertCylinderLight(
    const RenderSceneConverterEnv &env,
    const Path &light_abs_path,
    const CylinderLight &light,
    RenderLight *rlight_out) {

  if (!rlight_out) {
    PUSH_ERROR_AND_RETURN("rlight_out arg is nullptr.");
  }

  RenderLight rlight;
  rlight.name = light.name;
  rlight.abs_path = light_abs_path.full_path_name();
  rlight.type = RenderLight::Type::Cylinder;

  // Extract common properties
  if (!ExtractCommonLightProperties(env, light, &rlight)) {
    return false;
  }

  if (!ExtractShapingProperties(env, light, &rlight)) {
    return false;
  }

  // Extract length
  if (light.length.authored() && !light.length.is_blocked()) {
    float val;
    if (light.length.get_value().get(env.timecode, &val)) {
      rlight.length = val;
    }
  }

  // Extract radius
  if (light.radius.authored() && !light.radius.is_blocked()) {
    float val;
    if (light.radius.get_value().get(env.timecode, &val)) {
      rlight.radius = val;
    }
  }

  (*rlight_out) = std::move(rlight);
  return true;
}

bool RenderSceneConverter::ConvertGeometryLight(
    const RenderSceneConverterEnv &env,
    const Path &light_abs_path,
    const GeometryLight &light,
    RenderLight *rlight_out) {

  if (!rlight_out) {
    PUSH_ERROR_AND_RETURN("rlight_out arg is nullptr.");
  }

  RenderLight rlight;
  rlight.name = light.name;
  rlight.abs_path = light_abs_path.full_path_name();
  rlight.type = RenderLight::Type::Geometry;

  // Extract common properties
  if (!ExtractCommonLightProperties(env, light, &rlight)) {
    return false;
  }

  if (!ExtractShapingProperties(env, light, &rlight)) {
    return false;
  }

  // Extract geometry relationship to find the target mesh
  // GeometryLight uses a relationship to point to the mesh geometry
  if (!light.geometry.authored() || light.geometry.is_blocked()) {
    PUSH_ERROR_AND_RETURN("GeometryLight " << rlight.abs_path
                           << " missing geometry relationship");
  }

  const std::vector<Path> targets = light.geometry.get_targetPaths();
  if (targets.size() != 1) {
    PUSH_ERROR_AND_RETURN("GeometryLight " << rlight.abs_path
                           << " must have exactly one geometry target");
  }

  const Path &target_path = targets[0];
  const std::string geometry_path = target_path.full_path_name();

  const Prim *geometry_prim{nullptr};
  std::string err;
  if (!env.stage.find_prim_at_path(Path(target_path.prim_part(), ""),
                                   geometry_prim, &err)) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "GeometryLight {} references missing geometry target {}: {}",
        rlight.abs_path, geometry_path, err));
  }

  if (!geometry_prim) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "GeometryLight {} references invalid geometry target {}",
        rlight.abs_path, geometry_path));
  }

  // The actual mesh_id will be resolved during scene building.
  rlight.geometry_mesh_id = -1;
  rlight.geometry_target_path = geometry_path;

  DCOUT("GeometryLight " << rlight.abs_path
        << " references geometry: " << geometry_path);

  // Default material sync mode for GeometryLight
  rlight.material_sync_mode = "materialGlowTintsLight";

  (*rlight_out) = std::move(rlight);
  return true;
}

// Helper: populate flat topology/transform arrays on SkelHierarchy from a Skeleton prim.
static bool PopulateSkelFlatArrays(const Skeleton &skel, SkelHierarchy &dst, std::string *err) {
  std::vector<value::token> joints;
  if (!skel.joints.get_value(&joints) || joints.empty()) {
    return true;  // No joints authored; leave flat arrays empty
  }

  // Build topology
  if (!BuildSkelTopology(joints, dst.parent_joint_indices, err)) {
    return false;
  }

  // Bind transforms
  if (skel.bindTransforms.authored()) {
    if (!skel.bindTransforms.get_value(&dst.bind_transforms) ||
        dst.bind_transforms.empty()) {
      dst.bind_transforms.assign(joints.size(), value::matrix4d::identity());
    }
  } else {
    dst.bind_transforms.assign(joints.size(), value::matrix4d::identity());
  }
  if (dst.bind_transforms.size() != joints.size()) {
    dst.bind_transforms.assign(joints.size(), value::matrix4d::identity());
  }

  // Rest transforms
  bool restAuthored = skel.restTransforms.authored();
  if (skel.restTransforms.authored()) {
    if (!skel.restTransforms.get_value(&dst.rest_transforms) ||
        dst.rest_transforms.empty()) {
      restAuthored = false;
    }
  }
  if (!restAuthored && dst.bind_transforms.size() == joints.size()) {
    dst.rest_transforms.resize(joints.size());
    for (size_t i = 0; i < joints.size(); i++) {
      int parentIdx = dst.parent_joint_indices[i];
      if (parentIdx < 0) {
        dst.rest_transforms[i] = dst.bind_transforms[i];
      } else {
        value::matrix4d parentInverse;
        if (inverse(dst.bind_transforms[size_t(parentIdx)], parentInverse)) {
          dst.rest_transforms[i] = dst.bind_transforms[i] * parentInverse;
        } else {
          dst.rest_transforms[i] = value::matrix4d::identity();
        }
      }
    }
  } else if (dst.rest_transforms.size() != joints.size()) {
    dst.rest_transforms.assign(joints.size(), value::matrix4d::identity());
  }

  return true;
}

bool RenderSceneConverter::ConvertSkeletonFromPtr(const RenderSceneConverterEnv &env,
                       const Path &skelPath,
                       const Skeleton &skel,
                       const std::string &primName,
                       SkelHierarchy *out_skel) {
  (void)env;

  if (!out_skel) {
    return false;
  }

  SkelHierarchy dst;
  SkelNode root;
  if (!BuildSkelHierarchy(skel, root, &_err)) {
    return false;
  }
  dst.abs_path = skelPath.prim_part();
  dst.prim_name = primName;
  dst.display_name = skel.metas().has_displayName() ? skel.metas().get_displayName() : "";
  dst.root_node = root;

  PopulateSkelFlatArrays(skel, dst, &_err);

  (*out_skel) = std::move(dst);
  return true;
}

bool RenderSceneConverter::ConvertSkeletonImplWithPath(const RenderSceneConverterEnv &env, const Path &skelPath,
                       SkelHierarchy *out_skel) {

  if (!out_skel) {
    return false;
  }

  if (skelPath.is_valid()) {
    const Prim *skelPrim{nullptr};
    if (!env.stage.find_prim_at_path(skelPath, skelPrim, &_err)) {
      return false;
    }

    SkelHierarchy dst;
    if (const auto pskel = skelPrim->as<Skeleton>()) {
      SkelNode root;
      if (!BuildSkelHierarchy((*pskel), root, &_err)) {
        return false;
      }
      dst.abs_path = skelPath.prim_part();
      dst.prim_name = skelPrim->element_name();
      dst.display_name = pskel->metas().has_displayName() ? pskel->metas().get_displayName() : "";
      dst.root_node = root;

      PopulateSkelFlatArrays(*pskel, dst, &_err);
    } else {
      PUSH_ERROR_AND_RETURN("Prim is not Skeleton.");
    }

    (*out_skel) = std::move(dst);
    return true;
  }

  PUSH_ERROR_AND_RETURN("`skel:skeleton` path is invalid.");
}

bool RenderSceneConverter::ConvertAllSkelAnimations(const RenderSceneConverterEnv &env) {
  // This method processes all SkelAnimation prims discovered during pre-processing.
  // For each SkelAnimation, we find which Skeleton it belongs to via:
  //   1. Skeleton's skel:animationSource relationship
  //   2. SkelRoot's skel:animationSource relationship (inherited per USD spec)
  //   3. Parent path hierarchy (SkelAnimation as child of Skeleton)

  if (!_allAnimations || _allAnimations->empty()) {
    return true; // No animations to process
  }

  DCOUT("ConvertAllSkelAnimations: processing " << _allAnimations->size() << " SkelAnimation prims");

  // Build reverse map: animationPath -> list of skeleton_ids that reference it
  std::unordered_map<std::string, std::vector<int32_t>, FNV1StringHash>
      animPathToSkelIds;
  animPathToSkelIds.reserve(_allAnimations->size());

  // Helper: extract animation paths from a Relationship
  auto extractAnimPaths = [](const Relationship &rel, std::vector<Path> &out) {
    if (rel.is_path()) {
      out.push_back(rel.targetPath);
    } else if (rel.is_pathvector()) {
      out.insert(out.end(), rel.targetPathVector.begin(), rel.targetPathVector.end());
    }
  };

  // 1. Check Skeleton prims for skel:animationSource
  for (const auto &skelEntry : _skelPathToIndex) {
    const std::string &skelPathStr = skelEntry.first;
    const int32_t skel_id = skelEntry.second;

    Path skelPath(skelPathStr, "");
    const Prim *skelPrim{nullptr};
    if (!env.stage.find_prim_at_path(skelPath, skelPrim, &_err)) {
      continue;
    }

    const auto *pskel = skelPrim->as<Skeleton>();
    if (!pskel) continue;

    std::vector<Path> animPaths;

    if (pskel->animationSource.has_value()) {
      extractAnimPaths(pskel->animationSource.value(), animPaths);
    }

    // 2. If Skeleton has no animationSource, check ancestor SkelRoot prims
    //    (implements USD SkelBindingAPI inheritance)
    if (animPaths.empty() && _allSkelRoots) {
      // Walk up the path hierarchy to find a SkelRoot with animationSource
      size_t iter = 0;
      std::string parentPath = skelPathStr;
      while (!parentPath.empty()) {
        if (iter++ >= kMaxDefaultTraversalLimit) break;
        size_t lastSlash = parentPath.rfind('/');
        if (lastSlash == 0 || lastSlash == std::string::npos) {
          parentPath = "/";  // root
        } else {
          parentPath = parentPath.substr(0, lastSlash);
        }

        auto rootIt = _allSkelRoots->find(parentPath);
        if (rootIt != _allSkelRoots->end() && rootIt->second) {
          const SkelRoot *pskelRoot = rootIt->second;
          if (pskelRoot->animationSource.has_value()) {
            extractAnimPaths(pskelRoot->animationSource.value(), animPaths);
            DCOUT("Inherited animationSource from SkelRoot " << parentPath
                  << " for Skeleton " << skelPathStr);
            break;
          }
        }
        if (parentPath == "/") break;
      }
    }

    for (const Path &animPath : animPaths) {
      std::string ap = animPath.prim_part();
      animPathToSkelIds[ap].push_back(skel_id);
    }
  }

  DCOUT("Built reverse map: " << animPathToSkelIds.size() << " animations referenced by skeletons");

  // 3. For SkelAnimation prims not referenced by any animationSource,
  //    associate them with a parent Skeleton by path hierarchy.
  //    This enables multi-clip workflows where SkelAnimation prims are children
  //    of a Skeleton but not all are the active animationSource.
  for (const auto &animEntry : *_allAnimations) {
    const std::string &animPathStr = animEntry.first;

    // Skip if already referenced
    if (animPathToSkelIds.find(animPathStr) != animPathToSkelIds.end()) {
      continue;
    }

    // Walk up parent path to find a Skeleton
    size_t iter = 0;
    std::string parentPath = animPathStr;
    while (!parentPath.empty()) {
      if (iter++ >= kMaxDefaultTraversalLimit) break;
      size_t lastSlash = parentPath.rfind('/');
      if (lastSlash == 0 || lastSlash == std::string::npos) {
        parentPath.clear();
        break;
      }
      parentPath = parentPath.substr(0, lastSlash);

      auto skelIt = _skelPathToIndex.find(parentPath);
      if (skelIt != _skelPathToIndex.end()) {
        animPathToSkelIds[animPathStr].push_back(skelIt->second);
        DCOUT("Associated SkelAnimation " << animPathStr
              << " with parent Skeleton " << parentPath
              << " (skeleton_id=" << skelIt->second << ")");
        break;
      }
    }

    if (animPathToSkelIds.find(animPathStr) == animPathToSkelIds.end()) {
      DCOUT("SkelAnimation " << animPathStr << " has no associated skeleton (skipping)");
    }
  }

  // Now convert each SkelAnimation prim
  for (const auto &animEntry : *_allAnimations) {
    const std::string &animPathStr = animEntry.first;
    const SkelAnimation *panimPtr = animEntry.second;

    if (!panimPtr) {
      PUSH_WARN("Null SkelAnimation pointer for path: " + animPathStr);
      continue;
    }

    auto it = animPathToSkelIds.find(animPathStr);
    if (it == animPathToSkelIds.end() || it->second.empty()) {
      DCOUT("SkelAnimation " << animPathStr << " not associated with any skeleton (skipping)");
      continue;
    }

    // Convert the animation for each skeleton that references it
    for (int32_t skeleton_id : it->second) {
      std::string cacheKey = animPathStr + ":" + std::to_string(skeleton_id);
      if (_animPathToIndex.find(cacheKey) != _animPathToIndex.end()) {
        DCOUT("Animation " << animPathStr << " already converted for skeleton " << skeleton_id);
        continue;
      }

      Path animPath(animPathStr, "");
      AnimationClip anim;

      if (!ConvertSkelAnimation(env, animPath, *panimPtr, skeleton_id, &anim)) {
        PushWarn(fmt::format(
            "Skipping invalid SkelAnimation {} for skeleton {}: {}\n",
            animPathStr, skeleton_id, GetError()));
        _err.clear();
        continue;
      }

      DCOUT("Converted SkelAnimation " << animPathStr << " for skeleton " << skeleton_id);

      // Add to animations vector
      int32_t anim_id = int32_t(animations.size());
      _animPathToIndex[cacheKey] = anim_id;
      animations.emplace_back(std::move(anim));

      // Update skeleton's animation IDs.
      if (skeleton_id >= 0 && skeleton_id < int32_t(skeletons.size())) {
        auto &skel = skeletons[static_cast<size_t>(skeleton_id)];
        // animPath+skeleton_id is deduplicated via _animPathToIndex cache.
        skel.anim_ids.push_back(anim_id);

        // Keep legacy default animation field for backward compatibility.
        if (skeletons[static_cast<size_t>(skeleton_id)].anim_id < 0) {
          skeletons[static_cast<size_t>(skeleton_id)].anim_id = anim_id;
          DCOUT("Set skeleton " << skeleton_id << " anim_id to " << anim_id);
        }
      }
    }
  }

  DCOUT("ConvertAllSkelAnimations: converted " << animations.size() << " animation clips");
  return true;
}

size_t ResolveLightLinking(const Stage &stage, RenderScene *scene) {
  if (!scene) {
    return 0;
  }

  size_t resolved = 0;

  for (size_t li = 0; li < scene->lights.size(); li++) {
    RenderLight &light = scene->lights[li];

    Path lpath(light.abs_path, "");
    if (!lpath.is_valid()) {
      continue;
    }
    auto pret = stage.GetPrimAtPath(lpath);
    if (!pret || !pret.value()) {
      continue;
    }
    const Prim &light_prim = *pret.value();
    const Collection *coll = nullptr;
    GetCollection(light_prim, &coll);

    // Resolve one link collection instance ("lightLink" / "shadowLink").
    auto resolve_link = [&](const std::string &inst_name, bool *links_all,
                            std::vector<int> *mesh_indices) -> bool {
      CollectionMembershipQuery q;
      const CollectionInstance *inst = nullptr;
      if (coll && coll->get_instance(inst_name, &inst) && inst) {
        const bool authored = inst->has_membershipExpression() ||
                              inst->includes.authored() ||
                              inst->excludes.authored();
        if (!authored) return false;
        q = BuildCollectionMembershipQuery(stage, *inst, light.abs_path);
      } else {
        // Older/legacy prim reconstruction may retain multi-apply CollectionAPI
        // properties as generic properties rather than a typed Collection.
        // Resolve relationship mode directly so light linking does not depend
        // on whether `apiSchemas = ["CollectionAPI:<name>"]` was authored.
        const std::string base = "collection:" + inst_name + ":";
        Relationship includes_rel;
        Relationship excludes_rel;
        std::string includes_err;
        std::string excludes_err;
        const bool has_includes = GetRelationship(
            light_prim, base + "includes", &includes_rel, &includes_err);
        const bool has_excludes = GetRelationship(
            light_prim, base + "excludes", &excludes_rel, &excludes_err);
        if (!has_includes && !has_excludes) return false;
        q.mode = CollectionMembershipQuery::Mode::Relationship;
        q.owner_prim_path = light.abs_path;
        if (has_includes) {
          if (includes_rel.is_path()) {
            q.includes.push_back(includes_rel.targetPath);
          }
          if (includes_rel.is_pathvector()) {
            q.includes = includes_rel.targetPathVector;
          }
        }
        if (has_excludes) {
          if (excludes_rel.is_path()) {
            q.excludes.push_back(excludes_rel.targetPath);
          }
          if (excludes_rel.is_pathvector()) {
            q.excludes = excludes_rel.targetPathVector;
          }
        }
        Attribute attr;
        std::string attr_err;
        if (GetAttribute(light_prim, base + "includeRoot", &attr, &attr_err)) {
          if (auto value = attr.get_value<bool>()) q.include_root = *value;
        }
        attr_err.clear();
        if (GetAttribute(light_prim, base + "expansionRule", &attr,
                         &attr_err)) {
          if (auto value = attr.get_value<std::string>()) {
            if (*value == "explicitOnly") {
              q.expansion_rule =
                  CollectionInstance::ExpansionRule::ExplicitOnly;
            } else if (*value == "expandPrimsAndProperties") {
              q.expansion_rule =
                  CollectionInstance::ExpansionRule::ExpandPrimsAndProperties;
            }
          }
        }
      }

      *links_all = false;
      mesh_indices->clear();
      for (size_t mi = 0; mi < scene->meshes.size(); mi++) {
        const std::string &mpath = scene->meshes[mi].abs_path;
        if (mpath.empty()) {
          continue;
        }
        if (IsPathIncluded(q, stage, Path(mpath, ""))) {
          mesh_indices->push_back(static_cast<int>(mi));
        }
      }
      return true;
    };

    bool any = false;
    if (resolve_link("lightLink", &light.light_links_all,
                     &light.light_link_mesh_indices)) {
      any = true;
    }
    if (resolve_link("shadowLink", &light.shadow_links_all,
                     &light.shadow_link_mesh_indices)) {
      any = true;
    }
    if (any) {
      resolved++;
    }
  }

  return resolved;
}

}  // namespace tydra
}  // namespace lightusd
