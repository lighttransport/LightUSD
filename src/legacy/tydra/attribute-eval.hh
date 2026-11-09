// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment, Inc.

///
/// @file attribute-eval.hh
/// @brief High-level USD attribute evaluation and resolution API
///
/// Provides convenient utilities for evaluating USD attributes with proper
/// handling of connections, time samples, and value blocking. This API
/// resolves attribute values to their final "terminal" values by following
/// connections and computing time-sampled values.
///
/// Key classes:
/// - TerminalAttributeValue: Resolved final attribute value
/// - AttributeEvaluator: Main evaluation engine
///
/// Features:
/// - Connection resolution (follows input/output connections)
/// - Time sample interpolation
/// - Value blocking detection (None values)
/// - Type-safe attribute access
/// - Fallback value handling
///
/// Usage:
/// ```cpp
/// lightusd::tydra::TerminalAttributeValue result;
/// if (EvaluateAttribute(stage, prim_path, "points", 1.0, &result)) {
///   if (result.has_value()) {
///     // Use resolved value...
///   }
/// }
/// ```
///
/// TODO:
/// - [ ] Reduce template code to speed up compilation
///
#pragma once

#include <map>

#include "core/prim.hh"
#include "stage.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdSkel.hh"
#include "usdLux.hh"
#include "value-types.hh"
#include "value-type-macros.inc"
#include "prim-type-macros.inc"
#include "tiny-format.hh"

namespace lightusd {
namespace tydra {

///
/// Final resolved USD attribute value at a specific time.
///
/// This class represents the "terminal" value of a USD attribute after
/// all connections have been followed, time samples interpolated, and
/// value blocking resolved. It contains the final concrete value that
/// would be used for rendering or processing.
///
/// Properties:
/// - No `None` (value blocked) - blocked values are detected but not stored
/// - No connections - connection targets are followed and resolved
/// - No time samples - time-sampled values are interpolated to final value
/// - Type-safe value access with proper USD type information
///
/// This follows pxrUSD's "value producing attribute" concept where the
/// final resolved value is what matters for downstream consumers.
///
class TerminalAttributeValue {
 public:
  TerminalAttributeValue() = default;

  TerminalAttributeValue(const value::Value &v) : _empty{false}, _value(v) {}
  TerminalAttributeValue(value::Value &&v)
      : _empty{false}, _value(std::move(v)) {}

  // "empty" attribute(type info only)
  void set_empty_attribute(const std::string &type_name) {
    _empty = true;
    _type_name = type_name;
  }

  TerminalAttributeValue(const std::string &type_name) {
    set_empty_attribute(type_name);
  }

  bool is_empty() const { return _empty; }

  template <typename T>
  const T *as() const {
    if (_empty) {
      return nullptr;
    }
    return _value.as<T>();
  }

  template <typename T>
  bool is() const {
    if (_empty) {
      return false;
    }

    if (_value.as<T>()) {
      return true;
    }
    return false;
  }

  void set_value(const value::Value &v) {
    _value = v;
    _empty = false;
  }

  void set_value(value::Value &&v) {
    _value = std::move(v);
    _empty = false;
  }

  const std::string type_name() const {
    if (_empty) {
      return _type_name;
    }

    return _value.type_name();
  }

  uint32_t type_id() const {
    if (_empty) {
      return value::GetTypeId(_type_name);
    }

    return _value.type_id();
  }

  Variability variability() const { return _variability; }
  Variability &variability() { return _variability; }

  const AttrMeta &meta() const { return _meta; }
  AttrMeta &meta() { return _meta; }

 private:
  bool _empty{true};
  std::string _type_name;
  Variability _variability{Variability::Varying};
  value::Value _value{nullptr};
  AttrMeta _meta;
};

namespace detail {

inline void AppendAttributeEvalError(const std::string &msg,
                                     std::string *err) {
  if (err) {
    (*err) += msg;
  }
}

inline bool ResolveSingleConnectionTargetPath(const std::vector<Path> &paths,
                                              const std::string &attr_name,
                                              Path *target_path,
                                              std::string *err) {
  if (!target_path) {
    AppendAttributeEvalError(
        "[InternalError] ResolveSingleConnectionTargetPath requires a valid "
        "target_path.\n",
        err);
    return false;
  }

  if (paths.empty()) {
    AppendAttributeEvalError(
        fmt::format("Connection targetPath is empty for Attribute {}.",
                    attr_name),
        err);
    return false;
  }

  if (paths.size() != 1) {
    AppendAttributeEvalError(
        fmt::format(
            "Multiple targetPaths assigned to .connection for Attribute {} "
            "({} targets).",
            attr_name, paths.size()),
        err);
    return false;
  }

  *target_path = paths.front();
  return true;
}

}  // namespace detail

///
/// Evaluate Attribute of the specied Prim and retrieve terminal Attribute
/// value.
///
/// - If the attribute is empty(e.g. `float outputs:r`), return "empty"
/// Attribute
/// - If the attribute is scalar value, simply returns it.
/// - If the attribute is timeSamples value, evaluate the value at specified
/// time.
/// - If the attribute is connection, follow the connection target
///
/// @param[in] stage Stage
/// @param[in] prim Prim
/// @param[in] attr_name Attribute name
/// @param[out] value Evaluated terminal attribute value.
/// @param[out] err Error message(filled when false returned). Set nullptr if you don't need error message.
/// @param[in] t (optional) TimeCode(for timeSamples Attribute)
/// @param[in] tinterp (optional) Interpolation type for timeSamples value
///
/// Return false when:
///
/// - If the attribute is None(ValueBlock)
/// - Requested attribute not found in a Prim.
/// - Invalid connection(e.g. type mismatch, circular referencing, targetPath
/// points non-existing path, etc),
/// - Other error happens.
///
bool EvaluateAttribute(
    const lightusd::Stage &stage, const lightusd::Prim &prim,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, const double t = lightusd::value::TimeCode::Default(),
    const lightusd::value::TimeSampleInterpolationType tinterp =
        lightusd::value::TimeSampleInterpolationType::Linear);

// Layer/PrimSpec version
bool EvaluateAttribute(
    const lightusd::Layer &layer, const lightusd::PrimSpec &ps,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, const double t = lightusd::value::TimeCode::Default(),
    const lightusd::value::TimeSampleInterpolationType tinterp =
        lightusd::value::TimeSampleInterpolationType::Linear);

///
/// Evaluate Attribute and retrieve terminal Attribute value.
///
/// - If the attribute is empty(e.g. `float outputs:r`), return "empty"
/// Attribute
/// - If the attribute is scalar value, simply returns it.
/// - If the attribute is timeSamples value, evaluate the value at specified
/// time.
/// - If the attribute is connection, follow the connection target
///
/// @param[in] stage Stage
/// @param[in] attr Attribute
/// @param[in] attr_name Attribute name. This is only used in error message, so it can be empty.
/// @param[out] value Evaluated terminal attribute value.
/// @param[out] err Error message(filled when false returned). Set nullptr if you don't need error message.
/// @param[in] t (optional) TimeCode(for timeSamples Attribute)
/// @param[in] tinterp (optional) Interpolation type for timeSamples value
///
bool EvaluateAttribute(
    const lightusd::Stage &stage, const Attribute &attr,
    const std::string &attr_name, TerminalAttributeValue *value,
    std::string *err, const double t = lightusd::value::TimeCode::Default(),
    const lightusd::value::TimeSampleInterpolationType tinterp =
        lightusd::value::TimeSampleInterpolationType::Linear);
bool EvaluateAttributeFromClips(
    const lightusd::Prim &prim,
    const std::string &attr_name,
    TerminalAttributeValue *value,
    std::string *err, const double t = lightusd::value::TimeCode::Default(),
    const lightusd::value::TimeSampleInterpolationType tinterp =
        lightusd::value::TimeSampleInterpolationType::Linear);



//
// Typed version
//
template<typename T>
bool EvaluateTypedAttribute(
    const lightusd::Stage &stage,
    const TypedAttribute<T> &attr,
    const std::string &attr_name,
    T *value,
    std::string *err);


// NOTE: std::string uses the specialization, so no extern template.
#define EXTERN_EVALUATE_TYPED_ATTRIBUTE(__ty) \
extern template bool EvaluateTypedAttribute(const lightusd::Stage &stage, const TypedAttribute<__ty> &attr, const std::string &attr_name, __ty *value, std::string *err);

APPLY_FUNC_TO_VALUE_TYPES_NO_STRING(EXTERN_EVALUATE_TYPED_ATTRIBUTE)
template<> bool EvaluateTypedAttribute(const lightusd::Stage &stage, const TypedAttribute<std::string> &attr, const std::string &attr_name, std::string *value, std::string *err);

#undef EXTERN_EVALUATE_TYPED_ATTRIBUTE

template<typename T>
bool EvaluateTypedAnimatableAttribute(
    const lightusd::Stage &stage,
    const TypedAttribute<Animatable<T>> &attr,
    const std::string &attr_name,
    T *value,
    std::string *err, const double t = lightusd::value::TimeCode::Default(),
    const lightusd::value::TimeSampleInterpolationType tinterp =
        lightusd::value::TimeSampleInterpolationType::Linear);

#define EXTERN_EVALUATE_TYPED_ATTRIBUTE(__ty) \
extern template bool EvaluateTypedAnimatableAttribute(const lightusd::Stage &stage, const TypedAttribute<Animatable<__ty>> &attr, const std::string &attr_name, __ty *value, std::string *err, const double t, const value::TimeSampleInterpolationType tinter);

APPLY_FUNC_TO_VALUE_TYPES_NO_STRING(EXTERN_EVALUATE_TYPED_ATTRIBUTE)
template<> bool EvaluateTypedAnimatableAttribute(const lightusd::Stage &stage, const TypedAttribute<Animatable<std::string>> &attr, const std::string &attr_name, std::string *value, std::string *err, const double t, const value::TimeSampleInterpolationType tinter);

#undef EXTERN_EVALUATE_TYPED_ATTRIBUTE

template<typename T>
bool EvaluateTypedAttribute(
    const lightusd::Stage &stage,
    const TypedAttributeWithFallback<T> &attr,
    const std::string &attr_name,
    T *value,
    std::string *err);

#define EXTERN_EVALUATE_TYPED_ATTRIBUTE(__ty) \
extern template bool EvaluateTypedAttribute(const lightusd::Stage &stage, const TypedAttributeWithFallback<__ty> &attr, const std::string &attr_name, __ty *value, std::string *err);

APPLY_FUNC_TO_VALUE_TYPES_NO_STRING(EXTERN_EVALUATE_TYPED_ATTRIBUTE)
template<> bool EvaluateTypedAttribute(
    const lightusd::Stage &stage,
    const TypedAttributeWithFallback<std::string> &attr,
    const std::string &attr_name,
    std::string *value,
    std::string *err);

#undef EXTERN_EVALUATE_TYPED_ATTRIBUTE

template<typename T>
bool EvaluateTypedAnimatableAttribute(
    const lightusd::Stage &stage,
    const TypedAttributeWithFallback<Animatable<T>> &attr,
    const std::string &attr_name,
    T *value,
    std::string *err, const double t = lightusd::value::TimeCode::Default(),
    const lightusd::value::TimeSampleInterpolationType tinterp =
        lightusd::value::TimeSampleInterpolationType::Linear);

#define EXTERN_EVALUATE_TYPED_ATTRIBUTE(__ty) \
extern template bool EvaluateTypedAnimatableAttribute(const lightusd::Stage &stage, const TypedAttributeWithFallback<Animatable<__ty>> &attr, const std::string &attr_name, __ty *value, std::string *err, const double t, const value::TimeSampleInterpolationType tinter);

APPLY_FUNC_TO_VALUE_TYPES_NO_STRING(EXTERN_EVALUATE_TYPED_ATTRIBUTE)
template<> bool EvaluateTypedAnimatableAttribute(
    const lightusd::Stage &stage,
    const TypedAttributeWithFallback<Animatable<std::string>> &attr,
    const std::string &attr_name,
    std::string *value,
    std::string *err, const double t, const lightusd::value::TimeSampleInterpolationType tinterp);

#undef EXTERN_EVALUATE_TYPED_ATTRIBUTE

}  // namespace tydra
}  // namespace lightusd
