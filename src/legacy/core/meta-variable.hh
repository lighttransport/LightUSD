// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// meta-variable.hh - MetaVariable class and Dictionary types
//
#pragma once

#include <map>
#include <string>
#include <utility>

#include "nonstd/optional.hpp"
#include "value-types.hh"

namespace lightusd {

//
// variant in pxrUSD is something like a Scene variation(different scene
// composition based on variant) This is not a C++ variant. Variant is expressed
// strings for now
//
using VariantSelectionMap = std::map<std::string, std::string>;

class MetaVariable;

// TODO: Use `Dictionary` and deprecate CustomDataType
using CustomDataType = std::map<std::string, MetaVariable>;

using Dictionary = CustomDataType;  // alias to CustomDataType

///
/// Helper function to access CustomData(dictionary).
/// Recursively process into subdictionaries when a key contains namespaces(':')
///
bool HasCustomDataKey(const Dictionary &customData, const std::string &key);
bool GetCustomDataByKey(const Dictionary &customData, const std::string &key,
                        /* out */ MetaVariable *dst);
bool SetCustomDataByKey(const std::string &key, const MetaVariable &val,
                        /* inout */ Dictionary &customData);

void OverrideDictionary(Dictionary &customData, const Dictionary &src, const bool override_existing = true);

// Variable class for Prim and Attribute Metadataum.
//
// - Accepts limited number of types for value
// - No 'custom' keyword
// - 'None'(Value Block) is supported for some type(at least `references` and
// `payload` accepts None)
// - No TimeSamples, No Connection, No Relationship(`rel`)
// - Value must be assigned(e.g. "float myval = 1.3"). So no definition only
// syntax("float myval")
// - Can be string only(no type information)
//   - Its variable name is interpreted as "comment"
//
class MetaVariable {
 public:
  MetaVariable() = default;
  MetaVariable(const MetaVariable &rhs) = default;
  MetaVariable(MetaVariable &&rhs) noexcept = default;
  MetaVariable &operator=(const MetaVariable &rhs) = default;
  MetaVariable &operator=(MetaVariable &&rhs) noexcept = default;

  template <typename T>
  MetaVariable(const T &v) {
    set_value(v);
  }

  template <typename T>
  MetaVariable(const std::string &name, const T &v) {
    set_value(name, v);
  }

  bool is_valid() const {
    return _value.type_id() != value::TypeTraits<std::nullptr_t>::type_id();
  }

  //
  // custom data must have some value, so no set_type()
  // OK "float myval = 1"
  // NG "float myval"
  //
  template <typename T>
  void set_value(const T &v) {
    // TODO: Check T is supported type for Metadatum.
    _value = v;

    _name = std::string();  // empty
  }

  template <typename T>
  void set_value(const std::string &name, const T &v) {
    // TODO: Check T is supported type for Metadatum.
    _value = v;

    _name = name;
  }

  void set_value(value::Value &&v) {
    _value = std::move(v);
    _name = std::string();
  }

  void set_value(const std::string &name, value::Value &&v) {
    _value = std::move(v);
    _name = name;
  }

  template <typename T>
  bool get_value(T *dst) const {
    if (!dst) {
      return false;
    }

    if (const T *v = _value.as<T>()) {
      (*dst) = *v;
      return true;
    }

    return false;
  }

  template <typename T>
  nonstd::optional<T> get_value() const {
    if (const T *v = _value.as<T>()) {
      return *v;
    }

    return nonstd::nullopt;
  }

  void set_name(const std::string &name) { _name = name; }
  const std::string &get_name() const LIGHTUSD_LIFETIMEBOUND { return _name; }

  const value::Value &get_raw_value() const LIGHTUSD_LIFETIMEBOUND {
    return _value;
  }
  value::Value &get_raw_value() LIGHTUSD_LIFETIMEBOUND { return _value; }

  // No set_type_name()
  const std::string type_name() const { return TypeName(*this); }

  uint32_t type_id() const { return TypeId(*this); }

  bool is_blocked() const {
    return type_id() == value::TypeId::TYPE_ID_VALUEBLOCK;
  }

 private:
  static std::string TypeName(const MetaVariable &v) {
    return v._value.type_name();
  }

  static uint32_t TypeId(const MetaVariable &v) { return v._value.type_id(); }

 private:
  value::Value _value{nullptr};
  std::string _name;
};

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(VariantSelectionMap, "variants", TYPE_ID_VARIANT_SELECION_MAP,
                  0);
DEFINE_TYPE_TRAIT(CustomDataType, "customData", TYPE_ID_CUSTOMDATA,
                  1);  // TODO: Unify with `dict`?

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
