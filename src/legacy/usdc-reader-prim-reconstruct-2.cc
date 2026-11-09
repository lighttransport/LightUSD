// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 2/4: explicit instantiations of USDCReader::Impl::ReconstructPrim<T> for a subset
// of prim types (divides per-type back-end codegen across parallel compiles). See usdc-reader-prim.cc.
#include "usdc-reader-impl.hh"
#include "usdc-reader-prim-detail.inc"

namespace lightusd {
namespace usdc {

INSTANTIATE_RECONSTRUCT_PRIM(GeomCapsule_1);
INSTANTIATE_RECONSTRUCT_PRIM(GeomTetMesh);
INSTANTIATE_RECONSTRUCT_PRIM(GeomNurbsPatch);
INSTANTIATE_RECONSTRUCT_PRIM(GeomHermiteCurves);
INSTANTIATE_RECONSTRUCT_PRIM(GeomCamera);
INSTANTIATE_RECONSTRUCT_PRIM(GeomPointInstancer);
INSTANTIATE_RECONSTRUCT_PRIM(SphereLight);
INSTANTIATE_RECONSTRUCT_PRIM(DomeLight);
INSTANTIATE_RECONSTRUCT_PRIM(DiskLight);
INSTANTIATE_RECONSTRUCT_PRIM(DistantLight);
INSTANTIATE_RECONSTRUCT_PRIM(CylinderLight);
INSTANTIATE_RECONSTRUCT_PRIM(RectLight);
INSTANTIATE_RECONSTRUCT_PRIM(GeometryLight);
INSTANTIATE_RECONSTRUCT_PRIM(DomeLight_1);
INSTANTIATE_RECONSTRUCT_PRIM(LightFilter);

}  // namespace usdc
}  // namespace lightusd
