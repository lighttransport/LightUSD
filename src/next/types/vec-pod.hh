// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - POD vector aliases used by the render converter.
//
// The converter historically reached for the legacy value-type layer
// (`value-types.hh`, ~3000 lines of template-erasure machinery) just to get
// `value::float2` / `value::float3` -- both plain `std::array<float, N>`. This
// header is the whole dependency: 30 lines, no templates beyond <array>, and no
// nonstd/expected, optional or span include chain.
//
// Re-declaring these aliases is legal even in a translation unit that also
// pulls in the legacy header: they name the same type.
//
#pragma once

#include <array>

namespace lightusd {
namespace value {

using float2 = std::array<float, 2>;
using float3 = std::array<float, 3>;
using float4 = std::array<float, 4>;

}  // namespace value
}  // namespace lightusd
