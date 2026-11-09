// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita & Light Transport Entertainment Inc.
//
// Variant Applier for RenderScene
// Applies variant selections to a RenderScene by swapping meshes, materials, and other content
//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../tiny-hashmap.hh"

namespace lightusd {
namespace tydra {

// Forward declarations
class RenderScene;
class VariantManager;
struct VariantSelection;

/// Describes changes to apply when switching variants
struct VariantContentChange {
  enum class ChangeType {
    MeshSwap,           // Swap mesh reference in node
    MaterialSwap,       // Swap material on mesh
    NodeVisibility,     // Show/hide node
    AnimationSwap,      // Swap animation
  };

  ChangeType change_type;
  std::string prim_path;
  int32_t old_value{-1};  // Old mesh/material/animation id
  int32_t new_value{-1};  // New mesh/material/animation id
};

/// Applier for USD variant selections to RenderScene
/// Enables dynamic variant switching during rendering by swapping content in RenderScene
class VariantApplier {
 public:
  VariantApplier() = default;
  ~VariantApplier() = default;
  VariantApplier(const VariantApplier &) = delete;
  VariantApplier &operator=(const VariantApplier &) = delete;
  VariantApplier(VariantApplier &&) = default;
  VariantApplier &operator=(VariantApplier &&) = default;

  /// Apply a single variant selection to the RenderScene
  /// @param scene The RenderScene to modify
  /// @param prim_path Path to the Prim with variant
  /// @param variant_set_name Name of the variant set
  /// @param variant_option_name Name of the variant option
  /// @param err Error message (optional)
  /// @return true if selection was applied successfully
  bool ApplyVariantSelection(RenderScene *scene, const std::string &prim_path,
                             const std::string &variant_set_name,
                             const std::string &variant_option_name,
                             std::string *err = nullptr);

  /// Apply multiple variant selections atomically
  /// @param scene The RenderScene to modify
  /// @param selections Vector of variant selections to apply
  /// @param err Error message (optional)
  /// @return true if all selections were applied successfully
  bool ApplyVariantSelections(RenderScene *scene,
                              const std::vector<VariantSelection> &selections,
                              std::string *err = nullptr);

  /// Reset all variant selections to defaults
  /// @param scene The RenderScene to reset
  /// @param manager The VariantManager containing default information
  /// @param err Error message (optional)
  /// @return true if reset was successful
  bool ResetToDefaults(RenderScene *scene, const VariantManager &manager,
                       std::string *err = nullptr);

  /// Get the content changes that were made for last applied selection
  /// Useful for tracking what changed for undo/redo
  const std::vector<VariantContentChange> &GetLastChanges() const {
    return last_content_changes_;
  }

  /// Clear change history
  void ClearChangeHistory() { last_content_changes_.clear(); }

  /// Set whether to cache variant content for faster switching
  /// When enabled, extracted variant content is cached to avoid repeated lookups
  void SetEnableCaching(bool enable) { enable_caching_ = enable; }

 private:
  /// Extract content from a variant option and prepare it for rendering
  /// This involves finding meshes, materials, and animations defined in the variant
  bool ExtractVariantContent(RenderScene *scene, const std::string &prim_path,
                             const std::string &variant_set_name,
                             const std::string &variant_option_name,
                             std::string *err);

  /// Apply mesh swap for a node
  bool SwapNodeMesh(RenderScene *scene, const std::string &node_abs_path,
                    int32_t new_mesh_id, std::string *err);

  /// Apply material swap for a mesh
  bool SwapMeshMaterial(RenderScene *scene, int32_t mesh_id, int32_t new_material_id,
                        std::string *err);

  /// Set node visibility based on variant content
  bool SetNodeVisibility(RenderScene *scene, const std::string &node_abs_path,
                         bool visible, std::string *err);

  /// Find or create a mesh variant for the given variant option
  /// Returns mesh id on success, -1 on error
  int32_t FindVariantMesh(RenderScene *scene, const std::string &prim_path,
                          const std::string &variant_option_name);

  /// Find or create a material variant for the given variant option
  /// Returns material id on success, -1 on error
  int32_t FindVariantMaterial(RenderScene *scene, const std::string &prim_path,
                              const std::string &variant_option_name);

  std::vector<VariantContentChange> last_content_changes_;
  bool enable_caching_{false};

  // Cache for variant content lookups: {prim_path + variant_option -> mesh_id}
  lightusd::HashMap<std::string, int32_t> variant_mesh_cache_;
  // Cache for variant materials: {prim_path + variant_option -> material_id}
  lightusd::HashMap<std::string, int32_t> variant_material_cache_;
};

}  // namespace tydra
}  // namespace lightusd
