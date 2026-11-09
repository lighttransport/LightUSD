// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include "texture-fit.hh"

namespace lightusd {
namespace tydra {
namespace next {

constexpr uint64_t GiB(uint64_t value) {
  return value > ((std::numeric_limits<uint64_t>::max)() >> 30)
             ? (std::numeric_limits<uint64_t>::max)()
             : value << 30;
}
constexpr uint64_t MiB(uint64_t value) {
  return value > ((std::numeric_limits<uint64_t>::max)() >> 20)
             ? (std::numeric_limits<uint64_t>::max)()
             : value << 20;
}

inline uint64_t Percent(uint64_t value, uint64_t percent) {
  if (percent == 0) return 0;
  // Split before multiplying so hostile capacity values cannot overflow the
  // budget formulas.
  const uint64_t whole = value / 100;
  const uint64_t remainder = value % 100;
  const uint64_t first =
      whole > (std::numeric_limits<uint64_t>::max)() / percent
          ? (std::numeric_limits<uint64_t>::max)()
          : whole * percent;
  const uint64_t second =
      remainder > (std::numeric_limits<uint64_t>::max)() / percent
          ? (std::numeric_limits<uint64_t>::max)()
          : (remainder * percent) / 100;
  return first > (std::numeric_limits<uint64_t>::max)() - second
             ? (std::numeric_limits<uint64_t>::max)()
             : first + second;
}

enum class LargeSceneQuality : uint8_t { Full, Adaptive, Proxy };

struct ResourceBudget {
  uint64_t host_capacity = 0;
  uint64_t host_limit = 0;
  uint64_t stage_limit = 0;
  uint64_t cpu_geometry_limit = 0;
  uint64_t io_cache_limit = 0;

  uint64_t vram_capacity = 0;
  uint64_t vram_limit = 0;
  uint64_t gpu_geometry_limit = 0;
  uint64_t gpu_texture_limit = 0;
  uint64_t upload_staging_limit = 0;
  uint64_t proxy_geometry_threshold = 0;

  LargeSceneQuality quality = LargeSceneQuality::Adaptive;
};

// Leaves two GiB to the display/system on 10 GiB-class devices. Devices with
// at least 12 GiB use half their capacity but never less than eight GiB.
inline uint64_t ComputeVramLimit(uint64_t capacity) {
  if (capacity == 0) return GiB(8);
  if (capacity >= GiB(12)) {
    return std::min(capacity, std::max(GiB(8), capacity / 2));
  }
  if (capacity > GiB(2)) return capacity - GiB(2);
  return capacity - capacity / 4;
}

inline ResourceBudget ComputeResourceBudget(uint64_t host_capacity,
                                            uint64_t vram_capacity) {
  ResourceBudget budget;
  budget.host_capacity = host_capacity;
  // The 32 GiB target retains two GiB for the OS, driver, and allocator
  // fragmentation. Smaller machines retain at least one GiB or one eighth.
  const uint64_t host_reserve =
      host_capacity >= GiB(16)
          ? GiB(2)
          : std::max(GiB(1), host_capacity / uint64_t{8});
  budget.host_limit = host_capacity > host_reserve
                          ? host_capacity - host_reserve
                          : host_capacity - host_capacity / 4;
  budget.stage_limit = Percent(budget.host_limit, 55);
  budget.cpu_geometry_limit = Percent(budget.host_limit, 25);
  budget.io_cache_limit = Percent(budget.host_limit, 10);

  budget.vram_capacity = vram_capacity;
  budget.vram_limit = ComputeVramLimit(vram_capacity);
  budget.upload_staging_limit =
      std::min(MiB(512), std::max(MiB(64), budget.vram_limit / 16));
  const uint64_t gpu_resident =
      budget.vram_limit > budget.upload_staging_limit
          ? budget.vram_limit - budget.upload_staging_limit
          : budget.vram_limit;
  budget.gpu_geometry_limit = Percent(gpu_resident, 65);
  budget.gpu_texture_limit = Percent(gpu_resident, 25);
  budget.proxy_geometry_threshold =
      std::min(MiB(256), std::max(MiB(16), budget.gpu_geometry_limit / 16));
  return budget;
}

// Decode-time texture limits derived from a resource budget. Applications hand
// these to tydra::next::TextureDecoder so a large scene's textures are capped
// while they are decoded, rather than after they are all resident.
struct TextureBudget {
  // Longest-edge cap in texels. 0 = keep source resolution.
  uint32_t max_edge = 0;
  // Best-effort cap on total decoded texture bytes. 0 = unbounded.
  uint64_t budget_bytes = 0;
};

// The edge cap keys off the quality tier rather than the device size: a 4K
// albedo is wasted on a proxy-quality overview no matter how much VRAM is free,
// and the byte budget already handles the device-size axis.
inline TextureBudget DeriveTextureBudget(const ResourceBudget& budget) {
  TextureBudget texture;
  texture.budget_bytes = budget.gpu_texture_limit;
  switch (budget.quality) {
    case LargeSceneQuality::Full:
      texture.max_edge = 0;
      break;
    case LargeSceneQuality::Proxy:
      texture.max_edge = 1024;
      break;
    case LargeSceneQuality::Adaptive:
    default:
      texture.max_edge = 2048;
      break;
  }
  return texture;
}


// ---------------------------------------------------------------------------
// Texture-fit policy
//
// How eagerly the viewer should spend CPU time shrinking and block-compressing
// textures so a scene fits on the GPU. The threshold is compared against the
// scene's whole estimated resident footprint (geometry + decoded textures),
// because both share the device.
//
// This is a RESIDENCY axis and is deliberately separate from
// ResourceBudget::quality, which is a FIDELITY axis ("a 4K albedo is wasted on
// a proxy-quality overview no matter how much VRAM is free"). Conflating them
// would make "never shrink" also mean "render at proxy quality".
//
// The fractions are of total VRAM *capacity*, not currently-free VRAM, so the
// same scene decides the same way on the same card regardless of what else is
// running. --vram-budget rehearses a different capacity.
// "modest"|"default"|"aggressive"|"never"|"always"|"<N>[KMG]".
// Suffixes match lusdview's ParseByteCount. Returns false on anything else.
inline bool ParseTextureFit(const std::string& text, TextureFit* out) {
  if (!out || text.empty()) return false;
  if (text == "modest") { *out = {TextureFitPolicy::Modest, 0}; return true; }
  if (text == "default") { *out = {TextureFitPolicy::Default, 0}; return true; }
  if (text == "aggressive") { *out = {TextureFitPolicy::Aggressive, 0}; return true; }
  if (text == "never") { *out = {TextureFitPolicy::Never, 0}; return true; }
  if (text == "always") { *out = {TextureFitPolicy::Always, 0}; return true; }
  std::string v = text;
  uint64_t mul = 1;
  const char suffix = v.back();
  if (suffix == 'k' || suffix == 'K') { mul = 1024ull; v.pop_back(); }
  else if (suffix == 'm' || suffix == 'M') { mul = 1024ull * 1024ull; v.pop_back(); }
  else if (suffix == 'g' || suffix == 'G') { mul = 1024ull * 1024ull * 1024ull; v.pop_back(); }
  if (v.empty()) return false;
  uint64_t n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (n > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) {
      return false;
    }
    n = n * 10 + digit;
  }
  if (n == 0 || n > (std::numeric_limits<uint64_t>::max)() / mul) {
    return false;
  }
  *out = {TextureFitPolicy::Absolute, n * mul};
  return true;
}

// Resident-byte threshold the scene must stay under to be left alone.
// Never -> "everything fits"; Always -> "nothing fits".
inline uint64_t TextureFitThresholdBytes(const TextureFit& fit,
                                         uint64_t vram_capacity) {
  switch (fit.policy) {
    case TextureFitPolicy::Never:
      return (std::numeric_limits<uint64_t>::max)();
    case TextureFitPolicy::Always:
      return 0;
    case TextureFitPolicy::Absolute:
      return fit.absolute_bytes;
    case TextureFitPolicy::Modest:
      return Percent(vram_capacity, 33);
    case TextureFitPolicy::Aggressive:
      return Percent(vram_capacity, 90);
    case TextureFitPolicy::Default:
    default:
      return Percent(vram_capacity, 66);
  }
}

inline const char* TextureFitName(const TextureFit& fit) {
  switch (fit.policy) {
    case TextureFitPolicy::Modest: return "modest";
    case TextureFitPolicy::Aggressive: return "aggressive";
    case TextureFitPolicy::Never: return "never";
    case TextureFitPolicy::Always: return "always";
    case TextureFitPolicy::Absolute: return "absolute";
    case TextureFitPolicy::Default:
    default: return "default";
  }
}

// Percentage the policy resolves to, for logging. 0 for the non-fractional
// policies (their name already says what they do).
inline uint32_t TextureFitPercent(const TextureFit& fit) {
  switch (fit.policy) {
    case TextureFitPolicy::Modest: return 33;
    case TextureFitPolicy::Aggressive: return 90;
    case TextureFitPolicy::Default: return 66;
    default: return 0;
  }
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
