// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// attribute.hh - Attribute class for generic property attributes
//
#pragma once

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

#include "nonstd/optional.hpp"
#include "path.hh"
#include "attr-metas.hh"
#include "prim-enums.hh"
#include "primvar.hh"
#include "value-types.hh"

namespace lightusd {

// Attribute is a struct to hold generic attribute of a property(e.g. primvar)
// of Prim.
// It can have multiple values(default value(or ValueBlock), timeSamples and connection) at once.
//
// TODO: Refactor
class Attribute {

 public:
  Attribute() {
    //LIGHTUSD_LOG_I("Attribute default constructor called");
  }

  // Copy constructor
  Attribute(const Attribute& rhs)
    : _name(rhs._name),
      _variability(rhs._variability),
      _varying_authored(rhs._varying_authored),
      _type_name(rhs._type_name),
      _var(rhs._var),
      _paths(rhs._paths),
      _metas(rhs._metas) {
    //LIGHTUSD_LOG_I("Attribute copy constructor called");
  }

  // Move constructor
  Attribute(Attribute&& rhs) noexcept
    : _name(std::move(rhs._name)),
      _variability(rhs._variability),
      _varying_authored(rhs._varying_authored),
      _type_name(std::move(rhs._type_name)),
      _var(std::move(rhs._var)),
      _paths(std::move(rhs._paths)),
      _metas(std::move(rhs._metas)) {
    //LIGHTUSD_LOG_I("Attribute move constructor called");
    rhs._variability = Variability::Varying;
    rhs._varying_authored = false;
  }

  // Copy assignment operator
  Attribute& operator=(const Attribute& rhs) {
    //LIGHTUSD_LOG_I("Attribute copy assignment operator called");
    if (this != &rhs) {
      _name = rhs._name;
      _variability = rhs._variability;
      _varying_authored = rhs._varying_authored;
      _type_name = rhs._type_name;
      _var = rhs._var;
      _paths = rhs._paths;
      _metas = rhs._metas;
    }
    return *this;
  }

  // Move assignment operator
  Attribute& operator=(Attribute&& rhs) noexcept {
    //LIGHTUSD_LOG_I("Attribute move assignment operator called");
    if (this != &rhs) {
      _name = std::move(rhs._name);
      _variability = rhs._variability;
      _varying_authored = rhs._varying_authored;
      _type_name = std::move(rhs._type_name);
      _var = std::move(rhs._var);
      _paths = std::move(rhs._paths);
      _metas = std::move(rhs._metas);
      rhs._variability = Variability::Varying;
      rhs._varying_authored = false;
    }
    return *this;
  }

  ///
  /// Construct Attribute with typed value(`float`, `token`, ...).
  ///
  template <typename T>
  Attribute(const T &v, bool varying = true) {
    static_assert((value::TypeId::TYPE_ID_VALUE_BEGIN <=
                   value::TypeTraits<T>::type_id()) &&
                      (value::TypeId::TYPE_ID_VALUE_END >
                       value::TypeTraits<T>::type_id()),
                  "T is not a value type");
    set_value(v);
    variability() = varying ? Variability::Varying : Variability::Uniform;
  }

  ///
  /// Construct uniform attribute.
  ///
  template <typename T>
  static Attribute Uniform(const T &v) {

    static_assert((value::TypeId::TYPE_ID_VALUE_BEGIN <=
                   value::TypeTraits<T>::type_id()) &&
                      (value::TypeId::TYPE_ID_VALUE_END >
                       value::TypeTraits<T>::type_id()),
                  "T is not a value type");

    Attribute attr;
    attr.set_value(v);
    attr.variability() = Variability::Uniform;
    return attr;
  }


  ///
  /// Construct connection attribute.
  ///
  Attribute(const Path &v) {
    set_connection(v);
  }

  Attribute(const std::vector<Path> &vs) {
    set_connections(vs);
  }

  const std::string &name() const LIGHTUSD_LIFETIMEBOUND { return _name; }

  std::string &name() LIGHTUSD_LIFETIMEBOUND { return _name; }

  void set_name(const std::string &name) { _name = name; }

  void set_type_name(const std::string &tname) { _type_name = tname; }

  // `var` may be empty or ValueBlock, so store type info with set_type_name and
  // set_type_id.
  std::string type_name() const {
    if (_type_name.size()) {
      return _type_name;
    }

    if (!is_connection()) {
      // Fallback. May be unreliable(`var` could be empty).
      return _var.type_name();
    }

    return std::string();
  }

  uint32_t type_id() const {
    if (_type_name.size()) {
      return value::GetTypeId(_type_name);
    }

    if (!is_connection()) {
      // Fallback. May be unreliable(`var` could be empty).
      return _var.type_id();
    }

    return value::TYPE_ID_INVALID;
  }

  template <typename T>
  void set_value(const T &v) {
    if (_type_name.empty()) {
      _type_name = value::TypeTraits<T>::type_name();
    }
    _var.set_value(v);
  }

  template <typename T>
  void set_value(T &&v) {
    if (_type_name.empty()) {
      _type_name = value::TypeTraits<typename std::remove_reference<T>::type>::type_name();
    }
    _var.set_value(std::forward<T>(v));
  }

  void set_var(primvar::PrimVar &v) {
    if (_type_name.empty()) {
      _type_name = v.type_name();
    }

    _var = v;
  }

  void set_var(primvar::PrimVar &&v) {
    if (_type_name.empty()) {
      _type_name = v.type_name();
    }

    _var = std::move(v);
  }

  bool is_value() const {
    if (is_connection()) {
      return false;
    }

    if (is_timesamples()) {
      return false;
    }

    if (is_blocked()) {
      return false;
    }

    return true;
  }

  // check if Attribute has default value
  bool has_value() const {
    return _var.has_value();
  }

  /// @brief Get the value of Attribute of specified type.
  /// @tparam T value type
  /// @return The value if the underlying PrimVar is type T. Return
  /// nonstd::nullpt when type mismatch.
  template <typename T>
  nonstd::optional<T> get_value() const {
    return _var.get_value<T>();
  }

  template <typename T>
  bool get_value(T *v) const {
    if (!v) {
      return false;
    }

    nonstd::optional<T> ret = _var.get_value<T>();
    if (ret) {
      (*v) = std::move(ret.value());
      return true;
    }

    return false;
  }

  template <typename T>
  void set_timesample(const T &v, double t) {
    _var.set_timesample(t, v);
  }

  template <typename T>
  bool get(const double t, T *dst,
           value::TimeSampleInterpolationType tinterp =
           value::TimeSampleInterpolationType::Linear) const {
    if (!dst) {
      return false;
    }

    if (value::TimeCode(t).is_default()) {
      if (has_value()) {
        nonstd::optional<T> v = _var.get_value<T>();
        if (v) {
          (*dst) = v.value();
          return true;
        }
      }
    }

    if (has_timesamples()) {
      return _var.get_interpolated_value(t, tinterp, dst);
    }

    // try to get 'defaut' value
    return get_value(dst);
  }

  // TODO: Deprecate 'get_value' API
  template <typename T>
  bool get_value(const double t, T *dst,
                 value::TimeSampleInterpolationType tinterp =
                     value::TimeSampleInterpolationType::Linear) const {
    return get(t, dst, tinterp);
  }

  /// @brief Get TypedArrayView to the underlying array data of this Attribute.
  ///
  /// Returns a zero-copy view over array data for scalar (default) values only.
  /// This method does NOT support timesamples - only works with default values.
  /// For non-array types or timesamples, returns an empty view.
  template <typename T>
  TypedArrayView<const T> get_value_view(bool strict_cast = false) const {
    // Only support scalar (default) values, not timesamples
    if (has_timesamples()) {
      return TypedArrayView<const T>();  // Empty view for timesamples
    }

    if (is_blocked()) {
      return TypedArrayView<const T>();  // Empty view for blocked attributes
    }

    if (is_connection()) {
      return TypedArrayView<const T>();  // Empty view for connections
    }

    if (!has_value()) {
      return TypedArrayView<const T>();  // Empty view if no value
    }

    // Get the underlying value and create a view using Value::as_view()
    const primvar::PrimVar& pvar = get_var();
    const value::Value& val = pvar.value_raw();

    return val.as_view<T>(strict_cast);
  }

  /// @brief Mutable version of get_value_view() for write access to array data.
  template <typename T>
  TypedArrayView<T> get_value_view(bool strict_cast = false) {
    // Only support scalar (default) values, not timesamples
    if (has_timesamples()) {
      return TypedArrayView<T>();  // Empty view for timesamples
    }

    if (is_blocked()) {
      return TypedArrayView<T>();  // Empty view for blocked attributes
    }

    if (is_connection()) {
      return TypedArrayView<T>();  // Empty view for connections
    }

    if (!has_value()) {
      return TypedArrayView<T>();  // Empty view if no value
    }

    // Get the underlying value and create a view using Value::as_view()
    primvar::PrimVar& pvar = get_var();
    value::Value& val = pvar.value_raw();

    return val.as_view<T>(strict_cast);
  }


  const AttrMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return _metas; }
  AttrMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _metas; }

  const primvar::PrimVar &get_var() const LIGHTUSD_LIFETIMEBOUND {
    return _var;
  }
  primvar::PrimVar &get_var() LIGHTUSD_LIFETIMEBOUND { return _var; }

  void set_blocked(bool onoff) { _var.set_blocked(onoff); }

  bool is_blocked() const {
    if (has_timesamples()) {
      return false;
    }

    return _var.is_blocked();
  }
  bool has_blocked() const { return _var.is_blocked(); }

  Variability &variability() LIGHTUSD_LIFETIMEBOUND { return _variability; }
  Variability variability() const { return _variability; }

  bool is_uniform() const { return _variability == Variability::Uniform; }

  void set_varying_authored() { _varying_authored = true; }

  bool is_varying_authored() const { return _varying_authored; }

  bool is_connection() const {
    if (has_timesamples()) {
      return false;
    }

    if (has_blocked()) {
      return false;
    }

    if (has_value()) {
      return false;
    }

    return _paths.size() > 0;
  }

  bool has_connections() const {
    return _paths.size() > 0;
  }


  bool has_default() const {
    return has_value();
  }

  bool is_timesamples() const {
    if (has_default()) {
      return false;
    }

    if (has_connections()) {
      return false;
    }

    return !_var.has_value() && _var.has_timesamples();
  }

  bool has_timesamples() const {
    return _var.has_timesamples();
  }

  void set_connection(const Path &path) {
    _paths.clear();
    _paths.push_back(path);
  }
  void set_connections(const std::vector<Path> &paths) { _paths = paths; }

  nonstd::optional<Path> get_connection() const {
    if (_paths.size() == 1) {
      return _paths[0];
    }
    return nonstd::nullopt;
  }

  const std::vector<Path> &connections() const LIGHTUSD_LIFETIMEBOUND {
    return _paths;
  }
  std::vector<Path> &connections() LIGHTUSD_LIFETIMEBOUND { return _paths; }

  ///
  /// Estimate memory usage of this Attribute in bytes
  ///
  size_t estimate_memory_usage() const;

  /// Estimate actual (size-based) memory usage.
  size_t estimate_actual_usage() const;

 private:
  std::string _name;  // attrib name
  Variability _variability{
      Variability::Varying};  // 'uniform` qualifier is handled with
                              // `variability=uniform`

  // `varying` keyword is explicitly specified?
  bool _varying_authored{false};

  // bool _blocked{false};       // Attribute Block('None')
  std::string _type_name;
  primvar::PrimVar _var;
  std::vector<Path> _paths;
  AttrMeta _metas;
};

}  // namespace lightusd
