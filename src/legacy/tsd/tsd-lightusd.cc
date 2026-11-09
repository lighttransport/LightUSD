// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// tinysubdiv <-> lightusd adapter. The only tsd file that includes
// lightusd types.

#include "tsd-lightusd.hh"

#include <cstdint>
#include <limits>

#include "usdGeom.hh"

namespace lightusd {
namespace tsd {

namespace {

bool ToU32(size_t n, uint32_t *out) {
#if SIZE_MAX > UINT32_MAX
  if (n > size_t((std::numeric_limits<uint32_t>::max)())) {
    return false;
  }
#endif
  *out = uint32_t(n);
  return true;
}

// Reads the default value of an authored vector attribute (subdiv tags are
// not animatable in practice); absent attributes yield an empty vector.
template <typename T>
std::vector<T> GetTagValues(
    const TypedAttribute<Animatable<std::vector<T>>> &attr) {
  std::vector<T> values;
  if (attr.has_value()) {
    auto animatable = attr.get_value();
    if (animatable && animatable->has_default()) {
      animatable->get_default(&values);
    }
  }
  return values;
}

}  // namespace

bool RefineGeomMesh(const GeomMesh &mesh, int32_t level,
                    const std::vector<float> &points_xyz,
                    const std::vector<uint32_t> &faceVertexCounts,
                    const std::vector<uint32_t> &faceVertexIndices,
                    const FVarChannelView *fvar_channels,
                    uint32_t num_fvar_channels,
                    const VertexPrimvarView *vertex_primvars,
                    uint32_t num_vertex_primvars, RefinedMesh *out,
                    std::string *err) {
  auto fail = [&](const char *msg) {
    if (err) {
      (*err) += msg;
      (*err) += "\n";
    }
    return false;
  };

  if ((num_fvar_channels > 0 && !fvar_channels) ||
      (num_vertex_primvars > 0 && !vertex_primvars)) {
    return fail("subdivision primvar view pointer is null.");
  }
  if ((points_xyz.size() % 3) != 0) {
    return fail("points_xyz length must be divisible by 3.");
  }

  Options options;
  options.level = level;

  switch (mesh.subdivisionScheme.get_value()) {
    case GeomMesh::SubdivisionScheme::CatmullClark:
      options.scheme = Scheme::CatmullClark;
      break;
    case GeomMesh::SubdivisionScheme::Loop:
      options.scheme = Scheme::Loop;
      break;
    case GeomMesh::SubdivisionScheme::Bilinear:
      options.scheme = Scheme::Bilinear;
      break;
    case GeomMesh::SubdivisionScheme::SubdivisionSchemeNone:
      if (err) {
        (*err) +=
            "subdivisionScheme is 'none'; skip subdivision on the caller "
            "side.\n";
      }
      return false;
  }

  options.boundary = BoundaryInterpolation::EdgeAndCorner;
  {
    GeomMesh::InterpolateBoundary boundary =
        GeomMesh::InterpolateBoundary::EdgeAndCorner;
    const auto &animatable = mesh.interpolateBoundary.get_value();
    if (animatable.has_default()) {
      animatable.get_default(&boundary);
    }
    switch (boundary) {
      case GeomMesh::InterpolateBoundary::EdgeAndCorner:
        options.boundary = BoundaryInterpolation::EdgeAndCorner;
        break;
      case GeomMesh::InterpolateBoundary::EdgeOnly:
        options.boundary = BoundaryInterpolation::EdgeOnly;
        break;
      case GeomMesh::InterpolateBoundary::InterpolateBoundaryNone:
        options.boundary = BoundaryInterpolation::None;
        break;
    }
  }

  // `triangleSubdivisionRule` is a uniform token (no Animatable), so
  // get_value() yields the effective enum (authored or the CatmullClark
  // fallback) directly, like subdivisionScheme above.
  switch (mesh.triangleSubdivisionRule.get_value()) {
    case GeomMesh::TriangleSubdivisionRule::CatmullClark:
      options.triangle_subdivision = TriangleSubdivision::CatmullClark;
      break;
    case GeomMesh::TriangleSubdivisionRule::Smooth:
      options.triangle_subdivision = TriangleSubdivision::Smooth;
      break;
  }

  const std::vector<int32_t> corner_indices = GetTagValues(mesh.cornerIndices);
  const std::vector<float> corner_sharpnesses =
      GetTagValues(mesh.cornerSharpnesses);
  const std::vector<int32_t> crease_indices = GetTagValues(mesh.creaseIndices);
  const std::vector<int32_t> crease_lengths = GetTagValues(mesh.creaseLengths);
  const std::vector<float> crease_sharpnesses =
      GetTagValues(mesh.creaseSharpnesses);
  const std::vector<int32_t> hole_indices = GetTagValues(mesh.holeIndices);

  MeshView view;
  view.points = points_xyz.data();
  if (!ToU32(points_xyz.size() / 3, &view.num_points) ||
      !ToU32(faceVertexCounts.size(), &view.num_faces) ||
      !ToU32(faceVertexIndices.size(), &view.num_face_vertex_indices)) {
    return fail("subdivision mesh counts exceed 32-bit index space.");
  }
  view.face_vertex_counts = faceVertexCounts.data();
  view.face_vertex_indices = faceVertexIndices.data();

  if (!corner_indices.empty() &&
      corner_indices.size() == corner_sharpnesses.size()) {
    view.corner_indices = corner_indices.data();
    if (!ToU32(corner_indices.size(), &view.num_corners)) {
      return fail("corner index count exceeds 32-bit index space.");
    }
    view.corner_sharpnesses = corner_sharpnesses.data();
  }
  if (!crease_lengths.empty()) {
    view.crease_indices = crease_indices.data();
    view.crease_lengths = crease_lengths.data();
    view.crease_sharpnesses = crease_sharpnesses.data();
    if (!ToU32(crease_indices.size(), &view.num_crease_indices) ||
        !ToU32(crease_lengths.size(), &view.num_crease_lengths) ||
        !ToU32(crease_sharpnesses.size(), &view.num_crease_sharpnesses)) {
      return fail("crease tag counts exceed 32-bit index space.");
    }
  }
  if (!hole_indices.empty()) {
    view.hole_indices = hole_indices.data();
    if (!ToU32(hole_indices.size(), &view.num_holes)) {
      return fail("hole index count exceeds 32-bit index space.");
    }
  }

  // Stamp the mesh's authored faceVaryingLinearInterpolation on every
  // channel (it is a mesh-level attribute in USD).
  FVarLinearInterpolation fvar_mode = FVarLinearInterpolation::CornersPlus1;
  {
    GeomMesh::FaceVaryingLinearInterpolation usd_mode =
        GeomMesh::FaceVaryingLinearInterpolation::CornersPlus1;
    const auto &animatable = mesh.faceVaryingLinearInterpolation.get_value();
    if (animatable.has_default()) {
      animatable.get_default(&usd_mode);
    }
    switch (usd_mode) {
      case GeomMesh::FaceVaryingLinearInterpolation::CornersPlus1:
        fvar_mode = FVarLinearInterpolation::CornersPlus1;
        break;
      case GeomMesh::FaceVaryingLinearInterpolation::CornersPlus2:
        fvar_mode = FVarLinearInterpolation::CornersPlus2;
        break;
      case GeomMesh::FaceVaryingLinearInterpolation::CornersOnly:
        fvar_mode = FVarLinearInterpolation::CornersOnly;
        break;
      case GeomMesh::FaceVaryingLinearInterpolation::Boundaries:
        fvar_mode = FVarLinearInterpolation::Boundaries;
        break;
      case GeomMesh::FaceVaryingLinearInterpolation::
          FaceVaryingLinearInterpolationNone:
        fvar_mode = FVarLinearInterpolation::None;
        break;
      case GeomMesh::FaceVaryingLinearInterpolation::All:
        fvar_mode = FVarLinearInterpolation::All;
        break;
    }
  }
  std::vector<FVarChannelView> channels;
  if (num_fvar_channels) {
    channels.assign(fvar_channels, fvar_channels + num_fvar_channels);
  }
  for (FVarChannelView &ch : channels) {
    ch.interpolation = fvar_mode;
  }

  std::string tsd_err;
  const Result r =
      Refine(view, channels.empty() ? nullptr : channels.data(),
             num_fvar_channels, vertex_primvars, num_vertex_primvars, options,
             out, &tsd_err);
  if (r != Result::Success) {
    if (err) {
      (*err) += "tinysubdiv refinement failed (";
      (*err) += to_string(r);
      (*err) += "): " + tsd_err;
    }
    return false;
  }
  return true;
}

}  // namespace tsd
}  // namespace lightusd
