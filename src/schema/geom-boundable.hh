// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - UsdGeomBoundable extent computation
#pragma once

namespace lightusd {
class UsdPrim;

/// Compute the local-space extent of a Boundable gprim at `time` as
/// {min x, min y, min z, max x, max y, max z}, following the OpenUSD
/// UsdGeomBoundable::ComputeExtentFromPlugins rules:
///  - PointBased (Mesh, NurbsPatch, TetMesh, ...): bounds of `points`;
///  - Points: each point grown by widths[i] / 2; widths must match the point
///    count when authored (else no extent), unauthored widths -> points only;
///  - Curves (BasisCurves, NurbsCurves, HermiteCurves): control points grown
///    by the maximum width / 2;
///  - Cube, Sphere, Cylinder, Cone, Capsule, Cylinder_1, Capsule_1, Plane:
///    closed form from size/radius/height/axis (schema fallbacks apply).
/// Returns false for other prim types, empty points, an invalid axis or
/// mismatched widths.
bool ComputeExtent(const UsdPrim& prim, double time, double out[6]);

}  // namespace lightusd
