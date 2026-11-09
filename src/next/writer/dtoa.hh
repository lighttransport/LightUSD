// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - dragonbox-based float-to-string (dtoa)
//
// Same shortest-round-trippable formatter lightusd uses in `src/str-util.cc`
// (`dtos`), ported into the standalone next module. Emits OpenUSD-compatible
// notation (pxr_double_conversion ToShortest/ToShortestSingle): the shortest
// decimal that round-trips the value, fixed for decimal exponents in [-6, 15)
// and scientific outside, with NO `+` on positive exponents, NO zero-padding,
// and `-0` preserved -- matching `usdcat` byte-for-byte.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lightusd {
namespace next {

/// Minimum destination buffer size for dtos_to() (and any raw dtoa buffer).
/// The zmij SIMD fast path writes 8/16-byte chunks that can run past the
/// logical end of the shortest string, so callers must provide this much slack
/// even though no formatted value is ever this long.
static constexpr std::size_t kDtoaBufSize = 48;

/// Shortest round-trippable decimal string for a float / double (OpenUSD
/// notation). Used by the USDA value printer.
std::string dtos(float v);
std::string dtos(double v);

/// Same as dtos(), but appends directly into `out` with no intermediate
/// std::string allocation (hot path for large numeric arrays). Byte-identical
/// to dtos().
void dtos_append(std::string& out, float v);
void dtos_append(std::string& out, double v);

/// Shortest decimal that parses back to the same IEEE binary16 bit pattern.
/// `bits` is the raw half representation; the spelling may intentionally be
/// shorter than formatting its widened float32 value (for example 0.35 rather
/// than 0.35009766).
std::string htos(uint16_t bits);
size_t htos_to(char* dst, uint16_t bits);
void htos_append(std::string& out, uint16_t bits);

/// Same as dtos(), but formats into a caller-provided buffer and returns the
/// byte count (no std::string at all). `dst` capacity must be >= kDtoaBufSize
/// (the zmij SIMD fast path overshoots the logical end; see kDtoaBufSize).
/// Byte-identical to dtos(); used by the value printer to format a scalar
/// directly into its chunk buffer.
size_t dtos_to(char* dst, float v);
size_t dtos_to(char* dst, double v);

/// Reference formatter: the scalar dragonbox renderer only (no zmij fast path).
/// dtos_to() is required to be byte-identical to this for every input; exposed
/// solely so tests / fuzzers can check that equivalence. Same buffer contract.
size_t dtos_to_reference(char* dst, float v);
size_t dtos_to_reference(char* dst, double v);

/// Freestanding `printf("%.*g", precision, v)` formatter (no libc / no locale):
/// round to `precision` significant digits, choose fixed vs scientific by the %g
/// rule (fixed iff -4 <= decimal-exponent < precision), strip trailing zeros,
/// scientific exponent `e±dd`. Used for the USDA layer-meta + time-sample-key
/// doubles (which previously went through ostream `setprecision`).
///
/// Contract: BYTE-IDENTICAL to `%.*g` whenever the value's shortest round-trip
/// decimal fits in `precision` significant digits — which holds for every
/// authored USD scalar (metersPerUnit, fps, time codes like 13.944, …) and is
/// verified byte-identical across the flatten test scenes. It is built on the
/// dragonbox SHORTEST digits rounded to `precision`, so for an arbitrary
/// full-precision double whose shortest needs > precision digits it can differ
/// from libc by one ULP in the last digit (a double-rounding artifact);
/// exact-in-all-cases rounding would need exact (big-integer) digit extraction.
std::string format_g(double v, int precision);
void format_g_append(std::string& out, double v, int precision);

}  // namespace next
}  // namespace lightusd
