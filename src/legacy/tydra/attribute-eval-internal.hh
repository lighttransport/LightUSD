// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// Shared helpers for the typed attribute evaluation files.
// This is an internal header -- not part of the public API.
//
#pragma once

#include "attribute-eval.hh"
#include "scene-access.hh"

#include "common-macros.inc"
#include "tiny-format.hh"
#include "value-pprint.hh"

namespace lightusd {
namespace tydra {
namespace detail {

// --------------------------------------------------------------------------
// FollowConnection: resolve a single connection path, evaluate the target
// attribute, and cast the result to the requested type T.
//
// This is the ~30-line block that was duplicated 8+ times across the four
// typed evaluation files.
// --------------------------------------------------------------------------
template <typename T>
bool FollowConnection(const lightusd::Stage &stage,
                      const Attribute &conn_attr,
                      const std::string &attr_name,
                      T *value_out, std::string *err,
                      const double t,
                      const value::TimeSampleInterpolationType tinterp) {
  TerminalAttributeValue resolved;
  if (!EvaluateAttribute(stage, conn_attr, attr_name, &resolved, err,
                         t, tinterp)) {
    return false;
  }

  if (auto pv = resolved.as<T>()) {
    (*value_out) = *pv;
    return true;
  }

  if (err) {
    (*err) += fmt::format(
        "Type mismatch. Value producing attribute has type {}, but requested "
        "type is {}. Attribute: {}",
        resolved.type_name(), value::TypeTraits<T>::type_name(), attr_name);
  }
  return false;
}

// std::string specialization: also accepts token-typed values.
inline bool FollowConnection(const lightusd::Stage &stage,
                             const Attribute &conn_attr,
                             const std::string &attr_name,
                             std::string *value_out, std::string *err,
                             const double t,
                             const value::TimeSampleInterpolationType tinterp) {
  TerminalAttributeValue resolved;
  if (!EvaluateAttribute(stage, conn_attr, attr_name, &resolved, err,
                         t, tinterp)) {
    return false;
  }

  if (auto pv = resolved.as<std::string>()) {
    (*value_out) = *pv;
    return true;
  }

  // Allow `token` typed value in the attribute of targetPath.
  if (auto pv = resolved.as<value::token>()) {
    (*value_out) = pv->str();
    return true;
  }

  if (err) {
    (*err) += fmt::format(
        "Type mismatch. Value producing attribute has type {}, but requested "
        "type is {}. Attribute: {}",
        resolved.type_name(), value::TypeTraits<std::string>::type_name(),
        attr_name);
  }
  return false;
}

// --------------------------------------------------------------------------
// ToAttributeConnection: convert any typed attribute wrapper to a plain
// Attribute that carries only the connection paths (for delegation to
// the generic EvaluateAttribute codepath).
//
// Template parameters:
//   AttrT    - the typed attribute wrapper (e.g. TypedAttribute<T>,
//              TypedAttributeWithFallback<Animatable<T>>)
//   var      - Variability to assign (Uniform or Varying)
//   copyMeta - whether to copy metadata from the source attribute
// --------------------------------------------------------------------------
template <typename AttrT, Variability var, bool copyMeta = false>
Attribute ToAttributeConnection(const AttrT &input,
                                const std::string &type_name_str = {}) {
  Attribute attr;
  if (input.is_blocked()) {
    attr.set_blocked(true);
    attr.variability() = var;
  } else if (input.has_connections()) {
    // A connection is followed even when a fallback value is also authored (USD:
    // a connection overrides the value). Use has_connections(), not
    // is_connection() (which is false when a value is present).
    attr.set_connections(input.connections());
  } else if (input.is_value_empty()) {
    if (!type_name_str.empty()) {
      attr.set_type_name(type_name_str);
    }
    attr.variability() = var;
  } else {
    if (!type_name_str.empty()) {
      attr.set_type_name(type_name_str);
    }
    attr.variability() = var;
  }

  // Animatable connection converters in the original code copied metas.
  if (copyMeta) {
    attr.metas() = input.metas();
  }

  return attr;
}

}  // namespace detail
}  // namespace tydra
}  // namespace lightusd
