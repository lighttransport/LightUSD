// SPDX-License-Identifier: Apache-2.0
// Value-clip animation conversion split from render-data-anim.cc.
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common-macros.inc"
#include "common-types.hh"
#include "common-utils.hh"
#include "core/prim.hh"
#include "lightusd.hh"
#include "tiny-format.hh"
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#include "value-clip-utils.hh"
#include "value-pprint.hh"

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

}  // namespace

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


}  // namespace tydra
}  // namespace lightusd
