// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// lusdrender's view of the shared app math (examples/common) plus its
// purpose / up-axis enums.
#pragma once

#include "lightusd-value-math.hh"

namespace lusdr {

// UsdGeomImageable purpose.
enum class Purpose { Default, Render, Proxy, Guide };

// Stage up axis.
enum class Axis { X, Y, Z, Invalid };

namespace value = lvmath::value;
using lvmath::inverse;
using lvmath::to_matrix;
using lvmath::to_matrix3x3;

}  // namespace lusdr
