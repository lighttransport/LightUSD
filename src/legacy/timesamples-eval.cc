// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// TimeSamples evaluation/interpolation path — split out of timesamples.cc.
// Holds the per-type binary-direct scalar evaluator get_scalar_impl<T> (one
// instantiation per scalar value type, dispatched by the get_scalar() switch)
// plus the generic value::Value evaluator get_value_at(). These force ~60
// template instantiations + a big switch; isolating them from the binary
// storage/reconstruction path (update/reconstruct_binary_sample, which stays in
// timesamples.cc) shortens the build critical path. All methods are declared in
// timesamples.hh; the storage-side members (get_samples/update) are called
// cross-TU.
#include "value-types.hh"
// value-types.hh must be included before timesamples.hh for full type defs.
#include "timesamples.hh"
#include "value-eval-util.hh"  // lerp() for the binary fast path
#include "core/extent.hh"      // value::TypeTraits<Extent> (Extent scalar arm)
#include <algorithm>
#include <cstring>

namespace lightusd {
namespace value {

#define DEFINE_LERP_TRAIT(ty)       \
  template <>                       \
  struct LerpTraits<ty> {           \
    static constexpr bool supported() { return true; } \
  };                                \
  template <>                       \
  struct LerpTraits<std::vector<ty>> { \
    static constexpr bool supported() { return true; } \
  };
#include "value-type-macros.inc"
APPLY_FUNC_TO_LERP_VALUE_TYPES(DEFINE_LERP_TRAIT)
#undef DEFINE_LERP_TRAIT

// ============================================================================
// [Phase 1] Binary-direct scalar evaluator (declared in timesamples.hh).
// Mirrors get<T>() semantics exactly (default-time -> first non-blocked sample;
// single-sample; Held via upper_bound; Linear via lower_bound idx0/idx1 with
// blocked-endpoint fallback) but reads POD samples straight from the flat _data
// buffer with zero value::Value reconstruction. Generic storage (string/token/
// dict/AssetPath, or a POD stored as Value) falls back to the get_samples() path.
// ============================================================================
template <typename T>
bool TimeSamples::get_scalar_impl(void *dst_, double t,
                                  value::TimeSampleInterpolationType interp) const {
  T *dst = static_cast<T *>(dst_);

  if (_dirty) {
    update();
  }

  // ---- Fast binary path: read T directly from _data (no Value reconstruct) --
  if constexpr (value::uses_binary_timesample_scalar_storage_v<T>) {
    if (!_times.empty()) {
      const size_t n = _times.size();

      auto blocked_at = [&](size_t i) -> bool {
        if (i < _blocked.size() && _blocked[i] != 0) return true;
        if (i < _data_offsets.size() && _data_offsets[i] == BLOCKED_OFFSET) return true;
        return false;
      };
      auto read_at = [&](size_t i, T *out) -> bool {
        if (i >= _data_offsets.size()) return false;
        const size_t off = _data_offsets[i];
        if (off == BLOCKED_OFFSET) return false;
        if (static_cast<size_t>(off) + sizeof(T) > _data.size()) return false;
        std::memcpy(out, _data.data() + off, sizeof(T));
        return true;
      };

      if (value::TimeCode(t).is_default()) {
        for (size_t i = 0; i < n; ++i) {
          if (!blocked_at(i)) return read_at(i, dst);
        }
        return false;
      }

      if (n == 1) {
        if (blocked_at(0)) return false;
        return read_at(0, dst);
      }

      if constexpr (value::LerpTraits<T>::supported()) {
        if (interp == value::TimeSampleInterpolationType::Linear) {
          auto it = std::lower_bound(_times.begin(), _times.end(), t);
          const auto it_m1 = (it == _times.begin()) ? _times.begin() : (it - 1);
          const size_t idx0 = static_cast<size_t>(std::max<int64_t>(
              0, std::min<int64_t>(int64_t(n) - 1,
                                   int64_t(std::distance(_times.begin(), it_m1)))));
          const size_t idx1 = static_cast<size_t>(std::max<int64_t>(
              0, std::min<int64_t>(int64_t(n) - 1, int64_t(idx0) + 1)));

          if (blocked_at(idx0) && blocked_at(idx1)) return false;
          if (blocked_at(idx0)) return read_at(idx1, dst);
          if (blocked_at(idx1)) return read_at(idx0, dst);

          const double tl = _times[idx0];
          const double tu = _times[idx1];
          double dt = (t - tl);
          if (std::fabs(tu - tl) < std::numeric_limits<double>::epsilon()) {
            dt = 0.0;
          } else {
            dt /= (tu - tl);
          }
          dt = std::max(0.0, std::min(1.0, dt));

          T a{}, b{};
          if (!read_at(idx0, &a) || !read_at(idx1, &b)) return false;
          *dst = lerp(a, b, dt);
          return true;
        }
      }

      // Held (and all non-lerp'able types)
      auto it = std::upper_bound(_times.begin(), _times.end(), t);
      const auto it_held = (it == _times.begin()) ? _times.begin() : (it - 1);
      const size_t idx = static_cast<size_t>(std::distance(_times.begin(), it_held));
      if (blocked_at(idx)) return false;
      return read_at(idx, dst);
    }
  }

  // ---- Generic value::Value fallback. Delegate to the once-compiled non-template
  // get_value_at() — its interpolation semantics (default-time -> first non-blocked;
  // single-sample; Linear via lower_bound idx0/idx1 with blocked-endpoint fallback;
  // Held via upper_bound) mirror this path exactly — then extract once via as<T>()
  // (role-compat aware). Keeps each of the ~60 get_scalar_impl<T> instantiations tiny
  // instead of re-emitting the full interpolation logic per type.
  value::Value v;
  if (!get_value_at(&v, t, interp)) return false;
  if (const T *pv = v.template as<T>()) { *dst = *pv; return true; }
  return false;
}

// Non-template dispatch: select the per-type impl by stored type_id. One arm per
// binary scalar type (mirrors reconstruct_binary_sample) + common generic types.
// Role types (color3f, normal3f, ...) keep their own arm; the impl reads the
// stored layout, and eval_scalar/get<T> guarantee dst is layout-compatible.
bool TimeSamples::get_scalar(void *dst, double t,
                             value::TimeSampleInterpolationType interp) const {
#define TS_GET_SCALAR_CASE(TYPE)             \
  case value::TypeTraits<TYPE>::type_id():   \
    return get_scalar_impl<TYPE>(dst, t, interp);

  switch (_type_id) {
    TS_GET_SCALAR_CASE(bool)
    TS_GET_SCALAR_CASE(value::half)
    TS_GET_SCALAR_CASE(float)
    TS_GET_SCALAR_CASE(double)
    TS_GET_SCALAR_CASE(int32_t)
    TS_GET_SCALAR_CASE(uint32_t)
    TS_GET_SCALAR_CASE(int64_t)
    TS_GET_SCALAR_CASE(uint64_t)
    TS_GET_SCALAR_CASE(value::float2)
    TS_GET_SCALAR_CASE(value::float3)
    TS_GET_SCALAR_CASE(value::float4)
    TS_GET_SCALAR_CASE(value::double2)
    TS_GET_SCALAR_CASE(value::double3)
    TS_GET_SCALAR_CASE(value::double4)
    TS_GET_SCALAR_CASE(value::int2)
    TS_GET_SCALAR_CASE(value::int3)
    TS_GET_SCALAR_CASE(value::int4)
    TS_GET_SCALAR_CASE(value::half2)
    TS_GET_SCALAR_CASE(value::half3)
    TS_GET_SCALAR_CASE(value::half4)
    TS_GET_SCALAR_CASE(value::quath)
    TS_GET_SCALAR_CASE(value::quatf)
    TS_GET_SCALAR_CASE(value::quatd)
    TS_GET_SCALAR_CASE(value::point3f)
    TS_GET_SCALAR_CASE(value::point3d)
    TS_GET_SCALAR_CASE(value::normal3f)
    TS_GET_SCALAR_CASE(value::normal3d)
    TS_GET_SCALAR_CASE(value::vector3f)
    TS_GET_SCALAR_CASE(value::vector3d)
    TS_GET_SCALAR_CASE(value::color3f)
    TS_GET_SCALAR_CASE(value::color3d)
    TS_GET_SCALAR_CASE(value::color4f)
    TS_GET_SCALAR_CASE(value::color4d)
    TS_GET_SCALAR_CASE(value::texcoord2f)
    TS_GET_SCALAR_CASE(value::texcoord2d)
    TS_GET_SCALAR_CASE(value::texcoord3f)
    TS_GET_SCALAR_CASE(value::texcoord3d)
    TS_GET_SCALAR_CASE(value::matrix2f)
    TS_GET_SCALAR_CASE(value::matrix2d)
    TS_GET_SCALAR_CASE(value::matrix3f)
    TS_GET_SCALAR_CASE(value::matrix3d)
    TS_GET_SCALAR_CASE(value::matrix4f)
    TS_GET_SCALAR_CASE(value::matrix4d)
    // Generic (non-binary) scalar value types.
    TS_GET_SCALAR_CASE(value::token)
    TS_GET_SCALAR_CASE(std::string)
    TS_GET_SCALAR_CASE(value::AssetPath)
    // half role types + timecode + Extent (float3[2])
    TS_GET_SCALAR_CASE(value::vector3h)
    TS_GET_SCALAR_CASE(value::normal3h)
    TS_GET_SCALAR_CASE(value::point3h)
    TS_GET_SCALAR_CASE(value::color3h)
    TS_GET_SCALAR_CASE(value::color4h)
    TS_GET_SCALAR_CASE(value::texcoord2h)
    TS_GET_SCALAR_CASE(value::texcoord3h)
    TS_GET_SCALAR_CASE(value::timecode)
    TS_GET_SCALAR_CASE(Extent)
    default:
      return false;
  }
#undef TS_GET_SCALAR_CASE
}

// Generic value-path evaluator: produce the interpolated value::Value at time t.
// Non-template (dispatches on _type_id via Lerp/IsLerpSupportedType), so the heavy
// interpolation code is emitted once here rather than per element type in every
// includer. Used by the public get<T>() for array (and any non-binary) types;
// scalars take the binary-direct get_scalar() path. Mirrors the get_samples()+Lerp
// fallback of get_scalar_impl<T>() exactly: default-time -> first non-blocked
// sample; single sample; Linear via lower_bound idx0/idx1 with blocked-endpoint
// fallback; otherwise Held via upper_bound.
bool TimeSamples::get_value_at(value::Value *out, double t,
                               value::TimeSampleInterpolationType interp) const {
  if (!out) return false;
  if (_dirty) {
    update();
  }

  const std::vector<Sample> &samples = get_samples();
  if (samples.empty()) return false;

  if (value::TimeCode(t).is_default()) {
    for (const auto &s : samples) {
      if (!s.blocked) {
        *out = s.value;
        return true;
      }
    }
    return false;
  }

  if (samples.size() == 1) {
    if (samples[0].blocked) return false;
    *out = samples[0].value;
    return true;
  }

  if (interp == value::TimeSampleInterpolationType::Linear &&
      value::IsLerpSupportedType(_type_id)) {
    auto it = std::lower_bound(samples.begin(), samples.end(), t,
        [](const Sample &a, double tv) { return a.t < tv; });
    const auto it_m1 = (it == samples.begin()) ? samples.begin() : (it - 1);
    const size_t idx0 = static_cast<size_t>(std::max<int64_t>(0,
        std::min<int64_t>(int64_t(samples.size()) - 1,
                          int64_t(std::distance(samples.begin(), it_m1)))));
    const size_t idx1 = static_cast<size_t>(std::max<int64_t>(0,
        std::min<int64_t>(int64_t(samples.size()) - 1, int64_t(idx0) + 1)));
    if (samples[idx0].blocked && samples[idx1].blocked) return false;
    if (samples[idx0].blocked) { *out = samples[idx1].value; return true; }
    if (samples[idx1].blocked) { *out = samples[idx0].value; return true; }
    const double tl = samples[idx0].t;
    const double tu = samples[idx1].t;
    double dt = (t - tl);
    if (std::fabs(tu - tl) < std::numeric_limits<double>::epsilon()) {
      dt = 0.0;
    } else {
      dt /= (tu - tl);
    }
    dt = std::max(0.0, std::min(1.0, dt));
    return Lerp(samples[idx0].value, samples[idx1].value, dt, out);
  }

  // Held
  auto it = std::upper_bound(samples.begin(), samples.end(), t,
      [](double tv, const Sample &a) { return tv < a.t; });
  const auto it_held = (it == samples.begin()) ? samples.begin() : (it - 1);
  if (it_held->blocked) return false;
  *out = it_held->value;
  return true;
}

} // namespace value
} // namespace lightusd
