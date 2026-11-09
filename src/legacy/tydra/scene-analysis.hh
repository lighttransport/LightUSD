// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.

///
/// @file scene-analysis.hh
/// @brief USD scene analysis and geometric computation utilities
///
/// Provides geometric analysis functions for USD scenes including bounding
/// box computation, extent calculation, and spatial queries. These utilities
/// help with scene understanding, culling, and optimization.
///
/// Key functions:
/// - ComputeBound(): Calculate scene or prim bounding boxes
/// - ComputePrimSpecBound(): Calculate individual prim bounds
/// - Extent computation with time sample support
///
/// Features:
/// - Time-aware bounding box computation
/// - Option to use pre-computed extent attributes
/// - Support for animated geometry bounds
/// - Hierarchical bound computation
///
/// Limitations:
/// - Current implementation doesn't consider skinning transforms
/// - Some advanced deformation cases may not be handled
///
/// Note: This API avoids nonstd::optional, nonstd::expected, std::function
/// and other advanced STL features for easier language bindings.
///
#pragma once

#include "core/prim-spec.hh"
#include "core/layer-types.hh"
#include "core/extent.hh"
#include "value-types.hh"

namespace lightusd {
namespace tydra {

///
/// Compute bounding box of entire scene (Layer) at specified time.
///
/// Calculates the axis-aligned bounding box encompassing all boundable
/// geometry in the layer. Can use pre-computed extent attributes or
/// calculate from actual geometry data.
///
/// @param[in] layer USD layer to analyze
/// @param[in] use_extent Use 'extent' attributes when available (faster)
/// @param[out] bbox Computed bounding box extent
/// @param[in] t Time code for evaluation (default = no time sampling)
/// @return true on success, false if layer contains no boundable prims
///
bool ComputeBound(const Layer &layer, const bool use_extent, Extent &bbox,
                  const double t = value::TimeCode::Default());

///
/// Compute bounding box of individual PrimSpec at specified time.
///
/// Calculates the axis-aligned bounding box for a single prim's geometry.
/// Handles time-sampled geometry and can use pre-computed extent attributes.
///
/// @param[in] ps PrimSpec to analyze
/// @param[in] use_extent Use 'extent' attribute when available (faster)
/// @param[out] bbox Computed bounding box extent
/// @param[in] t Time code for evaluation (default = no time sampling)
/// @return true on success, false if prim is not boundable
///
/// Limitation: Current implementation does not consider skinning transforms
///
bool ComputePrimSpecBound(const PrimSpec &ps, const bool use_extent, Extent &bbox,
                  const double t = value::TimeCode::Default());

}  // namespace tydra
}  // namespace lightusd
