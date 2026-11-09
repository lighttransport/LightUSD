// SPDX-License-Identifier: Apache-2.0
// Shared property-read helpers for the render converter TUs. Reading a token
// or string property as text, and a bool property, is common across prim-type
// conversion; these were previously copy-pasted into six TUs. Named GetProp*
// (not GetToken/GetBool) to avoid clashing with scene-access.hh's stricter
// token-only reader.
#pragma once
#include <string>

#include "next/stage/stage.hh"
#include "next/types/value.hh"

namespace lightusd {
namespace tydra {
namespace next {

inline bool GetPropToken(const ::lightusd::next::UsdPrim& prim,
                         const std::string& name, std::string* out) {
  if (!out) return false;
  const ::lightusd::next::Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  if (auto token = value->as_token()) {
    *out = *token;
    return true;
  }
  if (auto text = value->as_string()) {
    *out = *text;
    return true;
  }
  return false;
}

inline bool GetPropBool(const ::lightusd::next::UsdPrim& prim,
                        const std::string& name, bool* out) {
  if (!out) return false;
  const ::lightusd::next::Value* value = prim.GetPropertyValue(name);
  if (!value) return false;
  const bool* result = value->as_bool();
  if (!result) return false;
  *out = *result;
  return true;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
