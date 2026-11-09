// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Hash function utilities for unordered containers
//
#pragma once

#include <cstddef>
#include <string>

namespace lightusd {

///
/// FNV-1a string hash function for use with std::unordered_map.
/// This provides better performance than the standard std::hash<std::string>
/// for string keys in hash tables.
///
struct FNV1StringHash {
  size_t operator()(const std::string &s) const noexcept {
    static constexpr uint64_t kFNV_Prime = 0x00000100000001B3ull;
    static constexpr uint64_t kFNV_Offset_Basis = 0xcbf29ce484222325ull;

    uint64_t hash = kFNV_Offset_Basis;
    for (char ch : s) {
      hash = (kFNV_Prime * hash) ^ static_cast<unsigned char>(ch);
    }
    return static_cast<size_t>(hash);
  }
};

}  // namespace lightusd
