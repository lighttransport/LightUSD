// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// Generic typed-attribute evaluation template bodies.
//
// Included (inside namespace lightusd::tydra) by the per-type-group
// instantiation TUs: attribute-eval-typed-inst-{scalar,array}.cc.
// The std::string specializations live in attribute-eval-typed-all.cc.

#define PushError(msg) \
  if (err) {           \
    (*err) +=  msg;     \
  }

template<typename T>
bool EvaluateTypedAttribute(
    const lightusd::Stage &stage, const TypedAttribute<T> &tattr,
    const std::string &attr_name,
    T *value_out,
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
    // the value only if the connection cannot be resolved. (is_connection() is
    // false when a value is also present, so dispatch on has_connections().)
    Attribute attr = detail::ToAttributeConnection<
        TypedAttribute<T>, Variability::Uniform>(tattr);

    std::string conn_err;
    if (detail::FollowConnection(
            stage, attr, attr_name, value_out, &conn_err,
            value::TimeCode::Default(),
            value::TimeSampleInterpolationType::Held)) {
      return true;
    }
    if (tattr.has_value() && tattr.get_value(value_out)) {
      return true;  // recovered via the authored fallback value
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

template<typename T>
bool EvaluateTypedAnimatableAttribute(
    const lightusd::Stage &stage, const TypedAttribute<Animatable<T>> &tattr,
    const std::string &attr_name,
    T *value_out,
    std::string *err,
    const double t,
    const value::TimeSampleInterpolationType tinterp) {

  if (!value_out) {
    PUSH_ERROR_AND_RETURN("`value_out` param is nullptr.");
  }

  // Eval order (USD: a connection overrides the authored value):
  // - ValueBlocked?
  // - has connection? (follow; fall back to the value if it can't resolve)
  // - has value? (default value or timesampled value)

  if (tattr.is_blocked()) {
    if (err) {
      (*err) += "Attribute is Blocked.\n";
    }
    return false;
  } else if (tattr.has_connections()) {

    Attribute attr = detail::ToAttributeConnection<
        TypedAttribute<Animatable<T>>, Variability::Varying,
        /* copyMeta */ true>(tattr, value::TypeTraits<T>::type_name());

    std::string conn_err;
    if (detail::FollowConnection(
            stage, attr, attr_name, value_out, &conn_err, t, tinterp)) {
      return true;
    }
    // Connection unresolved: fall back to the authored value if present.
    if (tattr.has_value()) {
      Animatable<T> value;
      if (tattr.get_value(&value) && value.get(t, value_out, tinterp)) {
        return true;
      }
    }
    if (err) { (*err) += conn_err; }
    return false;

  } else if (tattr.has_value()) {
    Animatable<T> value;
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
  } else if (tattr.is_value_empty()) {
    if (err) {
      (*err) += "Attribute value is empty.\n";
    }
    return false;
  } else {
    if (err) {
      (*err) += fmt::format("[Internal error] Invalid TypedAttribute? : {} \n", attr_name);
    }
  }
  return false;
}

template<typename T>
bool EvaluateTypedAttribute(
    const lightusd::Stage &stage, const TypedAttributeWithFallback<T> &tattr,
    const std::string &attr_name,
    T *value_out,
    std::string *err) {

  if (!value_out) {
    PUSH_ERROR_AND_RETURN("`value_out` param is nullptr.");
  }

  if (tattr.is_blocked()) {
    if (err) {
      (*err) += "Attribute is Blocked.\n";
    }
    return false;
  } else if (tattr.has_value()) {
    (*value_out) = tattr.get_value();
    return true;
  } else if (tattr.has_connections()) {

    Attribute attr = detail::ToAttributeConnection<
        TypedAttributeWithFallback<T>, Variability::Uniform>(tattr);

    return detail::FollowConnection(
        stage, attr, attr_name, value_out, err,
        value::TimeCode::Default(), value::TimeSampleInterpolationType::Held);

  } else if (tattr.is_value_empty()) {
    if (err) {
      (*err) += "Attribute value is empty.\n";
    }
    return false;
  }

  PUSH_ERROR_AND_RETURN(fmt::format("Internal error. Attribute {} has invalid form of TypedAttributeWithFallback<{}>.",
     attr_name, value::TypeTraits<T>::type_name()));

}

template<typename T>
bool EvaluateTypedAnimatableAttribute(
    const lightusd::Stage &stage, const TypedAttributeWithFallback<Animatable<T>> &tattr,
    const std::string &attr_name,
    T *value_out,
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
  } else if (tattr.has_value()) {
    const Animatable<T> &value = tattr.get_value();
    T v;
    if (value.get(t, &v, tinterp)) {
      *value_out = v;
      return true;
    } else {
      if (err) {
        (*err) += fmt::format("Failed to get TypedAnimatableAttribute value: {} \n", attr_name);
      }
      return false;
    }
  } else if (tattr.has_connections()) {

    Attribute attr = detail::ToAttributeConnection<
        TypedAttributeWithFallback<Animatable<T>>, Variability::Varying>(
            tattr, value::TypeTraits<T>::type_name());

    return detail::FollowConnection(
        stage, attr, attr_name, value_out, err,
        value::TimeCode::Default(), value::TimeSampleInterpolationType::Held);

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
