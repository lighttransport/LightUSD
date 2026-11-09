// SPDX-License-Identifier: Apache-2.0
// Shared schema predicates for the schema TUs. Checking a prim's applied API
// schemas and its concrete type is common across the per-schema converters;
// these were previously copy-pasted (HasAPISchema/HasApplied x4, IsType x2).
#pragma once
#include <string>

#include "../stage/stage.hh"
#include "../layer/prim-spec.hh"

namespace lightusd {
namespace next {

inline bool HasAPISchema(const UsdPrim& prim, const std::string& name) {
  for (const std::string& entry : prim.GetMeta().apiSchemas()) {
    if (entry == name) return true;
  }
  return false;
}

inline bool IsType(const UsdPrim& prim, const std::string& type) {
  return prim.IsValid() && prim.GetTypeName() == type;
}

}  // namespace next
}  // namespace lightusd
