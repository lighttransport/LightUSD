// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// std::string specializations of the typed attribute evaluators.
// The generic template bodies live in attribute-eval-typed-impl.inc and are
// instantiated by attribute-eval-typed-inst-{scalar,array}.cc.
//
#include "attribute-eval-internal.hh"

namespace lightusd {
namespace tydra {

#define PushError(msg) \
  if (err) {           \
    (*err) +=  msg;     \
  }

template<>
bool EvaluateTypedAttribute(
    const lightusd::Stage &stage, const TypedAttribute<std::string> &tattr,
    const std::string &attr_name,
    std::string *value_out,
    std::string *err) {

  if (!value_out) {
    PUSH_ERROR_AND_RETURN("`value_out` param is nullptr.");
  }

  if (tattr.is_blocked()) {
    if (err) {
      (*err) += "Attribute is Blocked.\n";
    }
    return false;
  } else if (tattr.is_value_empty()) {
    if (err) {
      (*err) += "Attribute value is empty.\n";
    }
    return false;
  } else if (tattr.has_connections()) {
    // A connection overrides the authored value (USD). Follow it; fall back to
    // the value only if the connection cannot be resolved.
    Attribute attr = detail::ToAttributeConnection<
        TypedAttribute<std::string>, Variability::Uniform>(tattr);

    std::string conn_err;
    if (detail::FollowConnection(
            stage, attr, attr_name, value_out, &conn_err,
            value::TimeCode::Default(),
            value::TimeSampleInterpolationType::Held)) {
      return true;
    }
    if (tattr.has_value() && tattr.get_value(value_out)) {
      return true;
    }
    if (err) { (*err) += conn_err; }
    return false;

  } else {
    if (tattr.get_value(value_out)) {
      return true;
    }

    if (err) {
      (*err) += fmt::format("[Internal error] Invalid TypedAttribute? : {} \n", attr_name);
    }
  }
  return false;
}

template<> bool EvaluateTypedAnimatableAttribute<std::string>(
    const lightusd::Stage &stage, const TypedAttribute<Animatable<std::string>> &tattr,
    const std::string &attr_name,
    std::string *value_out,
    std::string *err,
    const double t,
    const value::TimeSampleInterpolationType tinterp) {

  if (!value_out) {
    PUSH_ERROR_AND_RETURN("`value_out` param is nullptr.");
  }

  if (tattr.is_blocked()) {
    if (err) {
      (*err) += "Attribute is Blocked.\n";
    }
    return false;
  } else if (tattr.is_value_empty()) {
    if (err) {
      (*err) += "Attribute value is empty.\n";
    }
    return false;
  } else if (tattr.has_connections()) {
    // A connection overrides the authored value (USD). Follow it; fall back to
    // the value only if the connection cannot be resolved.
    Attribute attr = detail::ToAttributeConnection<
        TypedAttribute<Animatable<std::string>>, Variability::Varying,
        /* copyMeta */ true>(tattr, value::TypeTraits<std::string>::type_name());

    std::string conn_err;
    if (detail::FollowConnection(
            stage, attr, attr_name, value_out, &conn_err, t, tinterp)) {
      return true;
    }
    if (tattr.has_value()) {
      Animatable<std::string> value;
      if (tattr.get_value(&value) && value.get(t, value_out, tinterp)) {
        return true;
      }
    }
    if (err) { (*err) += conn_err; }
    return false;

  } else {
    Animatable<std::string> value;
    if (tattr.get_value(&value)) {
      if (value.get(t, value_out, tinterp)) {
        return true;
      } else {
        if (err) {
          (*err) += fmt::format("Failed to get TypedAnimatableAttribute value: {} \n", attr_name);
        }
        return false;
      }
    }

    if (err) {
      (*err) += fmt::format("[Internal error] Invalid TypedAttribute? : {} \n", attr_name);
    }
  }
  return false;
}

template<>
bool EvaluateTypedAttribute(
    const lightusd::Stage &stage, const TypedAttributeWithFallback<std::string> &tattr,
    const std::string &attr_name,
    std::string *value_out,
    std::string *err) {

  if (!value_out) {
    PUSH_ERROR_AND_RETURN("`value_out` param is nullptr.");
  }

  if (tattr.is_blocked()) {
    if (err) {
      (*err) += "Attribute is Blocked.\n";
    }
    return false;
  } else if (tattr.has_connections()) {
    // A connection overrides the authored value (USD). Follow it; fall back to
    // the (always-present) fallback value if it cannot be resolved.
    Attribute attr = detail::ToAttributeConnection<
        TypedAttributeWithFallback<std::string>, Variability::Uniform>(tattr);

    std::string conn_err;
    if (detail::FollowConnection(
            stage, attr, attr_name, value_out, &conn_err,
            value::TimeCode::Default(),
            value::TimeSampleInterpolationType::Held)) {
      return true;
    }
    (*value_out) = tattr.get_value();
    return true;

  } else if (tattr.is_value_empty()) {
    if (err) {
      (*err) += "Attribute value is empty.\n";
    }
    return false;
  } else {
    (*value_out) = tattr.get_value();
    return true;
  }
}

template<>
bool EvaluateTypedAnimatableAttribute(
    const lightusd::Stage &stage, const TypedAttributeWithFallback<Animatable<std::string>> &tattr,
    const std::string &attr_name,
    std::string *value_out,
    std::string *err,
    const double t,
    const value::TimeSampleInterpolationType tinterp) {

  if (!value_out) {
    PUSH_ERROR_AND_RETURN("`value_out` param is nullptr.");
  }

  if (tattr.is_blocked()) {
    if (err) {
      (*err) += "Attribute is Blocked.\n";
    }
    return false;
  } else if (tattr.has_connections()) {
    // A connection overrides the authored value (USD). Follow it; fall back to
    // the value if it cannot be resolved.
    Attribute attr = detail::ToAttributeConnection<
        TypedAttributeWithFallback<Animatable<std::string>>,
        Variability::Varying>(tattr, value::TypeTraits<std::string>::type_name());

    std::string conn_err;
    if (detail::FollowConnection(
            stage, attr, attr_name, value_out, &conn_err,
            value::TimeCode::Default(),
            value::TimeSampleInterpolationType::Held)) {
      return true;
    }
    {
      const Animatable<std::string> &value = tattr.get_value();
      std::string v;
      if (value.get(t, &v, tinterp)) {
        *value_out = v;
        return true;
      }
    }
    if (err) { (*err) += conn_err; }
    return false;

  } else if (tattr.has_value()) {
    const Animatable<std::string> &value = tattr.get_value();
    std::string v;
    if (value.get(t, &v, tinterp)) {
      *value_out = v;
      return true;
    } else {
      if (err) {
        (*err) += fmt::format("Failed to get TypedAnimatableAttribute value: {} \n", attr_name);
      }
      return false;
    }

  } else if (tattr.is_value_empty()) {
    if (err) {
      (*err) += "Attribute value is empty.\n";
    }
    return false;
  } else {
    if (err) {
      (*err) += fmt::format("Unsupported/Invalid TypedAnimatableAttribute value: {}", attr_name);
    }
  }
  return false;
}

}  // namespace tydra
}  // namespace lightusd
