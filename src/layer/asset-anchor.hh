// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - asset-path anchors.
//
// A relative asset path (`asset inputs:file = @../tex/albedo.png@`) is anchored
// to the DIRECTORY OF THE LAYER THAT AUTHORED IT, not to the stage's root
// layer. Production scenes rely on this: a look layer nested several
// directories deep reaches its textures with `../..`, and resolving those
// against the root layer yields a nonexistent path (the texture is then
// silently dropped).
//
// Composition flattens prims from many layers into one, so the source layer is
// lost unless it is recorded. Each loaded layer's directory (or full
// package-layer identifier for archive entries) is interned here and the
// resulting id is stamped onto that layer's PrimSpecs
// (`PrimSpec::asset_anchor_id`), then carried through composition. This mirrors
// the eager path's per-prim `current_working_path` stamp.
//
// Ids are stable for the life of the process and shared across stages: an
// anchor is a property of the layer file itself, so a layer cached by the
// LayerRegistry and referenced from several stages always has the same anchor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace lightusd {
class Layer;

/// Intern an anchor directory or package-layer identifier and return its id.
/// The empty string maps to 0
/// ("no anchor" -- the consumer falls back to its default base directory).
/// Thread-safe: composition loads and stamps layers from several threads.
uint32_t InternAssetAnchor(const std::string& dir,
                           const std::string& working_directory = {});

/// The directory or package-layer identifier for `id`, or "" when unknown.
const std::string& AssetAnchorPath(uint32_t id);

/// Resolve an authored asset against an anchor directory, or against a full
/// package-layer identifier. Package traversal cannot escape its archive root.
std::string ResolveAssetAnchorPath(const std::string& asset, uint32_t id);

/// Stamp a freshly parsed layer, including inline variant contents, with the
/// directory of its source identifier. Existing package/layer anchors can be
/// retained when only_unanchored is true.
void StampLayerAssetAnchor(Layer* layer, const std::string& identifier,
                           bool only_unanchored = false);

/// Resolve scalar asset-valued defaults/samples and inline variant properties
/// before composition merges fields from layers with different source anchors.
/// Exact remaps take precedence; unmatched relative assets retain their source
/// directory (including UDIM patterns whose concrete files appear in the map).
size_t RebaseLayerAssetPaths(Layer* layer,
                             const std::map<std::string, std::string>& remap);

/// Stamp a loaded package layer with its full package-layer identifier.
void StampPackageAssetAnchor(Layer* layer, const std::string& identifier);

}  // namespace lightusd
