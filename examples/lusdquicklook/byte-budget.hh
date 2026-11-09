// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <atomic>
#include <cstddef>
#include <limits>

namespace lusdql {
namespace budget_detail {

constexpr uint64_t SaturatingAdd(uint64_t total, uint64_t amount) {
  constexpr uint64_t max = (std::numeric_limits<uint64_t>::max)();
  return amount > max - total ? max : total + amount;
}

constexpr uint64_t SaturatingMul(uint64_t a, uint64_t b) {
  constexpr uint64_t max = (std::numeric_limits<uint64_t>::max)();
  return a != 0 && b > max / a ? max : a * b;
}

inline bool CheckedMulSize(size_t a, size_t b, size_t* out) noexcept {
  if (!out || (a != 0 && b > (std::numeric_limits<size_t>::max)() / a)) {
    return false;
  }
  *out = a * b;
  return true;
}

constexpr bool FitsQueueBudget(uint64_t queued, uint64_t incoming,
                               uint64_t limit) {
  // Subtract only after the invariant check; addition can wrap before a
  // conventional `queued + incoming <= limit` comparison sees the overflow.
  return queued <= limit && incoming <= limit - queued;
}

inline bool TryReserve(std::atomic<uint64_t>& used, uint64_t amount,
                       uint64_t limit) noexcept {
  uint64_t current = used.load(std::memory_order_relaxed);
  for (;;) {
    if (!FitsQueueBudget(current, amount, limit)) return false;
    if (used.compare_exchange_weak(current, current + amount,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return true;
    }
  }
}

}  // namespace budget_detail
}  // namespace lusdql
