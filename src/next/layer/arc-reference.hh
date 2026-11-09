// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once

#include <cstddef>
#include <string>

namespace lightusd {
namespace next {

// References retain their canonical asset/path/offset spelling. A dictionary
// suffix belongs to the reference item itself (including list-op identity),
// rather than a prim-wide map that would alias distinct reference items.
// Search only after the asset and prim path, so asset names cannot introduce
// the internal separator. The suffix is canonical, typed USDA dictionary text.
inline size_t ArcCustomDataPosition(const std::string& arc) {
  size_t start = 0;
  if (!arc.empty() && arc[0] == '@') {
    const size_t end = arc.find('@', 1);
    if (end == std::string::npos) return std::string::npos;
    start = end + 1;
  }
  if (start < arc.size() && arc[start] == '<') {
    const size_t end = arc.find('>', start + 1);
    if (end == std::string::npos) return std::string::npos;
    start = end + 1;
  }
  return arc.find('\x1f', start);
}

inline std::string ArcReferenceBody(const std::string& arc) {
  return arc.substr(0, ArcCustomDataPosition(arc));
}

inline std::string ArcReferenceCustomData(const std::string& arc) {
  const size_t pos = ArcCustomDataPosition(arc);
  return pos == std::string::npos ? std::string() : arc.substr(pos + 1);
}

}  // namespace next
}  // namespace lightusd
