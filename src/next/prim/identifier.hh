// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// AOUSD Unicode identifier and path validation for the standalone next core.
//
// The grammar and the UTF-8 decoder live in the C11 core
// (src/next/c/identifier.c), which reuses the shared XID module
// (src/unicode-xid-c.{c,h}). These thin adapters keep the historical C++ names.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../c/lightusd-core-c.h"

namespace lightusd {
namespace next {

namespace identifier_detail {

inline bool DecodeUtf8(const char* data, size_t size, size_t offset,
                       uint32_t* codepoint, size_t* width) {
  return lu_utf8_decode(data, size, offset, codepoint, width) != 0;
}

inline bool IsStart(uint32_t cp) {
  return lu_is_identifier_start(cp) != 0;
}

inline bool IsContinue(uint32_t cp) {
  return lu_is_identifier_continue(cp) != 0;
}

}  // namespace identifier_detail

inline bool IsValidIdentifier(const std::string& text) {
  return lu_is_valid_identifier(text.data(), text.size()) != 0;
}

inline bool IsValidNamespacedIdentifier(const std::string& text) {
  return lu_is_valid_namespaced_identifier(text.data(), text.size()) != 0;
}

/// Validate the AOUSD §8 textual Path production (angle brackets excluded).
inline bool IsValidPathString(const std::string& text) {
  return lu_is_valid_path_string(text.data(), text.size()) != 0;
}

}  // namespace next
}  // namespace lightusd
