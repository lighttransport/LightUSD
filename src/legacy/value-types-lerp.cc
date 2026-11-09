// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// value::Lerp / IsLerpSupportedType — split out of value-types.cc. Lerp() switches
// on type_id and instantiates lerp<T> (value-eval-util.hh) for ~44 interpolatable
// types — the codegen bulk of value-types.cc. Isolated from the half-precision
// conversion/arithmetic that stays in value-types.cc, so editing either recompiles
// faster. Declared in value-types.hh.
#include "value-types.hh"

#include <unordered_set>

#include "value-eval-util.hh"
#include "common-macros.inc"
#include "value-type-macros.inc"

namespace lightusd {
namespace value {

static const std::unordered_set<uint32_t> &GetLerpSupportedTypeIds() {
  static std::unordered_set<uint32_t> s = [] {
    std::unordered_set<uint32_t> ids;
    auto add = [&](uint32_t tid, uint32_t uid) {
      ids.insert(tid);
      ids.insert(uid);
      ids.insert(tid | value::TYPE_ID_1D_ARRAY_BIT);
      ids.insert(uid | value::TYPE_ID_1D_ARRAY_BIT);
    };
#define ADD_LERP_ID(__ty) \
    add(TypeTraits<__ty>::type_id(), TypeTraits<__ty>::underlying_type_id());
    APPLY_FUNC_TO_LERP_VALUE_TYPES(ADD_LERP_ID)
#undef ADD_LERP_ID
    return ids;
  }();
  return s;
}

bool IsLerpSupportedType(uint32_t tyid) {
  const auto &ids = GetLerpSupportedTypeIds();

  // Check direct match
  if (ids.count(tyid)) {
    return true;
  }

  // Check array bit variants
  if (tyid & value::TYPE_ID_1D_ARRAY_BIT) {
    uint32_t base = tyid & (~value::TYPE_ID_1D_ARRAY_BIT);
    if (ids.count(base)) {
      return true;
    }
  }

  // Check underlying type for role types (e.g. color3f -> float3)
  if (auto pv = TryGetUnderlyingTypeName(tyid)) {
    uint32_t underlying_tyid = GetTypeId(pv.value());
    if (ids.count(underlying_tyid)) {
      return true;
    }
    if (underlying_tyid & value::TYPE_ID_1D_ARRAY_BIT) {
      uint32_t base = underlying_tyid & (~value::TYPE_ID_1D_ARRAY_BIT);
      if (ids.count(base)) {
        return true;
      }
    }
  }

  return false;
}

bool Lerp(const value::Value &a, const value::Value &b, double dt, value::Value *dst) {
  if (!dst) {
    return false;
  }

  if (a.type_id() != b.type_id()) {
    return false;
  }

  uint32_t tyid = a.type_id();

  if (!IsLerpSupportedType(tyid)) {
    return false;
  }

  bool ok{false};
  value::Value result;

#define DO_LERP(__ty) \
  if (tyid == value::TypeTraits<__ty>::type_id()) { \
    const __ty *v0 = a.as<__ty>(); \
    const __ty *v1 = b.as<__ty>(); \
    __ty c; \
    if (v0 && v1) { \
      c = lerp(*v0, *v1, dt); \
      result = c; \
      ok = true; \
    } \
  } else if (tyid == value::TypeTraits<std::vector<__ty>>::type_id()) { \
    const std::vector<__ty> *v0 = a.as<std::vector<__ty>>(); \
    const std::vector<__ty> *v1 = b.as<std::vector<__ty>>(); \
    std::vector<__ty> c; \
    if (v0 && v1) { \
      c = lerp(*v0, *v1, dt); \
      result = c; \
      ok = true; \
    } \
  } else

  // Generated from the single lerpable-type list (shared with LerpTraits and
  // GetLerpSupportedTypeIds) so all three agree — fixes the prior drift where
  // matrices were "lerp-supported" but had no Lerp() arm.
  APPLY_FUNC_TO_LERP_VALUE_TYPES(DO_LERP)
  {
    DCOUT("TODO: type " << GetTypeName(tyid));
  }

#undef DO_LERP

  if (ok) {
    (*dst) = result;
  }

  return ok;
}


//
// half float
//

}  // namespace value
}  // namespace lightusd
