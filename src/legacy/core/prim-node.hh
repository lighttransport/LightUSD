// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// core/prim-node.hh - Deprecated PrimNode class
//
// TODO: Deprecate this class and use PrimSpec
//
#pragma once

#include <map>
#include <string>
#include <vector>

#include "value-types.hh"
#include "core/path.hh"
#include "core/meta-variable.hh"

namespace lightusd {

// Forward declarations
class Prim;
class PrimSpec;
struct VariantSet;

///
/// TODO: Deprecate this class and use PrimSpec
/// NOTE PrimNode is designed for Stage(freezed)
///
/// Contains concrete Prim object and composition elements.
///
/// PrimNode is near to the final state of `Prim`.
/// Doing one further step(Composition, Flatten, select Variant) to get `Prim`.
///
/// Similar to `PrimIndex` in pxrUSD
///

class PrimNode {
  Path path;
  Path elementPath;

  PrimNode(const value::Value &rhs);

  PrimNode(value::Value &&rhs);

  value::Value prim;  // GPrim, Xform, ...

  std::vector<PrimNode> children;  // child nodes

  ///
  /// Select variant.
  ///
  bool select_variant(const std::string &target_name,
                      const std::string &variant_name) {
    const auto m = _vsmap.find(target_name);
    if (m != _vsmap.end()) {
      _current_vsmap[target_name] = variant_name;
      return true;
    } else {
      return false;
    }
  }

  ///
  /// Get current variant selection.
  ///
  bool current_variant_selection(const std::string &target_name,
                      std::string *selected_variant_name) {

    if (!selected_variant_name) {
      return false;
    }

    const auto m = _vsmap.find(target_name);
    if (m != _vsmap.end()) {
      const auto sm = _current_vsmap.find(target_name);
      if (sm != _current_vsmap.end()) {
        (*selected_variant_name) = sm->second;
      } else {
        (*selected_variant_name) = m->second;
      }
      return true;
    } else {
      return false;
    }
  }

  ///
  /// List variants in this Prim
  ///
  /// key = variant prim name
  /// value = variants
  ///
  const VariantSelectionMap &get_variant_selection_map() const { return _vsmap; }

  ///
  /// Variants
  ///
  /// VariantSet = Prim metas + Properties and/or child Prims
  ///            = repsetent as PrimNode for a while.
  ///
  ///
  /// key = variant name
  using VariantSet = std::map<std::string, PrimNode>;
  std::map<std::string, VariantSet> varitnSetList;  // key = variant

  VariantSelectionMap _vsmap;          // Original variant selections
  VariantSelectionMap _current_vsmap;  // Currently selected variants

  std::vector<value::token> primChildren;  // List of child Prim nodes
  std::vector<value::token> properties;    // List of property names
  std::vector<value::token> variantChildren; // List of child VariantSet nodes.
};

} // namespace lightusd
