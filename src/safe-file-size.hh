// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Bounded file-size query for "open with ios::ate, then allocate that many
// bytes" call sites.
#pragma once

#include <cstdint>
#include <fstream>
#include <ios>
#include <limits>

namespace lightusd {
namespace next {

/// Absolute ceiling on a file this library will read whole into memory.
/// Deliberately generous -- it exists to reject the impossible, not to police
/// large assets; callers with a real budget pass a tighter `max_bytes`.
constexpr uint64_t kMaxReadableFileBytes = 16ull << 30;  // 16 GiB

/// Size of an already-opened (`std::ios::ate`) stream, or false if it cannot
/// be a readable regular file of at most `max_bytes` (0 = the ceiling above).
///
/// WHY THIS EXISTS: a DIRECTORY opens successfully with std::ifstream on POSIX
/// and reports `tellg() == LLONG_MAX`. Call sites fed that straight into
/// `std::string(n, '\0')` or `vector::resize(n)`, which throws length_error --
/// uncaught, so the process terminates. A USD file referencing
/// `@./somedir.mtlx@` where that path is a directory was enough to kill the
/// process; fuzz_next_compose found it in minutes.
///
/// The existing caps did not help because every one of them was conditional
/// (`if (max_memory > 0) ...`), so the default configuration left the path
/// completely unguarded. The bound here is unconditional.
///
/// Also rejects sizes past streamsize/size_t, which differ from streamoff on
/// 32-bit targets (wasm32) and would otherwise truncate silently.
inline bool SafeStreamSize(std::ifstream& f, uint64_t max_bytes, size_t* out) {
  if (!out) return false;
  const std::streamoff end = f.tellg();
  if (end <= 0) return false;
  const uint64_t n = static_cast<uint64_t>(end);
  const uint64_t cap = max_bytes ? max_bytes : kMaxReadableFileBytes;
  if (n > cap) return false;
  if (n > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
    return false;
  }
  if (n > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
    return false;
  }
  *out = static_cast<size_t>(n);
  return true;
}

}  // namespace next
}  // namespace lightusd
