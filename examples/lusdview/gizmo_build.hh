// SPDX-License-Identifier: Apache-2.0
// lusdview - generate wireframe gizmo geometry for USD lights and cameras.
#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "renderer.hh"
#include "tydra/render-data.hh"

namespace lusdview {

// Generate Maya-like wireframe gizmo lines for all lights in the RenderScene.
// `lightXforms` maps RenderLight index -> world matrix (std::array<float,16> col-major).
// Lines are appended to `out` as depth-tested helper line segments.
void BuildLightGizmos(const lightusd::legacy::tydra::RenderScene& rs,
                      const std::unordered_map<int, std::array<float, 16>>& lightXforms,
                      std::vector<HelperVertex>& out);

// Generate camera frustum wireframe gizmos for all cameras in the RenderScene.
// `camXforms` maps RenderCamera index -> world matrix (std::array<float,16> col-major).
// Lines are appended to `out` as depth-tested helper line segments.
void BuildCameraGizmos(const lightusd::legacy::tydra::RenderScene& rs,
                       const std::unordered_map<int, std::array<float, 16>>& camXforms,
                       std::vector<HelperVertex>& out);

// Collect world transforms for light and camera nodes from the node hierarchy.
// `lightXforms[i]` is the column-major world matrix for `rs.lights[i]`.
// `camXforms[i]` is the column-major world matrix for `rs.cameras[i]`.
void CollectLightCameraTransforms(
    const lightusd::legacy::tydra::Node& node,
    std::unordered_map<int, std::array<float, 16>>& lightXforms,
    std::unordered_map<int, std::array<float, 16>>& camXforms);

}  // namespace lusdview
