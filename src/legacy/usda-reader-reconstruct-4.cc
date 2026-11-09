// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 4/4: explicit instantiations of USDAReader::Impl::RegisterReconstructCallback<T>
// (which transitively instantiates ReconstructPrim<T>) for a subset of prim types, so the
// per-type back-end codegen is divided across parallel compiles. See usda-reader-impl.hh.
#include "usda-reader-impl.hh"

#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

namespace lightusd {
namespace usda {

#define USDA_INST_REGISTER_RECONSTRUCT(__T) \
  template bool USDAReader::Impl::RegisterReconstructCallback<__T>();
USDA_INST_REGISTER_RECONSTRUCT(PhysicsFixedJoint)
USDA_INST_REGISTER_RECONSTRUCT(PhysicsDistanceJoint)
USDA_INST_REGISTER_RECONSTRUCT(PhysicsCollisionGroup)
USDA_INST_REGISTER_RECONSTRUCT(MjcActuator)
USDA_INST_REGISTER_RECONSTRUCT(NewtonActuator)
USDA_INST_REGISTER_RECONSTRUCT(MjcTendon)
USDA_INST_REGISTER_RECONSTRUCT(MjcKeyframe)
USDA_INST_REGISTER_RECONSTRUCT(MjcSensor)
USDA_INST_REGISTER_RECONSTRUCT(Preliminary_PhysicsGravitationalForce)
USDA_INST_REGISTER_RECONSTRUCT(Preliminary_InfiniteColliderPlane)
USDA_INST_REGISTER_RECONSTRUCT(Preliminary_ReferenceImage)
USDA_INST_REGISTER_RECONSTRUCT(Preliminary_Behavior)
USDA_INST_REGISTER_RECONSTRUCT(Preliminary_Trigger)
USDA_INST_REGISTER_RECONSTRUCT(Preliminary_Action)
USDA_INST_REGISTER_RECONSTRUCT(Preliminary_Text)
USDA_INST_REGISTER_RECONSTRUCT(SpatialAudio)
#undef USDA_INST_REGISTER_RECONSTRUCT

}  // namespace usda
}  // namespace lightusd

#endif  // !LIGHTUSD_DISABLE_MODULE_USDA_READER
