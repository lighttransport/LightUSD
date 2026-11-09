// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita & Light Transport Entertainment Inc.
//
// Variant Support for Tydra RenderScene Implementation
//

#include "variant-support.hh"

#include <algorithm>
#include <functional>
#include <sstream>
#include "nonstd/expected.hpp"
#include "common-macros.inc"

namespace lightusd {
namespace tydra {

// Out-of-line virtual destructor to avoid weak vtables warning
VariantManager::~VariantManager() = default;

const std::vector<VariantGroup>& DefaultVariantManager::GetVariantGroups() const {
  return _variant_groups;
}

const VariantGroup* DefaultVariantManager::FindVariantGroup(
    const std::string& prim_path) const {
  auto it = _variant_group_map.find(prim_path);
  if (it != _variant_group_map.end()) {
    int32_t idx = it->second;
    if (idx >= 0 && idx < static_cast<int32_t>(_variant_groups.size())) {
      return &_variant_groups[static_cast<size_t>(idx)];
    }
  }
  return nullptr;
}

const VariantSet* DefaultVariantManager::FindVariantSet(
    const std::string& prim_path, const std::string& variant_set_name) const {
  const VariantGroup* group = FindVariantGroup(prim_path);
  if (!group) {
    return nullptr;
  }

  for (const auto& vs : group->variant_sets) {
    if (vs.name == variant_set_name) {
      return &vs;
    }
  }
  return nullptr;
}

const VariantOption* DefaultVariantManager::FindVariantOption(
    const std::string& prim_path, const std::string& variant_set_name,
    const std::string& variant_option_name) const {
  const VariantSet* vs = FindVariantSet(prim_path, variant_set_name);
  if (!vs) {
    return nullptr;
  }

  for (const auto& opt : vs->options) {
    if (opt.name == variant_option_name) {
      return &opt;
    }
  }
  return nullptr;
}

bool DefaultVariantManager::SelectVariant(const std::string& prim_path,
                                         const std::string& variant_set_name,
                                         const std::string& variant_option_name) {
  // Validate variant exists
  if (!FindVariantOption(prim_path, variant_set_name, variant_option_name)) {
    return false;
  }

  // Find and update or add selection
  for (auto& sel : _current_selections) {
    if (sel.prim_path == prim_path && sel.variant_set_name == variant_set_name) {
      sel.variant_option_name = variant_option_name;
      return true;
    }
  }

  // Add new selection
  _current_selections.emplace_back(variant_set_name, variant_option_name, prim_path);
  return true;
}

bool DefaultVariantManager::SelectVariantByIndex(const std::string& prim_path,
                                                const std::string& variant_set_name,
                                                uint32_t option_index) {
  const VariantSet* vs = FindVariantSet(prim_path, variant_set_name);
  if (!vs || option_index >= vs->options.size()) {
    return false;
  }

  return SelectVariant(prim_path, variant_set_name, vs->options[option_index].name);
}

nonstd::optional<VariantSelection> DefaultVariantManager::GetCurrentSelection(
    const std::string& prim_path, const std::string& variant_set_name) const {
  for (const auto& sel : _current_selections) {
    if (sel.prim_path == prim_path && sel.variant_set_name == variant_set_name) {
      return sel;
    }
  }
  return nonstd::nullopt;
}

const std::vector<VariantSelection>& DefaultVariantManager::GetAllSelections()
    const {
  return _current_selections;
}

void DefaultVariantManager::ResetToDefaults() {
  _current_selections.clear();

  // Set all variant sets to their default option
  for (const auto& group : _variant_groups) {
    for (const auto& vs : group.variant_sets) {
      if (vs.default_option_index >= 0 &&
          vs.default_option_index < static_cast<int32_t>(vs.options.size())) {
        VariantSelection sel(vs.name, vs.options[static_cast<size_t>(vs.default_option_index)].name,
                            group.prim_path);
        _current_selections.push_back(sel);
      }
    }
  }
}

VariantStatistics DefaultVariantManager::GetStatistics() const {
  VariantStatistics stats;
  stats.num_variant_groups = static_cast<uint32_t>(_variant_groups.size());

  uint32_t max_depth = 0;

  // Count total variant sets and options, and compute max nesting depth
  for (const auto& group : _variant_groups) {
    stats.num_variant_sets += static_cast<uint32_t>(group.variant_sets.size());

    // Calculate nesting depth for this group
    std::function<uint32_t(const VariantSet&, uint32_t)> calc_depth =
        [&](const VariantSet& vs, uint32_t rec_depth) -> uint32_t {
      if (size_t(rec_depth) >= kMaxDefaultTraversalLimit) return 1;
      uint32_t depth = 1;
      for (const auto& opt : vs.options) {
        if (!opt.nested_variant_sets.empty()) {
          for (const auto& nested_vs : opt.nested_variant_sets) {
            depth = std::max(depth, 1 + calc_depth(*nested_vs, rec_depth + 1));
          }
        }
      }
      return depth;
    };

    for (const auto& vs : group.variant_sets) {
      stats.num_variant_options += static_cast<uint32_t>(vs.options.size());
      max_depth = std::max(max_depth, calc_depth(vs, 0));
    }
  }

  stats.num_active_selections = static_cast<uint32_t>(_current_selections.size());
  stats.max_nesting_depth = max_depth;

  return stats;
}

bool DefaultVariantManager::HasVariants(const std::string& prim_path) const {
  const VariantGroup* group = FindVariantGroup(prim_path);
  return group != nullptr && !group->variant_sets.empty();
}

bool DefaultVariantManager::VariantSetExists(const std::string& prim_path,
                                            const std::string& variant_set_name) const {
  return FindVariantSet(prim_path, variant_set_name) != nullptr;
}

//
// Helper functions
//

std::string VariantSelectionToString(const VariantSelection& sel) {
  std::ostringstream oss;
  if (!sel.prim_path.empty()) {
    oss << sel.prim_path << ":";
  }
  oss << sel.variant_set_name << "=" << sel.variant_option_name;
  return oss.str();
}

bool ValidateVariantSelections(const VariantManager& manager,
                              const std::vector<VariantSelection>& selections) {
  for (const auto& sel : selections) {
    if (!manager.FindVariantOption(sel.prim_path, sel.variant_set_name,
                                  sel.variant_option_name)) {
      return false;
    }
  }
  return true;
}

std::string GetVariantHierarchyPath(const VariantGroup& group,
                                   const VariantSet& variant_set) {
  std::ostringstream oss;
  oss << group.prim_path << " -> " << variant_set.name;
  if (!variant_set.parent_variant_option_name.empty()) {
    oss << " (in " << variant_set.parent_variant_option_name << ")";
  }
  return oss.str();
}

}  // namespace tydra
}  // namespace lightusd
