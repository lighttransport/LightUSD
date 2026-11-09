// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita & Light Transport Entertainment Inc.
//
// Variant Support for Tydra RenderScene
// Inspired by glTF KHR_materials_variants extension, adapted for USD
//

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "nonstd/optional.hpp"
#include "../tiny-hashmap.hh"

namespace lightusd {
namespace tydra {

// Forward declaration for nested variants
struct VariantSet;

///
/// A single variant option within a VariantSet.
/// Maps to USD variant content.
///
/// Example: "red", "blue", "green" for a "color" variantSet
///
struct VariantOption {
  std::string name;  // Variant option name (e.g., "red", "blue", "high_poly")
  std::string description;  // Optional human-readable description

  // Content references - what changes in this variant
  // These are indices into RenderScene arrays
  std::vector<int32_t> mesh_ids;        // Mesh IDs to enable for this variant
  std::vector<int32_t> material_ids;    // Material IDs to apply
  std::vector<int32_t> node_ids;        // Node IDs with visibility/property changes
  std::vector<int32_t> animation_ids;   // Animation IDs specific to this variant

  // Property overrides as string-based key-value pairs
  // Format: "prim_path.property_name" -> "value"
  std::map<std::string, std::string> property_overrides;

  // Nested variant sets - supports USD's nested variant hierarchy
  // Fully qualified to avoid namespace collision with lightusd::VariantSet
  std::vector<std::shared_ptr<VariantSet>> nested_variant_sets;

  bool operator==(const VariantOption& other) const {
    return name == other.name;
  }
};

///
/// A set of mutually exclusive variant options.
///
/// Example: variantSet "color" with options ["red", "blue", "green"]
/// Example: variantSet "lod" with options ["high", "medium", "low"]
///
struct VariantSet {
  std::string name;  // VariantSet name (e.g., "color", "lod", "material_type")

  std::vector<VariantOption> options;  // All variant options in this set

  int32_t default_option_index{0};  // Index to default variant option (0 = first)

  // Parent information for nested variants
  int32_t parent_prim_id{-1};  // Parent node/prim ID if nested
  std::string parent_variant_option_name;  // Parent variant option that contains this

  bool operator==(const VariantSet& other) const {
    return name == other.name;
  }
};

///
/// A group of related variant sets for a specific prim/node.
///
/// Represents all variant information for a USD prim.
/// Example: All variants affecting the "Car" prim
///
struct VariantGroup {
  std::string prim_path;  // USD prim path (e.g., "/root/geo", "/Characters/Car")

  std::vector<VariantSet> variant_sets;  // Available variant sets for this prim

  int32_t affected_node_id{-1};  // Index to primary affected Node in RenderScene::nodes

  // Additional affected nodes (for complex variants affecting multiple nodes)
  std::vector<int32_t> secondary_node_ids;

  bool operator==(const VariantGroup& other) const {
    return prim_path == other.prim_path;
  }
};

///
/// Current active variant selection.
///
/// Represents a choice within a specific variant set.
///
struct VariantSelection {
  std::string variant_set_name;      // Name of the variantSet
  std::string variant_option_name;   // Name of selected option
  std::string prim_path;             // Path to the prim with this variantSet

  VariantSelection() = default;

  VariantSelection(const std::string& set_name, const std::string& option_name,
                   const std::string& path = "")
      : variant_set_name(set_name),
        variant_option_name(option_name),
        prim_path(path) {}

  bool operator==(const VariantSelection& other) const {
    return variant_set_name == other.variant_set_name &&
           variant_option_name == other.variant_option_name &&
           prim_path == other.prim_path;
  }

  bool operator!=(const VariantSelection& other) const {
    return !(*this == other);
  }
};

///
/// Statistics about variants in a RenderScene
///
struct VariantStatistics {
  uint32_t num_variant_groups{0};     // Total number of VariantGroups
  uint32_t num_variant_sets{0};       // Total number of VariantSets
  uint32_t num_variant_options{0};    // Total number of VariantOptions
  uint32_t num_active_selections{0};  // Current active selections
  uint32_t max_nesting_depth{0};      // Maximum nesting depth
};

///
/// Variant query and management interface
///
/// Provides high-level API for working with variants in RenderScene
///
class VariantManager {
 public:
  VariantManager() = default;
  virtual ~VariantManager();

  ///
  /// Get all available variant groups
  ///
  virtual const std::vector<VariantGroup>& GetVariantGroups() const = 0;

  ///
  /// Find variant group by prim path
  ///
  virtual const VariantGroup* FindVariantGroup(
      const std::string& prim_path) const = 0;

  ///
  /// Find variant set by name within a group
  ///
  virtual const VariantSet* FindVariantSet(const std::string& prim_path,
                                           const std::string& variant_set_name) const = 0;

  ///
  /// Find variant option within a set
  ///
  virtual const VariantOption* FindVariantOption(
      const std::string& prim_path, const std::string& variant_set_name,
      const std::string& variant_option_name) const = 0;

  ///
  /// Select a variant option
  /// Returns true if selection is valid and applied
  ///
  virtual bool SelectVariant(const std::string& prim_path,
                            const std::string& variant_set_name,
                            const std::string& variant_option_name) = 0;

  ///
  /// Select variant option by index
  ///
  virtual bool SelectVariantByIndex(const std::string& prim_path,
                                   const std::string& variant_set_name,
                                   uint32_t option_index) = 0;

  ///
  /// Get current selection for a variant set
  ///
  virtual nonstd::optional<VariantSelection> GetCurrentSelection(
      const std::string& prim_path,
      const std::string& variant_set_name) const = 0;

  ///
  /// Get all current selections
  ///
  virtual const std::vector<VariantSelection>& GetAllSelections() const = 0;

  ///
  /// Reset selections to defaults
  ///
  virtual void ResetToDefaults() = 0;

  ///
  /// Get variant statistics
  ///
  virtual VariantStatistics GetStatistics() const = 0;

  ///
  /// Check if prim has any variants
  ///
  virtual bool HasVariants(const std::string& prim_path) const = 0;

  ///
  /// Check if variant set exists
  ///
  virtual bool VariantSetExists(const std::string& prim_path,
                               const std::string& variant_set_name) const = 0;
};

///
/// Default implementation of VariantManager
///
class DefaultVariantManager : public VariantManager {
 public:
  DefaultVariantManager() = default;

  const std::vector<VariantGroup>& GetVariantGroups() const override;

  const VariantGroup* FindVariantGroup(const std::string& prim_path) const override;

  const VariantSet* FindVariantSet(const std::string& prim_path,
                                   const std::string& variant_set_name) const override;

  const VariantOption* FindVariantOption(
      const std::string& prim_path, const std::string& variant_set_name,
      const std::string& variant_option_name) const override;

  bool SelectVariant(const std::string& prim_path,
                    const std::string& variant_set_name,
                    const std::string& variant_option_name) override;

  bool SelectVariantByIndex(const std::string& prim_path,
                           const std::string& variant_set_name,
                           uint32_t option_index) override;

  nonstd::optional<VariantSelection> GetCurrentSelection(
      const std::string& prim_path,
      const std::string& variant_set_name) const override;

  const std::vector<VariantSelection>& GetAllSelections() const override;

  void ResetToDefaults() override;

  VariantStatistics GetStatistics() const override;

  bool HasVariants(const std::string& prim_path) const override;

  bool VariantSetExists(const std::string& prim_path,
                       const std::string& variant_set_name) const override;

  // Internal methods for converter to populate variants
  std::vector<VariantGroup>& GetMutableVariantGroups() {
    return _variant_groups;
  }

  void AddVariantGroup(const VariantGroup& group) {
    _variant_groups.push_back(group);
    _variant_group_map[group.prim_path] = static_cast<int32_t>(_variant_groups.size() - 1);
  }

 private:
  std::vector<VariantGroup> _variant_groups;
  std::vector<VariantSelection> _current_selections;
  lightusd::HashMap<std::string, int32_t> _variant_group_map;  // prim_path -> index
};

///
/// Helper functions for variant management
///

///
/// Convert variant selection to human-readable string
///
std::string VariantSelectionToString(const VariantSelection& sel);

///
/// Check if all selected variants are valid
///
bool ValidateVariantSelections(const VariantManager& manager,
                              const std::vector<VariantSelection>& selections);

///
/// Get variant hierarchy path (for nested variants)
///
std::string GetVariantHierarchyPath(const VariantGroup& group,
                                   const VariantSet& variant_set);

}  // namespace tydra
}  // namespace lightusd
