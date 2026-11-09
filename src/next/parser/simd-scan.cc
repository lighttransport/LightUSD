// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - SIMD scanning helpers (C++ forwarder)
//
// The implementation now lives in the C11 core (src/next/c/simd-scan.c) so the
// scalar/SSE2/NEON kernels are shared. This TU only bridges the C++ namespace
// to the C ABI.

#include "simd-scan.hh"

#include "../c/lightusd-core-c.h"

namespace lightusd {
namespace next {
namespace simdscan {

const char* ScanArrayStructural(const char* p, const char* end,
                                size_t* newlines, size_t* commas) {
  return lu_simd_scan_array_structural(p, end, newlines, commas);
}

const char* ScanPrimStructural(const char* p, const char* end,
                               size_t* newlines) {
  return lu_simd_scan_prim_structural(p, end, newlines);
}

const char* Backend() { return lu_simd_scan_backend(); }

}  // namespace simdscan
}  // namespace next
}  // namespace lightusd
