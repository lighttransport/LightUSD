// SPDX-License-Identifier: MIT
// Copyright 2022-Present Syoyo Fujita.

///
/// Simple Python-like format print utility in C++11 or later. Only supports
/// "{}".
///
#pragma once

#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "compiler-features.hh"

#include "nonstd/expected.hpp"

namespace lightusd {
namespace fmt {

/// Lower-case hexadecimal of `v`, zero-padded to `width` digits (0 = no
/// padding). A small type-safe replacement for `snprintf("%0*llx", ...)` /
/// `%016llx`, usable directly or via `format("...{}...", hex(v, 16))`.
inline std::string hex(uint64_t v, int width = 0) {
  std::ostringstream ss;
  ss.imbue(std::locale::classic());
  ss << std::hex << std::setfill('0');
  if (width > 0) ss << std::setw(width);
  ss << v;
  return ss.str();
}

/// Shortest round-trippable-ish decimal of `v`, matching C `snprintf("%g", v)`
/// under the C locale (default 6 significant digits, trailing zeros trimmed,
/// exponent form for very large/small magnitudes). A type-safe replacement for
/// the `char buf[32]; snprintf(buf, "%g", v)` idiom. Verified byte-identical to
/// `%g` over a wide value sweep. The classic locale is imbued so the decimal
/// point is always '.', regardless of the process global locale.
inline std::string g(double v) {
  std::ostringstream ss;
  ss.imbue(std::locale::classic());
  ss << v;
  return ss.str();
}

namespace detail {

template <class T>
std::ostringstream &format_sv_rec(
    std::ostringstream &ss LIGHTUSD_LIFETIMEBOUND,
                                  const std::vector<std::string> &sv,
                                  size_t idx, T const &v) {
  if (idx >= sv.size()) {
    return ss;
  }

  // Print remaininig items
  bool fmt_printed{false};

  for (size_t i = idx; i < sv.size(); i++) {
    if (sv[i] == "{}") {
      if (fmt_printed) {
        ss << sv[i];
      } else {
        ss << v;
        fmt_printed = true;
      }
    } else {
      ss << sv[i];
    }
  }

  return ss;
}

template <class T, class... Rest>
std::ostringstream &format_sv_rec(
    std::ostringstream &ss LIGHTUSD_LIFETIMEBOUND,
                                  const std::vector<std::string> &sv,
                                  size_t idx, T const &v, Rest const &...args) {
  if (idx >= sv.size()) {
    return ss;
  }

  if (sv[idx] == "{}") {
    ss << v;
    format_sv_rec(ss, sv, idx + 1, args...);
  } else {
    ss << sv[idx];
    format_sv_rec(ss, sv, idx + 1, v, args...);
  }

  return ss;
}

template <class... Args>
std::ostringstream &format_sv(std::ostringstream &ss LIGHTUSD_LIFETIMEBOUND,
                              const std::vector<std::string> &sv,
                              Args const &...args) {
  format_sv_rec(ss, sv, 0, args...);

  return ss;
}

std::ostringstream &format_sv(std::ostringstream &ss LIGHTUSD_LIFETIMEBOUND,
                              const std::vector<std::string> &sv);

nonstd::expected<std::vector<std::string>, std::string> tokenize(
    const std::string &s);

}  // namespace detail

template <class... Args>
std::string format(const std::string &in, Args const &...args) {
  auto ret = detail::tokenize(in);
  if (!ret) {
    return in + "(format error: " + ret.error() + ")";
  }

  std::ostringstream ss;
  detail::format_sv(ss, (*ret), args...);

  return ss.str();
}

std::string format(const std::string &in);

}  // namespace fmt
}  // namespace lightusd
