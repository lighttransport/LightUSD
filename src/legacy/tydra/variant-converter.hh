// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita & Light Transport Entertainment Inc.
//
// Variant Converter for LightUSD RenderScene
// Converts USD variant structures into RenderScene variant groups
//

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lightusd {

// Forward declarations
class Stage;
class Prim;
struct Variant;
struct VariantSet;
struct GeomMesh;
struct Material;

namespace tydra {

// Forward declarations
class RenderScene;
struct VariantGroup;
struct VariantSet;
struct VariantOption;

/// Converter for USD variants to RenderScene variants
/// Extracts variant information from a USD Stage and populates
/// the RenderScene's variant structures
class VariantConverter {
 public:
  VariantConverter() = default;
  ~VariantConverter() = default;
  VariantConverter(const VariantConverter &) = delete;
  VariantConverter &operator=(const VariantConverter &) = delete;
  VariantConverter(VariantConverter &&) = default;
  VariantConverter &operator=(VariantConverter &&) = default;

  /// Convert variants from a USD Stage into RenderScene
  /// @param stage The USD Stage to extract variants from
  /// @param scene The RenderScene to populate with variant data
  /// @param err Error message (optional)
  /// @return true if conversion succeeded
  bool ConvertVariants(const Stage &stage, RenderScene *scene, std::string *err = nullptr);

  /// Set maximum nesting depth for variants (default: 32)
  void set_max_nesting_depth(uint32_t depth) { max_nesting_depth_ = depth; }

 private:
  /// Extract VariantGroup from a Prim with variantSets
  /// @param prim The USD Prim to extract from
  /// @param scene The RenderScene to add to
  /// @param err Error message (optional)
  /// @return Index of the created VariantGroup, or -1 on error
  int32_t ExtractVariantGroup(const Prim &prim, RenderScene *scene, std::string *err);

  /// Extract a single VariantSet from USD variantSet structure
  /// @param variant_set_name Name of the variant set
  /// @param usd_variant_set USD variantSet structure
  /// @param scene The RenderScene for content reference
  /// @param err Error message
  /// @return The extracted VariantSet, or empty on error
  tydra::VariantSet ExtractVariantSetDefinition(
      const std::string &variant_set_name,
      const lightusd::VariantSet &usd_variant_set,
      RenderScene *scene,
      std::string *err);

  /// Extract a single VariantOption from a USD Variant
  /// @param variant_name Name of this variant option
  /// @param variant USD Variant object
  /// @param scene The RenderScene
  /// @param err Error message
  /// @return The extracted VariantOption
  VariantOption ExtractVariantOption(
      const std::string &variant_name,
      const lightusd::Variant &variant,
      RenderScene *scene,
      std::string *err);

  /// Recursively extract nested variantSets within a variant option
  /// @param variant USD Variant containing nested variantSets
  /// @param scene The RenderScene
  /// @param err Error message
  /// @return List of nested VariantSets
  std::vector<std::shared_ptr<tydra::VariantSet>> ExtractNestedVariantSets(
      const lightusd::Variant &variant,
      RenderScene *scene,
      std::string *err);

  /// Traverse a Prim tree and extract all variants
  /// @param prim The Prim to traverse
  /// @param scene The RenderScene
  /// @param err Error message
  bool TraverseForVariants(const Prim &prim, RenderScene *scene, std::string *err);

  /// Extract variant selections from Prim metadata
  /// @param prim The Prim with variant selections
  /// @param group_index Index of the VariantGroup
  /// @param scene The RenderScene
  /// @param err Error message
  bool ExtractVariantSelections(
      const Prim &prim,
      int32_t group_index,
      RenderScene *scene,
      std::string *err);

  uint32_t max_nesting_depth_ = 32;
};

}  // namespace tydra
}  // namespace lightusd
