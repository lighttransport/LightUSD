// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace lusdview {
// Bound temporary placements before multiplying counts or reserving storage.
inline size_t BoundedInstanceProduct(size_t outer, size_t inner, size_t cap) {
  if (outer == 0 || inner == 0) return 0;
  return inner > cap / outer ? cap : outer * inner;
}

// Prototype relationships form a graph, even though prim children form a tree.
// Keep only the current branch so shared prototypes remain usable by siblings.
class PrototypeExpansionGuard {
 public:
  static constexpr size_t kMaxDepth = 64;

  PrototypeExpansionGuard(std::vector<std::string>& active,
                          const std::string& path)
      : active_(active) {
    if (active.size() >= kMaxDepth ||
        std::find(active.begin(), active.end(), path) != active.end()) return;
    active.push_back(path);
    entered_ = true;
  }
  ~PrototypeExpansionGuard() {
    if (entered_) active_.pop_back();
  }
  PrototypeExpansionGuard(const PrototypeExpansionGuard&) = delete;
  PrototypeExpansionGuard& operator=(const PrototypeExpansionGuard&) = delete;
  bool entered() const { return entered_; }

 private:
  std::vector<std::string>& active_;
  bool entered_ = false;
};
}  // namespace lusdview
