// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// prim-metas.hh - PrimMetas structure for Prim metadata
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include "nonstd/optional.hpp"
#include "metadata-base.hh"
#include "composition-types.hh"
#include "prim-enums.hh"
#include "value-types.hh"

namespace lightusd {

// Metadata for Prim
// Uses Dictionary-based storage via MetadataBase for memory efficiency.
// Simple metadata fields (active, hidden, doc, comment, displayName, sceneName,
// instanceable, customData, sdrMetadata, assetInfo, clips) are stored in MetadataBase.
// Composition fields and structural fields remain as direct members.
//
// Example usage:
//   PrimMetas meta;
//   meta.set_active(false);
//   meta.set_kind(Kind::Component);
//   if (meta.has_displayName()) {
//     std::string name = meta.get_displayName();
//   }
//
struct PrimMetas : public MetadataBase {
  PrimMetas() = default;
  PrimMetas(const PrimMetas &) = default;
  PrimMetas(PrimMetas &&) noexcept = default;
  PrimMetas &operator=(const PrimMetas &) = default;
  PrimMetas &operator=(PrimMetas &&) noexcept = default;

  //
  // Kind - needs special handling for enum conversion
  //
  static constexpr const char* kKind = "kind";

  bool has_kind() const { return has(kKind); }

  Kind get_kind_enum() const {
    auto v = get<value::token>(kKind);
    if (!v.has_value()) {
      return Kind::Invalid;
    }
    const std::string& s = v.value().str();
    if (s == "model") return Kind::Model;
    if (s == "group") return Kind::Group;
    if (s == "assembly") return Kind::Assembly;
    if (s == "component") return Kind::Component;
    if (s == "subcomponent") return Kind::Subcomponent;
    if (s == "sceneLibrary") return Kind::SceneLibrary;
    // User-defined kind
    return Kind::UserDef;
  }

  // Get the string representation of kind (works for user-defined kinds too)
  std::string get_kind_str() const {
    auto v = get<value::token>(kKind);
    return v.has_value() ? v.value().str() : std::string();
  }

  void set_kind(Kind k) {
    const char* s = "";
    switch (k) {
      case Kind::Model: s = "model"; break;
      case Kind::Group: s = "group"; break;
      case Kind::Assembly: s = "assembly"; break;
      case Kind::Component: s = "component"; break;
      case Kind::Subcomponent: s = "subcomponent"; break;
      case Kind::SceneLibrary: s = "sceneLibrary"; break;
      case Kind::UserDef: s = ""; break;  // Use set_kind(string) for custom
      case Kind::Invalid: s = ""; break;
    }
    if (s[0] != '\0') {
      set(kKind, value::token(s));
    }
  }

  void set_kind(const std::string& kind_str) {
    set(kKind, value::token(kind_str));
  }

  void remove_kind() { remove(kKind); }

  // Legacy: get_kind() returns string for backward compatibility
  const std::string get_kind() const { return get_kind_str(); }

  //
  // apiSchemas - needs special handling for APISchemas type
  //
  static constexpr const char* kApiSchemas = "apiSchemas";

  bool has_apiSchemas() const { return _apiSchemas.has_value(); }

  const APISchemas* get_apiSchemas_ptr() const {
    return _apiSchemas.has_value() ? &_apiSchemas.value() : nullptr;
  }

  APISchemas get_apiSchemas() const {
    return _apiSchemas.has_value() ? _apiSchemas.value() : APISchemas();
  }

  APISchemas& get_apiSchemas_mutable() {
    if (!_apiSchemas.has_value()) {
      _apiSchemas = APISchemas();
    }
    return _apiSchemas.value();
  }

  void set_apiSchemas(const APISchemas& schemas) {
    _apiSchemas = schemas;
  }

  void remove_apiSchemas() { _apiSchemas = nonstd::nullopt; }

  //
  // AssetInfo utility function
  //
  // Convert Dictionary assetInfo to AssetInfo struct
  AssetInfo get_assetInfo_struct(bool *authored = nullptr) const;

  //
  // Compositions - keep as direct members due to complex types
  // Uses vector of pairs to support multiple listops per arc type
  // (e.g., "delete references" + "prepend references" on same prim)
  //
  nonstd::optional<std::vector<std::pair<ListEditQual, std::vector<Reference>>>> references;
  nonstd::optional<std::vector<std::pair<ListEditQual, std::vector<Payload>>>>
      payload;  // NOTE: not `payloads`
  nonstd::optional<std::vector<std::pair<ListEditQual, std::vector<Path>>>>
      inherits;  // 'inherits'
  nonstd::optional<std::vector<std::pair<ListEditQual, std::vector<std::string>>>>
      variantSets;  // 'variantSets'. Could be `token` but treat as
                    // `string`(Crate format uses `string`)

  nonstd::optional<VariantSelectionMap> variants;  // `variants`

  nonstd::optional<std::vector<std::pair<ListEditQual, std::vector<Path>>>>
      specializes;  // 'specializes'

  // AOUSD Core Spec 10.3.2.3: Arc origins for implied inherit/specialize propagation
  std::vector<ArcOrigin> arc_origins;

  // Unregistered metadatum. value is represented as string.
  std::map<std::string, std::string> unregisteredMetas;

  Dictionary meta;  // other non-buitin meta values. TODO: remove this variable
                    // and use `customData` instead, since pxrUSD does not allow
                    // non-builtin Prim metadatum

  ///
  /// Update metadatum with rhs(authored metadataum only)
  ///
  /// @param[in] override_authored true: override this.metadataum(authored or not-authored) when rhs.metadatum is authoerd, false override only when this.metadatum is not authored and rhs.metadataum is authored.
  ///
  void update_from(const PrimMetas &rhs, bool override_authored = true);

  // Check if any metadata is authored
  bool authored() const {
    return MetadataBase::authored() ||
           _apiSchemas.has_value() ||
           references.has_value() || payload.has_value() ||
           inherits.has_value() || variants.has_value() ||
           variantSets.has_value() || specializes.has_value() ||
           !unregisteredMetas.empty() || !meta.empty();
  }

  //
  // Infos used indirectly.
  //

  // Used to display/traverse Prim items based on this array
  // USDA: By appearance. USDC: "primChildren" TokenVector field
  std::vector<value::token> primChildren;

  // Used to display/traverse Property items based on this array
  // USDA: By appearance. USDC: "properties" TokenVector field
  std::vector<value::token> properties;

  // Authored `reorder nameChildren = [...]` / `reorder properties = [...]`
  // body statements (partial ordering hints, pxr keeps them verbatim in
  // unflattened output). Preserved for round-trip; not applied by the
  // legacy flatten engine.
  std::vector<value::token> nameChildrenReorder;
  std::vector<value::token> propertiesReorder;

  // Implied inherit/specialize paths propagated from referenced layers.
  // These are populated during reference/payload composition and consumed
  // during the inherits/specializes composition phase.
  nonstd::optional<std::vector<std::pair<ListEditQual, std::vector<Path>>>> inheritPaths;
  nonstd::optional<std::vector<std::pair<ListEditQual, std::vector<Path>>>> specializePaths;

  nonstd::optional<std::vector<value::token>> variantChildren;
  nonstd::optional<std::vector<value::token>> variantSetChildren;

 private:
  // apiSchemas needs special storage since it's a custom type
  nonstd::optional<APISchemas> _apiSchemas;
};

// For backward compatibility
using PrimMeta = PrimMetas;

}  // namespace lightusd
