// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP composition arc types
//
// Native pcp-style composition for the `next` module. Mirrors the DESIGN of the
// main library's composition-graph.hh / src/pcp (PrimIndex DAG, LIVRPS strength
// ordering) but is fully standalone (only next/ + STL). C++14.

#pragma once

#include <cstdint>

namespace lightusd {
namespace next {
namespace pcp {

/// Composition arc types in LIVRPS strength order (lower value == stronger).
/// Mirrors OpenUSD PcpArcType / composition_graph::ArcType.
enum class ArcType : uint8_t {
  Root = 0,        // The prim's own (root layer stack) opinions; no parent.
  SubLayer = 1,    // L - sublayer of a layer stack (folded into a LayerStack).
  Inherit = 2,     // I - class inheritance (phase 3).
  Variant = 3,     // V - variant selection (phase 4).
  Relocate = 4,    // namespace rename, between V and R (phase 4+).
  Reference = 5,   // R - reference to another prim/asset.
  Payload = 6,     // P - deferred reference (phase 2).
  Specialize = 7,  // S - globally weakest (phase 3).
};

inline bool IsClassBasedArc(ArcType t) {
  return t == ArcType::Inherit || t == ArcType::Specialize;
}

inline const char *ArcTypeName(ArcType t) {
  switch (t) {
    case ArcType::Root: return "root";
    case ArcType::SubLayer: return "sublayer";
    case ArcType::Inherit: return "inherit";
    case ArcType::Variant: return "variant";
    case ArcType::Relocate: return "relocate";
    case ArcType::Reference: return "reference";
    case ArcType::Payload: return "payload";
    case ArcType::Specialize: return "specialize";
  }
  return "unknown";
}

/// Per-node flags. Bit-flags so a node can carry several at once.
enum class NodeFlags : uint16_t {
  None = 0,
  HasSpecs = 1 << 0,          // At least one layer in the node's stack authors the site.
  Inert = 1 << 1,             // Structural placeholder; contributes no opinions.
  Culled = 1 << 2,            // Pruned (no opinions anywhere below).
  PayloadDeferred = 1 << 3,   // Payload exists but is not loaded (phase 2).
  PayloadLoaded = 1 << 4,     // Payload content has been loaded (phase 2).
  IsImpliedArc = 1 << 5,      // Propagated implied inherit/specialize (phase 3).
  IsDueToAncestor = 1 << 6,   // Introduced by ancestral composition (phase 2).
};

inline NodeFlags operator|(NodeFlags a, NodeFlags b) {
  return static_cast<NodeFlags>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline NodeFlags &operator|=(NodeFlags &a, NodeFlags b) { a = a | b; return a; }
inline bool HasFlag(NodeFlags flags, NodeFlags f) {
  return (static_cast<uint16_t>(flags) & static_cast<uint16_t>(f)) != 0;
}

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
