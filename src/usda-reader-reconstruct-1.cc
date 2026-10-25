// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 1/4: explicit instantiations of USDAReader::Impl::RegisterReconstructCallback<T>
// (which transitively instantiates ReconstructPrim<T>) for a subset of prim types, so the
// per-type back-end codegen is divided across parallel compiles. See usda-reader-impl.hh.
#include "usda-reader-impl.hh"

#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

namespace lightusd {
namespace usda {

#define USDA_INST_REGISTER_RECONSTRUCT(__T) \
  template bool USDAReader::Impl::RegisterReconstructCallback<__T>();
USDA_INST_REGISTER_RECONSTRUCT(Model)
USDA_INST_REGISTER_RECONSTRUCT(GPrim)
USDA_INST_REGISTER_RECONSTRUCT(Xform)
USDA_INST_REGISTER_RECONSTRUCT(GeomCube)
USDA_INST_REGISTER_RECONSTRUCT(GeomSphere)
USDA_INST_REGISTER_RECONSTRUCT(GeomCone)
USDA_INST_REGISTER_RECONSTRUCT(GeomPoints)
USDA_INST_REGISTER_RECONSTRUCT(GeomCylinder)
USDA_INST_REGISTER_RECONSTRUCT(GeomCapsule)
USDA_INST_REGISTER_RECONSTRUCT(GeomMesh)
USDA_INST_REGISTER_RECONSTRUCT(Volume)
USDA_INST_REGISTER_RECONSTRUCT(VolumeFieldBase)
USDA_INST_REGISTER_RECONSTRUCT(FieldBase)
USDA_INST_REGISTER_RECONSTRUCT(VolumeFieldAsset)
USDA_INST_REGISTER_RECONSTRUCT(FieldAsset)
USDA_INST_REGISTER_RECONSTRUCT(OpenVDBAsset)
USDA_INST_REGISTER_RECONSTRUCT(Field3DAsset)
USDA_INST_REGISTER_RECONSTRUCT(GeomSubset)
USDA_INST_REGISTER_RECONSTRUCT(GeomBasisCurves)
USDA_INST_REGISTER_RECONSTRUCT(GeomNurbsCurves)
USDA_INST_REGISTER_RECONSTRUCT(GeomPlane)
USDA_INST_REGISTER_RECONSTRUCT(GeomCylinder_1)
#undef USDA_INST_REGISTER_RECONSTRUCT

}  // namespace usda
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDA_READER
