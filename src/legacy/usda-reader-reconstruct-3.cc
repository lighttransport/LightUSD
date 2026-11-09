// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 3/4: explicit instantiations of USDAReader::Impl::RegisterReconstructCallback<T>
// (which transitively instantiates ReconstructPrim<T>) for a subset of prim types, so the
// per-type back-end codegen is divided across parallel compiles. See usda-reader-impl.hh.
#include "usda-reader-impl.hh"

#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

namespace lightusd {
namespace usda {

#define USDA_INST_REGISTER_RECONSTRUCT(__T) \
  template bool USDAReader::Impl::RegisterReconstructCallback<__T>();
USDA_INST_REGISTER_RECONSTRUCT(RectLight)
USDA_INST_REGISTER_RECONSTRUCT(GeometryLight)
USDA_INST_REGISTER_RECONSTRUCT(PortalLight)
USDA_INST_REGISTER_RECONSTRUCT(DomeLight_1)
USDA_INST_REGISTER_RECONSTRUCT(LightFilter)
USDA_INST_REGISTER_RECONSTRUCT(PluginLightFilter)
USDA_INST_REGISTER_RECONSTRUCT(SkelRoot)
USDA_INST_REGISTER_RECONSTRUCT(Skeleton)
USDA_INST_REGISTER_RECONSTRUCT(SkelAnimation)
USDA_INST_REGISTER_RECONSTRUCT(BlendShape)
USDA_INST_REGISTER_RECONSTRUCT(PhysicsJoint)
USDA_INST_REGISTER_RECONSTRUCT(PhysicsScene)
USDA_INST_REGISTER_RECONSTRUCT(PhysicsRevoluteJoint)
USDA_INST_REGISTER_RECONSTRUCT(PhysicsPrismaticJoint)
USDA_INST_REGISTER_RECONSTRUCT(PhysicsSphericalJoint)
#undef USDA_INST_REGISTER_RECONSTRUCT

}  // namespace usda
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDA_READER
