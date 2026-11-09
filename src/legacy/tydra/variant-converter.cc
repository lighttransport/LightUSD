// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita & Light Transport Entertainment Inc.
//
// Variant Converter Implementation
//

#include "variant-converter.hh"

#include <algorithm>
#include <functional>
#include <sstream>

#include "core/prim.hh"
#include "render-data.hh"
#include "stage.hh"
#include "variant-support.hh"

namespace lightusd {
namespace tydra {

bool VariantConverter::ConvertVariants(const Stage &stage, RenderScene *scene,
                                       std::string *err) {
  if (!scene) {
    if (err) {
      (*err) += "RenderScene is null\n";
    }
    return false;
  }

  // Traverse the stage's root prims to find variants
  const auto &root_prims = stage.root_prims();
  for (const auto &prim : root_prims) {
    if (!TraverseForVariants(prim, scene, err)) {
      if (err) {
        (*err) += "Failed to traverse variants in prim: " + prim.element_name() +
                  "\n";
      }
      // Continue processing other prims
    }
  }

  return true;
}

bool VariantConverter::TraverseForVariants(const Prim &prim, RenderScene *scene,
                                           std::string *err) {
  // Iterative pre-order DFS (explicit heap worklist) so deeply nested prim
  // hierarchies cannot overflow the call stack. Children are pushed in reverse
  // so they are visited left-to-right, matching the original recursive order
  // (variant group indices are assigned in traversal order).
  std::vector<const Prim *> stack;
  stack.push_back(&prim);

  while (!stack.empty()) {
    const Prim *p = stack.back();
    stack.pop_back();

    // Check if this Prim has variantSets
    const auto &variant_sets = p->variantSets();
    if (!variant_sets.empty()) {
      int32_t group_idx = ExtractVariantGroup(*p, scene, err);
      if (group_idx >= 0) {
        // Extract variant selections if they exist
        ExtractVariantSelections(*p, group_idx, scene, err);
      }
    }

    // Push children in reverse for left-to-right pre-order traversal.
    const auto &children = p->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back(&(*it));
    }
  }

  return true;
}

int32_t VariantConverter::ExtractVariantGroup(const Prim &prim,
                                               RenderScene *scene,
                                               std::string *err) {
  const auto &variant_sets = prim.variantSets();
  if (variant_sets.empty()) {
    return -1;
  }

  VariantGroup group;
  group.prim_path = prim.element_path().full_path_name();

  // Extract each variant set
  for (const auto &vs_pair : variant_sets) {
    const std::string &vs_name = vs_pair.first;
    const lightusd::VariantSet &usd_vs = vs_pair.second;

    auto extracted_vs =
        ExtractVariantSetDefinition(vs_name, usd_vs, scene, err);
    group.variant_sets.push_back(extracted_vs);
  }

  if (group.variant_sets.empty()) {
    return -1;
  }

  // Add to scene
  int32_t group_idx = static_cast<int32_t>(scene->variant_groups.size());
  scene->variant_groups.push_back(group);
  scene->variant_group_map[group.prim_path] = group_idx;

  return group_idx;
}

tydra::VariantSet VariantConverter::ExtractVariantSetDefinition(
    const std::string &variant_set_name,
    const lightusd::VariantSet &usd_variant_set,
    RenderScene *scene,
    std::string *err) {
  tydra::VariantSet vs;
  vs.name = variant_set_name;
  vs.default_option_index = 0;  // Default to first option

  // Extract each variant option
  // int32_t option_idx = 0;  // Currently unused
  for (const auto &var_pair : usd_variant_set.variantSet) {
    const std::string &var_name = var_pair.first;
    const lightusd::Variant &usd_variant = var_pair.second;

    auto extracted_option = ExtractVariantOption(var_name, usd_variant, scene, err);
    vs.options.push_back(extracted_option);

    // option_idx++;
  }

  return vs;
}

VariantOption VariantConverter::ExtractVariantOption(
    const std::string &variant_name,
    const lightusd::Variant &variant,
    RenderScene *scene,
    std::string *err) {
  VariantOption option;
  option.name = variant_name;
  option.description = "Variant option: " + variant_name;

  // Extract nested variant sets if they exist
  option.nested_variant_sets = ExtractNestedVariantSets(variant, scene, err);

  // TODO: Map variant content to RenderScene items
  // - Traverse variant's prim children and properties
  // - Map referenced meshes/materials to option indices
  // - Store animation clips if present

  return option;
}

std::vector<std::shared_ptr<tydra::VariantSet>>
VariantConverter::ExtractNestedVariantSets(const lightusd::Variant &variant,
                                            RenderScene *scene,
                                            std::string *err) {
  std::vector<std::shared_ptr<tydra::VariantSet>> nested_sets;

  const auto &usd_variant_sets = variant.variantSets();
  if (usd_variant_sets.empty()) {
    return nested_sets;
  }

  for (const auto &vs_pair : usd_variant_sets) {
    const std::string &vs_name = vs_pair.first;
    const lightusd::VariantSet &usd_vs = vs_pair.second;

    auto extracted_vs =
        ExtractVariantSetDefinition(vs_name, usd_vs, scene, err);
    nested_sets.push_back(std::make_shared<tydra::VariantSet>(extracted_vs));
  }

  return nested_sets;
}

bool VariantConverter::ExtractVariantSelections(
    const Prim &prim,
    int32_t group_index,
    RenderScene *scene,
    std::string *err) {
  (void)err;
  if (group_index < 0 ||
      group_index >= static_cast<int32_t>(scene->variant_groups.size())) {
    return false;
  }

  const auto &metas = prim.metas();
  if (!metas.variants) {
    return true;  // No selections, not an error
  }

  const auto &variant_selections = metas.variants.value();
  const auto &prim_path = prim.element_path().full_path_name();

  // Extract each selection
  for (const auto &sel_pair : variant_selections) {
    const std::string &variant_set_name = sel_pair.first;
    const std::string &variant_option_name = sel_pair.second;

    VariantSelection selection;
    selection.prim_path = prim_path;
    selection.variant_set_name = variant_set_name;
    selection.variant_option_name = variant_option_name;

    scene->active_selections.push_back(selection);
  }

  return true;
}

}  // namespace tydra
}  // namespace lightusd
