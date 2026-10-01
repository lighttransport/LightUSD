// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Value Printer
// Convert Value objects to string representation

#pragma once

#include "../types/value.hh"
#include <string>

namespace lightusd {
namespace next {

class StreamWriter;

/// Options for value printing
struct PrintOptions {
  /// Number of decimal places for floats
  int float_precision = 6;

  /// Number of decimal places for doubles
  int double_precision = 15;

  /// Maximum array elements to print (0 = no limit)
  size_t max_array_elements = 0;

  /// Indent string for nested structures
  std::string indent = "    ";

  /// Use compact format (single line arrays)
  bool compact = false;

  /// Canonicalize dictionary key order (including nested dictionaries).
  /// Reference-item identity uses this; ordinary metadata keeps authored order.
  bool sort_dictionary_keys = false;
};

/// Print a Value to string (USDA format)
std::string PrintValue(const Value& value, const PrintOptions& opts = {});

/// Append a Value's USDA representation directly into `out` (no intermediate
/// allocation; the hot path for large numeric arrays). Byte-identical to
/// appending PrintValue(value, opts).
void PrintValueInto(std::string& out, const Value& value,
                    const PrintOptions& opts = {});

/// Print a Value directly to a StreamWriter. Large arrays are chunked so the
/// writer does not allocate a full textual copy of the value before flushing.
/// Output is byte-identical to PrintValue(value, opts).
void PrintValue(StreamWriter& out, const Value& value,
                const PrintOptions& opts = {});

/// Number of USD elements in an array value (e.g. point count for point3f[]).
/// 0 if `value` is not an array.
size_t ArrayElementCount(const Value& value);

/// True if `value` is an array whose element TYPE supports range formatting
/// (int/uint/int64/uint64 or any float/double vector/matrix component type) and
/// is not truncated by `opts` -- regardless of lazy/borrow state. A chunkable-type
/// array that is not directly borrowable can still be range-formatted after being
/// materialized into an owned buffer (see Value::materialized_copy).
bool IsChunkableType(const Value& value, const PrintOptions& opts);

/// True if `value` is a large numeric array that can be formatted in independent
/// element-range chunks with zero-copy borrows: a non-lazy POD numeric array
/// (int/uint/int64/uint64 or any float/double vector/matrix component type) that
/// is not truncated by `opts`. The parallel writer uses this to split one giant
/// array across worker threads. Lazy arrays are excluded (a per-chunk borrow
/// could re-decode), as are token/string/bool/asset arrays.
bool IsChunkableArray(const Value& value, const PrintOptions& opts);

/// Format USD elements [elem_lo, elem_hi) of array `value` into `os`.
///   open  : emit the leading '[' (set only on the first chunk).
///   close : emit the trailing ']' (set only on the last chunk).
/// Element separators match the full printer: the element whose global index is
/// > 0 is preceded by ", ". Concatenating the ranges [0,k0)+[k0,k1)+...+[kN,n)
/// with open on the first and close on the last is byte-identical to
/// PrintValue(value) over the whole array. Returns false if `value` is not a
/// chunkable array type (see IsChunkableArray).
bool PrintArrayRangeToStream(StreamWriter& os, const Value& value,
                             const PrintOptions& opts, size_t elem_lo,
                             size_t elem_hi, bool open, bool close);

/// Print a Value type name
std::string PrintTypeName(TypeId type_id, bool is_array = false);

/// Format an asset path for usda text: `@path@`, or the pxr triple-delimiter
/// form `@@@path@@@` (with `\@@@` escaping) when the path itself contains '@'.
std::string FormatAssetPathForUsda(const std::string& path);

/// Print a Value to USDA attribute declaration format
/// e.g., "float3 points = (1, 2, 3)"
std::string PrintAttributeValue(const std::string& type_name, const std::string& attr_name,
                                 const Value& value, const PrintOptions& opts = {});

}  // namespace next
}  // namespace lightusd
