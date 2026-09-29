// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP cache local utility helpers.

#pragma once

#include "../strfmt.hh"

#include <cstdint>
#include <string>

namespace lightusd {
namespace next {
namespace pcp {

inline std::string FormatMilliseconds(double ms) {
  std::string s;
  if (ms < 0) {
    s += '-';
    ms = -ms;
  }
  uint64_t tenths = static_cast<uint64_t>(ms * 10.0 + 0.5);
  AppendUInt(s, tenths / 10);
  s += '.';
  AppendUInt(s, tenths % 10);
  return s;
}

inline bool IsPathAtOrUnder(const std::string& child,
                            const std::string& base) {
  if (child == base) return true;
  // The pseudo-root already ends in a separator. Requiring another one would
  // miss every descendant when a batch's common ancestor is "/".
  if (base == "/") return !child.empty() && child[0] == '/';
  return child.size() > base.size() &&
         child.compare(0, base.size(), base) == 0 && child[base.size()] == '/';
}

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
