// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// libFuzzer harness: src/next USDC (crate) reader.
//
// Build (clang): cmake -S src/next -B build-fuzz -DLIGHTUSD_BUILD_FUZZERS=ON
// Run: ./fuzz_next_usdc [-max_len=65536] corpus_dir
// Seed from tests/usdc/*.usdc (strip the 8-byte magic; it is re-prepended here
// so every input exercises the parser body, same trick as the legacy harness).

#include <cstdint>
#include <cstring>
#include <string>

#include "reader/usdc-reader.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  std::string buf("PXR-USDC");
  buf.append(reinterpret_cast<const char *>(data), size);

  lightusd::USDCLoadOptions opts;
  // Tight caps so allocator OOM on absurd-but-wellformed counts is not
  // reported as a finding; bugs we want are OOB/overflow/UB, not bad_alloc.
  opts.crate_options.max_array_elements = 1u << 20;  // 1M elems
  opts.crate_options.max_tokens = 1u << 18;
  opts.crate_options.max_strings = 1u << 18;
  opts.crate_options.max_fields = 1u << 18;
  opts.crate_options.max_specs = 1u << 18;
  opts.crate_options.max_paths = 1u << 18;

  lightusd::USDCLoadResult result = lightusd::LoadUSDCFromMemory(
      reinterpret_cast<const uint8_t *>(buf.data()), buf.size(), opts);
  (void)result;
  return 0;
}
