// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "usdPhysics.hh"

#include "core/relationship.hh"

namespace lightusd {

std::vector<Path> PhysicsFilteredPairsAPI::get_filtered_pair_paths() const {
  std::vector<Path> out;
  if (!filteredPairs.authored()) return out;
  const Relationship &rel = filteredPairs.relationship();
  if (rel.is_path()) {
    out.push_back(rel.targetPath);
  } else if (rel.is_pathvector()) {
    out = rel.targetPathVector;
  }
  return out;
}

// GetPhysicsFilteredPairsAPI / GetPhysicsCollidersCollection live in
// tydra/scene-access.cc (they use the higher-level tydra::GetProperty
// API to dispatch on Prim type).

}  // namespace lightusd
