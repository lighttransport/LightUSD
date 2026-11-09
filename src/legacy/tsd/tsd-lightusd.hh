// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// tinysubdiv <-> lightusd adapter: refine a GeomMesh's evaluated geometry
// using its authored USD subdivision attributes.

#ifndef LIGHTUSD_TSD_LIGHTUSD_HH_
#define LIGHTUSD_TSD_LIGHTUSD_HH_

#include <cstdint>
#include <string>
#include <vector>

#include "tsd/tinysubdiv.hh"

namespace lightusd {

struct GeomMesh;

namespace tsd {

// Uniformly refines pre-evaluated mesh geometry `level` times using the
// GeomMesh's authored subdivision attributes:
//   - subdivisionScheme (catmullClark/loop/bilinear; "none" is rejected --
//     skip subdivision on the caller side)
//   - interpolateBoundary, faceVaryingLinearInterpolation
//   - creaseIndices/creaseLengths/creaseSharpnesses, cornerIndices/
//     cornerSharpnesses, holeIndices (evaluated at default time; subdiv
//     tags are not animatable in practice)
// `points_xyz` is interleaved xyz (3 floats per point), `faceVertexCounts`/
// `faceVertexIndices` the evaluated topology at the caller's timecode.
//
// Optional primvar channels refine in lockstep with the geometry:
//   - `fvar_channels`: faceVarying primvars (e.g. UVs); each channel's
//     `interpolation` field is overridden with the mesh's authored
//     faceVaryingLinearInterpolation. Refined per-corner values land in
//     out->fvar[i].
//   - `vertex_primvars`: "vertex" (smooth) or "varying" (linear) per-point
//     primvars; refined values land in out->vertex_primvars[i].
//
// Output faces are quads (catmullClark/bilinear) or triangles (loop);
// `out->face_source` maps each refined face to its base-face id (use it to
// remap per-face data such as GeomSubset indices and uniform primvars).
bool RefineGeomMesh(const GeomMesh &mesh, int32_t level,
                    const std::vector<float> &points_xyz,
                    const std::vector<uint32_t> &faceVertexCounts,
                    const std::vector<uint32_t> &faceVertexIndices,
                    const FVarChannelView *fvar_channels,
                    uint32_t num_fvar_channels,
                    const VertexPrimvarView *vertex_primvars,
                    uint32_t num_vertex_primvars, RefinedMesh *out,
                    std::string *err);

// Geometry-only convenience overload.
inline bool RefineGeomMesh(const GeomMesh &mesh, int32_t level,
                           const std::vector<float> &points_xyz,
                           const std::vector<uint32_t> &faceVertexCounts,
                           const std::vector<uint32_t> &faceVertexIndices,
                           RefinedMesh *out, std::string *err) {
  return RefineGeomMesh(mesh, level, points_xyz, faceVertexCounts,
                        faceVertexIndices, nullptr, 0, nullptr, 0, out, err);
}

}  // namespace tsd
}  // namespace lightusd

#endif  // LIGHTUSD_TSD_LIGHTUSD_HH_
