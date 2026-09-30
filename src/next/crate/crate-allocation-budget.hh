// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "safe-arithmetic.hh"

namespace lightusd {
namespace next {

// Monotonic allocation accounting shared by a read and its deferred values.
// Temporary buffers are charged too, matching the reader's cumulative policy.
class CrateAllocationBudget {
 public:
  CrateAllocationBudget(size_t file_size, size_t max_memory)
      : allocation_cap_(ScaledCap(file_size, 256, 64ull << 20)),
        total_cap_(max_memory ? static_cast<uint64_t>(max_memory)
                              : ScaledCap(file_size, 512, 256ull << 20)),
        max_memory_(max_memory) {}

  const char* Charge(uint64_t bytes) {
    if (bytes > (std::numeric_limits<size_t>::max)())
      return " size exceeds addressable memory";
    if (bytes > allocation_cap_)
      return " exceeds file-size-relative allocation cap";
    if (max_memory_ && bytes > max_memory_)
      return " exceeds max_memory budget";
    uint64_t current = total_.load(std::memory_order_relaxed);
    for (;;) {
      if (current > total_cap_ || bytes > total_cap_ - current)
        return " exceeds cumulative allocation budget";
      if (total_.compare_exchange_weak(current, current + bytes,
                                      std::memory_order_relaxed)) return nullptr;
    }
  }

 private:
  static uint64_t ScaledCap(size_t size, uint64_t ratio, uint64_t slack) {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    const uint64_t bytes = static_cast<uint64_t>(size);
    return bytes > (maximum - slack) / ratio ? maximum : bytes * ratio + slack;
  }
  const uint64_t allocation_cap_;
  const uint64_t total_cap_;
  const uint64_t max_memory_;
  std::atomic<uint64_t> total_{0};
};

// Maximum delta-code workspace, including capacity growth headroom in the
// decompressor's vector. Reserve before invoking the integer codec.
inline bool CrateIntegerWorkspaceBytes(size_t count, size_t lane_bytes,
                                       const uint8_t* compressed, size_t size,
                                       size_t* bytes) {
  if (count == 0) { *bytes = 0; return true; }
  const size_t codes = count / 4 + (count % 4 != 0 ? 1u : 0u);
  size_t values;
  size_t maximum;
  if (!safe::mul(count, lane_bytes, &values) ||
      !safe::add(codes, lane_bytes, &maximum) ||
      !safe::add(maximum, values, &maximum)) return false;
  // The single-chunk codec clips its workspace by LZ4's expansion bound.
  // Mirror that clip rather than charge all possible non-common deltas for a
  // tiny, highly compressible block. Chunked streams retain the full bound.
  if (lane_bytes == sizeof(uint32_t) && size > 0 && compressed && compressed[0] == 0) {
    size_t ratio_limit;
    if (safe::mul(size - 1, size_t(255), &ratio_limit) &&
        ratio_limit < maximum) maximum = ratio_limit;
  }
  if (lane_bytes == sizeof(uint64_t)) { *bytes = maximum; return true; }
  return safe::mul(maximum, size_t(2), bytes);
}

}  // namespace next
}  // namespace lightusd
