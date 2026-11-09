// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 4/4: explicit instantiations of USDCReader::Impl::ReconstructPrim<T> for a subset
// of prim types (divides per-type back-end codegen across parallel compiles). See usdc-reader-prim.cc.
#include "usdc-reader-impl.hh"
#include "usdc-reader-prim-detail.inc"

namespace lightusd {
namespace usdc {

INSTANTIATE_RECONSTRUCT_PRIM(PhysicsDistanceJoint);
INSTANTIATE_RECONSTRUCT_PRIM(PhysicsCollisionGroup);
INSTANTIATE_RECONSTRUCT_PRIM(MjcActuator);
INSTANTIATE_RECONSTRUCT_PRIM(NewtonActuator);
INSTANTIATE_RECONSTRUCT_PRIM(MjcTendon);
INSTANTIATE_RECONSTRUCT_PRIM(MjcKeyframe);
INSTANTIATE_RECONSTRUCT_PRIM(MjcSensor);
INSTANTIATE_RECONSTRUCT_PRIM(Preliminary_PhysicsGravitationalForce);
INSTANTIATE_RECONSTRUCT_PRIM(Preliminary_InfiniteColliderPlane);
INSTANTIATE_RECONSTRUCT_PRIM(Preliminary_ReferenceImage);
INSTANTIATE_RECONSTRUCT_PRIM(Preliminary_Behavior);
INSTANTIATE_RECONSTRUCT_PRIM(Preliminary_Trigger);
INSTANTIATE_RECONSTRUCT_PRIM(Preliminary_Action);
INSTANTIATE_RECONSTRUCT_PRIM(Preliminary_Text);
INSTANTIATE_RECONSTRUCT_PRIM(SpatialAudio);

}  // namespace usdc
}  // namespace lightusd
