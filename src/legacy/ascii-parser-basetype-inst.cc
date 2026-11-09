// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - 2022, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment, Inc.
//
// Ascii Basic type parser
//

#include <cstdio>
#ifdef _MSC_VER
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <algorithm>
#include <atomic>
#include <array>
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
#include "path-util.hh"
#include "tiny-format.hh"
#include "typed-array-core.hh"

//
#if !defined(LIGHTUSD_DISABLE_MODULE_USDA_READER)

//

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// external

#include "external/fast_float/include/fast_float/fast_float.h"

#define CHECK_MEMORY_USAGE(__nbytes) do { \
  _memory_usage += (__nbytes); \
  if (_memory_usage > _max_memory_limit_bytes) { \
    PushError(fmt::format("Memory limit exceeded. Limit: {} MB, Current usage: {} MB", \
      _max_memory_limit_bytes / (1024*1024), _memory_usage / (1024*1024))); \
    return false; \
  }  \
  } while(0)

#define REDUCE_MEMORY_USAGE(__nbytes) do { \
  if (_memory_usage >= (__nbytes)) { \
    _memory_usage -= (__nbytes); \
  } \
  } while(0)
#include "external/jsteemann/atoi.h"
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

#include "io-util.hh"
#include "core/prim-spec.hh"
#include "str-util.hh"
#include "stream-reader.hh"
#include "lightusd.hh"
#include "value-pprint.hh"
#include "value-types.hh"

#include "common-macros.inc"
#include "tiny-string.hh"


namespace lightusd {
namespace ascii {

[[maybe_unused]] constexpr auto kAscii = "[ASCII]";

#include "ascii-parser-basetype-impl.inc"

template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<int32_t, 2>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<int32_t, 3>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<int32_t, 4>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<uint32_t, 2>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<uint32_t, 3>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<uint32_t, 4>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<int64_t, 2>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<int64_t, 3>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<int64_t, 4>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<uint64_t, 2>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<uint64_t, 3>>> *result);
template bool AsciiParser::ParseTupleArray(std::vector<nonstd::optional<std::array<uint64_t, 4>>> *result);

template bool AsciiParser::SepBy1BasicType<float>(const char sep,
                                                  std::vector<float> *result);

template bool AsciiParser::ParseBasicTypeArray(std::vector<bool> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::int2> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::int3> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::int4> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uint2> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uint3> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uint4> *result);
// char types
template bool AsciiParser::ParseBasicTypeArray(std::vector<char> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::char2> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::char3> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::char4> *result);
// uchar types
template bool AsciiParser::ParseBasicTypeArray(std::vector<uint8_t> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uchar2> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uchar3> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::uchar4> *result);
// short types
template bool AsciiParser::ParseBasicTypeArray(std::vector<int16_t> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::short2> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::short3> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::short4> *result);
// ushort types
template bool AsciiParser::ParseBasicTypeArray(std::vector<uint16_t> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::ushort2> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::ushort3> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::ushort4> *result);
// Note: int32_t, uint32_t, int64_t, uint64_t, token, string/StringData,
// half, float, double, half2/3/4,
// float2/3/4, point3f, normal3f, double2/3/4, and quat* arrays now use optimized implementations
// template bool AsciiParser::ParseBasicTypeArray(std::vector<int32_t> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<uint32_t> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<int64_t> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<uint64_t> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::token> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::StringData> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<std::string> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half2> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half3> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::half4> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<float> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::float2> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::float3> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::float4> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::point3f> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::normal3f> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<double> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::double2> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::double3> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::double4> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord2h> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord2f> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord2d> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord3h> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord3f> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::texcoord3d> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::point3h> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::point3d> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::normal3h> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::normal3d> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::vector3h> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::vector3f> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::vector3d> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color3h> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color3f> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color3d> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color4h> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color4f> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::color4d> *result);
// Note: matrix arrays now use optimized implementations
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix2f> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix3f> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix4f> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix2d> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix3d> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::matrix4d> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::frame4d> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::quath> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::quatf> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::quatd> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::token> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<value::StringData> *result);
// template bool AsciiParser::ParseBasicTypeArray(std::vector<std::string> *result);
//template bool AsciiParser::ParseBasicTypeArray(std::vector<Reference> *result);
//template bool AsciiParser::ParseBasicTypeArray(std::vector<Path> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::AssetPath> *result);
template bool AsciiParser::ParseBasicTypeArray(std::vector<value::PathExpression> *result);

//
// TypedArray template instantiations for memory optimization
//
template bool AsciiParser::ParseBasicTypeArray(TypedArray<bool> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<int32_t> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::int2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::int3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::int4> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<uint32_t> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::uint2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::uint3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::uint4> *result);
// char types
template bool AsciiParser::ParseBasicTypeArray(TypedArray<char> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::char2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::char3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::char4> *result);
// uchar types
template bool AsciiParser::ParseBasicTypeArray(TypedArray<uint8_t> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::uchar2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::uchar3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::uchar4> *result);
// short types
template bool AsciiParser::ParseBasicTypeArray(TypedArray<int16_t> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::short2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::short3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::short4> *result);
// ushort types
template bool AsciiParser::ParseBasicTypeArray(TypedArray<uint16_t> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::ushort2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::ushort3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::ushort4> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<int64_t> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<uint64_t> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::half> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::half2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::half3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::half4> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<float> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::float2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::float3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::float4> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<double> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::double2> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::double3> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::double4> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::texcoord2h> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::texcoord2f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::texcoord2d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::texcoord3h> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::texcoord3f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::texcoord3d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::point3h> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::point3f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::point3d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::normal3h> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::normal3f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::normal3d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::vector3h> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::vector3f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::vector3d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::color3h> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::color3f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::color3d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::color4h> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::color4f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::color4d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::matrix2f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::matrix3f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::matrix4f> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::matrix2d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::matrix3d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::matrix4d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::frame4d> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::quath> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::quatf> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::quatd> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::token> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::StringData> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<std::string> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::AssetPath> *result);
template bool AsciiParser::ParseBasicTypeArray(TypedArray<value::PathExpression> *result);

}  // namespace ascii
}  // namespace lightusd

#else  // LIGHTUSD_DISABLE_MODULE_USDA_READER

#endif  // LIGHTUSD_DISABLE_MODULE_USDA_READER
