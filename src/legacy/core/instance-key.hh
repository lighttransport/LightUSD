// SPDX-License-Identifier: Apache 2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// instance-key.hh - Instance key for composition-based instancing detection
//
// An InstanceKey captures the structural identity of a prim's composition
// arcs. Two prims with equal InstanceKeys have structurally identical
// composition and will produce identical child namespaces. This enables
// sharing prototype PrimIndices among instances (AOUSD Core Spec 11.4).
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "composition-types.hh"
#include "path.hh"
#include "prim-metas.hh"
#include "prim-spec.hh"

namespace lightusd {

/// 128-bit hash capturing the structural identity of a prim's composition.
///
/// Two prims with equal InstanceKeys reference the same layers via the
/// same arc types with the same variant selections, and will produce
/// identical child compositions. Combined with `instanceable=true`
/// metadata, these prims can share a prototype.
///
/// Uses SpookyHash for 128-bit hashing with near-zero collision probability.
struct InstanceKey {
  uint64_t hash_lo{0};
  uint64_t hash_hi{0};

  bool operator==(const InstanceKey &rhs) const {
    return hash_lo == rhs.hash_lo && hash_hi == rhs.hash_hi;
  }
  bool operator!=(const InstanceKey &rhs) const { return !(*this == rhs); }

  /// True if this key was computed (non-zero hash).
  bool is_valid() const { return hash_lo != 0 || hash_hi != 0; }
};

/// Hasher for use in unordered_map.
struct InstanceKeyHasher {
  size_t operator()(const InstanceKey &k) const {
    return static_cast<size_t>(k.hash_lo);
  }
};

/// Compute an InstanceKey from a PrimSpec's composition metadata.
///
/// Hashes: references, payloads, inherits, specializes, variant selections,
/// and type name. Does NOT hash direct properties (those are per-instance).
///
/// @param[in] ps The PrimSpec
/// @param[out] key The computed key
/// @return true if the prim has instanceable=true and a key was computed;
///         false if not instanceable (key left as default/invalid)
bool ComputeInstanceKeyFromPrimSpec(const PrimSpec &ps, InstanceKey *key);

/// Compute an InstanceKey directly from PrimMetas and type name.
///
/// This overload allows computing keys from a composed Prim's metadata
/// without needing to construct a PrimSpec. Used by Stage::BuildInstancePrototypes().
///
/// @param[in] metas The prim's composed metadata
/// @param[in] type_name The prim's typeName
/// @param[out] key The computed key
/// @return true if the prim has instanceable=true and a key was computed;
///         false if not instanceable (key left as default/invalid)
bool ComputeInstanceKeyFromPrimMetas(const PrimMeta &metas,
                                     const std::string &type_name,
                                     InstanceKey *key);

}  // namespace lightusd
