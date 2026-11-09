// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Small, dependency-free string helpers shared by the core and shared sources
// (io-util.cc, image-writer.cc). The full legacy string/number utilities live
// in src/legacy/str-util.hh (lightusd::legacy).
#pragma once

#include <algorithm>
#include <sstream>
#include <string>

namespace lightusd {

inline std::string to_lower(const std::string &str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
    if ((c >= 'A') && (c <= 'Z')) {
      c += 32;
    }
    return static_cast<char>(c);
  });
  return result;
}

inline bool startsWith(const std::string &str, const std::string &t) {
  return (str.size() >= t.size()) &&
         std::equal(std::begin(t), std::end(t), std::begin(str));
}

inline bool endsWith(const std::string &str, const std::string &suffix) {
  return (str.size() >= suffix.size()) &&
         (str.find(suffix, str.size() - suffix.size()) != std::string::npos);
}

inline std::string removePrefix(const std::string &str,
                                const std::string &prefix) {
  if (startsWith(str, prefix)) {
    return str.substr(prefix.length());
  }
  return str;
}

// Python like join  ", ".join(v)
template <typename It>
inline std::string join(const std::string &sep, const It &v) {
  std::ostringstream oss;
  if (!v.empty()) {
    typename It::const_iterator it = v.begin();
    oss << *it++;
    for (typename It::const_iterator e = v.end(); it != e; ++it)
      oss << sep << *it;
  }
  return oss.str();
}

}  // namespace lightusd
