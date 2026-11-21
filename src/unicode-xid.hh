// SPDX-License-Identifier: MIT
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// UTF-8 Unicode identifier XID_Start and XID_Continue validation utility.
//
// Based on UAX31 Default Identifier and Unicode 15.1.0. The tables and the
// lookups live in the C11 module src/unicode-xid-c.{c,h}; this header only
// forwards to them, so translation units that validate identifiers no longer
// parse a 1450-line table each.
//
#pragma once

#include <cstdint>

#include "unicode-xid-c.h"

namespace unicode_xid {

constexpr uint32_t kMaxCodepoint = 0x10FFFF;

inline bool is_xid_start(uint32_t codepoint) {
  return unicode_xid_is_start(static_cast<unsigned int>(codepoint)) != 0;
}

inline bool is_xid_continue(uint32_t codepoint) {
  return unicode_xid_is_continue(static_cast<unsigned int>(codepoint)) != 0;
}

}  // namespace unicode_xid
