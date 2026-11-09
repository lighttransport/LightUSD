// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate table-count limits
//
// Security bounds on the crate's structural table sizes. They cap allocations a
// malformed file could request from a small header value; every table is also
// charged to the reader's max_memory budget. Kept dependency-free so the
// composition options (pcp) can carry them to every USDC layer load.

#pragma once

#include <cstddef>

namespace lightusd {
namespace next {

struct CrateLimits {
  /// Maximum number of tokens allowed
  size_t max_tokens = 1024 * 1024;

  /// Maximum number of strings allowed
  size_t max_strings = 1024 * 1024;

  /// Maximum number of fields allowed
  size_t max_fields = 10 * 1024 * 1024;

  /// Maximum number of FIELDSETS index entries (field indices plus one
  /// terminator per fieldset). A flattened production scene has more of these
  /// than fields (Island: 13.4M indices for 9.2M fields), so this has its
  /// own, larger bound (256 MB of uint32 at the cap).
  size_t max_fieldset_indices = 64 * 1024 * 1024;

  /// Maximum number of specs allowed
  size_t max_specs = 10 * 1024 * 1024;

  /// Maximum number of paths allowed
  size_t max_paths = 10 * 1024 * 1024;

  /// Maximum recursion depth for path decoding
  size_t max_path_depth = 256;
};

}  // namespace next
}  // namespace lightusd
