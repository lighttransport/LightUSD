// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// layer-types.hh - SubLayer and LayerMetas types
//
#pragma once

#include <limits>
#include <map>
#include <string>
#include <vector>

#include "composition-types.hh"
#include "typed-attribute.hh"
#include "prim-enums.hh"
#include "meta-variable.hh"
#include "animatable.hh"
#include "value-types.hh"

namespace lightusd {

struct SubLayer
{
  value::AssetPath assetPath;
  LayerOffset layerOffset;
};


struct LayerMetas {
  enum class PlaybackMode {
    PlaybackModeNone,
    PlaybackModeLoop,
  };

  // TODO: Support more predefined properties: reference =
  // <pxrUSD>/pxr/usd/sdf/wrapLayer.cpp Scene global setting
  TypedAttributeWithFallback<Axis> upAxis{
      Axis::
          Y};  // This can be changed by plugInfo.json in USD:
               // https://graphics.pixar.com/usd/dev/api/group___usd_geom_up_axis__group.html#gaf16b05f297f696c58a086dacc1e288b5
  value::token defaultPrim;                               // prim node name
  TypedAttributeWithFallback<double> metersPerUnit{1.0};  // default [m]
  TypedAttributeWithFallback<double> timeCodesPerSecond{
      24.0};  // default 24 fps
  TypedAttributeWithFallback<double> framesPerSecond{24.0};
  TypedAttributeWithFallback<double> startTimeCode{
      0.0};  // FIXME: default = -inf?
  TypedAttributeWithFallback<double> endTimeCode{
      std::numeric_limits<double>::infinity()};
  std::vector<SubLayer> subLayers;  // `subLayers`
  value::StringData comment;  // 'comment' In Stage meta, comment must be string
                              // only(`comment = "..."` is not allowed)
  value::StringData doc;      // `documentation`

  // UsdPhysics
  TypedAttributeWithFallback<double> kilogramsPerUnit{1.0};

  CustomDataType customLayerData;  // customLayerData
  bool customLayerDataAuthored{false};  // Track if customLayerData was explicitly authored (even if empty)

  // AOUSD Core Spec fields (Spec 7.6)
  // colorConfiguration: asset path to OCIO config
  nonstd::optional<value::AssetPath> colorConfiguration;
  // colorManagementSystem: e.g. "ocio"
  nonstd::optional<value::token> colorManagementSystem;
  // Default UsdRenderSettings prim used to select the rendering color space.
  nonstd::optional<std::string> renderSettingsPrimPath;
  // owner: layer owner string
  nonstd::optional<std::string> owner;
  // hasOwnedSubLayers: whether sublayers are "owned" by this layer
  nonstd::optional<bool> hasOwnedSubLayers;
  // expressionVariables: dictionary of variable substitutions for asset paths
  nonstd::optional<CustomDataType> expressionVariables;
  // layerRelocates: list of (source path, target path) relocations
  // Each entry maps a source prim path to a target prim path in the namespace
  std::vector<std::pair<Path, Path>> layerRelocates;

  // USDZ extension
  TypedAttributeWithFallback<bool> autoPlay{
      true};  // default(or not authored) = auto play
  TypedAttributeWithFallback<PlaybackMode> playbackMode{
      PlaybackMode::PlaybackModeLoop};

  // Unregistered (unknown-to-lightusd) layer metadata, preserved verbatim as the
  // raw USDA text of the value (quotes included for string values), keyed by the
  // metadatum name. OpenUSD preserves such opinions (SdfUnregisteredValue);
  // dropping them silently loses authored data. Mirrors PrimMeta::unregisteredMetas.
  std::map<std::string, std::string> unregisteredMetas;

  // Indirectly used.
  std::vector<value::token> primChildren;

  ///
  /// Estimate memory usage of this LayerMetas in bytes
  ///
  size_t estimate_memory_usage() const {
    size_t total = sizeof(LayerMetas);
    // defaultPrim token
    total += defaultPrim.str().capacity();
    // subLayers
    for (const auto& sl : subLayers) {
      total += sizeof(SubLayer);
      total += sl.assetPath.GetAssetPath().capacity();
      total += sl.assetPath.GetResolvedPath().capacity();
    }
    // comment and doc strings
    total += comment.value.capacity();
    total += doc.value.capacity();
    if (renderSettingsPrimPath) {
      total += renderSettingsPrimPath.value().capacity();
    }
    // customLayerData (rough estimate - map of string to MetaVariable)
    for (const auto& kv : customLayerData) {
      total += kv.first.capacity();
      total += sizeof(MetaVariable);  // MetaVariable internal estimation not detailed
    }
    // unregistered layer metadata
    for (const auto& kv : unregisteredMetas) {
      total += kv.first.capacity();
      total += kv.second.capacity();
    }
    // primChildren tokens
    for (const auto& tok : primChildren) {
      total += tok.str().capacity();
    }
    return total;
  }
};

// Forward declaration for Layer class
// Layer class has been moved to layer.hh
class Layer;

}  // namespace lightusd
