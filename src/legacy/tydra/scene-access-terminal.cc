// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//

#include "common-macros.inc"
#include "pprint-enum.hh"
#include "scene-access.hh"
#include "tiny-format.hh"

#include <string>
#include <unordered_set>
#include <vector>

namespace lightusd {
namespace tydra {

#define PushError(msg) \
  if (err) {           \
    *err += msg;       \
  }

namespace {

//
// visited_paths : To prevent circular referencing of attribute connection.
//
bool GetTerminalAttributeImpl(const lightusd::Stage &stage,
                              const lightusd::Prim &prim,
                              const std::string &attr_name, Attribute *value,
                              std::string *err,
                              std::unordered_set<std::string, FNV1StringHash>
                                  &visited_paths) {
  DCOUT("Prim : " << prim.element_path().element_name() << "("
                  << prim.type_name() << ") attr_name " << attr_name);

  Property prop;
  if (!GetProperty(prim, attr_name, &prop, err)) {
    return false;
  }

  if (prop.is_attribute_connection()) {
    // Follow connection target Path(singple targetPath only).
    std::vector<Path> pv = prop.get_attribute().connections();
    if (pv.empty()) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Connection targetPath is empty for Attribute {}.", attr_name));
    }

    if (pv.size() > 1) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Multiple targetPaths assigned to .connection."));
    }

    auto target = pv[0];

    std::string targetPrimPath = target.prim_part();
    std::string targetPrimPropName = target.prop_part();
    DCOUT("connection targetPath : " << target << "(Prim: " << targetPrimPath
                                     << ", Prop: " << targetPrimPropName
                                     << ")");

    auto targetPrimRet =
        stage.GetPrimAtPath(Path(targetPrimPath, /* prop */ ""));
    if (targetPrimRet) {
      // Follow the connetion
      const Prim *targetPrim = targetPrimRet.value();

      std::string abs_path = target.full_path_name();

      if (!visited_paths.emplace(abs_path).second) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Circular referencing detected. connectionTargetPath = {}",
            to_string(target)));
      }

      return GetTerminalAttributeImpl(stage, *targetPrim, targetPrimPropName,
                                      value, err, visited_paths);

    } else {
      PUSH_ERROR_AND_RETURN(targetPrimRet.error());
    }
  } else if (prop.is_relationship()) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Property `{}` is a Relation.", attr_name));
  } else if (prop.is_empty()) {
    PUSH_ERROR_AND_RETURN(fmt::format(
        "Attribute `{}` is a define-only attribute(no value assigned).",
        attr_name));
  } else if (prop.is_attribute()) {
    (*value) = prop.get_attribute();

  } else {
    // ???
    PUSH_ERROR_AND_RETURN(
        fmt::format("[InternalError] Invalid Attribute `{}`.", attr_name));
  }

  return true;
}

}  // namespace

bool GetTerminalAttribute(const lightusd::Stage &stage,
                          const lightusd::Attribute &attr,
                          const std::string &attr_name, Attribute *value,
                          std::string *err) {
  if (!value) {
    PUSH_ERROR_AND_RETURN("`value` arg is nullptr.");
  }

  std::unordered_set<std::string, FNV1StringHash> visited_paths;
  visited_paths.reserve(16);

  if (attr.has_connections()) {
    // A connection overrides the authored value (USD). Follow it; fall back to
    // the attribute's own value if it cannot be resolved. (is_connection() is
    // false when a value is also present, so dispatch on has_connections().)
    std::string conn_err;
    bool resolved = false;

    std::vector<Path> pv = attr.connections();
    if (pv.empty()) {
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Connection targetPath is empty for Attribute {}.", attr_name));
    }

    if (pv.size() > 1) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Multiple targetPaths assigned to .connection."));
    }

    auto target = pv[0];

    std::string targetPrimPath = target.prim_part();
    std::string targetPrimPropName = target.prop_part();
    DCOUT("connection targetPath : " << target << "(Prim: " << targetPrimPath
                                     << ", Prop: " << targetPrimPropName
                                     << ")");

    auto targetPrimRet =
        stage.GetPrimAtPath(Path(targetPrimPath, /* prop */ ""));
    if (targetPrimRet) {
      // Follow the connetion
      const Prim *targetPrim = targetPrimRet.value();

      std::string abs_path = target.full_path_name();

      if (!visited_paths.emplace(abs_path).second) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Circular referencing detected. connectionTargetPath = {}",
            to_string(target)));
      }

      resolved = GetTerminalAttributeImpl(stage, *targetPrim, targetPrimPropName,
                                          value, &conn_err, visited_paths);

    } else {
      conn_err += targetPrimRet.error();
    }

    if (resolved) {
      return true;
    }
    // Connection unresolved — fall back to the attribute's own value if present.
    if (attr.has_value()) {
      (*value) = attr;
      return true;
    }
    if (err) {
      (*err) += conn_err;
    }
    return false;

  } else {
    (*value) = attr;
    return true;
  }
}

#undef PushError

}  // namespace tydra
}  // namespace lightusd
