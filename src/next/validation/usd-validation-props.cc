// SPDX-License-Identifier: Apache-2.0
// Time-sample, generic-property and relationship/connection validation
// rules, extracted from usd-validation.cc for compile-time parallelism.
// Part of the validation_detail namespace shared through
// usd-validation-internal.hh.

#include "usd-validation-internal.hh"

#include "../prim/identifier.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lightusd {
namespace next {
namespace validation_detail {

// ---------------------------------------------------------------------------
// Time samples
// ---------------------------------------------------------------------------

const std::vector<std::pair<double, uint32_t>> *GetTimeSamplesOf(
    const PrimSpec &ps, const std::string &name) {
  const PropNameId id = GetPropNameTable().find(name);
  if (!id.is_valid()) {
    return nullptr;
  }
  return ps.time_samples(id);
}

void ValidateTimeSamplesProp(const PrimSpec &ps, const std::string &prop_name,
                             const std::string &rule_id,
                             const std::string &location,
                             USDValidationResult *result) {
  const auto *samples = GetTimeSamplesOf(ps, prop_name);
  if (!samples) {
    return;
  }
  // NOTE: next TimeSampleBlock keeps samples sorted with last-wins upsert,
  // so the legacy "strictly increasing" violation is normally unrepresentable;
  // the check is kept as a defensive guard.
  bool have_prev = false;
  double prev = 0.0;
  for (size_t i = 0; i < samples->size(); i++) {
    const double t = (*samples)[i].first;
    if (!std::isfinite(t)) {
      AddError(result, rule_id, location,
               "timeSamples sample " + std::to_string(i) +
                   " has a non-finite time");
    }
    if (have_prev && t <= prev) {
      AddError(result, rule_id, location,
               "timeSamples must be strictly increasing; sample " +
                   std::to_string(i) + " is at " + std::to_string(t) +
                   " after " + std::to_string(prev));
    }
    have_prev = true;
    prev = t;
  }
}

bool IsLengthCompatible(size_t count, size_t expected, bool allow_singleton) {
  return count == expected || (allow_singleton && count == 1);
}

// Default + every time sample of `prop_name` must be an array of `expected`
// elements (optionally allowing a length-1 constant).
void ValidateArrayLengthAtAllSamples(const PrimSpec &ps,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     size_t expected,
                                     const std::string &expected_name,
                                     USDValidationResult *result) {
  const Value *v = GetAttrValue(ps, prop_name);
  if (v) {
    size_t count = 0;
    if (GetValueArrayLength(*v, &count) && count != expected) {
      AddError(result, rule_id, location,
               prop_name + " has " + std::to_string(count) +
                   " default elements, but " + expected_name + " has " +
                   std::to_string(expected) + " elements");
    }
  }

  const auto *samples = GetTimeSamplesOf(ps, prop_name);
  if (!samples) {
    return;
  }
  ValidateTimeSamplesProp(ps, prop_name, rule_id, location, result);
  for (size_t i = 0; i < samples->size(); i++) {
    const Value *sv = ps.time_sample_value((*samples)[i].second);
    if (!sv || sv->is_empty() || sv->is_block()) {
      continue;
    }
    size_t count = 0;
    if (!GetValueArrayLength(*sv, &count)) {
      AddWarning(result, rule_id, location,
                 prop_name + " timeSamples sample " + std::to_string(i) +
                     " should be an array value");
      continue;
    }
    if (count != expected) {
      AddError(result, rule_id, location,
               prop_name + " timeSamples sample " + std::to_string(i) +
                   " at time " + std::to_string((*samples)[i].first) +
                   " has " + std::to_string(count) + " elements, but " +
                   expected_name + " has " + std::to_string(expected) +
                   " elements");
    }
  }
}

// ---------------------------------------------------------------------------
// Generic property validators (rule-id parameterized; ported from legacy)
// ---------------------------------------------------------------------------

bool IsPrimvarPropertyName(const std::string &prop_name) {
  return StartsWith(prop_name, "primvars:") &&
         prop_name.size() > std::string("primvars:").size() &&
         !EndsWith(prop_name, ":indices");
}

bool IsValidPrimvarInterpolation(const std::string &interp) {
  return interp == "constant" || interp == "uniform" ||
         interp == "vertex" || interp == "varying" ||
         interp == "faceVarying";
}

void ValidateTokenSetProperty(const PrimSpec &ps, const std::string &prop_name,
                              const std::set<std::string> &allowed,
                              const std::string &rule_id,
                              const std::string &prim_location,
                              USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a token attribute");
    return;
  }
  // A declared-only or timeSamples-only attribute is valid authoring (the
  // schema fallback / animation supplies the value); only an authored
  // DEFAULT of the wrong type is an error. GetAttrValue returns null for
  // slots without an authored default.
  if (const Value *v = GetAttrValue(ps, prop_name)) {
    auto token = v->as_token();
    if (!token) {
      AddError(result, rule_id, location,
               prop_name + " must have a token value");
      return;
    }
    if (allowed.count(*token) == 0) {
      AddError(result, rule_id, location,
               prop_name + " token `" + *token + "` is not valid");
      return;
    }
  }
  // Animated tokens must also stay inside the allowed set. One diagnostic
  // (the first offending sample) per property keeps reports readable.
  if (const std::vector<std::pair<double, uint32_t>> *samples =
          GetTimeSamplesOf(ps, prop_name)) {
    for (const auto &sample : *samples) {
      const Value *sv = ps.time_sample_value(sample.second);
      if (!sv || sv->is_block()) continue;
      auto token = sv->as_token();
      if (token && allowed.count(*token) > 0) continue;
      AddError(result, rule_id, location,
               prop_name + " timeSample at " + std::to_string(sample.first) +
                   (token ? " token `" + *token + "` is not valid"
                          : " must be a token value"));
      return;
    }
  }
}

void ValidatePositiveNumericProperty(const PrimSpec &ps,
                                     const std::string &prop_name,
                                     const std::string &rule_id,
                                     const std::string &prim_location,
                                     USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a numeric attribute");
    return;
  }
  double value = 0.0;
  if (!GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric scalar value");
    return;
  }
  if (!std::isfinite(value) || value <= 0.0) {
    AddError(result, rule_id, location,
             prop_name + " must be finite and greater than 0");
  }
}

void ValidateNonNegativeNumericProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric scalar value");
    return;
  }
  if (!std::isfinite(value) || value < 0.0) {
    AddError(result, rule_id, location,
             prop_name + " must be finite and non-negative");
  }
}

void ValidateNonNegativeIntegerProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  int64_t value = 0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetIntegerScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have an integer scalar value");
    return;
  }
  if (value < 0) {
    AddError(result, rule_id, location, prop_name + " must be non-negative");
  }
}

void ValidateFiniteVec3Property(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result,
                                bool require_nonzero) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a 3-component numeric attribute");
    return;
  }
  std::array<double, 3> value;
  const Value *v = GetAttrValue(ps, prop_name);
  if (!v || !ValueToVec3(*v, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a 3-component numeric value");
    return;
  }
  const double len2 =
      value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
  if (!std::isfinite(value[0]) || !std::isfinite(value[1]) ||
      !std::isfinite(value[2]) || (require_nonzero && len2 == 0.0)) {
    AddError(result, rule_id, location,
             prop_name + " must be finite" +
                 std::string(require_nonzero ? " and non-zero" : ""));
  }
}

void ValidateFiniteNonNegativeVec3Property(const PrimSpec &ps,
                                           const std::string &prop_name,
                                           const std::string &rule_id,
                                           const std::string &prim_location,
                                           USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a 3-component numeric attribute");
    return;
  }
  std::array<double, 3> value;
  const Value *v = GetAttrValue(ps, prop_name);
  if (!v || !ValueToVec3(*v, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a 3-component numeric value");
    return;
  }
  for (double c : value) {
    if (!std::isfinite(c) || c < 0.0) {
      AddError(result, rule_id, location,
               prop_name + " must be finite and non-negative");
      return;
    }
  }
}

void ValidateQuaternionProperty(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result, bool allow_zero) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a quaternion attribute");
    return;
  }
  std::array<double, 4> value;
  const Value *v = GetAttrValue(ps, prop_name);
  if (!v || !ValueToQuat(*v, &value)) {
    AddError(result, rule_id, location,
             prop_name + " must have a quaternion value");
    return;
  }
  const double len2 = value[0] * value[0] + value[1] * value[1] +
                      value[2] * value[2] + value[3] * value[3];
  for (double c : value) {
    if (!std::isfinite(c)) {
      AddError(result, rule_id, location,
               prop_name + " must be finite and non-zero");
      return;
    }
  }
  if (len2 == 0.0) {
    if (allow_zero) return;
    AddError(result, rule_id, location,
             prop_name + " must be finite and non-zero");
  } else if (std::fabs(len2 - 1.0) > 1.0e-3) {
    AddWarning(result, rule_id, location,
               prop_name + " should usually be unit length");
  }
}

void ValidateNumericArrayLengthProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        size_t expected,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a numeric array attribute");
    return;
  }
  std::vector<double> values;
  if (!GetDoubleArrayProperty(ps, prop_name, &values)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric array value");
    return;
  }
  if (values.size() != expected) {
    AddError(result, rule_id, location,
             prop_name + " must have " + std::to_string(expected) +
                 " elements");
  }
  for (double value : values) {
    if (!std::isfinite(value)) {
      AddError(result, rule_id, location,
               prop_name + " elements must be finite");
      return;
    }
  }
}

void ValidateNumericArrayFiniteProperty(const PrimSpec &ps,
                                        const std::string &prop_name,
                                        const std::string &rule_id,
                                        const std::string &prim_location,
                                        USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             prop_name + " must be a numeric array attribute");
    return;
  }
  std::vector<double> values;
  if (!GetDoubleArrayProperty(ps, prop_name, &values)) {
    AddError(result, rule_id, location,
             prop_name + " must have a numeric array value");
    return;
  }
  for (double value : values) {
    if (!std::isfinite(value)) {
      AddError(result, rule_id, location,
               prop_name + " elements must be finite");
      return;
    }
  }
}

bool GetRelationshipTargetCount(const PrimSpec &ps,
                                const std::string &rel_name, size_t *count) {
  if (!count) {
    return false;
  }
  if (!IsRelationshipProp(ps, rel_name)) {
    return false;
  }
  *count = RelTargets(ps, rel_name).size();
  return true;
}

void ValidateIntArrayLengthMatchesRelationship(
    const PrimSpec &ps, const std::string &array_prop,
    const std::string &rel_prop, const std::string &rule_id,
    const std::string &prim_location, USDValidationResult *result) {
  if (!HasProperty(ps, array_prop)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, array_prop);
  std::vector<int32_t> values;
  if (IsRelationshipProp(ps, array_prop) ||
      !GetIntArrayProperty(ps, array_prop, &values)) {
    AddError(result, rule_id, location,
             array_prop + " must have an int array value");
    return;
  }
  size_t target_count = 0;
  if (!GetRelationshipTargetCount(ps, rel_prop, &target_count)) {
    AddError(result, rule_id, location,
             array_prop + " requires relationship " + rel_prop);
    return;
  }
  if (array_prop == "mjc:path:indices") {
    // The path relationship is a unique table; indices describe the route,
    // including repeated visits. Every target must participate in the route.
    std::set<int32_t> used;
    for (int32_t index : values) {
      if (index < 0 || static_cast<size_t>(index) >= target_count) {
        AddError(result, rule_id, location,
                 array_prop + " contains an out-of-range target index");
        return;
      }
      used.insert(index);
    }
    if (used.size() != target_count)
      AddError(result, rule_id, location,
               array_prop + " must cover every path target");
    return;
  }
  if (values.size() != target_count) {
    AddError(result, rule_id, location,
             array_prop + " has " + std::to_string(values.size()) +
                 " elements, but " + rel_prop + " has " +
                 std::to_string(target_count) + " targets");
  }
}

void ValidateNumericArrayLengthMatchesRelationship(
    const PrimSpec &ps, const std::string &array_prop,
    const std::string &rel_prop, const std::string &rule_id,
    const std::string &prim_location, USDValidationResult *result) {
  if (!HasProperty(ps, array_prop)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, array_prop);
  std::vector<double> values;
  if (IsRelationshipProp(ps, array_prop) ||
      !GetDoubleArrayProperty(ps, array_prop, &values)) {
    AddError(result, rule_id, location,
             array_prop + " must have a numeric array value");
    return;
  }
  size_t target_count = 0;
  if (!GetRelationshipTargetCount(ps, rel_prop, &target_count)) {
    AddError(result, rule_id, location,
             array_prop + " requires relationship " + rel_prop);
    return;
  }
  if (array_prop == "mjc:path:coef") {
    std::vector<int32_t> route;
    if (GetIntArrayProperty(ps, "mjc:path:indices", &route))
      target_count = route.size();
  }
  if (values.size() != target_count) {
    AddError(result, rule_id, location,
             array_prop + " has " + std::to_string(values.size()) +
                 " elements, but " + rel_prop + " has " +
                 std::to_string(target_count) + " targets");
  }
}

void ValidateNumericMinMaxPair(const PrimSpec &ps, const std::string &min_prop,
                               const std::string &max_prop,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  for (const std::string &prop_name : {min_prop, max_prop}) {
    if (HasProperty(ps, prop_name)) {
      double value = 0.0;
      if (IsRelationshipProp(ps, prop_name) ||
          !GetNumericScalarProperty(ps, prop_name, &value) ||
          !std::isfinite(value)) {
        AddError(result, rule_id,
                 MakePropertyLocation(prim_location, prop_name),
                 prop_name + " must have a finite numeric scalar value");
      }
    }
  }
  double min_value = 0.0;
  double max_value = 0.0;
  if (GetNumericScalarProperty(ps, min_prop, &min_value) &&
      GetNumericScalarProperty(ps, max_prop, &max_value) &&
      min_value > max_value) {
    AddError(result, rule_id, prim_location,
             min_prop + " must be <= " + max_prop);
  }
}

void ValidateArrayLengthProperty(const PrimSpec &ps,
                                 const std::string &prop_name, size_t expected,
                                 bool allow_singleton,
                                 const std::string &rule_id,
                                 const std::string &prim_location,
                                 const std::string &expected_name,
                                 USDValidationResult *result) {
  if (!HasAttributeProp(ps, prop_name)) {
    return;
  }
  const std::string location = MakePropertyLocation(prim_location, prop_name);
  const Value *v = GetAttrValue(ps, prop_name);
  size_t count = 0;
  if (v && GetValueArrayLength(*v, &count) &&
      !IsLengthCompatible(count, expected, allow_singleton)) {
    AddError(result, rule_id, location,
             prop_name + " has " + std::to_string(count) +
                 " elements, but must have " +
                 (allow_singleton ? "1 or " : "") + std::to_string(expected) +
                 " elements to match " + expected_name);
  }
  const auto *samples = GetTimeSamplesOf(ps, prop_name);
  if (!samples) {
    return;
  }
  ValidateTimeSamplesProp(ps, prop_name, rule_id, location, result);
  for (size_t i = 0; i < samples->size(); i++) {
    const Value *sv = ps.time_sample_value((*samples)[i].second);
    if (!sv || sv->is_empty() || sv->is_block()) {
      continue;
    }
    size_t sample_count = 0;
    if (!GetValueArrayLength(*sv, &sample_count)) {
      AddWarning(result, rule_id, location,
                 prop_name + " timeSamples sample " + std::to_string(i) +
                     " should be an array value");
      continue;
    }
    if (!IsLengthCompatible(sample_count, expected, allow_singleton)) {
      AddError(result, rule_id, location,
               prop_name + " timeSamples sample " + std::to_string(i) +
                   " at time " + std::to_string((*samples)[i].first) +
                   " has " + std::to_string(sample_count) +
                   " elements, but must have " +
                   (allow_singleton ? "1 or " : "") +
                   std::to_string(expected) + " elements to match " +
                   expected_name);
    }
  }
}

void ValidateNumericRangeProperty(const PrimSpec &ps,
                                  const std::string &prop_name,
                                  double min_value, double max_value,
                                  const std::string &rule_id,
                                  const std::string &prim_location,
                                  USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value) || value < min_value || value > max_value) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite and in range [" +
                 std::to_string(min_value) + ", " +
                 std::to_string(max_value) + "]");
  }
}

void ValidateNumericMinProperty(const PrimSpec &ps,
                                const std::string &prop_name, double min_value,
                                const std::string &rule_id,
                                const std::string &prim_location,
                                USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value) || value < min_value) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite and >= " + std::to_string(min_value));
  }
}

void ValidateFiniteNumericProperty(const PrimSpec &ps,
                                   const std::string &prop_name,
                                   const std::string &rule_id,
                                   const std::string &prim_location,
                                   USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  double value = 0.0;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetNumericScalarProperty(ps, prop_name, &value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be a numeric attribute");
    return;
  }
  if (!std::isfinite(value)) {
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be finite");
  }
}

void ValidateAssetPathProperty(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &rule_id,
                               const std::string &prim_location,
                               USDValidationResult *result) {
  if (!HasProperty(ps, prop_name)) {
    return;
  }
  std::string asset;
  if (IsRelationshipProp(ps, prop_name) ||
      !GetAssetPathProperty(ps, prop_name, &asset)) {
    // A connected asset input has no local value; treat a connection-only
    // property as satisfying the "asset attribute" requirement.
    if (HasConnections(ps, prop_name) && !IsRelationshipProp(ps, prop_name)) {
      return;
    }
    AddError(result, rule_id, MakePropertyLocation(prim_location, prop_name),
             prop_name + " must be an asset attribute");
    return;
  }
  if (asset.empty()) {
    AddWarning(result, rule_id, MakePropertyLocation(prim_location, prop_name),
               prop_name + " is authored as an empty asset path");
  }
}

// ---------------------------------------------------------------------------
// Relationship / connection target validators
// ---------------------------------------------------------------------------

const std::string *FindPrimType(const PrimTypeByPath &prim_types,
                                const std::string &prim_path) {
  auto it = prim_types.find(prim_path);
  if (it == prim_types.end()) {
    return nullptr;
  }
  return &it->second;
}

const PrimSpecifier *FindPrimSpecifier(
    const PrimSpecifierByPath &prim_specifiers, const std::string &prim_path) {
  auto it = prim_specifiers.find(prim_path);
  if (it == prim_specifiers.end()) {
    return nullptr;
  }
  return &it->second;
}

// Targets must be absolute non-root prim paths; empty target list warns.
void ValidateRelationshipTargetPaths(const PrimSpec &ps,
                                     const std::string &rel_name,
                                     const std::string &rule_id,
                                     const std::string &location,
                                     USDValidationResult *result) {
  if (!IsRelationshipProp(ps, rel_name)) {
    AddError(result, rule_id, location, "expected a relationship");
    return;
  }
  const std::vector<Path> &targets = RelTargets(ps, rel_name);
  if (targets.empty()) {
    AddWarning(result, rule_id, location, "relationship has no targets");
    return;
  }
  for (const Path &target : targets) {
    std::string err;
    if (!IsValidAbsolutePrimTargetPathString(target.str(), &err)) {
      AddError(result, rule_id, location,
               "contains invalid target path `" + target.str() + "`: " + err);
    }
  }
}

// Targets may be scene paths (prim or property paths); used by CollectionAPI.
void ValidateRelationshipScenePaths(const PrimSpec &ps,
                                    const std::string &rel_name,
                                    const std::string &rule_id,
                                    const std::string &location,
                                    USDValidationResult *result) {
  for (const Path &target : RelTargets(ps, rel_name)) {
    std::string err;
    if (!IsValidScenePathString(target.str(), &err)) {
      AddError(result, rule_id, location,
               "contains invalid target path `" + target.str() + "`: " + err);
    }
  }
}

void ValidateRelationshipTargetPrimTypes(
    const PrimSpec &ps, const std::string &rel_name,
    const std::string &rule_id, const std::string &location,
    const PrimTypeByPath &prim_types,
    const std::set<std::string> &expected_types, USDValidationResult *result) {
  if (!IsRelationshipProp(ps, rel_name)) {
    return;
  }
  for (const Path &target : RelTargets(ps, rel_name)) {
    std::string prim_part;
    SplitScenePathString(target.str(), &prim_part, nullptr);
    const std::string *target_type = FindPrimType(prim_types, prim_part);
    if (target_type && !target_type->empty() &&
        expected_types.count(*target_type) == 0) {
      AddWarning(result, rule_id, location,
                 "relationship target `" + prim_part + "` has type `" +
                     *target_type + "`");
    }
  }
}

void ValidateConnectionTargets(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &rule_id,
                               const std::string &location,
                               const PrimTypeByPath &prim_types,
                               USDValidationResult *result,
                               bool require_output_target) {
  const std::vector<Path> *conns = ps.connection(prop_name);
  if (!conns) {
    return;
  }
  for (const Path &target : *conns) {
    std::string err;
    if (!IsValidConnectionTargetPathString(target.str(), &err)) {
      AddError(result, rule_id, location,
               "contains invalid connection target `" + target.str() +
                   "`: " + err);
      continue;
    }
    std::string prim_part;
    std::string target_prop;
    SplitScenePathString(target.str(), &prim_part, &target_prop);
    if (require_output_target && !IsShaderOutputName(target_prop)) {
      AddWarning(result, rule_id, location,
                 "connection target `" + target.str() +
                     "` is not an outputs:* source");
    } else if (!require_output_target && !IsShaderOutputName(target_prop) &&
               !IsShaderInputName(target_prop)) {
      AddWarning(result, rule_id, location,
                 "connection target `" + target.str() +
                     "` is not an inputs:* or outputs:* shade port");
    }
    const std::string *target_type = FindPrimType(prim_types, prim_part);
    if (target_type && !target_type->empty() &&
        !IsShadeConnectionTargetTypeName(*target_type)) {
      AddWarning(result, rule_id, location,
                 "connection target prim `" + prim_part + "` has type `" +
                     *target_type + "`, expected Material, Shader, or "
                     "NodeGraph");
    }
  }
}

void ValidateDictionaryKeys(DictView dict, const std::string &rule_id,
                            const std::string &location,
                            USDValidationResult *result) {
  for (const auto &entry : dict.entries()) {
    const std::string key_location = location + "." + entry.first;
    if (!IsValidNamespacedIdentifier(entry.first)) {
      AddError(result, rule_id, location,
               "dictionary key `" + entry.first +
                   "` is not a valid namespaced identifier");
    }
    if (auto child = entry.second.as_dictionary()) {
      ValidateDictionaryKeys(*child, rule_id, key_location, result);
    }
  }
}

void ValidateDictValue(const Value &v, const std::string &rule_id,
                       const std::string &location,
                       USDValidationResult *result) {
  if (auto dict = v.as_dictionary()) {
    ValidateDictionaryKeys(*dict, rule_id, location, result);
  }
}

}  // namespace validation_detail
}  // namespace next
}  // namespace lightusd
