// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// SpillVec: the append-only column container behind the low profile's writer
// spill. Its contract is plain: whatever is appended comes back (Resolve) in
// the same order with the same values, whether the array stayed in memory,
// was chunked, or had its prefix moved to the spill file.

#include "next/crate/spill-array.hh"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Tiny chunks (64 KiB) so the randomized exercise crosses many eviction
// boundaries without large allocations; the default 8 MiB suits the writer.
template <typename T>
using SpillVec = lightusd::next::SpillVec<T, 64 * 1024>;

namespace {

struct Record {
  uint32_t a = 0;
  uint32_t b = 0;
  uint64_t c = 0;
};

// Deterministic LCG so failures reproduce.
struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint64_t next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s >> 33;
  }
};

template <typename T>
void CheckResolve(const SpillVec<T>& vec, const std::vector<T>& expect,
                  const char* what) {
  std::unique_ptr<T[]> copy;
  const T* view = nullptr;
  std::string err;
  if (!vec.Resolve(&copy, &view, &err)) {
    std::printf("FAIL %s: resolve error: %s\n", what, err.c_str());
    std::exit(1);
  }
  if (vec.size() != expect.size()) {
    std::printf("FAIL %s: size %zu != %zu\n", what, vec.size(), expect.size());
    std::exit(1);
  }
  for (size_t i = 0; i < expect.size(); ++i) {
    if (std::memcmp(&view[i], &expect[i], sizeof(T)) != 0) {
      std::printf("FAIL %s: element %zu differs\n", what, i);
      std::exit(1);
    }
  }
}

// Append a mix of single elements and ranges, evict sealed chunks between
// appends (as the writer does at batch boundaries), and verify every element
// survives in order. Runs with spill off (one plain vector) and on (8 MiB
// chunks with a forced prefix eviction).
void Exercise(bool spill_on, const char* what_prefix) {
  SpillVec<uint32_t> u32;
  SpillVec<Record> rec;
  u32.set_spill_enabled(spill_on);
  rec.set_spill_enabled(spill_on);

  std::vector<uint32_t> u32_ref;
  std::vector<Record> rec_ref;
  Rng rng;

  const size_t kRounds = 24;
  // >= 4 chunks per batch: every batch boundary has sealed chunks to evict.
  const size_t kChunkElems = SpillVec<uint32_t>::kChunkElems;
  uint32_t counter = 0;
  for (size_t round = 0; round < kRounds; ++round) {
    const size_t batch = kChunkElems * 4 + rng.next() % kChunkElems;
    // Element-wise pushes (the serial-mode emitter).
    for (size_t i = 0; i < batch; ++i) {
      u32.push_back(counter);
      u32_ref.push_back(counter);
      Record r;
      r.a = counter;
      r.b = counter * 3u + 1u;
      r.c = 0x0123456789ABCDEFull + counter;
      rec.push_back(r);
      rec_ref.push_back(r);
      ++counter;
    }
    // Range append (the parallel commit path).
    if (round % 3 == 0) {
      std::vector<uint32_t> chunk(1 + rng.next() % 1000);
      std::vector<Record> rchunk(chunk.size());
      for (size_t i = 0; i < chunk.size(); ++i) {
        chunk[i] = counter;
        rchunk[i].a = counter;
        rchunk[i].b = counter * 7u;
        rchunk[i].c = counter;
        ++counter;
      }
      u32.append(chunk.data(), chunk.size());
      u32_ref.insert(u32_ref.end(), chunk.begin(), chunk.end());
      rec.append(rchunk.data(), rchunk.size());
      rec_ref.insert(rec_ref.end(), rchunk.begin(), rchunk.end());
    }
    // Batch boundary: try to push the oldest chunks out of memory.
    std::string err;
    u32.SpillSealed(1ull << 28, &err);  // ~256 MiB target: evicts everything
    if (!err.empty()) {
      std::printf("FAIL %s: spill error: %s\n", what_prefix, err.c_str());
      std::exit(1);
    }
    rec.SpillSealed(1ull << 28, &err);
    if (!err.empty()) {
      std::printf("FAIL %s: spill error: %s\n", what_prefix, err.c_str());
      std::exit(1);
    }
#if defined(_WIN32)
    if (u32.spilled_bytes() != 0 || rec.spilled_bytes() != 0 ||
        u32.io_failed() || rec.io_failed()) {
      std::printf("FAIL %s: Windows fallback must remain in memory\n", what_prefix);
      std::exit(1);
    }
#else
    if (spill_on && round > 0 && u32.spilled_bytes() == 0) {
      std::printf("FAIL %s: expected the u32 column to spill\n", what_prefix);
      std::exit(1);
    }
#endif
    CheckResolve(u32, u32_ref, what_prefix);
    CheckResolve(rec, rec_ref, what_prefix);
  }

  // Release must drop everything (the writer frees each column right after
  // its section payload is encoded) and leave the array reusable.
  u32.Release();
  rec.Release();
  if (u32.size() != 0 || rec.size() != 0) {
    std::printf("FAIL %s: Release left elements behind\n", what_prefix);
    std::exit(1);
  }
  std::printf("ok %s\n", what_prefix);
}

// The disabled path must keep the exact plain-vector capacity growth the
// writer relied on for its reserve() calls.
void CheckReservePassthrough() {
  SpillVec<uint32_t> vec;
  vec.reserve(1u << 20);
  vec.push_back(7);
  vec.reserve(2u << 20);  // stays valid across a later reserve
  if (vec.size() != 1) {
    std::printf("FAIL reserve: size changed\n");
    std::exit(1);
  }
  std::vector<uint32_t> expect{7};
  CheckResolve(vec, expect, "reserve");
  std::printf("ok reserve\n");
}

}  // namespace

int main() {
  CheckReservePassthrough();
  Exercise(false, "in-memory");
  Exercise(true, "spilled");
  std::printf("PASS spill-array\n");
  return 0;
}
