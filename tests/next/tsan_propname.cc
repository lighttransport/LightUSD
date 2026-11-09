// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// ThreadSanitizer regression test for the PropNameTable frozen fast path.
// The parallel opinion fill freezes the table and then does lock-free
// find()/get() while a genuinely-new name may still be interned (unfreeze +
// insert). Readers and that writer must not race. Built and registered only
// with -DLIGHTUSD_NEXT_BUILD_TSAN_TESTS=ON; a detected race aborts nonzero.
#include "next/layer/property-index.hh"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace lightusd::next;

int main() {
  auto& t = GetPropNameTable();
  // Pre-intern a pool of names (all HIT after freeze).
  std::vector<PropNameId> ids;
  for (int i = 0; i < 4000; i++) {
    ids.push_back(t.intern("pre_" + std::to_string(i)));
  }

  // Enter the "frozen read-only" phase, as FillOpinions does.
  t.freeze();

  std::vector<std::thread> pool;
  for (int r = 0; r < 6; r++) {
    pool.emplace_back([&]() {
      for (int i = 0; i < 300000; i++) {
        (void)t.find("pre_" + std::to_string(i % 4000));   // frozen hit
        (void)t.get(ids[static_cast<size_t>(i) % ids.size()]);
        (void)t.find("absent_" + std::to_string(i % 97));  // frozen miss
      }
    });
  }
  // The writer re-freezes before each genuinely-new intern (as each
  // FillOpinions does), so readers observe frozen == true while this thread
  // mutates the table. Few, spaced inserts keep the racy window overlapping
  // live readers.
  pool.emplace_back([&]() {
    for (int i = 0; i < 400; i++) {
      t.freeze();
      (void)t.intern("new_" + std::to_string(i));
      for (volatile int s = 0; s < 20000; s = s + 1) {
      }
    }
  });
  for (auto& th : pool) th.join();
  std::printf("done size=%zu\n", t.size());
  return 0;
}
