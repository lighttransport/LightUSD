// SPDX-License-Identifier: Apache-2.0
// Small shared utilities for the next core (previously copy-pasted into the
// pcp/pipeline/reader/lightusd-next TUs).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace lightusd {
namespace next {

// Smallest non-zero of two sizes; zero means "unset".
inline size_t MinNonZero(size_t a, size_t b) {
  if (a == 0) return b;
  if (b == 0) return a;
  return std::min(a, b);
}

// Case-insensitive suffix match.
inline bool EndsWithNoCase(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  if (s.size() < n) return false;
  const size_t off = s.size() - n;
  for (size_t i = 0; i < n; ++i) {
    unsigned char a = static_cast<unsigned char>(s[off + i]);
    unsigned char b = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(a) != std::tolower(b)) return false;
  }
  return true;
}

// Directory component of a file path, including the trailing separator.
inline std::string GetBaseDir(const std::string& filename) {
  const size_t pos = filename.find_last_of("/\\");
  if (pos != std::string::npos) {
    return filename.substr(0, pos + 1);
  }
  return "";
}

}  // namespace next
}  // namespace lightusd
