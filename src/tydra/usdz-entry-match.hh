// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Matching a USD asset path against a .usdz archive entry name.
//
// Header-only and dependency-free so it can be unit tested without linking the
// texture decoder (which drags in the image and Ptex readers).
#pragma once

#include <cstring>
#include <string>

namespace lightusd {
namespace tydra {
namespace next {

// A (ptr, len) slice, so the matcher compares path components without the
// substr() allocations the original implementation did per candidate entry.
struct UsdzStrSlice {
  const char* data = nullptr;
  size_t size = 0;
  bool equals(const UsdzStrSlice& o) const {
    return size == o.size && (size == 0 || std::memcmp(data, o.data, size) == 0);
  }
};

inline UsdzStrSlice UsdzBaseName(UsdzStrSlice s) {
  for (size_t i = s.size; i-- > 0;) {
    if (s.data[i] == '/') return UsdzStrSlice{s.data + i + 1, s.size - i - 1};
  }
  return s;
}

// Strip a leading "./" the way the matcher does.
inline UsdzStrSlice UsdzNormalizeAsset(const std::string& asset) {
  UsdzStrSlice a{asset.data(), asset.size()};
  if (a.size >= 2 && a.data[0] == '.' && a.data[1] == '/') {
    a.data += 2;
    a.size -= 2;
  }
  return a;
}

/// Match a USD asset path against a .usdz entry name. Entries are archive-
/// relative and may carry a directory prefix the authored path omits, so
/// accept an exact match, a path-suffix match on a directory boundary, or a
/// basename match as the last resort.
///
/// INVARIANT: every one of those tiers implies the basenames are equal. That
/// is what lets TextureDecoder prefilter candidate entries by basename without
/// changing which entry wins.
inline bool UsdzEntryMatches(const std::string& entry,
                             const std::string& asset) {
  const UsdzStrSlice a = UsdzNormalizeAsset(asset);
  const UsdzStrSlice e{entry.data(), entry.size()};
  if (e.equals(a)) return true;
  if (e.size > a.size &&
      std::memcmp(e.data + e.size - a.size, a.data, a.size) == 0 &&
      e.data[e.size - a.size - 1] == '/') {
    return true;
  }
  return UsdzBaseName(e).equals(UsdzBaseName(a));
}

/// Basename of an asset path as a map key (leading "./" stripped), matching
/// what UsdzEntryMatches compares.
inline std::string UsdzAssetBaseKey(const std::string& asset) {
  const UsdzStrSlice b = UsdzBaseName(UsdzNormalizeAsset(asset));
  return std::string(b.data, b.size);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
