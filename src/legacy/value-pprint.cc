// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.

#include "value-pprint.hh"

#include <charconv>
#include <cstring>
#include <sstream>

#include "pprinter.hh"
#include "str-util.hh"
#include "value-types.hh"
// NOTE: usdGeom.hh/usdLux.hh/core/prim.hh were only needed by pprint_value's
// schema-prim arms, now in value-pprint-dispatch.cc. The operator<< and
// to_string definitions here are value-type-only.

//
#include "common-macros.inc"

// For fast int/float to ascii
// Default disabled.
//#define LIGHTUSD_LOCAL_USE_JEAIII_ITOA

#if defined(LIGHTUSD_LOCAL_USE_JEAIII_ITOA)
#include "external/jeaiii_to_text.h"
#endif

// NOTE: Using dragonbox-based dtos() from str-util.hh for all float/double
// conversions - it produces shortest representation for 100% of values.

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace {

inline void append_float_to_stream(std::ostream &os, float v) {
  char buf[lightusd::DTOS_MAX_CHARS_FLOAT];
  size_t len = lightusd::dtos(v, buf);
  os.write(buf, static_cast<std::streamsize>(len));
}

// 3-component tuples print as "(x, y, z)". Formatting that into a local buffer
// and writing it once turns seven stream insertions (five of them literals, each
// with its own sentry + xsputn hop) into one: point/normal/vector arrays are the
// bulk of an array-heavy layer, so this is the hottest single value printer.
inline void append_float3_tuple(std::ostream &os, float x, float y, float z) {
  char buf[3 * lightusd::DTOS_MAX_CHARS_FLOAT + 8];
  char *p = buf;
  *p++ = '(';
  p += lightusd::dtos(x, p);
  *p++ = ',';
  *p++ = ' ';
  p += lightusd::dtos(y, p);
  *p++ = ',';
  *p++ = ' ';
  p += lightusd::dtos(z, p);
  *p++ = ')';
  os.write(buf, static_cast<std::streamsize>(p - buf));
}

// --- buffered scalar-array writer -----------------------------------------
// "[1, 2, 3]" used to be emitted one element insertion at a time: each insertion
// pays the ostream sentry/xsputn toll, and integers additionally go through the
// locale-backed num_put path. Formatting into a local buffer and writing it out
// in a few large chunks removes both. The output is the same text as before.
constexpr size_t kArrayBufBytes = 4096;
// Widest element text this writer produces, plus the ", " separator.
constexpr size_t kArrayElemReserve = 40;

template <typename T, typename WriteFn>
inline std::ostream &write_scalar_array(std::ostream &ofs,
                                        const std::vector<T> &v,
                                        WriteFn write_element) {
  char buf[kArrayBufBytes];
  size_t n = 0;
  buf[n++] = '[';
  for (size_t i = 0; i < v.size(); i++) {
    if (i > 0) {
      buf[n++] = ',';
      buf[n++] = ' ';
    }
    n += write_element(buf + n, v[i]);
    if (n + kArrayElemReserve > kArrayBufBytes) {
      ofs.write(buf, static_cast<std::streamsize>(n));
      n = 0;
    }
  }
  buf[n++] = ']';
  ofs.write(buf, static_cast<std::streamsize>(n));
  return ofs;
}

inline size_t write_float_element(char *p, float v) {
  return lightusd::dtos(v, p);
}

inline size_t write_double_element(char *p, double v) {
  return lightusd::dtos(v, p);
}

// Integer elements: plain decimal digits, no locale involvement (same text as
// the stream's default numeric formatting).
template <typename T>
inline size_t write_int_element(char *p, T v) {
#if defined(LIGHTUSD_LOCAL_USE_JEAIII_ITOA)
  lightusd::itoa(v, p);
  return std::strlen(p);
#else
  auto res = std::to_chars(p, p + kArrayElemReserve, v);
  return static_cast<size_t>(res.ptr - p);
#endif
}

inline void append_double3_tuple(std::ostream &os, double x, double y,
                                 double z) {
  char buf[3 * lightusd::DTOS_MAX_CHARS_DOUBLE + 8];
  char *p = buf;
  *p++ = '(';
  p += lightusd::dtos(x, p);
  *p++ = ',';
  *p++ = ' ';
  p += lightusd::dtos(y, p);
  *p++ = ',';
  *p++ = ' ';
  p += lightusd::dtos(z, p);
  *p++ = ')';
  os.write(buf, static_cast<std::streamsize>(p - buf));
}

}  // namespace

namespace lightusd {
namespace pprint {

StringSink::~StringSink() = default;

std::string format_wrapped_array(const std::vector<std::string> &elements,
                                 uint32_t prefix_cols, uint32_t column_limit) {
  if (elements.empty()) {
    return "[]";
  }

  // Check if single-line fits
  size_t total_len = 2;  // "[" and "]"
  for (size_t i = 0; i < elements.size(); i++) {
    total_len += elements[i].size();
    if (i > 0) total_len += 2;  // ", "
  }

  if (prefix_cols + total_len <= column_limit) {
    // Fits on one line
    std::string result = "[";
    for (size_t i = 0; i < elements.size(); i++) {
      if (i > 0) result += ", ";
      result += elements[i];
    }
    result += "]";
    return result;
  }

  // Need wrapping. Compute continuation indent (column after '[').
  uint32_t cont_indent = prefix_cols + 1;
  bool deep_indent = false;

  // If too deep (>60% of column limit), fall back to newline + single indent
  if (cont_indent > column_limit * 3 / 5) {
    deep_indent = true;
    // Single indent width (default 4 spaces)
    cont_indent = static_cast<uint32_t>(Indent(1).size());
  }

  std::string result;
  uint32_t cur_col;

  if (deep_indent) {
    // Start array on next line with single indentation
    result = "\n";
    result += Indent(1);
    result += "[";
    cur_col = cont_indent + 1;
  } else {
    result = "[";
    cur_col = prefix_cols + 1;
  }

  for (size_t i = 0; i < elements.size(); i++) {
    const auto &elem = elements[i];
    // Width needed: element itself + trailing ", " (except for last element)
    uint32_t elem_width = static_cast<uint32_t>(elem.size());

    if (i > 0) {
      // Check if this element fits on the current line
      // +2 for the ", " separator before it
      if (cur_col + 2 + elem_width > column_limit) {
        // Wrap: emit comma at end of previous line, then newline
        result += ",\n";
        result += std::string(cont_indent, ' ');
        cur_col = cont_indent;
      } else {
        result += ", ";
        cur_col += 2;
      }
    }

    result += elem;
    cur_col += elem_width;
  }

  result += "]";
  return result;
}

// Non-template 1D-array emitter: takes already-stringified elements so the
// templated array operator<< (std::vector<T>/TypedArray<T>/ChunkedTypedArray<T>)
// stay thin (just the type-dependent stringify loop) instead of each re-emitting
// the wrap-vs-inline join logic. `wrappable` selects column-wrapping (numeric/
// geometric element types) vs a plain "[a, b, c]" join.
void print_1d_array(std::ostream &os, const std::vector<std::string> &elems,
                    bool wrappable) {
  if (wrappable) {
    uint32_t col_limit = GetColumnLimit();
    if (col_limit > 0 && elems.size() > 1) {
      os << format_wrapped_array(elems, GetPrefixColumns(), col_limit);
      return;
    }
  }
  os << "[";
  for (size_t i = 0; i < elems.size(); i++) {
    if (i > 0) os << ", ";
    os << elems[i];
  }
  os << "]";
}

}  // namespace pprint
}  // namespace lightusd

namespace std {

std::ostream &operator<<(std::ostream &os, const lightusd::value::half &v) {
  // Use direct half-precision dtos for shortest representation
  os << lightusd::dtos(v);
  return os;
}

// Note: operator<< for StringData is defined in pprinter.cc

std::ostream &operator<<(std::ostream &os, const lightusd::value::half2 &v) {
  os << "(" << v[0] << ", " << v[1] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::half3 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::half4 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
  return os;
}

namespace {

int SignedCharValue(char value) {
  const uint8_t bits = static_cast<uint8_t>(static_cast<unsigned char>(value));
  return bits <= 0x7fu ? static_cast<int>(bits)
                       : static_cast<int>(bits) - 0x100;
}

}  // namespace

// USD char is a signed 8-bit value regardless of the target's plain-char ABI.
std::ostream &operator<<(std::ostream &os, const lightusd::value::char2 &v) {
  os << "(" << SignedCharValue(v[0]) << ", " << SignedCharValue(v[1]) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::char3 &v) {
  os << "(" << SignedCharValue(v[0]) << ", " << SignedCharValue(v[1]) << ", "
     << SignedCharValue(v[2]) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::char4 &v) {
  os << "(" << SignedCharValue(v[0]) << ", " << SignedCharValue(v[1]) << ", "
     << SignedCharValue(v[2]) << ", " << SignedCharValue(v[3]) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::uchar2 &v) {
  os << "(" << int(v[0]) << ", " << int(v[1]) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::uchar3 &v) {
  os << "(" << int(v[0]) << ", " << int(v[1]) << ", " << int(v[2]) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::uchar4 &v) {
  os << "(" << int(v[0]) << ", " << int(v[1]) << ", " << int(v[2]) << ", "
     << int(v[3]) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::short2 &v) {
  os << "(" << v[0] << ", " << v[1] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::short3 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::short4 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::ushort2 &v) {
  os << "(" << v[0] << ", " << v[1] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::ushort3 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::ushort4 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::int2 &v) {
  os << "(" << v[0] << ", " << v[1] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::int3 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::int4 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::uint2 &v) {
  os << "(" << v[0] << ", " << v[1] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::uint3 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::uint4 &v) {
  os << "(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::float2 &v) {
  char buffer[lightusd::PRINT_FLOAT2_MAX_CHARS];
  size_t len = lightusd::print_float2(v, buffer);
  os.write(buffer, static_cast<std::streamsize>(len));
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::float3 &v) {
  char buffer[lightusd::PRINT_FLOAT3_MAX_CHARS];
  size_t len = lightusd::print_float3(v, buffer);
  os.write(buffer, static_cast<std::streamsize>(len));
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::float4 &v) {
  char buffer[lightusd::PRINT_FLOAT4_MAX_CHARS];
  size_t len = lightusd::print_float4(v, buffer);
  os.write(buffer, static_cast<std::streamsize>(len));
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::double2 &v) {
  char buffer[lightusd::PRINT_DOUBLE2_MAX_CHARS];
  size_t len = lightusd::print_double2(v, buffer);
  os.write(buffer, static_cast<std::streamsize>(len));
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::double3 &v) {
  char buffer[lightusd::PRINT_DOUBLE3_MAX_CHARS];
  size_t len = lightusd::print_double3(v, buffer);
  os.write(buffer, static_cast<std::streamsize>(len));
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::double4 &v) {
  char buffer[lightusd::PRINT_DOUBLE4_MAX_CHARS];
  size_t len = lightusd::print_double4(v, buffer);
  os.write(buffer, static_cast<std::streamsize>(len));
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::vector3h &v) {
  os << "(" << v.x << ", " << v.y << ", " << v.z << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::vector3f &v) {
  append_float3_tuple(os, v.x, v.y, v.z);
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::vector3d &v) {
  append_double3_tuple(os, v.x, v.y, v.z);
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::normal3h &v) {
  os << "(" << v.x << ", " << v.y << ", " << v.z << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::normal3f &v) {
  append_float3_tuple(os, v.x, v.y, v.z);
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::normal3d &v) {
  append_double3_tuple(os, v.x, v.y, v.z);
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::point3h &v) {
  os << "(" << v.x << ", " << v.y << ", " << v.z << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::point3f &v) {
  append_float3_tuple(os, v.x, v.y, v.z);
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::point3d &v) {
  append_double3_tuple(os, v.x, v.y, v.z);
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::color3h &v) {
  os << "(" << lightusd::value::half_to_float(v.r) << ", "
     << lightusd::value::half_to_float(v.g) << ", "
     << lightusd::value::half_to_float(v.b) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::color3f &v) {
  os << "(";
  append_float_to_stream(os, v.r);
  os << ", ";
  append_float_to_stream(os, v.g);
  os << ", ";
  append_float_to_stream(os, v.b);
  os << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::color3d &v) {
  os << "(" << lightusd::dtos(v.r) << ", " << lightusd::dtos(v.g) << ", "
     << lightusd::dtos(v.b) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::color4h &v) {
  os << "(" << lightusd::value::half_to_float(v.r) << ", "
     << lightusd::value::half_to_float(v.g) << ", "
     << lightusd::value::half_to_float(v.b) << ", "
     << lightusd::value::half_to_float(v.a) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::color4f &v) {
  os << "(";
  append_float_to_stream(os, v.r);
  os << ", ";
  append_float_to_stream(os, v.g);
  os << ", ";
  append_float_to_stream(os, v.b);
  os << ", ";
  append_float_to_stream(os, v.a);
  os << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::color4d &v) {
  os << "(" << lightusd::dtos(v.r) << ", " << lightusd::dtos(v.g) << ", "
     << lightusd::dtos(v.b) << ", " << lightusd::dtos(v.a) << ")";
  return os;
}

// pxrUSD prints quateron in [w, x, y, z] order
// https://github.com/PixarAnimationStudios/USD/blob/3abc46452b1271df7650e9948fef9f0ce602e3b2/pxr/base/gf/quatf.h#L287
std::ostream &operator<<(std::ostream &os, const lightusd::value::quath &v) {
  os << "(" << v.real << ", " << v.imag[0] << ", " << v.imag[1] << ", "
     << v.imag[2] << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::quatf &v) {
  os << "(" << lightusd::dtos(v.real) << ", " << lightusd::dtos(v.imag[0])
     << ", " << lightusd::dtos(v.imag[1]) << ", " << lightusd::dtos(v.imag[2])
     << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os, const lightusd::value::quatd &v) {
  os << "(" << lightusd::dtos(v.real) << ", " << lightusd::dtos(v.imag[0])
     << ", " << lightusd::dtos(v.imag[1]) << ", " << lightusd::dtos(v.imag[2])
     << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord2h &v) {
  os << "(" << v.s << ", " << v.t << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord2f &v) {
  os << "(";
  append_float_to_stream(os, v.s);
  os << ", ";
  append_float_to_stream(os, v.t);
  os << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord2d &v) {
  os << "(" << lightusd::dtos(v.s) << ", " << lightusd::dtos(v.t) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord3h &v) {
  os << "(" << v.s << ", " << v.t << ", " << v.r << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord3f &v) {
  os << "(";
  append_float_to_stream(os, v.s);
  os << ", ";
  append_float_to_stream(os, v.t);
  os << ", ";
  append_float_to_stream(os, v.r);
  os << ")";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const lightusd::value::texcoord3d &v) {
  os << "(" << lightusd::dtos(v.s) << ", " << lightusd::dtos(v.t) << ", "
     << lightusd::dtos(v.r) << ")";
  return os;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::matrix2f &m) {
  ofs << "( ";

  ofs << "(" << lightusd::dtos(m.m[0][0]) << ", " << lightusd::dtos(m.m[0][1])
      << "), ";
  ofs << "(" << lightusd::dtos(m.m[1][0]) << ", " << lightusd::dtos(m.m[1][1])
      << ")";

  ofs << " )";

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::matrix3f &m) {
  ofs << "( ";

  ofs << "(" << lightusd::dtos(m.m[0][0]) << ", " << lightusd::dtos(m.m[0][1])
      << ", " << lightusd::dtos(m.m[0][2]) << "), ";
  ofs << "(" << lightusd::dtos(m.m[1][0]) << ", " << lightusd::dtos(m.m[1][1])
      << ", " << lightusd::dtos(m.m[1][2]) << "), ";
  ofs << "(" << lightusd::dtos(m.m[2][0]) << ", " << lightusd::dtos(m.m[2][1])
      << ", " << lightusd::dtos(m.m[2][2]) << ")";

  ofs << " )";

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::matrix4f &m) {
  ofs << "( ";

  ofs << "(" << lightusd::dtos(m.m[0][0]) << ", " << lightusd::dtos(m.m[0][1])
      << ", " << lightusd::dtos(m.m[0][2]) << ", " << lightusd::dtos(m.m[0][3])
      << "), ";
  ofs << "(" << lightusd::dtos(m.m[1][0]) << ", " << lightusd::dtos(m.m[1][1])
      << ", " << lightusd::dtos(m.m[1][2]) << ", " << lightusd::dtos(m.m[1][3])
      << "), ";
  ofs << "(" << lightusd::dtos(m.m[2][0]) << ", " << lightusd::dtos(m.m[2][1])
      << ", " << lightusd::dtos(m.m[2][2]) << ", " << lightusd::dtos(m.m[2][3])
      << "), ";
  ofs << "(" << lightusd::dtos(m.m[3][0]) << ", " << lightusd::dtos(m.m[3][1])
      << ", " << lightusd::dtos(m.m[3][2]) << ", " << lightusd::dtos(m.m[3][3])
      << ")";

  ofs << " )";

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::matrix2d &m) {
  char buffer[lightusd::PRINT_MATRIX2D_MAX_CHARS];
  size_t len = lightusd::print_matrix2d(m, buffer);
  ofs.write(buffer, static_cast<std::streamsize>(len));
  return ofs;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::matrix3d &m) {
  char buffer[lightusd::PRINT_MATRIX3D_MAX_CHARS];
  size_t len = lightusd::print_matrix3d(m, buffer);
  ofs.write(buffer, static_cast<std::streamsize>(len));
  return ofs;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::matrix4d &m) {
  char buffer[lightusd::PRINT_MATRIX4D_MAX_CHARS];
  size_t len = lightusd::print_matrix4d(m, buffer);
  ofs.write(buffer, static_cast<std::streamsize>(len));
  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::value::frame4d &m) {
  ofs << "( ";

  ofs << "(" << lightusd::dtos(m.m[0][0]) << ", " << lightusd::dtos(m.m[0][1])
      << ", " << lightusd::dtos(m.m[0][2]) << ", " << lightusd::dtos(m.m[0][3])
      << "), ";
  ofs << "(" << lightusd::dtos(m.m[1][0]) << ", " << lightusd::dtos(m.m[1][1])
      << ", " << lightusd::dtos(m.m[1][2]) << ", " << lightusd::dtos(m.m[1][3])
      << "), ";
  ofs << "(" << lightusd::dtos(m.m[2][0]) << ", " << lightusd::dtos(m.m[2][1])
      << ", " << lightusd::dtos(m.m[2][2]) << ", " << lightusd::dtos(m.m[2][3])
      << "), ";
  ofs << "(" << lightusd::dtos(m.m[3][0]) << ", " << lightusd::dtos(m.m[3][1])
      << ", " << lightusd::dtos(m.m[3][2]) << ", " << lightusd::dtos(m.m[3][3])
      << ")";

  ofs << " )";

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::value::timecode &tc) {
  ofs << lightusd::dtos(tc.value);
  return ofs;
}

std::ostream &operator<<(std::ostream &ofs, const lightusd::value::token &tok) {
  // Escape the token content so that a token containing quotation chars,
  // backslashes or control chars re-parses correctly (matches pxr behavior).
  ofs << lightusd::buildEscapedAndQuotedStringForUSDA(tok.str());

  return ofs;
}


std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::AssetPath &asset) {
  std::string in_s = asset.GetAssetPath();

  if (in_s.empty()) {
    ofs << "@@";
  } else {
    std::string quote_str = "@";

    std::string s;

    if (lightusd::contains(in_s, '@')) {
      // Escape '@@@'(to '\@@@') if the input path contains '@@@'
      for (size_t i = 0; i < in_s.length(); i++) {
        if ((i + 2) < in_s.length() &&
            in_s[i] == '@' && in_s[i + 1] == '@' && in_s[i + 2] == '@') {
          s += "\\@@@";
          i += 2;
        } else {
          s += in_s[i];
        }
      }

      quote_str = "@@@";
    } else {
      s = in_s;
    }

    // Do not escape backslash for asset path
    ofs << quote_str << s << quote_str;
  }

  return ofs;
}

std::ostream &operator<<(std::ostream &ofs,
                         const lightusd::value::PathExpression &expr) {
  // SdfPathExpression is authored in USDA as a double-quoted string holding the
  // canonical expression text (e.g. `"/World/Geom//C*"`).
  ofs << lightusd::quote(lightusd::escapeControlSequence(expr.GetText()));
  return ofs;
}

template <>
std::ostream &operator<<(std::ostream &ofs, const std::vector<double> &v) {
  uint32_t col_limit = lightusd::pprint::GetColumnLimit();
  if (col_limit > 0 && v.size() > 1) {
    std::vector<std::string> elems;
    elems.reserve(v.size());
    for (const auto &e : v) {
      elems.push_back(lightusd::dtos(e));
    }
    ofs << lightusd::pprint::format_wrapped_array(
        elems, lightusd::pprint::GetPrefixColumns(), col_limit);
    return ofs;
  }
  return write_scalar_array(ofs, v, write_double_element);
}

template <>
std::ostream &operator<<(std::ostream &ofs, const std::vector<float> &v) {
  uint32_t col_limit = lightusd::pprint::GetColumnLimit();
  if (col_limit > 0 && v.size() > 1) {
    std::vector<std::string> elems;
    elems.reserve(v.size());
    for (const auto &e : v) {
      elems.push_back(lightusd::dtos(e));
    }
    ofs << lightusd::pprint::format_wrapped_array(
        elems, lightusd::pprint::GetPrefixColumns(), col_limit);
    return ofs;
  }
  return write_scalar_array(ofs, v, write_float_element);
}

template <>
std::ostream &operator<<(std::ostream &ofs, const std::vector<int32_t> &v) {
  uint32_t col_limit = lightusd::pprint::GetColumnLimit();
  if (col_limit > 0 && v.size() > 1) {
    std::vector<std::string> elems;
    elems.reserve(v.size());
    for (const auto &e : v) {
      std::ostringstream ess;
      ess << e;
      elems.push_back(ess.str());
    }
    ofs << lightusd::pprint::format_wrapped_array(
        elems, lightusd::pprint::GetPrefixColumns(), col_limit);
    return ofs;
  }
  return write_scalar_array(ofs, v, write_int_element<int32_t>);
}

template <>
std::ostream &operator<<(std::ostream &ofs, const std::vector<uint32_t> &v) {
  uint32_t col_limit = lightusd::pprint::GetColumnLimit();
  if (col_limit > 0 && v.size() > 1) {
    std::vector<std::string> elems;
    elems.reserve(v.size());
    for (const auto &e : v) {
      std::ostringstream ess;
      ess << e;
      elems.push_back(ess.str());
    }
    ofs << lightusd::pprint::format_wrapped_array(
        elems, lightusd::pprint::GetPrefixColumns(), col_limit);
    return ofs;
  }
  return write_scalar_array(ofs, v, write_int_element<uint32_t>);
}

template <>
std::ostream &operator<<(std::ostream &ofs, const std::vector<uint8_t> &v) {
  // uchar[]: print each element as an integer (`os << uint8_t` would emit a char).
  uint32_t col_limit = lightusd::pprint::GetColumnLimit();
  if (col_limit > 0 && v.size() > 1) {
    std::vector<std::string> elems;
    elems.reserve(v.size());
    for (const auto &e : v) {
      std::ostringstream ess;
      ess << static_cast<unsigned int>(e);
      elems.push_back(ess.str());
    }
    ofs << lightusd::pprint::format_wrapped_array(
        elems, lightusd::pprint::GetPrefixColumns(), col_limit);
    return ofs;
  }
  return write_scalar_array(
      ofs, v, [](char *p, uint8_t e) {
        return write_int_element(p, static_cast<unsigned int>(e));
      });
}

template <>
std::ostream &operator<<(std::ostream &ofs, const std::vector<int64_t> &v) {
  uint32_t col_limit = lightusd::pprint::GetColumnLimit();
  if (col_limit > 0 && v.size() > 1) {
    std::vector<std::string> elems;
    elems.reserve(v.size());
    for (const auto &e : v) {
      std::ostringstream ess;
      ess << e;
      elems.push_back(ess.str());
    }
    ofs << lightusd::pprint::format_wrapped_array(
        elems, lightusd::pprint::GetPrefixColumns(), col_limit);
    return ofs;
  }
  return write_scalar_array(ofs, v, write_int_element<int64_t>);
}

template <>
std::ostream &operator<<(std::ostream &ofs, const std::vector<uint64_t> &v) {
  uint32_t col_limit = lightusd::pprint::GetColumnLimit();
  if (col_limit > 0 && v.size() > 1) {
    std::vector<std::string> elems;
    elems.reserve(v.size());
    for (const auto &e : v) {
      std::ostringstream ess;
      ess << e;
      elems.push_back(ess.str());
    }
    ofs << lightusd::pprint::format_wrapped_array(
        elems, lightusd::pprint::GetPrefixColumns(), col_limit);
    return ofs;
  }
  return write_scalar_array(ofs, v, write_int_element<uint64_t>);
}

}  // namespace std

namespace lightusd {

std::string to_string(bool v) {
  if (v) {
    return "true";
  } else {
    return "false";
  }
}

std::string to_string(int32_t v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(uint32_t v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(int64_t v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(uint64_t v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(const lightusd::value::half &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(const lightusd::value::half2 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(const lightusd::value::half3 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(const lightusd::value::half4 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string to_string(const value::char2 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::char3 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::char4 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::short2 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::short3 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::short4 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::int2 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::int3 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::int4 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::uint2 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::uint3 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::uint4 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::float2 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::float3 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::float4 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::double2 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::double3 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::double4 &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::texcoord2h &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::texcoord2f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::texcoord2d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::texcoord3h &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::texcoord3f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::texcoord3d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::StringData &v) {
  // Use buildEscapedAndQuotedStringForUSDA directly for proper quoting
  return buildEscapedAndQuotedStringForUSDA(v.value);
}
std::string to_string(const value::token &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const std::string &s) {
  return buildEscapedAndQuotedStringForUSDA(s);
}
std::string to_string(const value::quath &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::quatf &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::quatd &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::matrix2f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::matrix3f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::matrix4f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::matrix2d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::matrix3d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::matrix4d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::frame4d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::normal3h &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::normal3f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::normal3d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::vector3h &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::vector3f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::vector3d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::point3h &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::point3f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::point3d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::color3f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::color3d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::color4h &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::color4f &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}
std::string to_string(const value::color4d &v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

}  // namespace lightusd
