// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// typed-attribute.hh - TypedAttribute templates
//
#pragma once

#include <string>
#include <vector>

#include "nonstd/optional.hpp"
#include "path.hh"
#include "attr-metas.hh"
#include "prim-enums.hh"
#include "animatable.hh"
#include "value-types.hh"

namespace lightusd {

///
/// Tyeped Attribute without fallback(default) value.
/// For attribute with `uniform` qualifier or TimeSamples, or have
/// `.connect`(Connection)
///
/// To support multiple definition of attribute(up to 2), we support both having
/// Connection and values.
///
/// e.g.  float var = 1.0
///       float var.connect = </path/to/value>
///       (metadata is shared)
///
/// - `authored() = true` : Attribute value is authored(attribute is
/// described in USDA/USDC)
/// - `authored() = false` : Attribute value is not authored(not described
/// in USD). If you call `get()`, fallback value is returned.
///
template <typename T>
class TypedAttribute {
 public:
  static std::string type_name() { return value::TypeTraits<T>::type_name(); }

  static uint32_t type_id() { return value::TypeTraits<T>::type_id(); }

  TypedAttribute() = default;

  TypedAttribute &operator=(const T &value) {
    _attrib = value;

    return (*this);
  }

  // Move overload for operator= - avoids copy for large vectors
  TypedAttribute &operator=(T &&value) {
    _attrib = std::move(value);

    return (*this);
  }

  // 'default' value or timeSampled value(when T = Animatable)
  void set_value(const T &v) { _attrib = v; }

  // Move overload for set_value - avoids copy for large vectors
  void set_value(T &&v) { _attrib = std::move(v); }

  bool has_value() const { return _attrib.has_value(); }

  const nonstd::optional<T> get_value() const {
    return _attrib;
  }

  /// Get const reference to the internal optional value (no copy)
  const nonstd::optional<T> &get_value_ref() const LIGHTUSD_LIFETIMEBOUND {
    return _attrib;
  }

  bool get_value(T *dst) const {
    if (!dst) return false;

    if (_attrib) {
      (*dst) = _attrib.value();
      return true;
    }
    return false;
  }

  bool is_blocked() const { return _blocked; }

  // for `uniform` attribute only
  void set_blocked(bool onoff) { _blocked = onoff; }

  bool is_connection() const { return _paths.size() && !has_value(); }

  void set_connection(const Path &path) {
    _paths.clear();
    _paths.push_back(path);
  }

  void set_connections(const std::vector<Path> &paths) { _paths = paths; }

  const std::vector<Path> &get_connections() const LIGHTUSD_LIFETIMEBOUND {
    return _paths;
  }
  const std::vector<Path> &connections() const LIGHTUSD_LIFETIMEBOUND {
    return _paths;
  }

  const nonstd::optional<Path> get_connection() const {
    if (_paths.size()) {
      return _paths[0];
    }

    return nonstd::nullopt;
  }

  bool has_connections() const {
    return _paths.size();
  }

  void clear_connections() {
    _paths.clear();
  }

  // TODO: Supply set_connection_empty()?

  void set_value_empty() { _value_empty = true; }

  //
  // Check if the attribute is authored, but no value(including ValueBlock) assigned.
  // e.g.
  //
  // float myval;
  //
  bool is_value_empty() const {
    if (has_connections()) {
      return false;
    }

    if (_attrib.has_value()) {
      return false;
    }

    if (_blocked) {
      return false;
    }

    return _value_empty;
  }

  // The attribute authroed?
  bool authored() const {
    if (_attrib) {
      return true;
    }

    if (has_connections()) {
      return true;
    }

    if (_value_empty) {
      // Declare only.
      return true;
    }

    if (_blocked) {
      return true;
    }

    return false;
  }

  void clear_value() {
    _attrib.reset();
    _value_empty = true;
  }

  const AttrMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return _metas; }
  AttrMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _metas; }


  // The typeName as actually authored in USDA/USDC when it differs from the
  // schema's spelling -- e.g. `token inputs:varname` (older UsdPrimvarReader
  // spec) parsed into a string-typed field, or `float3 inputs:diffuseColor`
  // parsed into a color3f field. OpenUSD preserves the authored typeName on
  // the Sdf spec; without this the printers/writers re-declare the attribute
  // with the schema type, silently changing what the author wrote.
  void set_actual_type_name(const std::string &type_name) {
    _actual_type_name = type_name;
  }
  bool has_actual_type() const { return _actual_type_name.size(); }
  const std::string &get_actual_type_name() const LIGHTUSD_LIFETIMEBOUND {
    return _actual_type_name;
  }


  // Authored `uniform` qualifier. The typed wrappers model a schema property's
  // VALUE; variability is authored data that pxr stores per spec, so keep it
  // here for the writer (a `uniform` opinion silently becoming `varying` makes
  // an intentionally constant attribute animatable, and pxr prints the
  // difference). `has_variability()` is false when the layer said nothing, so
  // the writer can fall back to the schema default instead of inventing one.
  void set_variability(Variability v) {
    _variability = v;
    _variability_authored = true;
  }
  Variability variability() const { return _variability; }
  bool has_variability() const { return _variability_authored; }

 private:
  Variability _variability{Variability::Varying};
  bool _variability_authored{false};
  AttrMeta _metas;
  bool _value_empty{false};  // applies `_attrib`
  std::vector<Path> _paths;
  nonstd::optional<T> _attrib;
  bool _blocked{false};
  std::string _actual_type_name;
};

///
/// Tyeped Terminal(Output) Attribute(No value assign, no fallback(default)
/// value, no connection)
///
/// - `authored() = true` : Attribute value is authored(attribute is
/// described in USDA/USDC)
/// - `authored() = false` : Attribute value is not authored(not described
/// in USD).
///
template <typename T>
class TypedTerminalAttribute {
 public:
  void set_authored(bool onoff) { _authored = onoff; }

  // value set?
  bool authored() const { return _authored; }

  static std::string type_name() { return value::TypeTraits<T>::type_name(); }
  static uint32_t type_id() { return value::TypeTraits<T>::type_id(); }

  // Actual type is a typeName in USDA or USDC
  // for example, we accect float3 type for TypedTerminalAttribute<color3f> and
  // print/serialize this attribute value with actual type.
  //
  void set_actual_type_name(const std::string &type_name) {
    _actual_type_name = type_name;
  }

  bool has_actual_type() const { return _actual_type_name.size(); }

  const std::string &get_actual_type_name() const LIGHTUSD_LIFETIMEBOUND {
    return _actual_type_name;
  }

  const AttrMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return _metas; }
  AttrMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _metas; }


  // Authored `uniform` qualifier. The typed wrappers model a schema property's
  // VALUE; variability is authored data that pxr stores per spec, so keep it
  // here for the writer (a `uniform` opinion silently becoming `varying` makes
  // an intentionally constant attribute animatable, and pxr prints the
  // difference). `has_variability()` is false when the layer said nothing, so
  // the writer can fall back to the schema default instead of inventing one.
  void set_variability(Variability v) {
    _variability = v;
    _variability_authored = true;
  }
  Variability variability() const { return _variability; }
  bool has_variability() const { return _variability_authored; }

 private:
  Variability _variability{Variability::Varying};
  bool _variability_authored{false};
  AttrMeta _metas;
  bool _authored{false};
  std::string _actual_type_name;
};

template <typename T>
class TypedAttributeWithFallback;

///
/// Attribute with fallback(default) value.
/// For attribute with `uniform` qualifier or TimeSamples, but don't have
/// `.connect`(Connection)
///
/// - `authored() = true` : Attribute value is authored(attribute is
/// described in USDA/USDC)
/// - `authored() = false` : Attribute value is not authored(not described
/// in USD). If you call `get()`, fallback value is returned.
///
template <typename T>
class TypedAttributeWithFallback {
 public:
  static std::string type_name() { return value::TypeTraits<T>::type_name(); }
  static uint32_t type_id() { return value::TypeTraits<T>::type_id(); }

  TypedAttributeWithFallback() = delete;

  ///
  /// Init with fallback value;
  ///
  TypedAttributeWithFallback(const T &fallback) : _fallback(fallback) {}

  TypedAttributeWithFallback &operator=(const T &value) {
    _attrib = value;

    // fallback Value should be already set with `AttribWithFallback(const T&
    // fallback)` constructor.

    return (*this);
  }

  void set_value(const T &v) { _attrib = v; }

  // Move overload for set_value - avoids copy for large vectors
  void set_value(T &&v) { _attrib = std::move(v); }

  void set_value_empty() { _empty = true; }

  bool has_connections() const { return _paths.size(); }

  //
  // Check if the attribute is authored, but no value(including ValueBlock) assigned.
  // e.g.
  //
  // float myval;
  // float myval.connect = </path>  (connection-only, no default value)
  //
  bool is_value_empty() const {
    // Check _empty first - this is set for connection-only attributes
    // and for definition-only attributes
    if (_empty) {
      return true;
    }

    if (_attrib) {
      return false;
    }

    // No explicit value authored
    return true;
  }

  bool has_value() const {
    if (_empty) {
      return false;
    }

    return true;
  }

  const T &get_value() const LIGHTUSD_LIFETIMEBOUND {
    if (_attrib) {
      return _attrib.value();
    }
    return _fallback;
  }

  bool is_blocked() const { return _blocked; }

  // for `uniform` attribute only
  void set_blocked(bool onoff) { _blocked = onoff; }

  bool is_connection() const { return _paths.size() && !has_value() ; }

  void set_connection(const Path &path) {
    _paths.clear();
    _paths.push_back(path);
  }

  void set_connections(const std::vector<Path> &paths) { _paths = paths; }

  const std::vector<Path> &get_connections() const LIGHTUSD_LIFETIMEBOUND {
    return _paths;
  }
  const std::vector<Path> &connections() const LIGHTUSD_LIFETIMEBOUND {
    return _paths;
  }

  const nonstd::optional<Path> get_connection() const {
    if (_paths.size()) {
      return _paths[0];
    }

    return nonstd::nullopt;
  }

  void clear_connections() { _paths.clear(); }

  // value set?
  bool authored() const {
    if (_empty) {  // authored with empty value.
      return true;
    }
    if (_attrib) {
      return true;
    }
    if (_paths.size()) {
      return true;
    }
    if (_blocked) {
      return true;
    }
    return false;
  }

  const AttrMeta &metas() const LIGHTUSD_LIFETIMEBOUND { return _metas; }
  AttrMeta &metas() LIGHTUSD_LIFETIMEBOUND { return _metas; }


  // The typeName as actually authored in USDA/USDC when it differs from the
  // schema's spelling -- e.g. `token inputs:varname` (older UsdPrimvarReader
  // spec) parsed into a string-typed field, or `float3 inputs:diffuseColor`
  // parsed into a color3f field. OpenUSD preserves the authored typeName on
  // the Sdf spec; without this the printers/writers re-declare the attribute
  // with the schema type, silently changing what the author wrote.
  void set_actual_type_name(const std::string &type_name) {
    _actual_type_name = type_name;
  }
  bool has_actual_type() const { return _actual_type_name.size(); }
  const std::string &get_actual_type_name() const LIGHTUSD_LIFETIMEBOUND {
    return _actual_type_name;
  }


  // Authored `uniform` qualifier. The typed wrappers model a schema property's
  // VALUE; variability is authored data that pxr stores per spec, so keep it
  // here for the writer (a `uniform` opinion silently becoming `varying` makes
  // an intentionally constant attribute animatable, and pxr prints the
  // difference). `has_variability()` is false when the layer said nothing, so
  // the writer can fall back to the schema default instead of inventing one.
  void set_variability(Variability v) {
    _variability = v;
    _variability_authored = true;
  }
  Variability variability() const { return _variability; }
  bool has_variability() const { return _variability_authored; }

 private:
  Variability _variability{Variability::Varying};
  bool _variability_authored{false};
  AttrMeta _metas;
  std::vector<Path> _paths;
  nonstd::optional<T> _attrib;
  bool _empty{false};
  T _fallback;
  bool _blocked{false};  // for `uniform` attribute.
  std::string _actual_type_name;
};

template <typename T>
using TypedAnimatableAttributeWithFallback =
    TypedAttributeWithFallback<Animatable<T>>;

bool ConvertTokenAttributeToStringAttribute(
      const TypedAttribute<Animatable<value::token>> &inp,
      TypedAttribute<Animatable<std::string>> &out);

}  // namespace lightusd
