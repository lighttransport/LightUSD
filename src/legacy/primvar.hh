// SPDX-License-Identifier: Apache 2.0
// Copyright 2021-2022 Syoyo Fujita.
// Copyright 2023-present Light Transport Entertainment Inc.

///
/// Type-erasure technique for Attribute/PrimVar(Primitive Variables), a Value class which can have 30+ different types(and can be compound-types(e.g. 1D/2D array, dictionary).
/// Neigher std::any nor std::variant is applicable for such usecases, so write our own, handy typesystem.
///
/// TODO: Rename PrimVar to something more better one(AttributeValue?).
///
#pragma once

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>
#include <cmath>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// TODO(syoyo): Use C++17 std::optional, std::string_view when compiled with C++-17 compiler
#include "nonstd/optional.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "value-types.hh"
#include "timesamples.hh"

namespace lightusd {
namespace primvar {

struct PrimVar {
  // Diet layout: most attributes author only a default value, so the
  // timeSamples and spline storage (~220 bytes combined) live in a lazily
  // allocated extension block instead of inline — sizeof(PrimVar) drops from
  // 280 to ~72, which dominates Attribute/Property size on large scenes
  // (hundreds of thousands of properties per flattened UE/film layer).
  value::Value _value{nullptr}; // For scalar(default) value
  bool _blocked{false}; // ValueBlocked.

  // Default constructor
  PrimVar() {
    //LIGHTUSD_LOG_I("PrimVar default ctor");
  }

  // Copy constructor
  PrimVar(const PrimVar& rhs)
    : _value(rhs._value), _blocked(rhs._blocked),
      _extras(rhs.clone_extras()) {
    //LIGHTUSD_LOG_I("PrimVar copy ctor");
  }

  // Move constructor
  PrimVar(PrimVar&& rhs) noexcept
    : _value(std::move(rhs._value)),
      _blocked(rhs._blocked),
      _extras(std::move(rhs._extras)) {
    //LIGHTUSD_LOG_I("PrimVar move ctor");
    rhs._blocked = false;
  }

  // Copy assignment operator
  PrimVar& operator=(const PrimVar& rhs) {
    //LIGHTUSD_LOG_I("PrimVar copy assignment op");
    if (this != &rhs) {
      _value = rhs._value;
      _blocked = rhs._blocked;
      _extras = rhs.clone_extras();
    }
    return *this;
  }

  // Move assignment operator
  PrimVar& operator=(PrimVar&& rhs) noexcept {
    //LIGHTUSD_LOG_I("PrimVar move assignment op");
    if (this != &rhs) {
      _value = std::move(rhs._value);
      _blocked = rhs._blocked;
      _extras = std::move(rhs._extras);
      rhs._blocked = false;
    }
    return *this;
  }

  template<typename T>
  PrimVar(T &&v) noexcept : _value(std::move(v)) {
    //LIGHTUSD_LOG_I("PrimVar templated ctor");
  }

  bool has_value() const {
    // ValueBlock is treated as having a value.
    if (_blocked) {
      return true;
    }
    bool ret = (_value.type_id() != value::TypeId::TYPE_ID_INVALID) && (_value.type_id() != value::TypeId::TYPE_ID_NULL);
    //LIGHTUSD_LOG_I("has_value " << ret);
    return ret;
  }

  bool has_default() const {
    return has_value();
  }

  bool has_timesamples() const {
    // `.authored()` is size() > 0 OR an explicitly-authored EMPTY block.
    return _extras && _extras->ts.authored();
  }

  bool is_blocked() const {
    // Fist check if stored value is ValueBlock, then return _blocked.
    if (_value.type_id() == value::TYPE_ID_VALUEBLOCK) {
      return true;
    }
    return _blocked;
  }

  void set_blocked(bool onoff) {
    // fast path
    _blocked = onoff;
  }

  std::string type_name() const {
    if (has_default()) {
      return _value.type_name();
    }

    // Check if timeSamples were authored (even if empty)
    if (_extras && (has_timesamples() || _extras->ts.type_id() != 0)) {
      return _extras->ts.type_name();
    }

    return "[[InvalidType]]";
  }

  uint32_t type_id() const {
    if (!has_value() && !has_timesamples()) {
      return value::TYPE_ID_INVALID;
    }

    if (has_default()) {
      return _value.type_id();
    }

    // Check if timeSamples were authored (even if empty)
    if (_extras && (has_timesamples() || _extras->ts.type_id() != 0)) {
      return _extras->ts.type_id();
    }

    return value::TypeId::TYPE_ID_INVALID;

  }

  // Type-safe way to get concrete value of default value(non-timesamples value).
  // NOTE: This consumes lots of stack size(rougly 1000 bytes),
  // If you need to handle multiple types, use as() insted.
  //
  template <class T>
  nonstd::optional<T> get_value() const {

    if (is_blocked()) {
      return nonstd::nullopt;
    }

    if (!has_default()) {
      return nonstd::nullopt;
    }

    return _value.get_value<T>();
  }

  // For Scalar only
  // Returns nullptr when type-mismatch.
  template <class T>
  const T* as() const {

    if (!has_default()) {
      return nullptr;
    }

    return _value.as<T>();
  }

  // Const ref version for lvalue arguments
  template <class T>
  void set_value(const T &v) {
    //LIGHTUSD_LOG_I("set_value const_ref");
    _value = v;
  }

  // Move version for rvalue arguments - avoids copy when caller passes temporary
  template <class T>
  void set_value(T &&v, typename std::enable_if<!std::is_lvalue_reference<T>::value && !std::is_same<typename std::decay<T>::type, value::Value>::value>::type* = nullptr) {
    //LIGHTUSD_LOG_I("set_value move");

    // Create Value object first, then call move ctor.
    value::Value src(std::move(v));
    _value = std::move(src);
  }

  // Special overload for value::Value to avoid double-wrapping
  void set_value(value::Value &&v) {
    //LIGHTUSD_LOG_I("set_value Value move");
    _value = std::move(v);
  }

  void set_value(const value::Value &v) {
    //LIGHTUSD_LOG_I("set_value Value copy");
    _value = v;
  }

  void clear_value() {
    _value = nullptr;
  }

  void set_timesamples(const value::TimeSamples &v) {
    extras().ts = v;
  }

  void set_timesamples(value::TimeSamples &&v) {
    extras().ts = std::move(v);
  }

  void clear_timesamples() {
    if (_extras) {
      _extras->ts.clear();
    }
  }

  template <typename T>
  void set_timesample(double t, const T &v) {
    extras().ts.add_sample(t, v);
  }

  void set_timesample(double t, value::Value &v) {
    extras().ts.add_sample(t, v);
  }




  ///
  /// Get interpolated timesample value.
  /// When input time is Default(qnan), return 'default' value if exists, otherwise return the first item of timesamples.
  ///
  bool get_interpolated_value(const double t, const value::TimeSampleInterpolationType tinterp, value::Value *v) const;


  template <typename T>
  bool get_interpolated_value(const double t, const value::TimeSampleInterpolationType tinterp, T *v) const {
    if (!v) {
      return false;
    }

    if (value::TimeCode(t).is_default()) {

      if (auto pv = get_value<T>()) {
        (*v) = pv.value();
        return true;
      }

      if (!has_timesamples()) {
        return false;
      }
    }

    if (has_timesamples()) {
      return _extras->ts.get(v, t, tinterp);
    }

    if (has_default()) {
      if (auto pv = get_value<T>()) {
        (*v) = pv.value();
        return true;
      }
    }

    return false;
  }

  size_t num_timesamples() const {
    if (has_timesamples()) {
      return _extras->ts.size();
    }
    return 0;
  }

  const value::TimeSamples &ts_raw() const LIGHTUSD_LIFETIMEBOUND {
    if (_extras) {
      return _extras->ts;
    }
    static const value::TimeSamples s_empty_ts;
    return s_empty_ts;
  }

  value::Value &value_raw() LIGHTUSD_LIFETIMEBOUND {
    return _value;
  }

  const value::Value &value_raw() const LIGHTUSD_LIFETIMEBOUND {
    return _value;
  }

  // Mutable access lazily allocates the extension block.
  value::TimeSamples &ts_raw() LIGHTUSD_LIFETIMEBOUND {
    return extras().ts;
  }

  size_t estimate_memory_usage() const {
    size_t total = sizeof(PrimVar);
    total += _value.estimate_memory_usage();
    if (_extras) {
      total += _extras->ts.estimate_memory_usage();
    }
    return total;
  }

  size_t estimate_actual_usage() const {
    size_t total = sizeof(PrimVar);
    total += _value.estimate_actual_usage();
    if (_extras) {
      total += _extras->ts.estimate_actual_usage();
    }
    return total;
  }

  //
  // AOUSD Core Spec 12.3.3 / 7.4.2.4: Spline data storage.
  // Splines are stored as a separate value source alongside default and timeSamples.
  // Priority: timeSamples > spline > default (Spec 12.3)
  //

  /// Spline knot data stored as type-erased values for the spline evaluator.
  /// Each entry: (time, value, preTangentSlope, preTangentWidth,
  ///              postTangentSlope, postTangentWidth, interpolationMode)
  struct SplineKnotData {
    double time{0.0};
    value::Value val;        // knot value
    value::Value preValue;   // dual-valued: value approaching from before
    bool hasDualValue{false};
    double preTangentSlope{0.0};
    double preTangentWidth{0.0};
    double postTangentSlope{0.0};
    double postTangentWidth{0.0};
    int interpolationMode{3};  // 0=none, 1=held, 2=linear, 3=curve
    // Tangent algorithm (OpenUSD TsTangentAlgorithm): 0=None, 1=Custom,
    // 2=AutoEase. A non-None algorithm requires spline binary version 2, which
    // in turn requires crate version 0.13.0 on write.
    int preTangentAlgorithm{0};
    int postTangentAlgorithm{0};
  };

  struct SplineData {
    int curveType{0};  // 0=bezier, 1=hermite
    std::vector<SplineKnotData> knots;
    int preExtrapolation{1};   // SplineExtrapolationMode::Held
    int postExtrapolation{1};  // SplineExtrapolationMode::Held
    double preExtrapolationSlope{0.0};
    double postExtrapolationSlope{0.0};

    // Inner-loop parameters (AOUSD Core Spec 7.4.2.4.5). Active when hasLoop.
    bool hasLoop{false};
    double loopProtoStart{0.0};
    double loopProtoEnd{0.0};
    int loopNumPreLoops{0};
    int loopNumPostLoops{0};
    double loopValueOffset{0.0};
  };

  bool has_spline() const { return _extras && !_extras->spline.knots.empty(); }

  const SplineData &spline_data() const LIGHTUSD_LIFETIMEBOUND {
    if (_extras) {
      return _extras->spline;
    }
    static const SplineData s_empty_spline;
    return s_empty_spline;
  }
  // Mutable access lazily allocates the extension block.
  SplineData &spline_data() LIGHTUSD_LIFETIMEBOUND { return extras().spline; }

  void set_spline(const SplineData &spline) { extras().spline = spline; }
  void set_spline(SplineData &&spline) { extras().spline = std::move(spline); }

  // Rarely-authored value sources, stored out-of-line (see the class comment).
  struct ExtrasBlock {
    value::TimeSamples ts;
    SplineData spline;
  };

 private:
  ExtrasBlock &extras() LIGHTUSD_LIFETIMEBOUND {
    if (!_extras) {
      _extras.reset(new ExtrasBlock());
    }
    return *_extras;
  }

  std::unique_ptr<ExtrasBlock> clone_extras() const {
    return _extras ? std::unique_ptr<ExtrasBlock>(new ExtrasBlock(*_extras))
                   : nullptr;
  }

  std::unique_ptr<ExtrasBlock> _extras;

 public:
};


} // namespace primvar

namespace value {

// SplineData carrier so a spline can be stored in a value::Value (used to flow
// a Crate type-59 value through to the attribute, like value::TimeSamples).
// Defined here because it needs the complete primvar::PrimVar::SplineData type.
#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(primvar::PrimVar::SplineData, "Spline", TYPE_ID_SPLINE_DATA, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

} // namespace value
} // namespace lightusd
