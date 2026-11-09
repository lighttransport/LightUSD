// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// animatable.hh - Animatable<T> template for scalar and timeSamples values
//
#pragma once

#include <memory>
#include <type_traits>

#include "timesamples.hh"

namespace lightusd {

namespace animatable_detail {
// True iff value::TypeTraits<T> is a complete specialization (i.e. T is a
// registered value type that value::TimeSamples stores directly). False for
// enums, which Animatable stores as their underlying int64 instead. Consumers
// (pprint / writer) branch on this to decide whether to render T directly or
// cast int64 -> enum.
template <typename T, typename = void>
struct has_value_type_traits : std::false_type {};
template <typename T>
struct has_value_type_traits<
    T, std::void_t<decltype(value::TypeTraits<T>::type_id())>>
    : std::true_type {};
}  // namespace animatable_detail

//
// Scalar(default) and/or TimeSamples
//
// TimeSamples are stored uniformly in a single heap-allocated, type-erased
// value::TimeSamples owned via unique_ptr (RAII; nullptr for the common
// scalar-only case -> 8 bytes instead of an always-present container):
//  - value types (have a value::TypeTraits registration): stored as themselves.
//  - enum types (no registration): stored as their underlying int64 (a registered
//    value type); the enum<->int64 cast happens at this boundary (to_store/
//    from_store). Rendering enum timesamples back to their token names stays at
//    the write/pprint layer, which knows the concrete enum type.
//
// This removes the second, per-type TypedTimeSamples<T> timesamples
// implementation and the kTyped dual storage path.
//
template <typename T>
struct Animatable {
 private:
  static constexpr bool kIsEnum = std::is_enum<T>::value;
  static_assert(animatable_detail::has_value_type_traits<T>::value || kIsEnum,
                "Animatable<T>: T must be a registered value type or an enum.");

  // Type actually stored in the type-erased value::TimeSamples: T for value
  // types, int64 for enums (which are not registered value types).
  using StoreT = typename std::conditional<kIsEnum, int64_t, T>::type;

  static StoreT to_store(const T &v) {
    if constexpr (kIsEnum) {
      return static_cast<int64_t>(v);
    } else {
      return v;
    }
  }
  static T from_store(const StoreT &s) {
    if constexpr (kIsEnum) {
      return static_cast<T>(s);
    } else {
      return s;
    }
  }

 public:
  bool is_blocked() const { return _blocked; }

  bool is_timesamples() const {
    if (is_blocked() || _has_value) {
      return false;
    }
    return _ts && !_ts->empty();
  }

  bool is_scalar() const {
    if (is_blocked()) {
      return false;
    }
    return !_ts || _ts->empty();
  }

  ///
  /// Get value at specific time.
  ///
  bool get(double t, T *v,
           const value::TimeSampleInterpolationType tinerp =
               value::TimeSampleInterpolationType::Linear) const {
    if (!v) {
      return false;
    }

    if (is_blocked()) {
      return false;
    }

    if (value::TimeCode(t).is_default()) {
      if (has_value()) {
        (*v) = _value;
        return true;
      }
    }

    if (has_timesamples()) {
      if constexpr (kIsEnum) {
        // Enums are stored as int64 and are non-interpolatable (Held).
        StoreT s{};
        if (_ts->get(&s, t, tinerp)) {
          (*v) = from_store(s);
          return true;
        }
        return false;
      } else {
        return _ts->get(v, t, tinerp);
      }
    }

    if (has_default()) {
      return get_scalar(v);
    }

    return false;
  }

  ///
  /// Get scalar(default) value.
  ///
  bool get_scalar(T *v) const {
    if (!v) {
      return false;
    }

    if (is_blocked()) {
      return false;
    } else if (has_value()) {
      (*v) = _value;
      return true;
    }

    // timesamples
    return false;
  }

  bool get_default(T *v) const {
    return get_scalar(v);
  }

  // TimeSamples
  // void set(double t, const T &v);

  void add_sample(const double t, const T &v) {
    if (!_ts) _ts.reset(new value::TimeSamples());
    _ts->add_sample(t, to_store(v));
  }

  // Add None(ValueBlock) sample to timesamples
  void add_blocked_sample(const double t) {
    if (!_ts) _ts.reset(new value::TimeSamples());
    _ts->template add_blocked_sample<StoreT>(t);
  }

  // Scalar
  void set(const T &v) {
    _value = v;
    _blocked = false;
    _has_value = true;
  }

  // Move overload for scalar - avoids copy for large vectors
  void set(T &&v) {
    _value = std::move(v);
    _blocked = false;
    _has_value = true;
  }

  void set_default(const T &v) {
    set(v);
  }

  void set_default(T &&v) {
    set(std::move(v));
  }

  // --- TimeSamples setters (type-erased value::TimeSamples) ---
  // Used by value-type callers. Enum-valued attributes populate timesamples via
  // add_sample() (the reconstruct path), so they do not use these.
  void set_timesamples(value::TimeSamples &&ts) {
    _ts.reset(new value::TimeSamples(std::move(ts)));
  }

  void set_timesamples(const value::TimeSamples &ts) {
    _ts.reset(new value::TimeSamples(ts));
  }

  void clear_scalar() {
    _has_value = false;
  }

  void clear_timesamples() {
    _ts.reset();
  }

  bool has_value() const {
    return _has_value;
  }

  bool has_default() const {
    return has_value();
  }

  bool has_timesamples() const {
    return _ts && (_ts->size() > 0);
  }

  // Direct access to the type-erased timesamples store. For enum-valued T the
  // samples hold the enum's underlying int64; render them by casting int64 -> T
  // (see EnumTimeSamplesToTypelessTimeSamples and the pprint enum path).
  const value::TimeSamples *get_timesamples_ptr() const
      LIGHTUSD_LIFETIMEBOUND {
    return _ts.get();
  }
  value::TimeSamples *get_timesamples_ptr() LIGHTUSD_LIFETIMEBOUND {
    return _ts.get();
  }

  /// Get const reference to the scalar/default value (no copy).
  /// Only valid when has_default() is true.
  const T &get_scalar_ref() const LIGHTUSD_LIFETIMEBOUND { return _value; }

  Animatable() = default;

  Animatable(const T &v) {
    set(v);
  }

  // Deep copy (the unique_ptr store makes the implicit copy ops deleted).
  Animatable(const Animatable &other)
      : _value(other._value),
        _has_value(other._has_value),
        _blocked(other._blocked) {
    if (other._ts) {
      _ts.reset(new value::TimeSamples(*other._ts));
    }
  }

  Animatable &operator=(const Animatable &other) {
    if (this != &other) {
      _value = other._value;
      _has_value = other._has_value;
      _blocked = other._blocked;
      _ts.reset(other._ts ? new value::TimeSamples(*other._ts) : nullptr);
    }
    return *this;
  }

  Animatable(Animatable &&) noexcept = default;
  Animatable &operator=(Animatable &&) noexcept = default;

 private:
  // scalar
  T _value{};
  bool _has_value{false};
  bool _blocked{false};

  // timesamples (type-erased; enums stored as int64)
  std::unique_ptr<value::TimeSamples> _ts;
};

}  // namespace lightusd
