// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita & Light Transport Entertainment Inc.
//
// Variant Applier Implementation
//

#include "variant-applier.hh"

#include <algorithm>
#include <sstream>

#include "render-data.hh"
#include "variant-support.hh"
#include "common-macros.inc"

namespace lightusd {
namespace tydra {

bool VariantApplier::ApplyVariantSelection(RenderScene *scene,
                                           const std::string &prim_path,
                                           const std::string &variant_set_name,
                                           const std::string &variant_option_name,
                                           std::string *err) {
  if (!scene) {
    if (err) {
      (*err) += "RenderScene is null\n";
    }
    return false;
  }

  // Clear previous changes
  last_content_changes_.clear();

  // Extract and prepare variant content
  if (!ExtractVariantContent(scene, prim_path, variant_set_name,
                             variant_option_name, err)) {
    if (err) {
      (*err) += "Failed to extract variant content\n";
    }
    return false;
  }

  // Update active selections
  bool found_selection = false;
  for (auto &sel : scene->active_selections) {
    if (sel.prim_path == prim_path && sel.variant_set_name == variant_set_name) {
      sel.variant_option_name = variant_option_name;
      found_selection = true;
      break;
    }
  }

  if (!found_selection) {
    VariantSelection new_sel;
    new_sel.prim_path = prim_path;
    new_sel.variant_set_name = variant_set_name;
    new_sel.variant_option_name = variant_option_name;
    scene->active_selections.push_back(new_sel);
  }

  return true;
}

bool VariantApplier::ApplyVariantSelections(
    RenderScene *scene,
    const std::vector<VariantSelection> &selections,
    std::string *err) {
  if (!scene) {
    if (err) {
      (*err) += "RenderScene is null\n";
    }
    return false;
  }

  // Clear previous changes
  last_content_changes_.clear();

  bool all_success = true;
  for (const auto &sel : selections) {
    if (!ApplyVariantSelection(scene, sel.prim_path, sel.variant_set_name,
                              sel.variant_option_name, err)) {
      all_success = false;
      if (err) {
        (*err) += "Failed to apply variant selection for prim: " + sel.prim_path +
                  " variant_set: " + sel.variant_set_name + "\n";
      }
    }
  }

  return all_success;
}

bool VariantApplier::ResetToDefaults(RenderScene *scene,
                                     const VariantManager &manager,
                                     std::string *err) {
  if (!scene) {
    if (err) {
      (*err) += "RenderScene is null\n";
    }
    return false;
  }

  // Clear previous changes
  last_content_changes_.clear();

  // Get all variant groups and extract their default selections
  const auto &variant_groups = manager.GetVariantGroups();
  bool all_success = true;

  for (const auto &group : variant_groups) {
    // For each variant set in the group, apply default option
    for (const auto &vs : group.variant_sets) {
      if (vs.default_option_index >= 0 &&
          vs.default_option_index < static_cast<int32_t>(vs.options.size())) {
        const auto &default_option = vs.options[static_cast<size_t>(vs.default_option_index)];

        if (!ApplyVariantSelection(scene, group.prim_path, vs.name,
                                   default_option.name, err)) {
          all_success = false;
        }
      }
    }
  }

  return all_success;
}

bool VariantApplier::ExtractVariantContent(RenderScene *scene,
                                           const std::string &prim_path,
                                           const std::string &variant_set_name,
                                           const std::string &variant_option_name,
                                           std::string *err) {
  // NOTE: This is a placeholder implementation.
  // A full implementation would:
  // 1. Find variant definitions in scene->variant_groups
  // 2. Locate the variant option
  // 3. Traverse USD structures to find referenced meshes/materials
  // 4. Map USD content to RenderScene indices
  // 5. Record content changes in last_content_changes_

  // For now, we just track that the selection was made.
  // The actual content swapping would happen in the rendering pipeline.

  // Find the variant group for this prim path
  auto it = scene->variant_group_map.find(prim_path);
  if (it == scene->variant_group_map.end()) {
    if (err) {
      (*err) += "No variant group found for prim: " + prim_path + "\n";
    }
    return false;
  }

  int32_t group_idx = it->second;
  if (group_idx < 0 || group_idx >= static_cast<int32_t>(scene->variant_groups.size())) {
    if (err) {
      (*err) += "Invalid variant group index for prim: " + prim_path + "\n";
    }
    return false;
  }

  const auto &group = scene->variant_groups[static_cast<size_t>(group_idx)];

  // Find the variant set
  auto vs_it = std::find_if(
      group.variant_sets.begin(), group.variant_sets.end(),
      [&variant_set_name](const VariantSet &vs) { return vs.name == variant_set_name; });

  if (vs_it == group.variant_sets.end()) {
    if (err) {
      (*err) += "Variant set not found: " + variant_set_name + "\n";
    }
    return false;
  }

  // Find the variant option
  auto opt_it = std::find_if(
      vs_it->options.begin(), vs_it->options.end(),
      [&variant_option_name](const VariantOption &opt) {
        return opt.name == variant_option_name;
      });

  if (opt_it == vs_it->options.end()) {
    if (err) {
      (*err) += "Variant option not found: " + variant_option_name + "\n";
    }
    return false;
  }

  // Successfully validated the variant selection
  // Content extraction logic would go here

  return true;
}

bool VariantApplier::SwapNodeMesh(RenderScene *scene,
                                   const std::string &node_abs_path,
                                   int32_t new_mesh_id,
                                   std::string *err) {
  if (!scene) {
    return false;
  }

  // Find the node with matching absolute path
  std::function<Node *(std::vector<Node> &, const std::string &, int32_t)>
      find_node_recursive = [&](std::vector<Node> &nodes,
                                 const std::string &target_path,
                                 int32_t depth) -> Node * {
    if (size_t(depth) >= kMaxDefaultTraversalLimit) return nullptr;
    for (auto &node : nodes) {
      if (node.abs_path == target_path) {
        return &node;
      }
      if (!node.children.empty()) {
        auto *result = find_node_recursive(node.children, target_path, depth + 1);
        if (result) {
          return result;
        }
      }
    }
    return nullptr;
  };

  Node *target_node = find_node_recursive(scene->nodes, node_abs_path, 0);
  if (!target_node) {
    if (err) {
      (*err) += "Node not found: " + node_abs_path + "\n";
    }
    return false;
  }

  // Record the change
  if (target_node->id >= 0) {
    VariantContentChange change;
    change.change_type = VariantContentChange::ChangeType::MeshSwap;
    change.prim_path = node_abs_path;
    change.old_value = target_node->id;
    change.new_value = new_mesh_id;
    last_content_changes_.push_back(change);
  }

  // Apply the change
  target_node->id = new_mesh_id;
  return true;
}

bool VariantApplier::SwapMeshMaterial(RenderScene *scene, int32_t mesh_id,
                                       int32_t new_material_id,
                                       std::string *err) {
  if (!scene) {
    return false;
  }

  if (mesh_id < 0 || mesh_id >= static_cast<int32_t>(scene->meshes.size())) {
    if (err) {
      (*err) += "Invalid mesh id: " + std::to_string(mesh_id) + "\n";
    }
    return false;
  }

  auto &mesh = scene->meshes[static_cast<size_t>(mesh_id)];

  // Record the change
  VariantContentChange change;
  change.change_type = VariantContentChange::ChangeType::MaterialSwap;
  change.prim_path = mesh.abs_path;
  change.old_value = mesh.material_id;
  change.new_value = new_material_id;
  last_content_changes_.push_back(change);

  // Apply the change
  mesh.material_id = new_material_id;
  return true;
}

bool VariantApplier::SetNodeVisibility(RenderScene *scene,
                                        const std::string &node_abs_path,
                                        bool visible,
                                        std::string *err) {
  if (!scene) {
    return false;
  }

  // NOTE: RenderScene doesn't have a built-in visibility flag.
  // This would require extending RenderScene or the Node structure.
  // For now, we can use mesh_id = -1 to indicate "hidden"

  std::function<Node *(std::vector<Node> &, const std::string &, int32_t)>
      find_node_recursive = [&](std::vector<Node> &nodes,
                                 const std::string &target_path,
                                 int32_t depth) -> Node * {
    if (size_t(depth) >= kMaxDefaultTraversalLimit) return nullptr;
    for (auto &node : nodes) {
      if (node.abs_path == target_path) {
        return &node;
      }
      if (!node.children.empty()) {
        auto *result = find_node_recursive(node.children, target_path, depth + 1);
        if (result) {
          return result;
        }
      }
    }
    return nullptr;
  };

  Node *target_node = find_node_recursive(scene->nodes, node_abs_path, 0);
  if (!target_node) {
    if (err) {
      (*err) += "Node not found: " + node_abs_path + "\n";
    }
    return false;
  }

  // Record the change
  VariantContentChange change;
  change.change_type = VariantContentChange::ChangeType::NodeVisibility;
  change.prim_path = node_abs_path;
  change.old_value = (target_node->id >= 0) ? 1 : 0;
  change.new_value = visible ? 1 : 0;
  last_content_changes_.push_back(change);

  // For now, hidden nodes are indicated by mesh_id = -2 (special marker)
  // A real implementation would extend the Node struct with a visibility flag
  if (!visible) {
    target_node->id = -2;  // -2 = hidden
  } else if (target_node->id == -2) {
    // Restore to default (no mesh)
    target_node->id = -1;
  }

  return true;
}

int32_t VariantApplier::FindVariantMesh(RenderScene *scene,
                                         const std::string &prim_path,
                                         const std::string &variant_option_name) {
  (void)scene;
  if (!enable_caching_) {
    // Without caching, would need to extract from variant definitions
    // Not implemented in this basic version
    return -1;
  }

  std::string cache_key = prim_path + ":" + variant_option_name;
  auto it = variant_mesh_cache_.find(cache_key);
  if (it != variant_mesh_cache_.end()) {
    return it->second;
  }

  return -1;
}

int32_t VariantApplier::FindVariantMaterial(RenderScene *scene,
                                             const std::string &prim_path,
                                             const std::string &variant_option_name) {
  (void)scene;
  if (!enable_caching_) {
    // Without caching, would need to extract from variant definitions
    // Not implemented in this basic version
    return -1;
  }

  std::string cache_key = prim_path + ":" + variant_option_name;
  auto it = variant_material_cache_.find(cache_key);
  if (it != variant_material_cache_.end()) {
    return it->second;
  }

  return -1;
}

}  // namespace tydra
}  // namespace lightusd
