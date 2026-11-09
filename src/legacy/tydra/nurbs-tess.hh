// SPDX-License-Identifier: Apache 2.0

///
/// @file nurbs-tess.hh
/// @brief NURBS surface tessellation for rendering
///
/// Provides tessellation utilities for converting NURBS surfaces to
/// polygonal meshes suitable for real-time rendering. NURBS surfaces
/// are converted to triangle meshes with configurable subdivision levels.
///
/// Features:
/// - Parametric surface evaluation
/// - Configurable tessellation density (U/V divisions)
/// - Output to render-ready mesh format
/// - Normal and texture coordinate generation
///
/// Usage:
/// ```cpp
/// lightusd::tydra::NurbsTesselator tesselator;
/// lightusd::tydra::RenderMesh mesh;
/// bool success = tesselator.tesselate(nurbs_surface, 32, 32, mesh);
/// ```
///
#pragma once

#include "render-data.hh"

namespace lightusd {

namespace tydra {

struct Nurbs;

///
/// NURBS surface tessellator for generating render-ready polygonal meshes.
///
/// Converts parametric NURBS surfaces into triangle meshes with specified
/// tessellation density. The tessellator evaluates the NURBS surface at
/// regular intervals and generates vertex positions, normals, and texture
/// coordinates suitable for GPU rendering.
///
class NurbsTesselator
{
public:
  ///
  /// Tessellate NURBS surface into triangular mesh.
  ///
  /// @param[in] nurbs NURBS surface definition
  /// @param[in] u_divs Number of divisions in U parameter direction
  /// @param[in] v_divs Number of divisions in V parameter direction
  /// @param[out] dst Output render mesh with tessellated geometry
  /// @return true on successful tessellation, false on error
  ///
  bool tesselate(const Nurbs &nurbs, uint32_t u_divs, uint32_t v_divs, RenderMesh &dst );
};

} // namespace tydra

} // namespace lightusd
