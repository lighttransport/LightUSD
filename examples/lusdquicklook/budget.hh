// SPDX-License-Identifier: Apache-2.0
//
// lusdquicklook — the preview memory budget.
//
// Thin policy layer over the shared tracking budget in
// src/tydra/mem-budget.hh: it owns the split of the single --max-mem number
// into the per-phase shares that the loader, the scene sink and the renderer
// each check against.
#pragma once

#include <cstdint>
#include <string>

#include "tydra/mem-budget.hh"

namespace lusdql {

using MemBudget = lightusd::tydra::MemBudget;
using MemPool = lightusd::tydra::MemPool;
template <class T>
using PoolAlloc = lightusd::tydra::PoolAlloc<T>;

// How the cap is divided. These are advisory ceilings for the individual
// phases; the hard stop is the shared MemBudget cap, which counts every
// PoolAlloc byte regardless of which share it came from.
struct PreviewBudget {
  uint64_t total = 0;

  uint64_t stage = 0;     // USD stage + composition (::lightusd::StageSession)
  uint64_t geometry = 0;  // QlMesh vertex/index data
  uint64_t textures = 0;  // decoded, downsampled base-color maps
  uint64_t render = 0;    // framebuffers, accumulation, BVH

  // Installs `total_bytes` as the process cap (clamped to what the machine can
  // back) and derives the shares.
  static PreviewBudget Install(uint64_t total_bytes);

  // Bytes still available against the process cap, from the tracking counter.
  static uint64_t RemainingTracked();

  // "142 / 512 MB" for the status bar.
  std::string FormatUsage() const;
};

}  // namespace lusdql
