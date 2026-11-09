// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

///
/// @file timesamples.hh
/// @brief value::TimeSamples data structure for USD time-varying values
///
/// Type-erased container for time-sampled USD values, with support for
/// interpolation (Held / Linear) and value blocking (USD "None").
///
///
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>  // for SIZE_MAX
#include <limits>
#include <vector>
#include <type_traits>

#include "nonstd/optional.hpp"
#include "typed-array-core.hh"
#include "value-types.hh"
#include "buffer-util.hh"
// NOTE: value-eval-util.hh (the lerp<T>/slerp template math) is intentionally
// NOT included here — interpolation now lives in the non-template cores in
// timesamples.cc, which includes it directly. Pulling it through this header
// (widely included via animatable.hh / primvar.hh) was a leftover from the
// deleted header-inline TypedTimeSamples::get.

namespace lightusd {

namespace value {

// Forward declarations from value-types.hh
class Value;
template<typename T> struct TypeTraits;
template<typename T> struct LerpTraits;
class TimeCode;
enum class TimeSampleInterpolationType;
bool Lerp(const Value &p0, const Value &p1, double dt, Value *result);

// Forward declaration
struct TimeSamples;

///
/// Type-erased time samples container
/// Each sample contains a time value and associated Value object.
/// `None`(ValueBlock) is represented by setting `Sample::blocked` true.
///
struct TimeSamples {
  struct Sample {
    double t;
    value::Value value;
    bool blocked{false};

    // Value-identity group for generic (boxed) storage: samples added via
    // duplicate_sample() share the group id of their source, so consumers
    // can detect read-side-deduplicated duplicates in O(1) instead of
    // comparing contents (the analog of shared _data_offsets in binary
    // storage). 0 = unknown (content compare required). Ids are only
    // comparable within one TimeSamples object.
    uint64_t value_group{0};
  };

  // Sentinel value for blocked samples in _data_offsets
  static constexpr size_t BLOCKED_OFFSET = SIZE_MAX;

  bool empty() const {
    // Check unified storage first, then legacy storage
    return _times.empty() && _samples.empty();
  }

  size_t size() const {
    if (_dirty) {
      update();
    }
    // Prefer unified storage, fallback to legacy
    return !_times.empty() ? _times.size() : _samples.size();
  }

  bool is_initialized() const {
    return _type_id != 0;
  }

  // "timeSamples was AUTHORED", independent of whether it has any samples.
  // `float x.timeSamples = {}` is authored-but-empty, and size()/empty() cannot
  // tell that apart from "no timeSamples at all" -- which is why an empty
  // timeSamples block was dropped on write.
  void set_authored(bool onoff = true) { _authored = onoff; }
  bool authored() const { return _authored || (size() > 0); }

  void clear();

  /// Pre-allocate internal vectors for the expected number of samples.
  /// Call before adding samples in a loop when the total count is known.
  void reserve(size_t n) {
    _times.reserve(n);
    _blocked.reserve(n);
  }

  /// Duplicate an existing sample at a new time, sharing the underlying data.
  /// For binary storage, points to the same byte range in _data (zero-copy).
  /// For generic Value storage, copies the value::Value.
  /// @param src_idx Index of the source sample to duplicate
  /// @param new_time Time value for the new sample
  /// @return true if successful
  bool duplicate_sample(size_t src_idx, double new_time) {
    if (!_times.empty()) {
      // Binary storage path
      if (src_idx >= _times.size()) return false;
      if (src_idx >= _blocked.size()) return false;
      if (src_idx >= _data_offsets.size()) return false;

      const bool src_blocked = (_blocked[src_idx] != 0) ||
                               (_data_offsets[src_idx] == BLOCKED_OFFSET);
      if (_is_array && !src_blocked && src_idx >= _array_counts.size()) {
        return false;
      }

      _times.push_back(new_time);
      _blocked.push_back(_blocked[src_idx]);

      // Reuse the same byte offset - zero-copy dedup.
      _data_offsets.push_back(_data_offsets[src_idx]);

      if (_is_array) {
        _array_counts.push_back(src_blocked ? 0 : _array_counts[src_idx]);
      }

      invalidate_reconstructed_samples_cache();
      _dirty = true;
      return true;
    }

    // Generic Value storage path
    if (src_idx < _samples.size()) {
      Sample s;
      s.t = new_time;
      s.value = _samples[src_idx].value;
      s.blocked = _samples[src_idx].blocked;
      // Share the source's value-identity group (assign one if the source
      // doesn't have any yet) so consumers can detect the duplicate in O(1).
      if (_samples[src_idx].value_group == 0) {
        _samples[src_idx].value_group = ++_value_group_counter;
      }
      s.value_group = _samples[src_idx].value_group;
      _samples.push_back(std::move(s));
      _dirty = true;
      return true;
    }

    return false;
  }

  /// Move constructor
  TimeSamples(TimeSamples&& other) noexcept;

  /// Move assignment operator
  TimeSamples& operator=(TimeSamples&& other) noexcept;

  /// Copy constructor
  TimeSamples(const TimeSamples& other);

  /// Copy assignment operator
  TimeSamples& operator=(const TimeSamples& other);

  // Default constructor
  TimeSamples() = default;

  /// Set the type_id for metadata-only use (e.g. all-blocked TimeSamples).
  /// For normal usage, add_sample<T>() auto-detects the type on first call.
  /// Replaces the old init() method.
  void set_type_id(uint32_t tid) {
    if (tid != 0 && _type_id == 0) {
      _type_id = tid;
    }
  }


  /// Cast the TimeSamples' type to a role type if the underlying types are compatible.
  /// This allows reinterpreting stored base types (e.g., float3) as role types (e.g., color3f).
  /// @param role_type_id The target role type's type_id
  /// @return true if the cast was successful, false if the underlying types don't match
  bool cast_to_role_type(uint32_t role_type_id);

  /// Check if unified binary storage has samples (i.e., _times is not empty)
  bool is_using_binary_storage() const { return !_times.empty(); }

  /// Check if storing array data
  bool is_array() const { return _is_array; }

  /// Element size for binary storage (sizeof(T))
  uint32_t element_size() const { return _element_size; }

 private:
  bool has_unified_samples() const {
    return !_times.empty();
  }

  bool has_generic_samples() const {
    return !_samples.empty() && !has_unified_samples();
  }

  bool validate_type_or_init(uint32_t expected_type_id, std::string* err,
                             const char* op_name) {
    if (!is_initialized()) {
      set_type_id(expected_type_id);
      return true;
    }

    if (_type_id != expected_type_id) {
      if (err) {
        (*err) += std::string(op_name) + " type mismatch: expected type_id " +
                  std::to_string(_type_id) + " but got " +
                  std::to_string(expected_type_id) + ".\n";
      }
      return false;
    }

    return true;
  }

  template<typename T>
  struct UnifiedArrayRef {
    const T* data{nullptr};
    size_t count{0};
    bool blocked{false};
    bool available{false};
  };

  template<typename T>
  bool resolve_unified_array_ref(size_t idx, uint32_t expected_type_id,
                                 UnifiedArrayRef<T>* ref) const {
    if (!ref) {
      return false;
    }

    *ref = {};

    if (_dirty) {
      update();
    }

    if (_times.empty() || !_is_array) {
      return false;
    }

    if (idx >= _times.size() || idx >= _blocked.size()) {
      return false;
    }

    if (_type_id != expected_type_id) {
      return false;
    }

    ref->available = true;
    ref->blocked = (_blocked[idx] != 0);
    if (ref->blocked) {
      return true;
    }

    if (idx >= _data_offsets.size()) {
      return false;
    }

    size_t byte_offset = _data_offsets[idx];
    if (byte_offset == BLOCKED_OFFSET) {
      ref->blocked = true;
      return true;
    }

    ref->count = get_array_count(idx);
    if (ref->count == 0) {
      ref->data = nullptr;
      return true;
    }

    if (static_cast<size_t>(byte_offset) + sizeof(T) * ref->count > _data.size()) {
      return false;
    }

    ref->data = reinterpret_cast<const T*>(_data.data() + byte_offset);
    return true;
  }

  template<typename T>
  bool reconstruct_unified_array_value(size_t idx, uint32_t expected_type_id,
                                       value::Value* dst) const {
    if (!dst) {
      return false;
    }

    UnifiedArrayRef<T> ref;
    if (!resolve_unified_array_ref<T>(idx, expected_type_id, &ref) || ref.blocked) {
      return false;
    }

    if (expected_type_id == value::TypeTraits<std::vector<T>>::type_id()) {
      std::vector<T> vec;
      if (ref.count > 0) {
        vec.assign(ref.data, ref.data + ref.count);
      }
      *dst = value::Value(std::move(vec));
      return true;
    }

    if (expected_type_id == value::TypeTraits<TypedArray<T>>::type_id()) {
      TypedArray<T> typed;
      typed.resize(ref.count);
      if (ref.count > 0) {
        std::memcpy(typed.data(), ref.data, sizeof(T) * ref.count);
      }
      *dst = value::Value(std::move(typed));
      return true;
    }

    return false;
  }

  bool reconstruct_binary_sample(size_t idx, Sample* sample) const;  // Defined in timesamples.cc


 public:
  /// Finalize (sort) the samples and clear the dirty flag.
  ///
  /// const read accessors (size(), get(), ...) call this lazily when `_dirty`,
  /// which mutates the internal `mutable` storage. Call update() ONCE
  /// (single-threaded) after populating a TimeSamples so that subsequent const
  /// reads are pure and the object can be safely shared across threads. The
  /// USDA/USDC parsers call this at load time; user-built TimeSamples (via
  /// add_sample()) should call it before sharing for concurrent reads.
  /// Idempotent: a no-op when already sorted/clean.
  void update() const;

  // Returns true if an internal invariant violation was detected during
  // update() (e.g. parallel arrays get out of sync). When true, getters may
  // return incomplete / mismatched data; callers should not trust the
  // TimeSamples and treat it as unusable. See concern 4 in review.md.
  bool has_error() const { return _has_error; }

  bool has_sample_at(const double t) const;

  nonstd::optional<double> get_time(size_t idx) const {
    // Check unified storage first
    if (!_times.empty()) {
      if (idx >= _times.size()) {
        return nonstd::nullopt;
      }
      if (_dirty) {
        update();
      }
      return _times[idx];
    }

    // Fallback to generic sample storage
    if (idx >= _samples.size()) {
      return nonstd::nullopt;
    }
    if (_dirty) {
      update();
    }
    return _samples[idx].t;
  }

  nonstd::optional<value::Value> get_value(size_t idx) const {
    const auto &samples = get_samples();
    if (idx >= samples.size()) {
      return nonstd::nullopt;
    }
    return samples[idx].value;
  }

  /// Reconstruct a single sample by index WITHOUT materializing all samples.
  /// Unlike get_samples() (which builds and caches the full N-sample vector and
  /// thus expands read-side-deduplicated values into N independent copies), this
  /// touches only one sample's worth of memory. Pair it with get_data_offsets()
  /// so callers can skip reconstructing samples that share an underlying value
  /// block. Returns false if idx is out of range.
  bool get_sample_at(size_t idx, Sample *out) const {
    if (!out) {
      return false;
    }
    if (_dirty) {
      update();
    }
    if (!_times.empty()) {
      return reconstruct_binary_sample(idx, out);
    }
    if (idx >= _samples.size()) {
      return false;
    }
    *out = _samples[idx];
    return true;
  }

  uint32_t type_id() const {
    // Unified storage uses _type_id directly
    if (!_times.empty() || _type_id != 0) {
      return _type_id;
    }
    // Legacy storage: get from samples if available
    if (_samples.size()) {
      if (_dirty) {
        update();
      }
      // If first sample has valid type (not type_id 1), use it
      uint32_t sample_type_id = _samples[0].value.type_id();
      if (sample_type_id != 1) {
        return sample_type_id;
      }
      // Otherwise use stored type_id (for all-VALUE_BLOCK case)
      return _type_id;
    } else {
      return _type_id; // Return stored type_id if initialized
    }
  }

  std::string type_name() const {
    // Check if using unified storage or have a type_id
    if (!_times.empty() || _type_id != 0) {
      // Get type name from type_id
      if (_type_id != 0) {
        return value::GetTypeName(_type_id);
      } else {
        return std::string();
      }
    }

    // Original path for generic Value storage
    if (_samples.size()) {
      if (_dirty) {
        update();
      }
      return _samples[0].value.type_name();
    } else {
      return std::string();
    }
  }

  bool add_sample(const Sample &s, std::string *err = nullptr);  // Defined in timesamples.cc

  // Value may be None(ValueBlock)
  bool add_sample(double t, const value::Value &v, std::string *err = nullptr);      // Defined in timesamples.cc

  // We still need "dummy" value for type_name() and type_id()
  bool add_blocked_sample(double t, const value::Value &v, std::string *err = nullptr);  // Defined in timesamples.cc


  template <typename T,
            typename std::enable_if<
                !std::is_same<typename std::decay<T>::type, value::Value>::value &&
                !std::is_same<typename std::decay<T>::type, Sample>::value,
                int>::type = 0>
  bool add_sample(double t, const T& value, std::string *err = nullptr,
                  size_t expected_total_samples = 0) {
    (void)expected_total_samples;

    if constexpr (value::uses_binary_timesample_scalar_storage_v<T>) {
      set_type_id(value::TypeTraits<T>::type_id());
      return add_binary_sample<T>(t, value, err);
    } else {
      return add_sample(t, value::Value(value), err);
    }
  }

  template <typename T>
  bool add_array_sample(double t, const std::vector<T>& value,
                        std::string *err = nullptr,
                        size_t expected_total_samples = 0) {
    (void)expected_total_samples;

    if constexpr (value::uses_binary_timesample_array_storage_v<T>) {
      if (!validate_type_or_init(value::TypeTraits<std::vector<T>>::type_id(),
                                   err, "add_array_sample<std::vector>")) {
        return false;
      }
      return add_array_sample<T>(t, value.data(), value.size(), err);
    } else {
      return add_sample(t, value::Value(value), err);
    }
  }

  template <typename T>
  bool add_array_sample(double t, const TypedArray<T>& value,
                        std::string *err = nullptr,
                        size_t expected_total_samples = 0) {
    (void)expected_total_samples;

    if constexpr (value::uses_binary_timesample_array_storage_v<T>) {
      if (!validate_type_or_init(value::TypeTraits<TypedArray<T>>::type_id(),
                                   err, "add_array_sample<TypedArray>")) {
        return false;
      }
      return add_array_sample<T>(t, value.data(), value.size(), err);
    } else {
      std::vector<T> vec(value.data(), value.data() + value.size());
      return add_sample(t, value::Value(vec), err);
    }
  }

  template<typename T>
  bool add_blocked_sample(double t, std::string *err = nullptr,
                          size_t expected_total_samples = 0) {
    (void)expected_total_samples;

    if constexpr (value::uses_binary_timesample_scalar_storage_v<T>) {
      set_type_id(value::TypeTraits<T>::type_id());
      return add_binary_blocked_sample<T>(t, err);
    } else {
      return add_blocked_sample(t, value::Value(T{}), err);
    }
  }

  /// Get TypedArray sample at specific time
  template<typename T>
  bool get_typed_array_at_time(double t, TypedArray<T>* typed_array, bool* blocked = nullptr) const {
    if (!typed_array) {
      return false;
    }

    if (_dirty) {
      update();
    }

    // Check if storing TypedArray data
    if (_type_id != value::TypeTraits<TypedArray<T>>::type_id()) {
      return false;
    }

    // Use unified storage - find index by time
    if (!_times.empty()) {
      size_t idx = find_time_index_in_unified(t);
      if (idx != kNotFound) {
        return get_typed_array_at<T>(idx, typed_array, blocked);
      }
    }

    return false;
  }

  /// Get TypedArray sample at specific index
  template<typename T>
  bool get_typed_array_at(size_t idx, TypedArray<T>* typed_array, bool* blocked = nullptr) const {
    if (!typed_array) {
      return false;
    }

    if (_dirty) {
      update();
    }

    UnifiedArrayRef<T> ref;
    if (resolve_unified_array_ref<T>(idx, value::TypeTraits<TypedArray<T>>::type_id(), &ref)) {
      if (ref.blocked) {
        if (blocked) *blocked = true;
        return false;
      }

      typed_array->resize(ref.count);
      if (ref.count > 0) {
        std::memcpy(typed_array->data(), ref.data, sizeof(T) * ref.count);
      }
      if (blocked) *blocked = false;
      return true;
    }

    return false;
  }

  /// Get TypedArrayView at specific index
  /// Returns a view for TypedArray or array data (std::vector)
  /// Returns an empty view for blocked values or non-array data
  template<typename T>
  TypedArrayView<const T> get_typed_array_view_at(size_t idx) const {
    if (_dirty) {
      update();
    }

    UnifiedArrayRef<T> ref;
    if (resolve_unified_array_ref<T>(idx, value::TypeTraits<TypedArray<T>>::type_id(), &ref)) {
      if (ref.blocked || ref.count == 0) {
        return TypedArrayView<const T>();
      }
      return TypedArrayView<const T>(ref.data, ref.count);
    }

    // For regular Value storage (generic path)
    if (idx >= _samples.size()) {
      return TypedArrayView<const T>();  // Empty view
    }

    const Sample& sample = _samples[idx];

    // Check if blocked
    if (sample.blocked) {
      return TypedArrayView<const T>();  // Empty view for blocked values
    }

    // Try to get as TypedArray first
    if (const TypedArray<T>* typed_array = sample.value.as<TypedArray<T>>()) {
      if (typed_array->data() && typed_array->size() > 0) {
        return TypedArrayView<const T>(*typed_array);
      }
    }

    // Try to get as std::vector
    if (const std::vector<T>* vec = sample.value.as<std::vector<T>>()) {
      if (!vec->empty()) {
        return TypedArrayView<const T>(*vec);
      }
    }

    // Not array data or unsupported type
    return TypedArrayView<const T>();  // Empty view
  }

  /// Get TypedArrayView at specific time
  template<typename T>
  TypedArrayView<const T> get_typed_array_view_at_time(double t) const {
    if (_dirty) {
      update();
    }

    // Check unified storage first
    if (!_times.empty()) {
      size_t idx = find_time_index_in_unified(t);
      if (idx != kNotFound) {
        return get_typed_array_view_at<T>(idx);
      }
    }

    // For regular Value storage
    size_t idx = find_time_index_in_samples(t);
    if (idx != kNotFound) {
      return get_typed_array_view_at<T>(idx);
    }

    return TypedArrayView<const T>();  // Empty view
  }

  const std::vector<Sample> &get_samples() const;  // Defined in timesamples.cc

  std::vector<Sample> &samples();  // Defined in timesamples.cc

  // Get value at time `t`, cast to T. Single entry point for the type-erased
  // timesamples: accepts an exact type_id match or a role-compatible underlying
  // layout (scalar or array), mirroring value::Value::as<T>(). Scalar T uses the
  // binary-direct evaluator get_scalar(); array (and other non-binary) T use the
  // generic value evaluator get_value_at() plus a layout-compatible cast. Held vs
  // Linear and blocked-sample handling live entirely in those non-template cores
  // (timesamples.cc), so this stays a thin per-T forwarder rather than ~190 lines
  // of interpolation re-instantiated in every includer.
  template <typename T>
  bool get(T *dst, double t = value::TimeCode::Default(),
           value::TimeSampleInterpolationType interp =
               value::TimeSampleInterpolationType::Linear) const {
    if (!dst || empty()) {
      return false;
    }

    // Acceptance: exact type_id, or role-compatible underlying layout
    // (scalar e.g. color3f<->float3, or array e.g. normal3f[]<->float3[]).
    constexpr uint32_t want = value::TypeTraits<T>::type_id();
    const uint32_t tid = _type_id;
    bool accept = (want == tid);
    if (!accept) {
      constexpr bool want_array = (want & value::TYPE_ID_1D_ARRAY_BIT) != 0;
      const bool have_array = (tid & value::TYPE_ID_1D_ARRAY_BIT) != 0;
      if (want_array && have_array) {
        accept = ((value::TypeTraits<T>::underlying_type_id() &
                   ~value::TYPE_ID_1D_ARRAY_BIT) ==
                  (value::GetUnderlyingTypeId(tid) &
                   ~value::TYPE_ID_1D_ARRAY_BIT));
      } else if (!want_array && !have_array) {
        accept = (value::TypeTraits<T>::underlying_type_id() ==
                  value::GetUnderlyingTypeId(tid));
      }
    }
    if (!accept) {
      return false;
    }

    if constexpr ((want & value::TYPE_ID_1D_ARRAY_BIT) != 0) {
      // Array / non-binary path: interpolate to a value::Value, then cast out.
      value::Value tmp;
      if (!get_value_at(&tmp, t, interp)) {
        return false;
      }
      if (const T *pv = tmp.as<T>()) {
        (*dst) = *pv;
        return true;
      }
      return false;
    } else {
      // Scalar path: binary-direct evaluator writes straight into dst
      // (dst is layout-compatible with the stored type by the check above).
      return get_scalar(static_cast<void *>(dst), t, interp);
    }
  }

  size_t estimate_memory_usage() const;  // Defined in timesamples.cc

  /// Estimate actual (size-based) memory usage, as opposed to
  /// estimate_memory_usage() which reports allocated (capacity-based) usage.
  size_t estimate_actual_usage() const;  // Defined in timesamples.cc

  //
  // Unified array methods (work directly with TimeSamples storage)
  //

  /// Add array sample to flat byte buffer
  template<typename T>
  bool add_array_sample(double t, const T* values, size_t count, std::string* err = nullptr) {
    static_assert(value::uses_binary_timesample_array_storage_v<T>,
                  "add_array_sample requires binary-serializable element types except bool");

    if ((count > 0) && (values == nullptr)) {
      if (err) {
        (*err) += "add_array_sample values is null for a non-empty array.\n";
      }
      return false;
    }

    // Compare in a type that can represent both operands. On wasm32 size_t and
    // uint32_t have the same range, so a direct comparison is provably false
    // and Clang diagnoses it; native 64-bit builds still need this storage cap.
    if (static_cast<uintmax_t>(count) >
        static_cast<uintmax_t>((std::numeric_limits<uint32_t>::max)())) {
      if (err) {
        (*err) += "add_array_sample element count exceeds uint32_t storage.\n";
      }
      return false;
    }

    if (count > ((std::numeric_limits<size_t>::max)() / sizeof(T))) {
      if (err) {
        (*err) += "add_array_sample byte size overflow.\n";
      }
      return false;
    }

    const size_t data_size = sizeof(T) * count;
    if (data_size > (_data.max_size() - _data.size())) {
      if (err) {
        (*err) += "add_array_sample data buffer size overflow.\n";
      }
      return false;
    }

    if (!validate_type_or_init(value::TypeTraits<std::vector<T>>::type_id(),
                               err, "add_array_sample")) {
      return false;
    }

    _element_size = static_cast<uint32_t>(sizeof(T));
    _is_array = true;

    _times.push_back(t);
    _blocked.push_back(0);

    // Append array data to flat buffer
    const size_t byte_offset = _data.size();
    _data.resize(_data.size() + data_size);
    if (data_size > 0) {
      std::memcpy(_data.data() + byte_offset, values, data_size);
    }

    _data_offsets.push_back(byte_offset);
    _array_counts.push_back(static_cast<uint32_t>(count));

    invalidate_reconstructed_samples_cache();
    _dirty = true;
    return true;
  }

  /// Add matrix array sample (delegates to add_array_sample)
  template<typename T>
  bool add_matrix_array_sample(double t, const T* matrices, size_t count, std::string* err = nullptr) {
    return add_array_sample<T>(t, matrices, count, err);
  }

  bool add_bool_array_sample(double t, const std::vector<bool>& value,
                             std::string *err = nullptr) {
    return add_sample(t, value::Value(value), err);
  }

  //
  // Unified binary scalar sample methods — all types go into _data
  //

  /// Add a binary-serializable scalar sample to flat byte buffer
  template<typename T>
  bool add_binary_sample(double t, const T& value, std::string* err = nullptr) {
    static_assert(value::uses_binary_timesample_scalar_storage_v<T>,
                  "add_binary_sample requires binary-serializable types except bool");

    if (!validate_type_or_init(value::TypeTraits<T>::type_id(), err,
                                 "add_binary_sample")) {
      return false;
    }

    _element_size = static_cast<uint32_t>(sizeof(T));

    _times.push_back(t);
    _blocked.push_back(0);

    size_t byte_offset = _data.size();
    _data.resize(_data.size() + sizeof(T));
    std::memcpy(_data.data() + byte_offset, &value, sizeof(T));
    _data_offsets.push_back(byte_offset);

    invalidate_reconstructed_samples_cache();
    _dirty = true;
    return true;
  }

  /// Add a blocked binary-serializable scalar sample
  template<typename T>
  bool add_binary_blocked_sample(double t, std::string* err = nullptr) {
    static_assert(value::uses_binary_timesample_scalar_storage_v<T>,
                  "add_binary_blocked_sample requires binary-serializable types except bool");

    if (!validate_type_or_init(value::TypeTraits<T>::type_id(), err,
                                 "add_binary_blocked_sample")) {
      return false;
    }

    _element_size = static_cast<uint32_t>(sizeof(T));

    _times.push_back(t);
    _blocked.push_back(1);  // Blocked
    _data_offsets.push_back(BLOCKED_OFFSET);

    invalidate_reconstructed_samples_cache();
    _dirty = true;
    return true;
  }

  /// Add a blocked (None / ValueBlock) sample to an ARRAY-valued binary
  /// timesamples. Mirrors add_array_sample's bookkeeping — it validates the
  /// ARRAY type id, sets `_is_array`, and pushes an aligned `_array_counts`
  /// entry (0) alongside the BLOCKED_OFFSET. The scalar add_binary_blocked_sample
  /// validates the *scalar* type id, which conflicts with an array-typed
  /// TimeSamples and fails the add — that aborts reconstruction and silently
  /// drops every sample of an animated array attribute that authors a `None`
  /// time sample. Use this for the array case instead.
  template<typename T>
  bool add_array_blocked_sample(double t, std::string* err = nullptr,
                                size_t expected_total_samples = 0) {
    (void)expected_total_samples;
    static_assert(value::uses_binary_timesample_array_storage_v<T>,
                  "add_array_blocked_sample requires binary-serializable array "
                  "element types");

    if (!validate_type_or_init(value::TypeTraits<std::vector<T>>::type_id(),
                               err, "add_array_blocked_sample")) {
      return false;
    }

    _element_size = static_cast<uint32_t>(sizeof(T));
    _is_array = true;

    _times.push_back(t);
    _blocked.push_back(1);  // Blocked
    _data_offsets.push_back(BLOCKED_OFFSET);
    _array_counts.push_back(0);  // keep _array_counts aligned with _times

    invalidate_reconstructed_samples_cache();
    _dirty = true;
    return true;
  }

  //
  // std::vector<T> support (Phase 2 Step 3)
  //
  // NOTE: We do NOT override add_sample(double t, const std::vector<T>&) because
  // that would break interpolation support for std::vector<T> types.
  // The existing Value-based add_sample works correctly with interpolation.
  // We only provide convenience getters below.

  /// Get sample as std::vector<T> - retrieves array data into a vector
  /// Works with both unified binary storage and generic Value storage
  /// @param idx Sample index
  /// @param out_vec Output vector to fill with data
  /// @param out_blocked Optional: set to true if sample is blocked
  /// @return true if successful, false if index out of range or wrong type
  template<typename T>
  bool get_vector_at(size_t idx, std::vector<T>* out_vec, bool* out_blocked = nullptr) const {
    if (!out_vec) {
      return false;
    }

    if (_dirty) {
      update();
    }

    if constexpr (value::uses_binary_timesample_array_storage_v<T>) {
      UnifiedArrayRef<T> ref;
      if (resolve_unified_array_ref<T>(idx, value::TypeTraits<std::vector<T>>::type_id(), &ref)) {
        if (ref.blocked) {
          if (out_blocked) *out_blocked = true;
          return false;
        }

        if (ref.count == 0) {
          out_vec->clear();
        } else {
          out_vec->assign(ref.data, ref.data + ref.count);
        }
        if (out_blocked) *out_blocked = false;
        return true;
      }
    }

    // Check generic Value storage
    if (idx >= _samples.size()) {
      return false;
    }

    const Sample& sample = _samples[idx];

    // Check if blocked
    if (sample.blocked) {
      if (out_blocked) *out_blocked = true;
      return false;
    }

    // Try to get as std::vector<T>
    if (const std::vector<T>* vec = sample.value.as<std::vector<T>>()) {
      *out_vec = *vec;
      if (out_blocked) *out_blocked = false;
      return true;
    }

    // Try to get as TypedArray<T>
    if constexpr (value::uses_binary_timesample_array_storage_v<T>) {
      if (const TypedArray<T>* typed_array = sample.value.as<TypedArray<T>>()) {
        if (typed_array->data() && typed_array->size() > 0) {
          out_vec->assign(typed_array->data(), typed_array->data() + typed_array->size());
          if (out_blocked) *out_blocked = false;
          return true;
        }
      }
    }

    // Type mismatch or unsupported
    return false;
  }

  /// Get sample as std::vector<T> at specific time
  /// @param t Time value
  /// @param out_vec Output vector to fill with data
  /// @param out_blocked Optional: set to true if sample is blocked
  /// @return true if successful, false if no sample at time or wrong type
  template<typename T>
  bool get_vector_at_time(double t, std::vector<T>* out_vec, bool* out_blocked = nullptr) const {
    if (!out_vec) {
      return false;
    }

    if (_dirty) {
      update();
    }

    // Check unified binary storage
    if (!_times.empty()) {
      size_t idx = find_time_index_in_unified(t);
      if (idx != kNotFound) {
        return get_vector_at<T>(idx, out_vec, out_blocked);
      }
      return false;  // Time not found
    }

    // Check generic Value storage
    size_t idx = find_time_index_in_samples(t);
    if (idx != kNotFound) {
      return get_vector_at<T>(idx, out_vec, out_blocked);
    }

    return false;  // Time not found
  }

  //
  // Accessor methods for binary storage
  //

  const std::vector<double>& get_times() const LIGHTUSD_LIFETIMEBOUND {
    if (_dirty) { update(); }
    return _times;
  }

  const Buffer<16>& get_blocked() const LIGHTUSD_LIFETIMEBOUND {
    if (_dirty) { update(); }
    return _blocked;
  }

  const std::vector<uint8_t>& get_data() const LIGHTUSD_LIFETIMEBOUND {
    if (_dirty) { update(); }
    return _data;
  }

  const std::vector<size_t>& get_data_offsets() const LIGHTUSD_LIFETIMEBOUND {
    if (_dirty) { update(); }
    return _data_offsets;
  }

  size_t get_array_count(size_t idx) const {
    if (idx >= _array_counts.size()) {
      return 0;
    }
    return _array_counts[idx];
  }

  const std::vector<uint32_t>& get_array_counts() const LIGHTUSD_LIFETIMEBOUND {
    return _array_counts;
  }

 private:
  // [Phase 1] Binary-direct scalar evaluator. `get_scalar` is a non-template
  // switch on `_type_id` that delegates to the per-type `get_scalar_impl<T>`;
  // both are defined (and `get_scalar_impl<T>` explicitly instantiated) only in
  // timesamples.cc, so the heavy per-type code is not re-instantiated in every
  // includer. `dst` points to a T whose layout is compatible with `_type_id`
  // (enforced by eval_scalar / the public get<T> wrapper).
  bool get_scalar(void *dst, double t,
                  value::TimeSampleInterpolationType interp) const;
  template <typename T>
  bool get_scalar_impl(void *dst, double t,
                       value::TimeSampleInterpolationType interp) const;

  // [Phase 1] Generic value-path evaluator: produces the interpolated
  // value::Value at time t (non-template; dispatches on _type_id via Lerp).
  // The public get<T>() uses this for array (and other non-binary) types, then
  // casts the result to T. Defined in timesamples.cc.
  bool get_value_at(value::Value *out, double t,
                    value::TimeSampleInterpolationType interp) const;

  // Generic path storage (for non-binary Value types: string, token, dict, etc.)
  mutable std::vector<Sample> _samples;

  // Monotonic source for Sample::value_group ids (generic storage only).
  mutable uint64_t _value_group_counter{0};

  // Flat binary storage (for trivially-copyable POD types)
  mutable std::vector<double> _times;
  mutable Buffer<16> _blocked;                      // Blocked flags (one byte per sample)
  mutable std::vector<uint8_t> _data;               // Flat byte buffer for ALL binary values
  mutable std::vector<size_t> _data_offsets;        // Per-sample byte offset into _data (size_t: no 4GB limit)
  mutable std::vector<uint32_t> _array_counts;      // Per-sample element count (arrays only)

  // Metadata
  uint32_t _type_id{0};
  uint32_t _element_size{0};                        // sizeof(T) for binary elements
  mutable bool _dirty{false};
  mutable bool _has_error{false};                   // Set if update() detected a parallel-array invariant violation
  bool _is_array{false};                            // true if storing array data

  // "timeSamples was AUTHORED", even if it holds no samples. See set_authored().
  // NOTE: TimeSamples has hand-written copy/move ctors and assignment operators
  // (timesamples.cc) that copy member-by-member -- a new member added here MUST
  // be added to all four, or it is silently dropped on every copy.
  bool _authored{false};

  // Guards the one-time lazy materialization of `_samples` from unified
  // (`_times`/`_data`) storage in get_samples(): once finalized (set at parse
  // time, see update()), reads must be pure so a shared TimeSamples is safe to
  // read from multiple threads. Lock-free fast path once set; the cold-path
  // build serializes on a function-local static mutex (see get_samples()).
  mutable std::atomic<bool> _samples_ready{false};

  // _pod_samples removed - using unified storage directly

  void invalidate_reconstructed_samples_cache() {
    _samples.clear();
    // Force get_samples() to re-materialize from unified storage on next call.
    _samples_ready.store(false);
  }

 public:
  /// Apply a time transform to all sample times: t_new = t_old * scale + offset.
  /// Used for LayerOffset composition (AOUSD Core Spec 10.3.1).
  /// Works for both unified binary storage(_times) and generic storage(_samples).
  void apply_time_transform(double scale, double offset) {
    if ((offset == 0.0) && (scale == 1.0)) {
      return;  // Identity
    }

    if (!_times.empty()) {
      for (auto &t : _times) {
        t = t * scale + offset;
      }
      // _samples may hold a reconstructed cache of _times. Drop it so it gets
      // rebuilt with the transformed times.
      invalidate_reconstructed_samples_cache();
    } else {
      for (auto &s : _samples) {
        s.t = s.t * scale + offset;
      }
    }

    // Negative scale reverses the ordering; mark dirty to force re-sort.
    _dirty = true;
  }

 private:

  /// Find index for time value in _times vector using epsilon comparison
  /// @param t Time value to search for
  /// @return Index if found, or size_t(-1) if not found
  size_t find_time_index_in_unified(double t) const {
    const double eps = std::numeric_limits<double>::epsilon();
    auto it = std::lower_bound(_times.begin(), _times.end(), t - eps);
    if ((it != _times.end()) && (std::fabs(*it - t) < eps)) {
      return static_cast<size_t>(std::distance(_times.begin(), it));
    }
    if (it != _times.begin()) {
      auto prev = it - 1;
      if (std::fabs(*prev - t) < eps) {
        return static_cast<size_t>(std::distance(_times.begin(), prev));
      }
    }
    return static_cast<size_t>(-1);  // Not found
  }

  /// Find index for time value in _samples vector using epsilon comparison
  /// @param t Time value to search for
  /// @return Index if found, or size_t(-1) if not found
  size_t find_time_index_in_samples(double t) const {
    const double eps = std::numeric_limits<double>::epsilon();
    auto it = std::lower_bound(
        _samples.begin(), _samples.end(), t - eps,
        [](const Sample &s, double v) { return s.t < v; });
    if ((it != _samples.end()) && (std::fabs(it->t - t) < eps)) {
      return static_cast<size_t>(std::distance(_samples.begin(), it));
    }
    if (it != _samples.begin()) {
      auto prev = it - 1;
      if (std::fabs(prev->t - t) < eps) {
        return static_cast<size_t>(std::distance(_samples.begin(), prev));
      }
    }
    return static_cast<size_t>(-1);  // Not found
  }

  static constexpr size_t kNotFound = static_cast<size_t>(-1);

};

} // namespace value


namespace value {

// TypeTrait for the `TimeSamples` value type.
//
// Defined here (rather than in value-types.hh) because it requires the complete
// `TimeSamples` type, which is defined above in this header. Keeping it here lets
// value-types.hh avoid including timesamples.hh entirely. Any TU that stores a
// `TimeSamples` inside a `value::Value` (and hence needs TypeTraits<TimeSamples>)
// already includes this header.
#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(TimeSamples, "TimeSamples", TYPE_ID_TIMESAMPLES, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

} // namespace lightusd
