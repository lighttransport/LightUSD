// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment Inc.
#pragma once

#include <cstdint>
#include <limits>
#include <unordered_set>

namespace lightusd {

///
/// Simple handle resource manager
/// Assume T is an unsigned integer type.
/// TODO(LTE): Allocate handle for a given value range. e.g. [minVal, maxVal)
///
template<typename T = uint32_t>
class HandleAllocator {
public:
  // id = 0 is reserved.
  HandleAllocator() : counter_(static_cast<T>(1)){}
  //~HandleAllocator(){}

  /// Allocates handle object.
  bool Allocate(T *dst) {

    if (!dst) {
      return false;
    }

    T handle = 0;

    if (!freeList_.empty()) {
      // Reuse an arbitrary released handle.
      auto it = freeList_.begin();
      handle = *it;
      freeList_.erase(it);
      (*dst) = handle;
      return true;
    }

    handle = counter_;
    if ((handle >= static_cast<T>(1)) && (handle < (std::numeric_limits<T>::max)())) {
      counter_++;
      //std::cout << "conter = " << counter_ << "\n";
      (*dst) = handle;
      return true;
    }

    return false;
  }

  /// Release handle object.
  bool Release(const T handle) {
    if (handle == counter_ - static_cast<T>(1)) {
      if (counter_ > static_cast<T>(1)) {
        counter_--;
      } else {
        return false;
      }
    } else {
      if (handle >= static_cast<T>(1)) {
        freeList_.insert(handle);
      } else {
        // invalid handle
        return false;
      }
    }

    return true;
  }

  bool Has(const T handle) const {
    if (handle < 1) {
      return false;
    }

    // O(1) lookup: handle is a released slot.
    if (freeList_.count(handle) > 0) {
      return false;
    }

    if (handle >= counter_) {
      return false;
    }

    return true;
  }

  int64_t Size() const {
    return counter_ - freeList_.size() - 1;
  }

private:
  std::unordered_set<T> freeList_;
  T counter_{};
};

} // namespace lightusd
