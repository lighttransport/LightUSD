// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// Explicit instantiations of the typed attribute evaluators -- SCALAR types.
// Split from attribute-eval-typed-all.cc for parallel compilation.
//
#include "attribute-eval-internal.hh"

namespace lightusd {
namespace tydra {

#include "attribute-eval-typed-impl.inc"

#define EVALUATE_TYPED_ATTRIBUTE_INSTANCIATE(__ty) \
template bool EvaluateTypedAttribute(const lightusd::Stage &stage, const TypedAttribute<__ty> &attr, const std::string &attr_name, __ty *value, std::string *err); \
template bool EvaluateTypedAnimatableAttribute(const lightusd::Stage &stage, const TypedAttribute<Animatable<__ty>> &attr, const std::string &attr_name, __ty *value, std::string *err, const double t, const value::TimeSampleInterpolationType tinterp); \
template bool EvaluateTypedAttribute(const lightusd::Stage &stage, const TypedAttributeWithFallback<__ty> &attr, const std::string &attr_name, __ty *value, std::string *err); \
template bool EvaluateTypedAnimatableAttribute(const lightusd::Stage &stage, const TypedAttributeWithFallback<Animatable<__ty>> &attr, const std::string &attr_name, __ty *value, std::string *err, const double t, const value::TimeSampleInterpolationType tinterp);

APPLY_FUNC_TO_VALUE_TYPES_SCALAR_NO_STRING(EVALUATE_TYPED_ATTRIBUTE_INSTANCIATE)

#undef EVALUATE_TYPED_ATTRIBUTE_INSTANCIATE

}  // namespace tydra
}  // namespace lightusd
