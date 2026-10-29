// SPDX-License-Identifier: MIT
// Copyright 2024-Present Syoyo Fujita.

///
/// Simple but fast string library.
///

#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>   // SIZE_MAX / UINT64_C used by std::hash<tstring_view>
#include <functional>
#include <ostream>
#include <string>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "tiny-container.hh"
#include "value-types.hh"

namespace lightusd {

// default: Up to 1G char
//
// Bounded length: never reads past `max_len` bytes (hardening guard against
// unterminated buffers). Uses strnlen() so the libc vectorised implementation
// does the scanning - this is called from every tstring_view built from a char*
// and showed up as >3% of a USDC load as a byte-at-a-time loop.
static size_t strlen(const char *s, size_t max_len = 1024u*1024u*1024u) {
  if (!s) {
    return 0;
  }

  return ::strnlen(s, max_len);
}


template<size_t N = 8>
struct tstring_n {
  // 8 = enough size to store pointer address
  static_assert(N >= 8, "N must be 8 or larger.");

 public:

  tstring_n() {}
  ~tstring_n() {
    _delete_string();
  }

  tstring_n(const char *s) {
    _copy_string(s);
  }

  tstring_n(const std::string &s) : tstring_n(s.c_str()) {
  }

  tstring_n(const tstring_n &rhs) : tstring_n(rhs.c_str()) {
  }

  tstring_n(tstring_n &&rhs) {

    _delete_string();

    _u = std::exchange(rhs._u, nullptr);
    _len = std::exchange(rhs._len, 0);
  }

  tstring_n &operator=(const tstring_n &rhs) {
    if (this == &rhs) {
      return *this;
    }

    _copy_string(rhs.c_str());

    return *this;
  }

  tstring_n &operator=(tstring_n &&rhs) noexcept {
    if (this == &rhs) {
      return *this;
    }

    _delete_string();

    _u = std::exchange(rhs._u, nullptr);
    _len = std::exchange(rhs._len, 0);

    return *this;
  }

  const char *c_str() const {
    if (_len > N) {
      return reinterpret_cast<const char *>(_u._ptr);
    } else {
      return reinterpret_cast<const char *>(_u._buf);
    }
  }

  size_t size() const {
    return _len;
  }

  std::string to_std_string() {
    const char *p;
    if (_len > N) {
      p = reinterpret_cast<const char *>(_u._ptr);
    } else {
      p = reinterpret_cast<const char *>(_u._buf);
    }
    std::string s(p, _len);
    return s;
  }

 private:
  void _delete_string() {
    if ((_len > N) && (_u._ptr)) {
      delete[] _u._ptr;
    }
    memset(_u._buf, 0, 8);
    _len = 0;
  }

  void _copy_string(const char *s) {

    _delete_string();

    //if (_len > 0) {
    //  if (_len >= N) {
    //    //char *p = reinterpret_cast<char *>(_buf);
    //    //delete[] p;
    //    //memset(_buf, 0, 8);
    //  }
    //}

    //char *dst = reinterpret_cast<char *>(_buf);

    _len = strlen(s);
    if (_len > N) {
      char *dst = new char[_len+1];
      memcpy(dst, s, _len);
      dst[_len] = '\0';

      _u._ptr = dst;
    } else {
      memcpy(_u._buf, s, _len);
      _u._buf[_len] = '\0';
    }

  }

  // TODO: Ues custom vector class.
  union {
    char _buf[N+1]{};
    char *_ptr;
  } _u;

  size_t _len{0};
};

using tstring = tstring_n<>;

// just a retain the pointer address.
struct tstring_view {
 public:

  constexpr tstring_view() {}
  // No user-provided destructor: a view owns nothing, and a trivial dtor keeps
  // tstring_view trivially copyable and avoids -Wdeprecated-copy.

  tstring_view(const char *s) {
    _len = strlen(s);
    _s = s;
  }

  constexpr tstring_view(const char *s, size_t n) {
    _len = n;
    _s = s;
  }

  tstring_view(const std::string &s) : tstring_view(s.c_str()) {
  }

  tstring_view(const tstring &s) : tstring_view(s.c_str()) {
  }

  bool operator==(const tstring_view &rhs) const {
    if (_len != rhs.size()) {
      return false;
    }
    const char *rs = rhs.c_str();
    for (size_t i = 0; i < _len; i++) {
      if (_s[i] != rs[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const tstring_view &rhs) const {
    return !(*this == rhs);
  }

  // Lexicographic ordering (byte-wise), for use in ordered containers.
  bool operator<(const tstring_view &rhs) const {
    const size_t n = (_len < rhs._len) ? _len : rhs._len;
    const char *rs = rhs.c_str();
    for (size_t i = 0; i < n; i++) {
      const unsigned char a = static_cast<unsigned char>(_s[i]);
      const unsigned char b = static_cast<unsigned char>(rs[i]);
      if (a != b) {
        return a < b;
      }
    }
    return _len < rhs._len;
  }

  const char *c_str() const {
    return _s;
  }

  // NOTE: a tstring_view obtained as a *slice* of a larger buffer is NOT
  // guaranteed to be NUL-terminated at c_str()[size()]. Use size()-aware
  // operations (==, <, starts_with, ...). Do not hand c_str() to length-
  // agnostic C functions (strlen, printf("%s"), std::string(const char*));
  // build an owned copy with lightusd::to_string(view) instead.
  const char *data() const LIGHTUSD_LIFETIMEBOUND {
    return _s;
  }

  bool empty() const {
    return _len == 0;
  }

  size_t size() const {
    return _len;
  }

  // Implicit owning copy to std::string. This is a migration-compatibility
  // convenience so a view can be passed where a std::string is expected
  // (constructors, concatenation, ostream, map keys). It ALLOCATES — hot paths
  // should use the size-aware view operations (==, <, starts_with, ...) which
  // do not convert. Safe for interior slices (uses size, not NUL-termination).
  operator std::string() const {
    return _s ? std::string(_s, _len) : std::string();
  }

  // C++20-like API
  bool starts_with( const tstring_view &sv ) const noexcept
  {
    size_t sv_size = sv.size();

    if (_len < sv_size) {
      return false;
    }
    const char *sv_str = sv.c_str();

    for (size_t i = 0; i < sv_size; i++) {
      if (_s[i] != sv_str[i]) {
        return false;
      }
    }
    return true;
  }

  bool starts_with( const char *s) const {
    size_t s_size = strlen(s);

    if (_len < s_size) {
      return false;
    }
    for (size_t i = 0; i < s_size; i++) {
      if (_s[i] != s[i]) {
        return false;
      }
    }
    return true;
  }

  bool ends_with( const tstring_view &sv ) const noexcept
  {
    size_t sv_size = sv.size();

    if (_len < sv_size) {
      return false;
    }
    const char *sv_str = sv.c_str();

    for (size_t i = 0; i < sv_size; i++) {
      if (_s[_len - i - 1] != sv_str[sv_size - i - 1]) {
        return false;
      }
    }
    return true;
  }

  bool ends_with( const char *s) const {
    size_t s_size = strlen(s);

    if (_len < s_size) {
      return false;
    }
    for (size_t i = 0; i < s_size; i++) {
      if (_s[_len - i - 1] != s[s_size - i - 1]) {
        return false;
      }
    }
    return true;
  }

  // Size-aware substring search (checks every candidate position, unlike a
  // first-char-only scan). `s` need not be NUL-terminated; `n` is its length.
  bool contains(const char *s, size_t n) const {
    if (n == 0) {
      return false;
    }
    if (_len < n) {
      return false;
    }
    for (size_t i = 0; i + n <= _len; i++) {
      size_t j = 0;
      for (; j < n; j++) {
        if (_s[i + j] != s[j]) {
          break;
        }
      }
      if (j == n) {
        return true;
      }
    }
    return false;
  }

  bool contains(const char *s) const {
    return contains(s, strlen(s));
  }

  bool contains(const tstring_view &sv) const {
    return contains(sv.c_str(), sv.size());
  }

 private:
  // TODO: Ues custom vector class.
  const char *_s{nullptr}; // end with '\0'
  size_t _len{0};
};

// Comparisons against const char* / std::string in both operand orders.
// (view == "x" and view == std::string already work via the implicit
// tstring_view constructors; these add the left-hand-side forms and !=.)
// Exact-match overloads in both operand orders. With the implicit
// operator std::string() above these are required to keep comparisons
// unambiguous (an exact overload outranks both the implicit string->view and
// view->string conversions).
inline bool operator==(const tstring_view &a, const char *b) { return a == tstring_view(b); }
inline bool operator==(const char *a, const tstring_view &b) { return tstring_view(a) == b; }
inline bool operator!=(const tstring_view &a, const char *b) { return !(a == tstring_view(b)); }
inline bool operator!=(const char *a, const tstring_view &b) { return !(tstring_view(a) == b); }
inline bool operator==(const tstring_view &a, const std::string &b) { return a == tstring_view(b); }
inline bool operator==(const std::string &a, const tstring_view &b) { return tstring_view(a) == b; }
inline bool operator!=(const tstring_view &a, const std::string &b) { return !(a == tstring_view(b)); }
inline bool operator!=(const std::string &a, const tstring_view &b) { return !(tstring_view(a) == b); }
inline bool operator<(const tstring_view &a, const std::string &b) { return a < tstring_view(b); }
inline bool operator<(const std::string &a, const tstring_view &b) { return tstring_view(a) < b; }
inline bool operator<(const tstring_view &a, const char *b) { return a < tstring_view(b); }
inline bool operator<(const char *a, const tstring_view &b) { return tstring_view(a) < b; }

// Owning copy of a view (use when a NUL-terminated std::string is required,
// e.g. passing a slice view to a C API or storing it).
inline std::string to_string(const tstring_view &v) {
  return std::string(v.c_str(), v.size());
}

// Stream + concatenation as non-template overloads. std::string's operator+/<<
// are function templates, so template deduction (which ignores the implicit
// std::string conversion) would otherwise reject a tstring_view operand.
inline std::ostream &operator<<(std::ostream &os, const tstring_view &v) {
  return os.write(v.c_str(), static_cast<std::streamsize>(v.size()));
}
inline std::string operator+(const std::string &a, const tstring_view &b) {
  std::string s(a);
  s.append(b.c_str(), b.size());
  return s;
}
inline std::string operator+(const tstring_view &a, const std::string &b) {
  std::string s;
  s.reserve(a.size() + b.size());
  s.append(a.c_str(), a.size());
  s.append(b);
  return s;
}
inline std::string operator+(const char *a, const tstring_view &b) {
  std::string s(a);
  s.append(b.c_str(), b.size());
  return s;
}
inline std::string operator+(const tstring_view &a, const char *b) {
  std::string s;
  s.append(a.c_str(), a.size());
  s.append(b);
  return s;
}

// Simple std::ostringstream like class
class tostringstream
{
 public:

    tostringstream &operator<<( const tstring &str );
    tostringstream &operator<<( const tstring_view &str );

    void write(const char *p, const size_t n);

    uint64_t size() const {
      return binary_.size();
    }

    std::string str() const;
    tstring tstr() const;


    const char *data() const LIGHTUSD_LIFETIMEBOUND { return binary_.data(); }

 private:
  const std::vector<char> binary_;
  mutable uint64_t idx_{0};
};

namespace str {

bool parse_int(const tstring_view &sv, int32_t *ret);
bool parse_int64(const tstring_view &sv, int64_t *ret);

bool parse_uint(const tstring_view &sv, uint32_t *ret);
bool parse_uint64(const tstring_view &sv, uint64_t *ret);

bool parse_float(const tstring_view &sv, float *ret);
bool parse_double(const tstring_view &sv, double *ret);

bool parse_int_array(const tstring_view &sv, std::vector<int32_t> *result);
bool parse_uint_array(const tstring_view &sv, std::vector<uint32_t> *result);
bool parse_int64_array(const tstring_view &sv, std::vector<int64_t> *result);
bool parse_uint64_array(const tstring_view &sv, std::vector<uint64_t> *result);
bool parse_half_array(const tstring_view &sv, std::vector<lightusd::value::half> *result);
bool parse_float_array(const tstring_view &sv, std::vector<float> *result);
bool parse_double_array(const tstring_view &sv, std::vector<double> *result);
bool parse_token_array(const tstring_view &sv, std::vector<lightusd::value::token> *result);
bool parse_string_array(const tstring_view &sv, std::vector<lightusd::value::StringData> *result);
bool parse_std_string_array(const tstring_view &sv, std::vector<std::string> *result);

// Compound-type array parsers
bool parse_half2_array(const tstring_view &sv, std::vector<lightusd::value::half2> *result);
bool parse_half3_array(const tstring_view &sv, std::vector<lightusd::value::half3> *result);
bool parse_half4_array(const tstring_view &sv, std::vector<lightusd::value::half4> *result);
bool parse_float2_array(const tstring_view &sv, std::vector<lightusd::value::float2> *result);
bool parse_float3_array(const tstring_view &sv, std::vector<lightusd::value::float3> *result);
bool parse_float4_array(const tstring_view &sv, std::vector<lightusd::value::float4> *result);
bool parse_point3f_array(const tstring_view &sv, std::vector<lightusd::value::point3f> *result);
bool parse_normal3f_array(const tstring_view &sv, std::vector<lightusd::value::normal3f> *result);
bool parse_double2_array(const tstring_view &sv, std::vector<lightusd::value::double2> *result);
bool parse_double3_array(const tstring_view &sv, std::vector<lightusd::value::double3> *result);
bool parse_double4_array(const tstring_view &sv, std::vector<lightusd::value::double4> *result);
bool parse_quath_array(const tstring_view &sv, std::vector<lightusd::value::quath> *result);
bool parse_quatf_array(const tstring_view &sv, std::vector<lightusd::value::quatf> *result);
bool parse_quatd_array(const tstring_view &sv, std::vector<lightusd::value::quatd> *result);
bool parse_matrix2f_array(const tstring_view &sv, std::vector<lightusd::value::matrix2f> *result);
bool parse_matrix3f_array(const tstring_view &sv, std::vector<lightusd::value::matrix3f> *result);
bool parse_matrix4f_array(const tstring_view &sv, std::vector<lightusd::value::matrix4f> *result);
bool parse_matrix2d_array(const tstring_view &sv, std::vector<lightusd::value::matrix2d> *result);
bool parse_matrix3d_array(const tstring_view &sv, std::vector<lightusd::value::matrix3d> *result);
bool parse_matrix4d_array(const tstring_view &sv, std::vector<lightusd::value::matrix4d> *result);

}

} // namespace lightusd

namespace std {

// Hash a tstring_view by its bytes (FNV-1a), so views can key unordered
// containers. Equal byte sequences hash equally regardless of origin buffer.
// Uses 64-bit FNV-1a on 64-bit size_t platforms and 32-bit FNV-1a on 32-bit
// size_t platforms (e.g. wasm32). The 64-bit constant 0xCBF29CE484222325
// (= 14695981039346656037) does NOT fit in a 32-bit size_t, so it must not
// be used as-is on 32-bit platforms (suppresses -Wconstant-conversion).
template <>
struct hash<lightusd::tstring_view> {
  size_t operator()(const lightusd::tstring_view &v) const noexcept {
#if SIZE_MAX >= UINT64_C(0xFFFFFFFFFFFFFFFF)
    // 64-bit FNV-1a
    constexpr size_t kFnvOffsetBasis = static_cast<size_t>(UINT64_C(0xCBF29CE484222325));
    constexpr size_t kFnvPrime = static_cast<size_t>(UINT64_C(0x100000001B3));
#else
    // 32-bit FNV-1a
    constexpr size_t kFnvOffsetBasis = static_cast<size_t>(0x811C9DC5);
    constexpr size_t kFnvPrime = static_cast<size_t>(0x01000193);
#endif
    size_t h = kFnvOffsetBasis;
    const char *p = v.c_str();
    const size_t n = v.size();
    for (size_t i = 0; i < n; i++) {
      h ^= static_cast<unsigned char>(p[i]);
      h *= kFnvPrime;
    }
    return h;
  }
};

}  // namespace std
