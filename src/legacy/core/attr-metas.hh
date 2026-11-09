// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// attr-metas.hh - AttrMetas structure for Property/Attribute metadata
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include "metadata-base.hh"
#include "prim-enums.hh"
#include "value-types.hh"

namespace lightusd {

// Metadata for Property(Relationship and Attribute)
// Uses Dictionary-based storage via MetadataBase for memory efficiency.
// Provides typed accessor methods for commonly used metadata fields.
//
// Example usage:
//   AttrMetas meta;
//   meta.set_displayName("myAttr");
//   if (meta.has_interpolation()) {
//     Interpolation interp = meta.get_interpolation_enum();
//   }
//
struct AttrMetas : public MetadataBase {
  AttrMetas() = default;
  AttrMetas(const AttrMetas&) = default;
  AttrMetas(AttrMetas&&) noexcept = default;
  AttrMetas& operator=(const AttrMetas&) = default;
  AttrMetas& operator=(AttrMetas&&) noexcept = default;

  //
  // Interpolation - needs special handling for enum conversion
  //
  bool has_interpolation_enum() const { return has_interpolation(); }

  Interpolation get_interpolation_enum() const {
    value::token tok = get_interpolation();
    if (tok.str() == "constant") return Interpolation::Constant;
    if (tok.str() == "uniform") return Interpolation::Uniform;
    if (tok.str() == "varying") return Interpolation::Varying;
    if (tok.str() == "vertex") return Interpolation::Vertex;
    if (tok.str() == "faceVarying") return Interpolation::FaceVarying;
    return Interpolation::Invalid;
  }

  void set_interpolation_enum(Interpolation value) {
    const char* s = "constant";
    switch (value) {
      case Interpolation::Constant: s = "constant"; break;
      case Interpolation::Uniform: s = "uniform"; break;
      case Interpolation::Varying: s = "varying"; break;
      case Interpolation::Vertex: s = "vertex"; break;
      case Interpolation::FaceVarying: s = "faceVarying"; break;
      case Interpolation::Invalid: s = ""; break;
    }
    set_interpolation(s);
  }

  //
  // displayGroup (string)
  //
  static constexpr const char* kDisplayGroup = "displayGroup";

  bool has_displayGroup() const { return has(kDisplayGroup); }

  std::string get_displayGroup() const {
    auto v = get<std::string>(kDisplayGroup);
    return v.has_value() ? v.value() : std::string();
  }

  void set_displayGroup(const std::string& value) {
    set(kDisplayGroup, value);
  }

  void remove_displayGroup() { remove(kDisplayGroup); }

  //
  // unauthoredValuesIndex - usdSkel specific
  //
  static constexpr const char* kUnauthoredValuesIndex = "unauthoredValuesIndex";

  bool has_unauthoredValuesIndex() const { return has(kUnauthoredValuesIndex); }

  int get_unauthoredValuesIndex() const {
    auto v = get<int>(kUnauthoredValuesIndex);
    return v.has_value() ? v.value() : -1;
  }

  void set_unauthoredValuesIndex(int value) {
    set(kUnauthoredValuesIndex, value);
  }

  //
  // allowedTokens (token[])
  //
  static constexpr const char* kAllowedTokens = "allowedTokens";

  bool has_allowedTokens() const { return has(kAllowedTokens); }

  std::vector<value::token> get_allowedTokens() const {
    auto v = get<std::vector<value::token>>(kAllowedTokens);
    return v.has_value() ? v.value() : std::vector<value::token>();
  }

  void set_allowedTokens(const std::vector<value::token>& value) {
    set(kAllowedTokens, value);
  }

  void remove_allowedTokens() { remove(kAllowedTokens); }

  //
  // String-only metadata (legacy support)
  // TODO: Migrate to use comment field instead
  //
  std::vector<value::StringData> stringData;

  // Unregistered (unknown-to-lightusd) property metadata, preserved verbatim as
  // the raw USDA text of the value (quotes included for string values), keyed by
  // the metadatum name. OpenUSD preserves such opinions (SdfUnregisteredValue);
  // dropping them silently loses authored data. Mirrors PrimMeta::unregisteredMetas.
  std::map<std::string, std::string> unregisteredMetas;

  //
  // Check if any metadata is authored
  //
  bool authored() const {
    return MetadataBase::authored() || !stringData.empty() ||
           !unregisteredMetas.empty();
  }

  //
  // Legacy accessor aliases for backwards compatibility
  // These will be deprecated in future versions
  //

  // Legacy: direct field access pattern -> use accessor methods instead
  // Example migration:
  //   Old: if (meta.interpolation) { ... }
  //   New: if (meta.has_interpolation()) { ... }
  //
  //   Old: meta.displayName = "foo";
  //   New: meta.set_displayName("foo");
};

// For backward compatibility
using AttrMeta = AttrMetas;

using PropMetas = AttrMetas;

}  // namespace lightusd
