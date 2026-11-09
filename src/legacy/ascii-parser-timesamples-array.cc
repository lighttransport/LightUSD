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

#include "ascii-parser.hh"
#include "str-util.hh"
#include "tiny-format.hh"

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

#include "common-macros.inc"
#include "io-util.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"

// Extern template declarations for ParseBasicTypeArray
// These templates are explicitly instantiated in ascii-parser-basetype.cc
namespace lightusd {
namespace ascii {

// Int tuple types
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::int2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::int3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::int4> *result);
// Uint tuple types
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uint2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uint3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uint4> *result);
// Char types
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<char> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::char2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::char3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::char4> *result);
// Uchar types
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<uint8_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uchar2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uchar3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uchar4> *result);
// Short types
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<int16_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::short2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::short3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::short4> *result);
// Ushort types
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<uint16_t> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::ushort2> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::ushort3> *result);
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::ushort4> *result);
// Frame type
extern template bool AsciiParser::ParseBasicTypeArray(std::vector<value::frame4d> *result);

}  // namespace ascii
}  // namespace lightusd

namespace lightusd {

namespace ascii {

// Deduplication removed — O(n^2) comparison was expensive and rarely triggered.

extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<bool> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<int32_t> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<uint32_t> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<int64_t> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<uint64_t> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half2> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half3> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half4> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<float> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::float2> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::float3> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::float4> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<double> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::double2> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::double3> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::double4> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord2h> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord2f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord2d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord3h> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord3f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord3d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::point3h> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::point3f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::point3d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::normal3h> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::normal3f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::normal3d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::vector3h> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::vector3f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::vector3d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color3h> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color3f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color3d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color4h> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color4f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color4d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix2f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix3f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix4f> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix2d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix3d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix4d> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::quath> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::quatf> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::quatd> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::token> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::StringData> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<std::string> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<Reference> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<Path> *result);
extern  template bool AsciiParser::ParseBasicTypeArray(std::vector<value::AssetPath> *result);

//
// -- impl ParseTimeSampleData
//

bool AsciiParser::ParseTimeSampleValueOfArrayType(const uint32_t type_id, value::Value *result) {

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
    std::vector<__type> typed_val; \
    if (!ParseBasicTypeArray(&typed_val)) {                             \
      PUSH_ERROR_AND_RETURN("Failed to parse value with requested type `" + value::GetTypeName(__tyid) + "[]`"); \
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

// `type_name` does not contain "[]"
bool AsciiParser::ParseTimeSampleValueOfArrayType(const std::string &type_name, value::Value *result) {
  nonstd::optional<uint32_t> type_id = value::TryGetTypeId(type_name);
  if (!type_id) {
    PUSH_ERROR_AND_RETURN("Unsupported/invalid type name: " + type_name);
  }

  return ParseTimeSampleValueOfArrayType(type_id.value(), result);
}

bool AsciiParser::ParseTimeSamplesOfArray(const std::string &type_name,
                                   value::TimeSamples *ts_out) {

  value::TimeSamples ts;

  // No early init() needed — add_sample<T>() / add_array_sample<T>() auto-detect
  // the type on first call. Early init caused a silent data-loss bug for token[]
  // timeSamples because non-binary types use different type_id patterns.

  if (!Expect('{')) {
    return false;
  }

  // `{` seen: a timeSamples block WAS authored, even if it turns out to hold no
  // samples (`float x.timeSamples = {}`). Without this, size()==0 is
  // indistinguishable from "no timeSamples at all" and the block is dropped.
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
    if (!ParseTimeSampleValueOfArrayType(type_name, &value)) { // could be None(ValueBlock)
      return false;
    }

    // Simple add helper (dedup removed — was O(n^2) and rarely triggered)
    auto add_sample_now = [&]() {
      ts.add_sample(timeVal, value);
    };

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
        add_sample_now();
        break;
      } else if (sep == ',') {
        // ok - continue to next iteration
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
            add_sample_now();
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

    // Add the sample
    add_sample_now();
  }

  DCOUT("Parse TimeSamples success. # of items = " << ts.size());

  if (ts_out) {
    (*ts_out) = std::move(ts);
  }

  return true;
}

}  // namespace ascii
}  // namespace lightusd

#else  // LIGHTUSD_DISABLE_MODULE_USDA_READER

#endif  // LIGHTUSD_DISABLE_MODULE_USDA_READER
