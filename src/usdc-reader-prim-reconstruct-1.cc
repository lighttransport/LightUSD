// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 1/4: explicit instantiations of USDCReader::Impl::ReconstructPrim<T> for a subset
// of prim types (divides per-type back-end codegen across parallel compiles). See usdc-reader-prim.cc.
#include "usdc-reader-impl.hh"
#include "usdc-reader-prim-detail.inc"

namespace lightusd {
namespace usdc {

INSTANTIATE_RECONSTRUCT_PRIM(Xform);
INSTANTIATE_RECONSTRUCT_PRIM(Model);
INSTANTIATE_RECONSTRUCT_PRIM(Scope);
INSTANTIATE_RECONSTRUCT_PRIM(RenderSettings);
INSTANTIATE_RECONSTRUCT_PRIM(RenderProduct);
INSTANTIATE_RECONSTRUCT_PRIM(RenderVar);
INSTANTIATE_RECONSTRUCT_PRIM(GenerativeProcedural);
INSTANTIATE_RECONSTRUCT_PRIM(GeomPoints);
INSTANTIATE_RECONSTRUCT_PRIM(GeomMesh);
INSTANTIATE_RECONSTRUCT_PRIM(Volume);
INSTANTIATE_RECONSTRUCT_PRIM(VolumeFieldBase);
INSTANTIATE_RECONSTRUCT_PRIM(FieldBase);
INSTANTIATE_RECONSTRUCT_PRIM(VolumeFieldAsset);
INSTANTIATE_RECONSTRUCT_PRIM(FieldAsset);
INSTANTIATE_RECONSTRUCT_PRIM(OpenVDBAsset);
INSTANTIATE_RECONSTRUCT_PRIM(Field3DAsset);
INSTANTIATE_RECONSTRUCT_PRIM(GeomCapsule);
INSTANTIATE_RECONSTRUCT_PRIM(GeomCube);
INSTANTIATE_RECONSTRUCT_PRIM(GeomCone);
INSTANTIATE_RECONSTRUCT_PRIM(GeomCylinder);
INSTANTIATE_RECONSTRUCT_PRIM(GeomSphere);
INSTANTIATE_RECONSTRUCT_PRIM(GeomSubset);
INSTANTIATE_RECONSTRUCT_PRIM(GeomBasisCurves);
INSTANTIATE_RECONSTRUCT_PRIM(GeomNurbsCurves);
INSTANTIATE_RECONSTRUCT_PRIM(GeomPlane);
INSTANTIATE_RECONSTRUCT_PRIM(GeomCylinder_1);

}  // namespace usdc
}  // namespace lightusd
