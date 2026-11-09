// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// collection-api.hh - Collection, CollectionInstance, ColorSpaceAPI
//
#pragma once

#include <string>

#include "nonstd/optional.hpp"
#include "ordered-dict.hh"
#include "typed-attribute.hh"
#include "relationship.hh"
#include "animatable.hh"
#include "value-types.hh"

namespace lightusd {

// Collection API
///
/// ColorSpaceAPI - API schema for specifying color space of a prim
/// See: https://openusd.org/dev/user_guides/color_user_guide.html
///
/// Apply to prims: prepend apiSchemas = ["ColorSpaceAPI"]
/// Sets source color space via: uniform token colorSpace:name
///
struct ColorSpaceAPI {
  TypedAttributeWithFallback<Animatable<value::token>> colorSpace_name;  // uniform token colorSpace:name
};

// https://openusd.org/release/api/class_usd_collection_a_p_i.html

constexpr auto kExpandPrims = "expandPrims";
constexpr auto kExplicitOnly = "explicitOnly";
constexpr auto kExpandPrimsAndProperties = "expandPrimsAndProperties";

struct CollectionInstance {

  enum class ExpansionRule {
    ExpandPrims, // "expandPrims" (default)
    ExplicitOnly, // "explicitOnly"
    ExpandPrimsAndProperties, // "expandPrimsAndProperties"
  };

  TypedAttributeWithFallback<ExpansionRule> expansionRule{ExpansionRule::ExpandPrims}; // uniform token collection:collectionName:expansionRule
  TypedAttributeWithFallback<Animatable<bool>> includeRoot{false}; // bool collection:<collectionName>:includeRoot
  RelationshipProperty includes; // rel collection:<collectionName>:includes
  RelationshipProperty excludes; // rel collection:<collectionName>:excludes

  // uniform pathExpression collection:<collectionName>:membershipExpression
  // When authored, the collection is in "expression mode" (pattern-based
  // membership) instead of the relationship-based includes/excludes mode.
  TypedAttribute<value::PathExpression> membershipExpression;

  bool has_membershipExpression() const { return membershipExpression.authored(); }
};

class Collection
{
 public:
  Collection() = default;
  Collection(const Collection &) = default;
  Collection(Collection &&) noexcept = default;
  Collection &operator=(const Collection &) = default;
  Collection &operator=(Collection &&) noexcept = default;

  const ordered_dict<CollectionInstance> instances() const {
    return _instances;
  }

  bool add_instance(const std::string &name, CollectionInstance &instance) {
    if (_instances.count(name)) {
      return false;
    }

    _instances.insert(name, instance);

    return true;
  }

  bool get_instance(const std::string &name, const CollectionInstance **coll) const {
    if (!coll) {
      return false;
    }

    return _instances.at(name, coll);
  }

  CollectionInstance &get_or_add_instance(const std::string &name)
      LIGHTUSD_LIFETIMEBOUND {
    return _instances.get_or_add(name);
  }

  bool has_instance(const std::string &name) const {
    return _instances.count(name);
  }

  bool del_instance(const std::string &name) {
    return _instances.erase(name);
  }

  ///
  /// Compute the set of member paths for a named collection instance.
  ///
  /// Per AOUSD Core Spec 15.1:
  ///   - Start with `includes` relationship targets
  ///   - If `includeRoot` is true, add the prim itself
  ///   - Remove `excludes` relationship targets
  ///   - Apply `expansionRule` to expand prims/properties
  ///
  /// Note: This is a simplified implementation that returns the explicit
  /// include/exclude paths without hierarchical expansion (which requires
  /// full Stage traversal).
  ///
  /// @param[in] name Collection instance name
  /// @param[in] prim_path Path of the prim bearing this collection
  /// @param[out] included Output set of included paths
  /// @param[out] excluded Output set of excluded paths
  /// @return true if the collection instance was found
  ///
  bool ComputeMemberPaths(const std::string &name,
                           const Path &prim_path,
                           std::vector<Path> *included,
                           std::vector<Path> *excluded) const {
    const CollectionInstance *inst = nullptr;
    if (!get_instance(name, &inst) || !inst) {
      return false;
    }

    if (included) {
      included->clear();

      // includeRoot: add the prim itself
      // includeRoot is TypedAttributeWithFallback<Animatable<bool>>
      bool include_root = false;
      const auto &ir = inst->includeRoot.get_value();
      if (ir.has_default()) {
        include_root = ir.get_scalar_ref();
      }
      if (include_root) {
        included->push_back(prim_path);
      }

      // includes relationship targets
      if (inst->includes.authored()) {
        const auto &rel = inst->includes.relationship();
        if (rel.is_path()) {
          included->push_back(rel.targetPath);
        } else if (rel.is_pathvector()) {
          for (const auto &p : rel.targetPathVector) {
            included->push_back(p);
          }
        }
      }
    }

    if (excluded) {
      excluded->clear();

      // excludes relationship targets
      if (inst->excludes.authored()) {
        const auto &rel = inst->excludes.relationship();
        if (rel.is_path()) {
          excluded->push_back(rel.targetPath);
        } else if (rel.is_pathvector()) {
          for (const auto &p : rel.targetPathVector) {
            excluded->push_back(p);
          }
        }
      }
    }

    return true;
  }

  /// Check if a path is a member of a named collection.
  /// Simple check: path is in includes and not in excludes.
  bool IsMember(const std::string &name, const Path &prim_path,
                const Path &query_path) const {
    std::vector<Path> included, excluded;
    if (!ComputeMemberPaths(name, prim_path, &included, &excluded)) {
      return false;
    }

    // Check excluded first
    for (const auto &ep : excluded) {
      if (ep == query_path) return false;
      // Prefix match for hierarchical exclusion
      const std::string &eps = ep.prim_part();
      const std::string &qps = query_path.prim_part();
      if (!eps.empty() && qps.size() > eps.size() &&
          qps.substr(0, eps.size()) == eps && qps[eps.size()] == '/') {
        return false;
      }
    }

    // Check included
    for (const auto &ip : included) {
      if (ip == query_path) return true;
      // Prefix match for hierarchical inclusion
      const std::string &ips = ip.prim_part();
      const std::string &qps = query_path.prim_part();
      if (!ips.empty() && qps.size() > ips.size() &&
          qps.substr(0, ips.size()) == ips && qps[ips.size()] == '/') {
        return true;
      }
    }

    return false;
  }

 private:
  ordered_dict<CollectionInstance> _instances;
};

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(Collection, "Collection", TYPE_ID_COLLECTION, 1);
DEFINE_TYPE_TRAIT(CollectionInstance, "CollectionInstance", TYPE_ID_COLLECTION_INSTANCE, 1);
DEFINE_TYPE_TRAIT(ColorSpaceAPI, "ColorSpaceAPI", TYPE_ID_COLOR_SPACE_API, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
