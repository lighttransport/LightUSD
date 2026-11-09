// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// TimeSamples implementation

#include "value-types.hh"
// value-types.hh must be included before timesamples.hh
// to have full definitions of types
#include "timesamples.hh"
#include "core/extent.hh"  // value::TypeTraits<Extent> (extent timesamples)
#include <algorithm>
#include <cstring>
#include <mutex>

namespace lightusd {

namespace {

// Insertion sort for Sample array (nearly-sorted optimization)
inline void insertion_sort_samples(std::vector<value::TimeSamples::Sample>& samples) {
  const size_t n = samples.size();
  if (n < 2) return;

  for (size_t i = 1; i < n; ++i) {
    if (samples[i].t >= samples[i - 1].t) {
      continue;
    }

    value::TimeSamples::Sample key = std::move(samples[i]);
    size_t j = i;

    while (j > 0 && samples[j - 1].t > key.t) {
      samples[j] = std::move(samples[j - 1]);
      --j;
    }

    samples[j] = std::move(key);
  }
}

// Sort flat binary storage by permuting parallel arrays.
// Returns true on success, false if the parallel-array invariant
// (times.size() == blocked.size() == data_offsets.size() ==
//  array_counts->size() if array_counts) is violated. On false, the
// caller's data is left unchanged. See concern 4 in review.md.
inline bool sort_flat_storage(
    std::vector<double>& times,
    Buffer<16>& blocked,
    std::vector<size_t>& data_offsets,
    std::vector<uint32_t>* array_counts) {

  const size_t n = times.size();
  if (times.size() != blocked.size() ||
      times.size() != data_offsets.size() ||
      (array_counts && times.size() != array_counts->size())) {
    return false;
  }
  if (n < 2) return true;

  // Create index array
  std::vector<size_t> indices(n);
  for (size_t i = 0; i < n; ++i) indices[i] = i;
  std::sort(indices.begin(), indices.end(),
            [&times](size_t a, size_t b) { return times[a] < times[b]; });

  std::vector<double> sorted_times(n);
  Buffer<16> sorted_blocked;
  sorted_blocked.resize(n);
  std::vector<size_t> sorted_offsets(data_offsets.size());
  std::vector<uint32_t> sorted_counts;
  if (array_counts && array_counts->size() == n) {
    sorted_counts.resize(n);
  }

  for (size_t i = 0; i < n; ++i) {
    sorted_times[i] = times[indices[i]];
    sorted_blocked[i] = blocked[indices[i]];
    if (indices[i] < data_offsets.size()) {
      sorted_offsets[i] = data_offsets[indices[i]];
    }
    if (!sorted_counts.empty()) {
      sorted_counts[i] = (*array_counts)[indices[i]];
    }
  }

  times = std::move(sorted_times);
  blocked = std::move(sorted_blocked);
  data_offsets = std::move(sorted_offsets);
  if (!sorted_counts.empty()) {
    (*array_counts) = std::move(sorted_counts);
  }
  return true;
}

} // anonymous namespace

namespace value {

// TimeSamples::update() implementation
void TimeSamples::update() const {
  if (!_times.empty()) {
    // Flat binary storage
    if (_times.size() < 2 || std::is_sorted(_times.begin(), _times.end())) {
      _dirty = false;
      return;
    }

    if (!sort_flat_storage(_times, _blocked, _data_offsets,
                           _is_array ? &_array_counts : nullptr)) {
      // Parallel-array invariant violated; mark the TimeSamples as
      // broken so callers can detect via has_error(). See concern 4 in
      // review.md.
      _has_error = true;
      return;
    }
  } else if (!_samples.empty()) {
    if (_samples.size() < 2) {
      _dirty = false;
      return;
    }

    if (std::is_sorted(_samples.begin(), _samples.end(),
              [](const Sample &a, const Sample &b) { return a.t < b.t; })) {
      _dirty = false;
      return;
    }

    size_t inversions = 0;
    size_t scan_limit = std::min(_samples.size() - 1, size_t(100));
    for (size_t i = 0; i < scan_limit; ++i) {
      if (_samples[i].t > _samples[i + 1].t) {
        ++inversions;
      }
    }

    if (inversions * 20 < _samples.size()) {
      insertion_sort_samples(_samples);
    } else {
      std::sort(_samples.begin(), _samples.end(),
                [](const Sample &a, const Sample &b) { return a.t < b.t; });
    }
  }
  _dirty = false;
}

// ============================================================================
// reconstruct_binary_sample — reconstructs value::Value from flat _data buffer
// ============================================================================

bool TimeSamples::reconstruct_binary_sample(size_t idx, Sample* sample) const {
    if (!sample || idx >= _times.size() || idx >= _blocked.size()) {
      return false;
    }

    sample->t = _times[idx];
    sample->blocked = (_blocked[idx] != 0);
    sample->value = value::Value();

    if (sample->blocked) {
      return true;
    }

    if (idx >= _data_offsets.size()) {
      return true;
    }

    const size_t byte_offset = _data_offsets[idx];
    if (byte_offset == BLOCKED_OFFSET) {
      sample->blocked = true;
      return true;
    }

    // Array reconstruction
    if (_is_array) {
      const size_t count = get_array_count(idx);

#define RECONSTRUCT_ARRAY(TYPE)                                                   \
      if (reconstruct_unified_array_value<TYPE>(                                  \
              idx, value::TypeTraits<std::vector<TYPE>>::type_id(),               \
              &sample->value)) {                                                  \
        return true;                                                              \
      }                                                                           \
      if (reconstruct_unified_array_value<TYPE>(                                  \
              idx, value::TypeTraits<TypedArray<TYPE>>::type_id(),                \
              &sample->value)) {                                                  \
        return true;                                                              \
      }

      (void)count;
      RECONSTRUCT_ARRAY(float)
      RECONSTRUCT_ARRAY(double)
      RECONSTRUCT_ARRAY(int32_t)
      RECONSTRUCT_ARRAY(uint32_t)
      RECONSTRUCT_ARRAY(int64_t)
      RECONSTRUCT_ARRAY(uint64_t)
      RECONSTRUCT_ARRAY(value::half)
      RECONSTRUCT_ARRAY(value::half2)
      RECONSTRUCT_ARRAY(value::half3)
      RECONSTRUCT_ARRAY(value::half4)
      RECONSTRUCT_ARRAY(value::float2)
      RECONSTRUCT_ARRAY(value::float3)
      RECONSTRUCT_ARRAY(value::float4)
      RECONSTRUCT_ARRAY(value::double2)
      RECONSTRUCT_ARRAY(value::double3)
      RECONSTRUCT_ARRAY(value::double4)
      RECONSTRUCT_ARRAY(value::int2)
      RECONSTRUCT_ARRAY(value::int3)
      RECONSTRUCT_ARRAY(value::int4)
      RECONSTRUCT_ARRAY(value::quath)
      RECONSTRUCT_ARRAY(value::quatf)
      RECONSTRUCT_ARRAY(value::quatd)
      RECONSTRUCT_ARRAY(value::point3f)
      RECONSTRUCT_ARRAY(value::point3d)
      RECONSTRUCT_ARRAY(value::normal3f)
      RECONSTRUCT_ARRAY(value::normal3d)
      RECONSTRUCT_ARRAY(value::vector3f)
      RECONSTRUCT_ARRAY(value::vector3d)
      RECONSTRUCT_ARRAY(value::color3f)
      RECONSTRUCT_ARRAY(value::color3d)
      RECONSTRUCT_ARRAY(value::color4f)
      RECONSTRUCT_ARRAY(value::color4d)
      RECONSTRUCT_ARRAY(value::texcoord2f)
      RECONSTRUCT_ARRAY(value::texcoord2d)
      RECONSTRUCT_ARRAY(value::texcoord3f)
      RECONSTRUCT_ARRAY(value::texcoord3d)
      RECONSTRUCT_ARRAY(value::matrix2f)
      RECONSTRUCT_ARRAY(value::matrix2d)
      RECONSTRUCT_ARRAY(value::matrix3f)
      RECONSTRUCT_ARRAY(value::matrix3d)
      RECONSTRUCT_ARRAY(value::matrix4f)
      RECONSTRUCT_ARRAY(value::matrix4d)
#undef RECONSTRUCT_ARRAY
      return true;
    }

    // Scalar reconstruction from flat _data buffer
    const uint32_t tid = _type_id;

#define RECONSTRUCT_SCALAR(TYPE) \
    case value::TypeTraits<TYPE>::type_id(): { \
      if (static_cast<size_t>(byte_offset) + sizeof(TYPE) <= _data.size()) { \
        TYPE val; \
        std::memcpy(&val, _data.data() + byte_offset, sizeof(TYPE)); \
        sample->value = value::Value(val); \
      } \
      break; \
    }

    switch (tid) {
      RECONSTRUCT_SCALAR(value::half)
      RECONSTRUCT_SCALAR(float)
      RECONSTRUCT_SCALAR(double)
      RECONSTRUCT_SCALAR(int32_t)
      RECONSTRUCT_SCALAR(uint32_t)
      RECONSTRUCT_SCALAR(int64_t)
      RECONSTRUCT_SCALAR(uint64_t)
      case value::TypeTraits<bool>::type_id(): {
        if (static_cast<size_t>(byte_offset) + 1 <= _data.size()) {
          bool bval = (_data[byte_offset] != 0);
          sample->value = value::Value(bval);
        }
        break;
      }
      RECONSTRUCT_SCALAR(value::float2)
      RECONSTRUCT_SCALAR(value::float3)
      RECONSTRUCT_SCALAR(value::float4)
      RECONSTRUCT_SCALAR(value::double2)
      RECONSTRUCT_SCALAR(value::double3)
      RECONSTRUCT_SCALAR(value::double4)
      RECONSTRUCT_SCALAR(value::int2)
      RECONSTRUCT_SCALAR(value::int3)
      RECONSTRUCT_SCALAR(value::int4)
      RECONSTRUCT_SCALAR(value::half2)
      RECONSTRUCT_SCALAR(value::half3)
      RECONSTRUCT_SCALAR(value::half4)
      RECONSTRUCT_SCALAR(value::quath)
      RECONSTRUCT_SCALAR(value::quatf)
      RECONSTRUCT_SCALAR(value::quatd)
      RECONSTRUCT_SCALAR(value::point3f)
      RECONSTRUCT_SCALAR(value::point3d)
      RECONSTRUCT_SCALAR(value::normal3f)
      RECONSTRUCT_SCALAR(value::normal3d)
      RECONSTRUCT_SCALAR(value::vector3f)
      RECONSTRUCT_SCALAR(value::vector3d)
      RECONSTRUCT_SCALAR(value::color3f)
      RECONSTRUCT_SCALAR(value::color3d)
      RECONSTRUCT_SCALAR(value::color4f)
      RECONSTRUCT_SCALAR(value::color4d)
      RECONSTRUCT_SCALAR(value::texcoord2f)
      RECONSTRUCT_SCALAR(value::texcoord2d)
      RECONSTRUCT_SCALAR(value::texcoord3f)
      RECONSTRUCT_SCALAR(value::texcoord3d)
      RECONSTRUCT_SCALAR(value::matrix2f)
      RECONSTRUCT_SCALAR(value::matrix2d)
      RECONSTRUCT_SCALAR(value::matrix3f)
      RECONSTRUCT_SCALAR(value::matrix3d)
      RECONSTRUCT_SCALAR(value::matrix4f)
      RECONSTRUCT_SCALAR(value::matrix4d)
      // half role types
      RECONSTRUCT_SCALAR(value::vector3h)
      RECONSTRUCT_SCALAR(value::normal3h)
      RECONSTRUCT_SCALAR(value::point3h)
      RECONSTRUCT_SCALAR(value::color3h)
      RECONSTRUCT_SCALAR(value::color4h)
      RECONSTRUCT_SCALAR(value::texcoord2h)
      RECONSTRUCT_SCALAR(value::texcoord3h)
      RECONSTRUCT_SCALAR(value::timecode)
      RECONSTRUCT_SCALAR(Extent)
      default:
        break;
    }
#undef RECONSTRUCT_SCALAR
    return true;
}

bool TimeSamples::add_sample(const Sample &s, std::string *err) {
    if (has_unified_samples()) {
      if (err) {
        (*err) += "add_sample cannot append generic Value samples after unified storage samples.\n";
      }
      return false;
    }

    if (!s.value.is_none()) {
      if (!is_initialized()) {
        set_type_id(s.value.type_id());
      } else if (s.value.type_id() != _type_id) {
        if (err) {
          (*err) += "Type mismatch in TimeSamples: expected type_id " +
                    std::to_string(_type_id) + " but got " +
                    std::to_string(s.value.type_id()) + " (expected type: " +
                    type_name() + ", got: " + s.value.type_name() + ").\n";
        }
        return false;
      }
    }

    _samples.push_back(s);
    _dirty = true;
    return true;
}

bool TimeSamples::add_sample(double t, const value::Value &v, std::string *err) {
    if (has_unified_samples()) {
      if (err) {
        (*err) += "add_sample cannot append generic Value samples after unified storage samples.\n";
      }
      return false;
    }

    if (!v.is_none()) {
      if (!is_initialized()) {
        set_type_id(v.type_id());
      } else if (v.type_id() != _type_id) {
        if (err) {
          (*err) += "Type mismatch in TimeSamples: expected type_id " +
                    std::to_string(_type_id) + " but got " +
                    std::to_string(v.type_id()) + " (expected type: " +
                    type_name() + ", got: " + v.type_name() + ").\n";
        }
        return false;
      }
    }

    Sample s;
    s.t = t;
    s.value = v;
    s.blocked = v.is_none();
    _samples.push_back(s);
    _dirty = true;
    return true;
}

bool TimeSamples::add_blocked_sample(double t, const value::Value &v, std::string *err) {
    if (has_unified_samples()) {
      if (err) {
        (*err) += "add_blocked_sample cannot append generic Value samples after unified storage samples.\n";
      }
      return false;
    }

    if (!v.is_none() && v.type_id() != 1) {
      if (!is_initialized()) {
        set_type_id(v.type_id());
      } else if (v.type_id() != _type_id) {
        if (err) {
          (*err) += "Type mismatch in TimeSamples (blocked sample): expected type_id " +
                    std::to_string(_type_id) + " but got " +
                    std::to_string(v.type_id()) + ".\n";
        }
        return false;
      }
    }

    Sample s;
    s.t = t;
    s.value = v;
    s.blocked = true;
    _samples.emplace_back(s);
    _dirty = true;
    return true;
}

size_t TimeSamples::estimate_memory_usage() const {
    size_t total = sizeof(TimeSamples);

    total += _times.capacity() * sizeof(double);
    total += _blocked.capacity();
    total += _data.capacity();
    total += _data_offsets.capacity() * sizeof(size_t);
    total += _array_counts.capacity() * sizeof(uint32_t);

    total += _samples.capacity() * sizeof(Sample);
    for (const auto &sample : _samples) {
      total += sample.value.estimate_memory_usage();
    }

    return total;
}

size_t TimeSamples::estimate_actual_usage() const {
    size_t total = sizeof(TimeSamples);

    total += _times.size() * sizeof(double);
    total += _blocked.size();  // vector<bool> counts elements, matches estimate pattern
    total += _data.size();
    total += _data_offsets.size() * sizeof(size_t);
    total += _array_counts.size() * sizeof(uint32_t);

    total += _samples.size() * sizeof(Sample);
    for (const auto &sample : _samples) {
      total += sample.value.estimate_actual_usage();
    }

    return total;
}


std::vector<TimeSamples::Sample> &TimeSamples::samples() {
    if (!_times.empty() && _samples.empty()) {
      (void)get_samples();
    }
    if (_dirty) {
      update();
    }
    return _samples;
}

const std::vector<TimeSamples::Sample> &TimeSamples::get_samples() const {
    // Unified storage (_times/_data): convert to generic `_samples` once.
    //
    // This is a lazy const-mutation of `mutable _samples`. To keep a shared
    // (e.g. file-loaded) TimeSamples safe to read from multiple threads, the
    // one-time build is guarded: a lock-free acquire fast path once built, and a
    // function-local static mutex for the cold initial build (materialization is
    // kept lazy so unified storage isn't duplicated unless generic samples are
    // actually requested).
    if (!_times.empty() &&
        !_samples_ready.load(std::memory_order_acquire)) {
      static std::mutex s_materialize_mu;
      std::lock_guard<std::mutex> lk(s_materialize_mu);
      if (!_samples_ready.load(std::memory_order_relaxed)) {
        if (_dirty) {
          update();
        }

        _samples.clear();
        _samples.reserve(_times.size());

        for (size_t i = 0; i < _times.size(); ++i) {
          Sample s;
          if (!reconstruct_binary_sample(i, &s)) {
            s.t = (i < _times.size()) ? _times[i] : 0.0;
            s.value = value::Value();
            s.blocked = true;
          }
          _samples.push_back(s);
        }
        _samples_ready.store(true, std::memory_order_release);
      }
      return _samples;
    }

    if (_dirty) {
      update();
    }
    return _samples;
}


} // namespace value


// ============================================================================
// TimeSamples Implementation
// ============================================================================

namespace value {

// Move constructor
TimeSamples::TimeSamples(TimeSamples&& other) noexcept
    : _samples(std::move(other._samples)),
      _value_group_counter(other._value_group_counter),
      _times(std::move(other._times)),
      _blocked(std::move(other._blocked)),
      _data(std::move(other._data)),
      _data_offsets(std::move(other._data_offsets)),
      _array_counts(std::move(other._array_counts)),
      _type_id(other._type_id),
      _element_size(other._element_size),
      _dirty(other._dirty),
      _has_error(other._has_error),
      _is_array(other._is_array),
      _authored(other._authored),
      _samples_ready(other._samples_ready.load()) {
  other._value_group_counter = 0;
  other._type_id = 0;
  other._element_size = 0;
  other._dirty = false;
  other._has_error = false;
  other._is_array = false;
  other._authored = false;
  other._samples_ready.store(false);
}

// Move assignment operator
TimeSamples& TimeSamples::operator=(TimeSamples&& other) noexcept {
  if (this != &other) {
    _samples = std::move(other._samples);
    _times = std::move(other._times);
    _blocked = std::move(other._blocked);
    _data = std::move(other._data);
    _data_offsets = std::move(other._data_offsets);
    _array_counts = std::move(other._array_counts);
    _type_id = other._type_id;
    _element_size = other._element_size;
    _dirty = other._dirty;
    _has_error = other._has_error;
    _is_array = other._is_array;
    _authored = other._authored;
    _value_group_counter = other._value_group_counter;
    _samples_ready.store(other._samples_ready.load());

    other._value_group_counter = 0;
    other._type_id = 0;
    other._element_size = 0;
    other._dirty = false;
    other._has_error = false;
    other._is_array = false;
    other._authored = false;
    other._samples_ready.store(false);
  }
  return *this;
}

// Copy constructor
TimeSamples::TimeSamples(const TimeSamples& other)
    : _samples(other._samples),
      _value_group_counter(other._value_group_counter),
      _times(other._times),
      _blocked(other._blocked),
      _data(other._data),
      _data_offsets(other._data_offsets),
      _array_counts(other._array_counts),
      _type_id(other._type_id),
      _element_size(other._element_size),
      _dirty(other._dirty),
      _has_error(other._has_error),
      _is_array(other._is_array),
      _authored(other._authored),
      _samples_ready(other._samples_ready.load()) {
}

// Copy assignment operator
TimeSamples& TimeSamples::operator=(const TimeSamples& other) {
  if (this != &other) {
    _samples = other._samples;
    _times = other._times;
    _blocked = other._blocked;
    _data = other._data;
    _data_offsets = other._data_offsets;
    _array_counts = other._array_counts;
    _type_id = other._type_id;
    _element_size = other._element_size;
    _dirty = other._dirty;
    _has_error = other._has_error;
    _is_array = other._is_array;
    _authored = other._authored;
    _value_group_counter = other._value_group_counter;
    _samples_ready.store(other._samples_ready.load());
  }
  return *this;
}

// clear() method
void TimeSamples::clear() {
  _samples.clear();
  _times.clear();
  _blocked.clear();
  _data.clear();
  _data_offsets.clear();
  _array_counts.clear();
  _samples_ready.store(false);
  _type_id = 0;
  _element_size = 0;
  _dirty = true;
  _is_array = false;
  _authored = false;
  _value_group_counter = 0;
}



namespace {

// Helper function to get underlying type_id from a type_id
// For role types (color3f, point3f, etc.), returns the base type's type_id
// For non-role types, returns the same type_id
uint32_t GetUnderlyingTypeIdFromTypeId(uint32_t tyid) {
  // Strip array bit if present
  bool is_array = (tyid & TYPE_ID_1D_ARRAY_BIT) != 0;
  uint32_t base_tyid = tyid & (~TYPE_ID_1D_ARRAY_BIT);

  // Map role types to their underlying types
  // This is needed because we don't have TypeTraits access at runtime
#define MAP_ROLE_TO_UNDERLYING(role_id, underlying_id) \
  if (base_tyid == role_id) return is_array ? (underlying_id | TYPE_ID_1D_ARRAY_BIT) : underlying_id;

  // Texcoord types
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_TEXCOORD2H, TYPE_ID_HALF2)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_TEXCOORD2F, TYPE_ID_FLOAT2)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_TEXCOORD2D, TYPE_ID_DOUBLE2)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_TEXCOORD3H, TYPE_ID_HALF3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_TEXCOORD3F, TYPE_ID_FLOAT3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_TEXCOORD3D, TYPE_ID_DOUBLE3)

  // Normal types
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_NORMAL3H, TYPE_ID_HALF3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_NORMAL3F, TYPE_ID_FLOAT3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_NORMAL3D, TYPE_ID_DOUBLE3)

  // Vector types
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_VECTOR3H, TYPE_ID_HALF3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_VECTOR3F, TYPE_ID_FLOAT3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_VECTOR3D, TYPE_ID_DOUBLE3)

  // Point types
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_POINT3H, TYPE_ID_HALF3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_POINT3F, TYPE_ID_FLOAT3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_POINT3D, TYPE_ID_DOUBLE3)

  // Color types
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_COLOR3H, TYPE_ID_HALF3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_COLOR3F, TYPE_ID_FLOAT3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_COLOR3D, TYPE_ID_DOUBLE3)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_COLOR4H, TYPE_ID_HALF4)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_COLOR4F, TYPE_ID_FLOAT4)
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_COLOR4D, TYPE_ID_DOUBLE4)

  // Frame type
  MAP_ROLE_TO_UNDERLYING(TYPE_ID_FRAME4D, TYPE_ID_MATRIX4D)

#undef MAP_ROLE_TO_UNDERLYING

  // Not a role type, return as-is
  return tyid;
}

}  // namespace

bool TimeSamples::cast_to_role_type(uint32_t role_type_id) {
  if (_type_id == 0) {
    return false;  // Not initialized
  }

  // If already the target type, nothing to do
  if (_type_id == role_type_id) {
    return true;
  }

  // Get underlying type_ids for both current and target types
  uint32_t current_underlying = GetUnderlyingTypeIdFromTypeId(_type_id);
  uint32_t target_underlying = GetUnderlyingTypeIdFromTypeId(role_type_id);

  // Check if the underlying types match
  if (current_underlying != target_underlying) {
    return false;  // Incompatible types
  }

  // Safe to cast - just update the type_id
  _type_id = role_type_id;
  return true;
}

} // namespace value
} // namespace lightusd
