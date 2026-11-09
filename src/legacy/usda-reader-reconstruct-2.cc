// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 2/4: explicit instantiations of USDAReader::Impl::RegisterReconstructCallback<T>
// (which transitively instantiates ReconstructPrim<T>) for a subset of prim types, so the
// per-type back-end codegen is divided across parallel compiles. See usda-reader-impl.hh.
#include "usda-reader-impl.hh"

#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

namespace lightusd {
namespace usda {

#define USDA_INST_REGISTER_RECONSTRUCT(__T) \
  template bool USDAReader::Impl::RegisterReconstructCallback<__T>();
USDA_INST_REGISTER_RECONSTRUCT(GeomCapsule_1)
USDA_INST_REGISTER_RECONSTRUCT(GeomTetMesh)
USDA_INST_REGISTER_RECONSTRUCT(GeomNurbsPatch)
USDA_INST_REGISTER_RECONSTRUCT(GeomHermiteCurves)
USDA_INST_REGISTER_RECONSTRUCT(GeomCamera)
USDA_INST_REGISTER_RECONSTRUCT(GeomPointInstancer)
USDA_INST_REGISTER_RECONSTRUCT(Material)
USDA_INST_REGISTER_RECONSTRUCT(Shader)
USDA_INST_REGISTER_RECONSTRUCT(NodeGraph)
USDA_INST_REGISTER_RECONSTRUCT(Scope)
USDA_INST_REGISTER_RECONSTRUCT(RenderSettings)
USDA_INST_REGISTER_RECONSTRUCT(RenderProduct)
USDA_INST_REGISTER_RECONSTRUCT(RenderVar)
USDA_INST_REGISTER_RECONSTRUCT(GenerativeProcedural)
USDA_INST_REGISTER_RECONSTRUCT(SphereLight)
USDA_INST_REGISTER_RECONSTRUCT(DomeLight)
USDA_INST_REGISTER_RECONSTRUCT(DiskLight)
USDA_INST_REGISTER_RECONSTRUCT(DistantLight)
USDA_INST_REGISTER_RECONSTRUCT(CylinderLight)
#undef USDA_INST_REGISTER_RECONSTRUCT

}  // namespace usda
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDA_READER
