// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once

namespace lightusd {
namespace next {
class Stage;
class UsdPrim;

// Compute local transform matrix from xformOps
bool ComputeLocalTransform(const UsdPrim& prim, float* matrix16, double time = 0.0);
bool ComputeLocalTransform(const UsdPrim& prim, double* matrix16, double time = 0.0);

// Compute world transform (including all parent transforms)
bool ComputeWorldTransform(const Stage& stage, const UsdPrim& prim, float* matrix16, double time = 0.0);
bool ComputeWorldTransform(const Stage& stage, const UsdPrim& prim, double* matrix16, double time = 0.0);

// Check if prim has resetXformStack
bool HasResetXformStack(const UsdPrim& prim);

}  // namespace next
}  // namespace lightusd
