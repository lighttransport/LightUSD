// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

#pragma once

#include <algorithm>
#include <iostream>
#include <sstream>

#include "value-types.hh"
#include "chunked-typed-array.hh"
#include "pprint-buf.hh"   // pprint::Scratch (reused format buffer)
#include "pprint-enum.hh"  // pprint::GetColumnLimit, format_wrapped_array, is_wrappable_element_v

// forward decl
namespace lightusd {

// in prim-types.hh
class Path;
struct Reference;
struct Payload;
struct LayerOffset;
struct SubLayer;
class Collection;

namespace value {
struct TimeSamples;
}  // namespace value

}  // namespace lightusd

namespace std {

std::ostream &operator<<(std::ostream &os, const lightusd::value::char2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::char3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::char4 &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::uchar2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::uchar3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::uchar4 &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::short2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::short3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::short4 &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::ushort2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::ushort3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::ushort4 &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::int2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::int3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::int4 &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::uint2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::uint3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::uint4 &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::half &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::half2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::half3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::half4 &v);

// Note: operator<< for StringData is declared in pprinter.hh

std::ostream &operator<<(std::ostream &os, const lightusd::value::float2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::float3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::float4 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::double2 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::double3 &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::double4 &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::point3h &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::point3f &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::point3d &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::normal3h &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::normal3f &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::normal3d &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::vector3h &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::vector3f &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::vector3d &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::color3h &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::color3f &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::color3d &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::color4h &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::color4f &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::color4d &v);

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord2h &v);
std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord2f &v);
std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord2d &v);

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord3h &v);
std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord3f &v);
std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord3d &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::quath &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::quatf &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::quatd &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::token &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::dict &v);
std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::TimeSamples &ts);

std::ostream &operator<<(std::ostream &os, const lightusd::value::matrix2f &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::matrix3f &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::matrix4f &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::matrix2d &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::matrix3d &v);
std::ostream &operator<<(std::ostream &os, const lightusd::value::matrix4d &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::frame4d &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::timecode &v);

std::ostream &operator<<(std::ostream &os, const lightusd::value::AssetPath &v);

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::PathExpression &v);

// NOTE: Implemented in pprinter.cc
std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::StringData &v);

// NOTE: Implemented in pprinter.cc
std::ostream &operator<<(std::ostream &os, const lightusd::Path &v);
std::ostream &operator<<(std::ostream &os, const lightusd::Reference &v);
std::ostream &operator<<(std::ostream &os, const lightusd::Payload &v);
std::ostream &operator<<(std::ostream &os, const lightusd::LayerOffset &v);
std::ostream &operator<<(std::ostream &os, const lightusd::SubLayer &v);
std::ostream &operator<<(std::ostream &os, const lightusd::Collection &v);

// 1D array
// All three array containers (std::vector / TypedArray / ChunkedTypedArray)
// share the same element access (size()/operator[]) and the same output format,
// so each operator<< only does the type-dependent work — stringify each element —
// then hands the strings to the once-compiled non-template print_1d_array(). This
// keeps per-(element-type x container) instantiations thin instead of re-emitting
// the wrap/join logic in each.
//
// Two paths:
//  - No column wrapping (the default) needs no element widths, so elements are
//    formatted straight into a reused per-thread buffer and the finished array is
//    written out in one go. A fresh std::ostringstream per element used to be
//    built here, and its ios_base/locale setup alone cost ~400 instructions per
//    element — about a third of the whole print pass on an array-heavy layer.
//  - Wrapped layout (SetColumnLimit) needs every width up front; keep the
//    element-string vector for it.
template <typename ArrayT>
inline std::ostream &print_array_impl_wrapped(std::ostream &os, const ArrayT &v,
                                              bool wrappable) {
  std::vector<std::string> elems;
  elems.reserve(v.size());
  for (size_t i = 0; i < v.size(); i++) {
    std::ostringstream ess;
    ess << v[i];
    elems.push_back(ess.str());
  }
  lightusd::pprint::print_1d_array(os, elems, wrappable);
  return os;
}

template <typename ArrayT>
inline std::ostream &print_array_impl(std::ostream &os, const ArrayT &v,
                                      bool wrappable) {
  // Flush every so often so a huge array does not double its memory here.
  constexpr size_t kFlushBytes = 1u << 16;

  if (wrappable && lightusd::pprint::GetColumnLimit() > 0) {
    // Column-wrapped layout needs every element width up front.
    return print_array_impl_wrapped(os, v, wrappable);
  }

  lightusd::pprint::ScratchRef buf;
  std::ostream &ess = buf.os();
  std::string &out = buf.buffer();

  out.push_back('[');
  for (size_t i = 0; i < v.size(); i++) {
    if (i > 0) {
      out.append(", ", 2);
    }
    ess << v[i];
    if (out.size() >= kFlushBytes) {
      os.write(out.data(), static_cast<std::streamsize>(out.size()));
      out.clear();
    }
  }
  out.push_back(']');
  os.write(out.data(), static_cast<std::streamsize>(out.size()));

  return os;
}

template <typename T>
std::ostream &operator<<(std::ostream &os, const std::vector<T> &v) {
  return print_array_impl(os, v, lightusd::pprint::is_wrappable_element_v<T>);
}

template <typename T>
std::ostream &operator<<(std::ostream &os, const lightusd::TypedArray<T> &v) {
  return print_array_impl(os, v, lightusd::pprint::is_wrappable_element_v<T>);
}

template <typename T>
std::ostream &operator<<(std::ostream &os, const lightusd::ChunkedTypedArray<T> &v) {
  return print_array_impl(os, v, lightusd::pprint::is_wrappable_element_v<T>);
}

// Provide specialized version for int and float array.
template <>
std::ostream &operator<<(std::ostream &os, const std::vector<double> &v);

template <>
std::ostream &operator<<(std::ostream &os, const std::vector<float> &v);

template <>
std::ostream &operator<<(std::ostream &os, const std::vector<int32_t> &v);

template <>
std::ostream &operator<<(std::ostream &os, const std::vector<uint32_t> &v);

template <>
std::ostream &operator<<(std::ostream &os, const std::vector<uint8_t> &v);

template <>
std::ostream &operator<<(std::ostream &os, const std::vector<int64_t> &v);

template <>
std::ostream &operator<<(std::ostream &os, const std::vector<uint64_t> &v);

}  // namespace std

namespace lightusd {

std::string to_string(bool v);
std::string to_string(int32_t v);
std::string to_string(uint32_t v);
std::string to_string(int64_t v);
std::string to_string(uint64_t v);

std::string to_string(const value::char2 &v);
std::string to_string(const value::char3 &v);
std::string to_string(const value::char4 &v);
std::string to_string(const value::short2 &v);
std::string to_string(const value::short3 &v);
std::string to_string(const value::short4 &v);
std::string to_string(const value::int2 &v);
std::string to_string(const value::int3 &v);
std::string to_string(const value::int4 &v);
std::string to_string(const value::uint2 &v);
std::string to_string(const value::uint3 &v);
std::string to_string(const value::uint4 &v);
std::string to_string(const value::float2 &v);
std::string to_string(const value::float3 &v);
std::string to_string(const value::float4 &v);
std::string to_string(const value::double2 &v);
std::string to_string(const value::double3 &v);
std::string to_string(const value::double4 &v);
std::string to_string(const value::texcoord2h &v);
std::string to_string(const value::texcoord2f &v);
std::string to_string(const value::texcoord2d &v);
std::string to_string(const value::texcoord3h &v);
std::string to_string(const value::texcoord3f &v);
std::string to_string(const value::texcoord3d &v);
std::string to_string(const value::StringData &s);
std::string to_string(const value::token &s);
std::string to_string(const std::string &s); // do USD specific escaping
std::string to_string(const value::quath &v);
std::string to_string(const value::quatf &v);
std::string to_string(const value::quatd &v);
std::string to_string(const value::matrix2f &v);
std::string to_string(const value::matrix3f &v);
std::string to_string(const value::matrix4f &v);
std::string to_string(const value::matrix2d &v);
std::string to_string(const value::matrix3d &v);
std::string to_string(const value::matrix4d &v);
std::string to_string(const value::frame4d &v);
std::string to_string(const value::half &v);
std::string to_string(const value::half2 &v);
std::string to_string(const value::half3 &v);
std::string to_string(const value::half4 &v);
std::string to_string(const value::normal3h &v);
std::string to_string(const value::normal3f &v);
std::string to_string(const value::normal3d &v);
std::string to_string(const value::vector3h &v);
std::string to_string(const value::vector3f &v);
std::string to_string(const value::vector3d &v);
std::string to_string(const value::point3h &v);
std::string to_string(const value::point3f &v);
std::string to_string(const value::point3d &v);
std::string to_string(const value::color3f &v);
std::string to_string(const value::color3d &v);
std::string to_string(const value::color4h &v);
std::string to_string(const value::color4f &v);
std::string to_string(const value::color4d &v);

namespace value {

std::string pprint_value(const lightusd::value::Value &v,
                         const uint32_t indent = 0, bool closing_brace = true);

// Renders a value::Value holding a schema *prim* type (Model/Xform/GeomMesh/
// Material/.../SpatialAudio) via to_string(). Defined in value-pprint-prim.cc
// (which carries the schema headers); pprint_value() forwards here for type_ids
// in [TYPE_ID_MODEL_BEGIN, TYPE_ID_MODEL_END). Split out so value-pprint-dispatch.cc
// (the base-type/array renderer) compiles without the per-prim-type instantiation.
std::string pprint_prim_value(const lightusd::value::Value &v,
                              const uint32_t indent = 0, bool closing_brace = true);

// Print first N and last N items.
// 0 = print all items.
// Callee must ensure access to `vals` does not trigger out-of-bounds error.
template <typename T>
std::string print_array_snipped(const T *vals, size_t n, size_t N = 16) {
  std::stringstream os;

  if ((N == 0) || ((N * 2) >= n)) {
    os << "[";
    for (size_t i = 0; i < n; i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }
    os << "]";
  } else {
    size_t head_end = (std::min)(N, n);
    size_t tail_start = (std::max)(n - N, head_end);

    os << "[";

    for (size_t i = 0; i < head_end; i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }

    os << ", ..., ";

    for (size_t i = tail_start; i < n; i++) {
      if (i > tail_start) {
        os << ", ";
      }
      os << vals[i];
    }

    os << "]";
  }
  return os.str();
}

// Account for stride.
// stride 0 => use sizeof(T)
// Callee must ensure access to `vals` does not trigger out-of-bounds error.
template <typename T>
std::string print_strided_array_snipped(const uint8_t *vals, size_t stride_bytes, const size_t n, size_t N = 16) {
  std::stringstream os;

  if ((stride_bytes == 0) || (stride_bytes == sizeof(T))) { // tightly packed.
    return print_array_snipped(reinterpret_cast<const T*>(vals), n, N);
  }

  if ((N == 0) || ((N * 2) >= n)) {
    os << "[";
    for (size_t i = 0; i < n; i++) {
      if (i > 0) {
        os << ", ";
      }
      os << *reinterpret_cast<const T *>(&vals[i * stride_bytes]);
    }
    os << "]";
  } else {
    size_t head_end = (std::min)(N, n);
    size_t tail_start = (std::max)(n - N, head_end);

    os << "[";

    for (size_t i = 0; i < head_end; i++) {
      if (i > 0) {
        os << ", ";
      }
      os << *reinterpret_cast<const T *>(&vals[i * stride_bytes]);
    }

    os << ", ..., ";

    for (size_t i = tail_start; i < n; i++) {
      if (i > tail_start) {
        os << ", ";
      }
      os << *reinterpret_cast<const T *>(&vals[i * stride_bytes]);
    }

    os << "]";
  }
  return os.str();
}

// Print first N and last N items.
// 0 = print all items.
// Useful when dump
template <typename T>
std::string print_array_snipped(const std::vector<T> &vals, size_t N = 16) {
  std::stringstream os;

  if ((N == 0) || ((N * 2) >= vals.size())) {
    os << "[";
    for (size_t i = 0; i < vals.size(); i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }
    os << "]";
  } else {
    size_t head_end = (std::min)(N, vals.size());
    size_t tail_start = (std::max)(vals.size() - N, head_end);

    os << "[";

    for (size_t i = 0; i < head_end; i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }

    os << ", ..., ";

    for (size_t i = tail_start; i < vals.size(); i++) {
      if (i > tail_start) {
        os << ", ";
      }
      os << vals[i];
    }

    os << "]";
  }
  return os.str();
}

// Print first N and last N items.
// 0 = print all items.
// Useful when dump
template <typename T>
std::string print_array_snipped(const TypedArray<T> &vals, size_t N = 16) {
  std::stringstream os;

  if ((N == 0) || ((N * 2) >= vals.size())) {
    os << "[";
    for (size_t i = 0; i < vals.size(); i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }
    os << "]";
  } else {
    size_t head_end = (std::min)(N, vals.size());
    size_t tail_start = (std::max)(vals.size() - N, head_end);

    os << "[";

    for (size_t i = 0; i < head_end; i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }

    os << ", ..., ";

    for (size_t i = tail_start; i < vals.size(); i++) {
      if (i > tail_start) {
        os << ", ";
      }
      os << vals[i];
    }

    os << "]";
  }
  return os.str();
}

template <typename T>
std::string print_array_snipped(const ChunkedTypedArray<T> &vals, size_t N = 16) {
  std::stringstream os;

  if ((N == 0) || ((N * 2) >= vals.size())) {
    os << "[";
    for (size_t i = 0; i < vals.size(); i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }
    os << "]";
  } else {
    size_t head_end = (std::min)(N, vals.size());
    size_t tail_start = (std::max)(vals.size() - N, head_end);

    os << "[";

    for (size_t i = 0; i < head_end; i++) {
      if (i > 0) {
        os << ", ";
      }
      os << vals[i];
    }

    os << ", ..., ";

    for (size_t i = tail_start; i < vals.size(); i++) {
      if (i > tail_start) {
        os << ", ";
      }
      os << vals[i];
    }

    os << "]";
  }
  return os.str();
}

}  // namespace value
}  // namespace lightusd
