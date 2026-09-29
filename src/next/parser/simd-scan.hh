// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - SIMD scanning helpers for the USDA parser
//
// Bulk byte scanning for the array-literal hot path. The lexer captures an array
// `[...]` by walking it byte-by-byte to find the matching `]`; on a flattened
// scene that per-char scan is the single biggest parse cost. These helpers skip
// runs of "boring" bytes (digits/commas/whitespace) at SIMD speed and stop only
// at structurally-interesting bytes. Pure scanning — they change only *how fast*
// the same byte positions are found, never *which* (byte-identical parse).
#pragma once

#include <cstddef>

namespace lightusd {
namespace next {
namespace simdscan {

// Scan [p, end) for the first byte in the array-structural set
//   { '[' , ']' , '"' , '\'' , '@' , '#' }
// and return a pointer to it, or `end` if none. Adds the number of '\n' bytes in
// the scanned-over prefix to *newlines (for the lexer's line tracking) and the
// number of ',' bytes to *commas. The comma tally lets the capture predict the
// scalar count of a simple array (scalars = commas + 1 whether the elements are
// bare scalars or paren-wrapped tuples — every adjacent scalar pair is separated
// by exactly one comma), so the parser can pre-size its output vector.
const char* ScanArrayStructural(const char* p, const char* end,
                                size_t* newlines, size_t* commas);

// Scan [p, end) for the first byte in the prim-block-structural set
//   { '{' , '}' , '(' , ')' , '[' , ']' , '"' , '\'' , '@' , '#' , '/' }
// and return a pointer to it, or `end` if none. Adds the number of '\n' bytes
// in the scanned-over prefix to *newlines. Used by the prim-block capture
// (parallel subtree parse) to skip property/value bytes at SIMD speed; parens
// are structural so the capture can tell a metadata-dict '{' (inside parens)
// from the prim body '{'. '/' is structural so the capture can detect a C-style
// block comment (`/* ... */`) and leave that prim to the inline parser.
const char* ScanPrimStructural(const char* p, const char* end,
                               size_t* newlines);

// Name of the active scan backend ("sse2" / "scalar" / ...), for logging.
const char* Backend();

}  // namespace simdscan
}  // namespace next
}  // namespace lightusd
