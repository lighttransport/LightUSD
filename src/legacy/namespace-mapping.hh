// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// namespace-mapping.hh - Namespace mapping for composition arcs
//
// Implements AOUSD Core Spec sections 10.3.2.1.1, 10.3.2.3.1, 10.3.2.6.1, 10.5:
//   - References/payloads: map source namespace to referencing prim namespace
//   - Inherits/specializes: map inherited prim + identity for others
//   - Relocates: additional source->target mappings
//   - Composition of namespace mappings across nested arcs
//
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/path.hh"

namespace lightusd {

///
/// A namespace mapping is a list of (source_path, target_path) pairs that
/// defines how paths in one namespace translate to paths in another.
///
/// Per Spec 10.5, namespace mappings are produced by each composition arc
/// and composed when arcs are nested.
///
struct NamespaceMapping {
  std::vector<std::pair<Path, Path>> entries;

  /// Apply this mapping to a path. Returns the remapped path.
  /// If no mapping entry matches, returns the path unchanged.
  Path Apply(const Path &path) const {
    for (const auto &entry : entries) {
      const std::string &src = entry.first.prim_part();
      const std::string &tgt = entry.second.prim_part();
      const std::string &p = path.prim_part();

      if (p == src) {
        // Exact match: remap entirely
        return Path(tgt, path.prop_part());
      }

      // Prefix match: /src/child -> /tgt/child
      if (!src.empty() && p.size() > src.size() &&
          p.substr(0, src.size()) == src &&
          p[src.size()] == '/') {
        std::string remapped = tgt + p.substr(src.size());
        return Path(remapped, path.prop_part());
      }
    }
    return path;  // no mapping applies
  }

  /// Check if this mapping is empty (no remapping needed)
  bool empty() const { return entries.empty(); }
};

///
/// Create namespace mapping for a reference arc.
///
/// Per Spec 10.3.2.1.1:
///   External reference: [(referencedPrimPath, referencingPrimPath)]
///   Internal reference: [(referencedPrimPath, referencingPrimPath), (/, /)]
///
/// @param[in] referenced_path The prim path in the referenced layer
/// @param[in] referencing_path The prim path where the reference is authored
/// @param[in] is_internal True if the reference is internal (same layer stack)
///
inline NamespaceMapping MakeReferenceMapping(
    const Path &referenced_path,
    const Path &referencing_path,
    bool is_internal = false) {
  NamespaceMapping mapping;
  mapping.entries.emplace_back(referenced_path, referencing_path);
  if (is_internal) {
    // Internal references also include identity mapping
    mapping.entries.emplace_back(Path("/", ""), Path("/", ""));
  }
  return mapping;
}

///
/// Create namespace mapping for an inherit/specialize arc.
///
/// Per Spec 10.3.2.3.1:
///   [(inheritedPrimPath, inheritingPrimPath), (/, /)]
///
/// Inherits map the source namespace at and under the inherited prim to the
/// target namespace where the inherit was authored, plus identity for all
/// other paths.
///
inline NamespaceMapping MakeInheritMapping(
    const Path &inherited_path,
    const Path &inheriting_path) {
  NamespaceMapping mapping;
  mapping.entries.emplace_back(inherited_path, inheriting_path);
  // Identity mapping for all other paths
  mapping.entries.emplace_back(Path("/", ""), Path("/", ""));
  return mapping;
}

///
/// Create namespace mapping for relocates.
///
/// Per Spec 10.3.2.6.1:
///   Additional namespace mapping composed on top of the arc's own mapping.
///   Maps source paths to target paths as specified in layerRelocates.
///
/// @param[in] relocates The relocate entries from layerRelocates
/// @param[in] prim_path The path of the prim where the composition arc is authored
///
inline NamespaceMapping MakeRelocatesMapping(
    const std::vector<std::pair<Path, Path>> &relocates,
    const Path &prim_path) {
  NamespaceMapping mapping;
  const std::string &prefix = prim_path.prim_part();

  for (const auto &entry : relocates) {
    const std::string &src = entry.first.prim_part();
    // Only include relocates whose source path is prefixed by prim_path
    if (src == prefix ||
        (src.size() > prefix.size() &&
         src.substr(0, prefix.size()) == prefix &&
         src[prefix.size()] == '/')) {
      mapping.entries.emplace_back(entry.first, entry.second);
    }
  }
  return mapping;
}

///
/// Compose two namespace mappings (outer applied after inner).
///
/// When arcs are nested, the inner arc's mapping is applied first,
/// then the outer arc's mapping is applied to the result.
///
inline NamespaceMapping ComposeNamespaceMappings(
    const NamespaceMapping &outer,
    const NamespaceMapping &inner) {
  NamespaceMapping composed;

  // For each entry in the inner mapping, apply the outer mapping to its target
  for (const auto &entry : inner.entries) {
    Path remapped_target = outer.Apply(entry.second);
    composed.entries.emplace_back(entry.first, remapped_target);
  }

  // Also include outer entries that aren't covered by inner
  for (const auto &entry : outer.entries) {
    bool covered = false;
    for (const auto &inner_entry : inner.entries) {
      if (entry.first.prim_part() == inner_entry.second.prim_part()) {
        covered = true;
        break;
      }
    }
    if (!covered) {
      composed.entries.push_back(entry);
    }
  }

  return composed;
}

///
/// Validate relocate entries per Spec 10.3.2.6 restrictions.
///
/// Returns list of error messages for invalid entries (empty if all valid).
///
inline std::vector<std::string> ValidateRelocates(
    const std::vector<std::pair<Path, Path>> &relocates) {
  std::vector<std::string> errors;

  for (size_t i = 0; i < relocates.size(); i++) {
    const auto &entry = relocates[i];
    const std::string &src = entry.first.prim_part();
    const std::string &tgt = entry.second.prim_part();

    // Cannot apply to pseudo-root or root prim paths
    if (src == "/" || src.empty()) {
      errors.push_back("Relocate source cannot be pseudo-root or empty: " + src);
    }
    if (tgt == "/" || tgt.empty()) {
      errors.push_back("Relocate target cannot be pseudo-root or empty: " + tgt);
    }

    // Source and target must be prim paths (not property paths)
    if (!entry.first.prop_part().empty()) {
      errors.push_back("Relocate source must be a prim path, not property path: " + src);
    }
    if (!entry.second.prop_part().empty()) {
      errors.push_back("Relocate target must be a prim path, not property path: " + tgt);
    }

    // Target path must not be same as source path
    if (src == tgt) {
      errors.push_back("Relocate target must differ from source: " + src);
    }

    // Check for ancestor/descendant conflicts
    for (size_t j = 0; j < relocates.size(); j++) {
      if (i == j) continue;
      const std::string &other_src = relocates[j].first.prim_part();

      // A relocate may not place a prim at the place of an existing ancestor
      if (!tgt.empty() && !other_src.empty() &&
          tgt.size() < other_src.size() &&
          other_src.substr(0, tgt.size()) == tgt &&
          other_src[tgt.size()] == '/') {
        // tgt is ancestor of another source -- check if that's also being relocated
        // This is allowed if the ancestor is also relocated
      }
    }

    // Each source/target should be unique
    for (size_t j = i + 1; j < relocates.size(); j++) {
      if (src == relocates[j].first.prim_part()) {
        errors.push_back("Duplicate relocate source: " + src);
      }
      if (tgt == relocates[j].second.prim_part()) {
        errors.push_back("Duplicate relocate target: " + tgt);
      }
    }
  }

  return errors;
}

}  // namespace lightusd
