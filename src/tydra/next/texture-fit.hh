// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include <cstdint>

namespace lightusd {
namespace tydra {
namespace next {

// Controls whether texture data is left at source resolution or processed to
// fit the resident scene footprint into a byte threshold.
enum class TextureFitPolicy : uint8_t {
  Modest,      // leave textures alone only when clearly small (1/3 VRAM)
  Default,     // 2/3 VRAM
  Aggressive,  // 90% VRAM
  Never,       // never shrink/compress: assume it fits
  Always,      // always shrink/compress
  Absolute,    // explicit byte threshold
};

struct TextureFit {
  TextureFitPolicy policy = TextureFitPolicy::Default;
  uint64_t absolute_bytes = 0;  // used only with Absolute
};

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
