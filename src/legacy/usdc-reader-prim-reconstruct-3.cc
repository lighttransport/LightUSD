// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Split TU 3/4: explicit instantiations of USDCReader::Impl::ReconstructPrim<T> for a subset
// of prim types (divides per-type back-end codegen across parallel compiles). See usdc-reader-prim.cc.
#include "usdc-reader-impl.hh"
#include "usdc-reader-prim-detail.inc"

namespace lightusd {
namespace usdc {

INSTANTIATE_RECONSTRUCT_PRIM(PluginLightFilter);
INSTANTIATE_RECONSTRUCT_PRIM(SkelRoot);
INSTANTIATE_RECONSTRUCT_PRIM(SkelAnimation);
INSTANTIATE_RECONSTRUCT_PRIM(Skeleton);
INSTANTIATE_RECONSTRUCT_PRIM(BlendShape);
INSTANTIATE_RECONSTRUCT_PRIM(Material);
INSTANTIATE_RECONSTRUCT_PRIM(Shader);
INSTANTIATE_RECONSTRUCT_PRIM(NodeGraph);
INSTANTIATE_RECONSTRUCT_PRIM(PhysicsJoint);
INSTANTIATE_RECONSTRUCT_PRIM(PhysicsScene);
INSTANTIATE_RECONSTRUCT_PRIM(PhysicsRevoluteJoint);
INSTANTIATE_RECONSTRUCT_PRIM(PhysicsPrismaticJoint);
INSTANTIATE_RECONSTRUCT_PRIM(PhysicsSphericalJoint);
INSTANTIATE_RECONSTRUCT_PRIM(PhysicsFixedJoint);

}  // namespace usdc
}  // namespace lightusd
