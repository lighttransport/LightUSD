// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>

namespace lightusd {
class Layer;
}

// Canonicalize intra-layer instance prototypes so two layers that differ only
// in prototype naming (e.g. "__Prototype_1" vs "Proto_A") or in emitting
// duplicate identical prototypes diff as equal.
//
// Prototype roots are top-level prims that are the target of an internal
// (asset-less) reference. Each is content-hashed (its subtree, ignoring its
// own name, with nested prototype references replaced by their hash) and
// renamed to "__Proto_<hash>"; every internal reference is retargeted, and
// identical prototypes collapse to one root. Returns the number of
// prototype roots found.
size_t CanonicalizeInstances(lightusd::Layer& layer);
