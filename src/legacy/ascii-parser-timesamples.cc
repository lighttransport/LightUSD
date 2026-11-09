// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// To deal with too many sections in generated .obj error(happens in MinGW and MSVC)
// Split ParseTimeSamples to two .cc files.
//

#include <cstdio>
#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <atomic>
//#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stack>
#if defined(__wasi__)
#else
#include <mutex>
#include <thread>
#endif
#include <vector>

//
#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

//

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// external

//#include "external/fast_float/include/fast_float/fast_float.h"
//#include "external/jsteemann/atoi.h"
#include "nonstd/expected.hpp"

//

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//

// Tentative
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif

#include "ascii-parser.hh"
#include "str-util.hh"
#include "tiny-format.hh"
//
#include "io-util.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"
#include "timesamples.hh"  // For unified binary storage
//
#include "common-macros.inc"

namespace lightusd {

namespace ascii {

#define LIGHTUSD_FOR_EACH_BINARY_TIMESAMPLE_TYPE(X) \
  X(int32_t)                                        \
  X(uint32_t)                                       \
  X(int64_t)                                        \
  X(uint64_t)                                       \
  X(value::half)                                    \
  X(value::half2)                                   \
  X(value::half3)                                   \
  X(value::half4)                                   \
  X(float)                                          \
  X(value::float2)                                  \
  X(value::float3)                                  \
  X(value::float4)                                  \
  X(double)                                         \
  X(value::double2)                                 \
  X(value::double3)                                 \
  X(value::double4)                                 \
  X(value::int2)                                    \
  X(value::int3)                                    \
  X(value::int4)                                    \
  X(value::quath)                                   \
  X(value::quatf)                                   \
  X(value::quatd)                                   \
  X(value::color3f)                                 \
  X(value::color4f)                                 \
  X(value::color3d)                                 \
  X(value::color4d)                                 \
  X(value::vector3f)                                \
  X(value::normal3f)                                \
  X(value::point3f)                                 \
  X(value::texcoord2f)                              \
  X(value::texcoord3f)                              \
  X(value::matrix2f)                                \
  X(value::matrix3f)                                \
  X(value::matrix4f)                                \
  X(value::matrix2d)                                \
  X(value::matrix3d)                                \
  X(value::matrix4d)

// Templated function to parse typed TimeSamples for types that use binary storage.
template<typename T>
bool AsciiParser::ParseTypedTimeSamples(value::TimeSamples *ts_out) {
  if (!value::UsesBinaryTimesampleStorageType(value::TypeTraits<T>::type_id())) {
    return false;
  }
  if (!ts_out) {
    return false;
  }

  // Set type hint for metadata. The add_sample<T>() calls below auto-detect
  // the backend on first call, so this is just for metadata consistency.
  ts_out->set_type_id(value::TypeTraits<T>::type_id());

  // `{` seen: a timeSamples block WAS authored, even if it turns out to hold no
  // samples (`float x.timeSamples = {}`). Without this, size()==0 is
  // indistinguishable from "no timeSamples at all" and the block is dropped.
  if (!Expect('{')) {
    return false;
  }
  ts_out->set_authored();

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  // Phase 3: Use TimeSamples methods directly
  // Note: Scalar dedup not yet implemented in TimeSamples - only arrays support dedup

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == '}') {
      break;
    }

    Rewind(1);

    double timeVal;
    if (!ReadBasicType(&timeVal)) {
      PushError("Parse time value failed.");
      return false;
    }

    if (!SkipWhitespace()) {
      return false;
    }

    if (!Expect(':')) {
      return false;
    }

    if (!SkipWhitespace()) {
      return false;
    }

    // Check for None/ValueBlock
    if (MaybeNone()) {
      std::string err;
      if (!ts_out->add_blocked_sample<T>(timeVal, &err)) {
        if (!err.empty()) {
          PushError(err);
        }
        return false;
      }
    } else {
      // Parse typed value directly
      T typed_val;
      if (!ReadBasicType(&typed_val)) {
        PushError("Failed to parse value of type " + std::string(value::TypeTraits<T>::type_name()));
        return false;
      }

      std::string err;
      if (!ts_out->add_sample(timeVal, typed_val, &err)) {
        if (!err.empty()) {
          PushError(err);
        }
        return false;
      }
    }

    // Handle separator
    {
      if (!SkipWhitespace()) {
        return false;
      }

      char sep{};
      if (!Char1(&sep)) {
        return false;
      }

      if (sep == '}') {
        break;
      } else if (sep == ',') {
        // ok
      } else {
        Rewind(1);

        auto loc = CurrLoc();
        if (SkipWhitespaceAndNewline()) {
          char nc;
          if (!Char1(&nc)) {
            return false;
          }

          if (nc == '}') {
            break;
          }
        }

        SeekTo(loc);
      }
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }
  }

  return true;
}

bool AsciiParser::ParseTimeSampleValue(const uint32_t type_id, value::Value *result) {

  if (!result) {
    return false;
  }

  if (MaybeNone()) {
    (*result) = value::ValueBlock();
    return true;
  }

  value::Value val;

#define PARSE_TYPE(__tyid, __type)                       \
  if (__tyid == value::TypeTraits<__type>::type_id()) {             \
    __type typed_val; \
    if (!ReadBasicType(&typed_val)) {                             \
      PUSH_ERROR_AND_RETURN("Failed to parse value with requested type `" + value::GetTypeName(__tyid) + "`"); \
    }                                                                  \
    val = value::Value(typed_val); \
  } else

#include "ascii-parser-timesamples-type-list.inc"
  {
    PUSH_ERROR_AND_RETURN(" : TODO: timeSamples type " + value::GetTypeName(type_id));
  }

#undef PARSE_TYPE

  (*result) = val;

  return true;
}

bool AsciiParser::ParseTimeSampleValue(const std::string &type_name, value::Value *result) {

  nonstd::optional<uint32_t> type_id = value::TryGetTypeId(type_name);

  if (!type_id) {
    PUSH_ERROR_AND_RETURN("Unsupported/invalid timeSamples type " + type_name);
  }

  return ParseTimeSampleValue(type_id.value(), result);
}


bool AsciiParser::ParseTimeSamples(const std::string &type_name,
                                   value::TimeSamples *ts_out) {

  // Get type_id to check whether unified binary storage can be used.
  nonstd::optional<uint32_t> type_id = value::TryGetTypeId(type_name);
  if (!type_id) {
    PUSH_ERROR_AND_RETURN("Unknown type for timeSamples: " + type_name);
  }

  // Clear ts_out to ensure clean state before parsing
  // This prevents issues where init() fails if ts_out was partially initialized
  if (ts_out) {
    ts_out->clear();
  }

  // Try optimized binary-serializable parsing first.
  // IMPORTANT: Save cursor position BEFORE attempting binary-storage path
  // The typed path will consume the '{' if it tries to parse,
  // but we need to restore position for the generic fallback path
  uint64_t saved_cursor = CurrLoc();
#define TRY_BINARY_TYPE(__type)                                   \
  if (type_id.value() == value::TypeTraits<__type>::type_id()) {  \
    if (ParseTypedTimeSamples<__type>(ts_out)) {                  \
      return true;                                                \
    }                                                             \
    /* typed path failed - restore cursor to original position */ \
    /* so the generic fallback can parse from the beginning */    \
    SeekTo(saved_cursor);                                         \
  }

  // Try binary-storage numeric, role, quaternion, and matrix types.
  LIGHTUSD_FOR_EACH_BINARY_TIMESAMPLE_TYPE(TRY_BINARY_TYPE)

#undef TRY_BINARY_TYPE

  // Fall back to generic value::Value-based parsing for other types.
  // (strings, tokens, paths, arrays, etc.)
  value::TimeSamples ts;

  // `{` seen: a timeSamples block WAS authored, even if it turns out to hold no
  // samples (`float x.timeSamples = {}`). Without this, size()==0 is
  // indistinguishable from "no timeSamples at all" and the block is dropped.
  if (!Expect('{')) {
    return false;
  }
  ts.set_authored();

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == '}') {
      break;
    }

    Rewind(1);

    double timeVal;
    // -inf, inf and nan are handled.
    if (!ReadBasicType(&timeVal)) {
      PushError("Parse time value failed.");
      return false;
    }

    if (!SkipWhitespace()) {
      return false;
    }

    if (!Expect(':')) {
      return false;
    }

    if (!SkipWhitespace()) {
      return false;
    }

    value::Value value;
    if (!ParseTimeSampleValue(type_name, &value)) { // could be None(ValueBlock)
      return false;
    }

    // The last element may have separator ','
    {
      // Semicolon ';' is not allowed as a separator for timeSamples array
      // values.
      if (!SkipWhitespace()) {
        return false;
      }

      char sep{};
      if (!Char1(&sep)) {
        return false;
      }

      DCOUT("sep = " << sep);
      if (sep == '}') {
        // End of item
        ts.add_sample(timeVal, value);
        break;
      } else if (sep == ',') {
        // ok
      } else {
        Rewind(1);

        // Look ahead Newline + '}'
        auto loc = CurrLoc();

        if (SkipWhitespaceAndNewline()) {
          char nc;
          if (!Char1(&nc)) {
            return false;
          }

          if (nc == '}') {
            // End of item
            ts.add_sample(timeVal, value);
            break;
          }
        }

        // Rewind and continue parsing.
        SeekTo(loc);
      }
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    ts.add_sample(timeVal, value);
  }

  DCOUT("Parse TimeSamples success. # of items = " << ts.size());

  if (ts_out) {
    (*ts_out) = std::move(ts);
  }

  return true;
}

// Explicit template instantiations for binary-storage types.
#define INSTANTIATE_BINARY_TIMESAMPLE_TYPE(__type) \
  template bool AsciiParser::ParseTypedTimeSamples<__type>(value::TimeSamples*);

LIGHTUSD_FOR_EACH_BINARY_TIMESAMPLE_TYPE(INSTANTIATE_BINARY_TIMESAMPLE_TYPE)

#undef INSTANTIATE_BINARY_TIMESAMPLE_TYPE
#undef LIGHTUSD_FOR_EACH_BINARY_TIMESAMPLE_TYPE

}  // namespace ascii
}  // namespace lightusd

#else  // LIGHTUSD_DISABLE_MODULE_USDA_READER

#endif  // LIGHTUSD_DISABLE_MODULE_USDA_READER
