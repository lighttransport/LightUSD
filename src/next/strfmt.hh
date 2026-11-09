// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - integer<->string formatting (C++ adapter).
//
// The digit loop lives in the C11 core (src/next/c/strfmt.c, `lu_uint_to` /
// `lu_int_to`); these wrappers forward to it so every "next" subsystem and C
// consumer shares one copy. Locale-independent, libc-free, output byte-
// identical to `ss << v` under the classic locale: plain decimal, leading '-'
// for negatives, no '+'/padding/grouping. Kept dependency-light (<string>/
// <cstdint> only).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "c/lightusd-core-c.h"

namespace lightusd {
namespace next {

// Format the decimal form of `v` into a caller buffer, returning the byte count.
// `buf` capacity must be >= 20 (a uint64_t needs at most 20 digits). No heap.
inline size_t UIntTo(char* buf, uint64_t v) { return lu_uint_to(buf, v); }

// Format the decimal form of `v` into a caller buffer, returning the byte count.
// `buf` capacity must be >= 21 (leading '-' + up to 20 digits). No heap.
inline size_t IntTo(char* buf, int64_t v) { return lu_int_to(buf, v); }

// Append the decimal form of `v` to `o` (no heap churn for the common short case;
// a uint64_t needs at most 20 digits).
inline void AppendUInt(std::string& o, uint64_t v) {
  char buf[20];
  o.append(buf, UIntTo(buf, v));
}

inline void AppendInt(std::string& o, int64_t v) {
  char buf[21];
  o.append(buf, IntTo(buf, v));
}

inline std::string UIntToStr(uint64_t v) {
  std::string s;
  AppendUInt(s, v);
  return s;
}

inline std::string IntToStr(int64_t v) {
  std::string s;
  AppendInt(s, v);
  return s;
}

}  // namespace next
}  // namespace lightusd
