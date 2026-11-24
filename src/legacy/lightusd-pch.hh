// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Precompiled-header aggregation for the lightusd library build.
//
// This header is force-included (via CMake `target_precompile_headers`) into
// every C++ translation unit of the library when LIGHTUSD_USE_PCH is ON. Its
// only purpose is to amortize the parse cost of large, *stable* headers that
// are pulled into the vast majority of TUs.
//
// RULES for what may live here:
//   * Must be heavy AND stable (rarely edited). Editing any header listed here
//     invalidates the whole PCH and forces every TU to recompile.
//   * Do NOT add headers that are actively being refactored (e.g. the schema
//     headers) or that change often.
//   * C++ only - the build guards this with $<COMPILE_LANGUAGE:CXX> because the
//     library also compiles C sources (zstd.c, lz4.c, yyjson.c, ...).
//
#pragma once

#if defined(__cplusplus)

// --- Standard library (pure parse cost; never changes) ---
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// --- Stable project / vendored headers included almost everywhere ---
// token-type.hh transitively pulls nonstd/optional.hpp and the string_id
// vendored library, which are among the most-parsed headers in the build.
#include "token-type.hh"

#endif  // __cplusplus
