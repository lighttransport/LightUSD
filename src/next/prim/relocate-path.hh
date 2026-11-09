// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <utility>
#include "identifier.hh"

namespace lightusd {
namespace next {

// Anchor relative prim paths used by relocates. Absolute paths are preserved
// for separate authoring validation. Empty input is the deletion sentinel.
// Failure leaves output unchanged: an invalid target must never become delete.
inline bool AnchorRelocatePath(const std::string& path, const std::string& anchor,
                               std::string* output) {
  if (!output) return false;
  if (path.empty() || path[0] == '/') {
    *output = path;
    return true;
  }
  if (anchor.empty() || anchor[0] != '/') return false;
  size_t pos = 1;
  while (pos < anchor.size()) {
    const size_t end = anchor.find('/', pos);
    const size_t count = end == std::string::npos ? anchor.size() - pos : end - pos;
    if (!IsValidIdentifier(anchor.substr(pos, count))) return false;
    if (end == std::string::npos) break;
    pos = end + 1;
    if (pos == anchor.size()) return false;
  }
  std::string result = anchor;
  if (path == ".") {
    *output = result;
    return true;
  }
  pos = 0;
  while (path.compare(pos, 3, "../") == 0 || (path.size() - pos == 2 && path.compare(pos, 2, "..") == 0)) {
    if (result == "/") return false;
    result.resize(result.rfind('/'));
    if (result.empty()) result = "/";
    if ((path.size() - pos == 2 && path.compare(pos, 2, "..") == 0)) {
      *output = result;
      return true;
    }
    pos += 3;
    if (pos == path.size()) return false;
  }
  while (pos < path.size()) {
    const size_t end = path.find('/', pos);
    const size_t count = end == std::string::npos ? path.size() - pos : end - pos;
    const std::string name = path.substr(pos, count);
    if (!IsValidIdentifier(name)) return false;
    if (result != "/") result += '/';
    result += name;
    if (end == std::string::npos) break;
    pos = end + 1;
    if (pos == path.size()) return false;
  }
  *output = std::move(result);
  return true;
}

}  // namespace next
}  // namespace lightusd
