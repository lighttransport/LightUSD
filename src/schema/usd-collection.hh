// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - UsdCollectionAPI membership (relationship mode)
#pragma once

#include <map>
#include <string>

namespace lightusd {
class Stage;
class UsdPrim;

/// Membership of `path` in the collection `collection:<name>` on `owner`,
/// following UsdCollectionMembershipQuery for relationship-mode collections:
///  - the most specific (nearest ancestor-or-self) path among includes and
///    excludes decides; includeRoot adds the pseudo-root `/` as an include, so
///    a nearer exclude wins over a farther include and vice versa;
///  - expansionRule "explicitOnly" matches only exact include paths (excludes
///    still remove exact paths), "expandPrims" (default) and
///    "expandPrimsAndProperties" match descendants;
///  - an include target `</P.collection:other>` includes that collection
///    (cycles are rejected).
/// Returns false for an unauthored collection (no includes/excludes/
/// includeRoot); `*authored` reports whether anything was authored.
/// Precomputed membership rules for one collection (cheap repeated queries).
class CollectionMembershipQuery {
 public:
  bool Compute(const Stage& stage, const UsdPrim& owner, const std::string& name);
  bool IsPathIncluded(const std::string& path) const;
  bool authored() const { return authored_; }
  /// Flattened path -> include(true)/exclude(false) entries.
  const std::map<std::string, bool>& entries() const { return entries_; }
  bool explicit_only() const { return explicit_only_; }

 private:
  std::map<std::string, bool> entries_;
  bool explicit_only_ = false;
  bool authored_ = false;
};

bool IsPathInCollection(const Stage& stage, const UsdPrim& owner, const std::string& name,
                        const std::string& path, bool* authored = nullptr);

}  // namespace lightusd
