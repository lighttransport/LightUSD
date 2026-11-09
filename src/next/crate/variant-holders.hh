// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Variant holder materialization for the crate writer.
//
// The crate (USDC) format represents variants as bracketed HOLDER prims
// ("/Prim/{set=}" VariantSet specs and "/Prim/{set=var}" Variant specs) plus a
// variantSelection field on the owning prim. USDA-parsed / API-authored
// layers instead carry variants inline in PrimMetadata::variantSets()
// (PrimVariantSetData with per-variant properties and an optional content
// subtree). The crate writer only emits holder prims that exist in the layer,
// so inline-authored variants would lose their option names and content on a
// USDC round-trip.
//
// MaterializeVariantHolders() bridges the two representations: it clones the
// layer and appends holder prims synthesized from each prim's inline
// PrimVariantSetData (option properties, relationships, composition arcs, and the
// content subtree). Holders that already exist (crate-read layers) are left
// untouched. The appended prims are reachable by path only (no child links);
// the crate writer is fully path-based, so that is sufficient.

#pragma once

#include "../layer/layer.hh"

namespace lightusd {
namespace next {

/// True when `layer` carries inline variant data (PrimVariantSetData with variant
/// options) for which no bracketed holder prim exists yet — i.e. a crate
/// write would drop variant information without materialization.
bool LayerNeedsVariantHolders(const Layer& layer);

/// Clone `layer` and append bracketed holder prims for all inline-authored
/// variants (see file comment). The clone's path index is rebuilt.
Layer MaterializeVariantHolders(const Layer& layer);

}  // namespace next
}  // namespace lightusd
