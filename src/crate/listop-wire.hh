// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "stream-reader.hh"
#include <cstddef>
#include <cstdint>

namespace lightusd {

// AOUSD Core 1.1, List Operations: vector order is independent of bit order.
// The framing decoder retains the raw header. Separate helpers below model
// OpenUSD setter-mode semantics without skipping validation of discarded runs.
inline constexpr uint8_t kCrateListOpWireBits[6] = {2, 4, 32, 64, 8, 16};
inline constexpr const char* kCrateListOpRunMarkers[6] = {
    "\x01" "E", "\x01" "G", "\x01" "P", "\x01" "A", "\x01" "D", "\x01" "O"};

// OpenUSD applies setters in wire order. Any composable vector switches out
// of explicit mode and discards explicit items, even when its count is zero.
// Otherwise an explicit-items vector itself enters explicit mode (bit 0 need
// not have been authored). Preserve vector-presence bits within the final mode.
inline constexpr uint8_t CrateListOpSemanticHeader(uint8_t wire) {
  return (wire & 0x7cu) ? static_cast<uint8_t>(wire & 0x7cu)
                       : static_cast<uint8_t>((wire & 3u) | ((wire & 2u) ? 1u : 0u));
}

inline constexpr bool CrateListOpRetainsRun(uint8_t wire, std::size_t run) {
  return run == 0 ? (CrateListOpSemanticHeader(wire) & 1u) != 0
                  : (CrateListOpSemanticHeader(wire) & 1u) == 0;
}

// run(index, count) must consume exactly count items of its element encoding,
// validating indices/byte availability and charging allocations before use.
// The cursor and callback output may be partial on failure and must be discarded.
template <class ReadRun>
bool DecodeCrateListOpWire(StreamReader& input, uint64_t max_elements,
                          uint8_t& header, ReadRun&& run) {
  if (!input.read_u8(header) || (header & 0x80u)) return false;
  for (std::size_t index = 0; index < 6; ++index) {
    if (!(header & kCrateListOpWireBits[index])) continue;
    uint64_t count = 0;
    if (!input.read_u64(count) || count > max_elements || !run(index, count)) return false;
  }
  return true;
}

}  // namespace lightusd
