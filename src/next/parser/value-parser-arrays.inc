// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - array value parsing helpers and optimized bracket-literal parser.
// Included inside namespace lightusd::next.

// Helper to check if type stores float3-like data
bool IsFloat3Like(TypeId type) {
  return type == TypeId::Float3 || type == TypeId::Half3 ||
         type == TypeId::Point3h || type == TypeId::Point3f ||
         type == TypeId::Vector3h || type == TypeId::Vector3f ||
         type == TypeId::Normal3h || type == TypeId::Normal3f ||
         type == TypeId::Color3h || type == TypeId::Color3f ||
         type == TypeId::Texcoord3h || type == TypeId::Texcoord3f;
}
// Helper to check if type stores float2-like data
bool IsFloat2Like(TypeId type) {
  return type == TypeId::Float2 || type == TypeId::Half2 ||
         type == TypeId::Texcoord2h || type == TypeId::Texcoord2f;
}

// Helper to check if type stores float4-like data
bool IsFloat4Like(TypeId type) {
  return type == TypeId::Float4 || type == TypeId::Half4 ||
         type == TypeId::Quath || type == TypeId::Quatf ||
         type == TypeId::Color4h || type == TypeId::Color4f;
}

bool IsHalfLike(TypeId type) {
  return type == TypeId::Half || type == TypeId::Half2 ||
         type == TypeId::Half3 || type == TypeId::Half4 ||
         type == TypeId::Point3h || type == TypeId::Vector3h ||
         type == TypeId::Normal3h || type == TypeId::Color3h ||
         type == TypeId::Color4h || type == TypeId::Texcoord2h ||
         type == TypeId::Texcoord3h || type == TypeId::Quath;
}

void QuantizeHalfValues(std::vector<float>* values) {
  if (!values) return;
  for (float& value : *values) value = HalfToFloat(FloatToHalf(value));
}

bool IsDouble2Like(TypeId type) {
  return type == TypeId::Double2 || type == TypeId::Texcoord2d;
}

bool IsDouble3Like(TypeId type) {
  return type == TypeId::Double3 || type == TypeId::Point3d ||
         type == TypeId::Vector3d || type == TypeId::Normal3d ||
         type == TypeId::Color3d || type == TypeId::Texcoord3d;
}

bool IsDouble4Like(TypeId type) {
  return type == TypeId::Double4 || type == TypeId::Quatd ||
         type == TypeId::Color4d;
}

bool SliceIsIdentifierStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool SliceIsIdentifierContinue(char c) {
  return SliceIsIdentifierStart(c) || (c >= '0' && c <= '9');
}

struct SliceParser {
  const char* p{nullptr};
  const char* end{nullptr};

  void skip_ws() {
    while (p < end) {
      if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
      } else if (*p == '#') {
        while (p < end && *p != '\n') ++p;
      } else {
        break;
      }
    }
  }

  bool consume(char c) {
    skip_ws();
    if (p >= end || *p != c) return false;
    ++p;
    return true;
  }

  bool maybe_consume(char c) {
    skip_ws();
    if (p < end && *p == c) {
      ++p;
      return true;
    }
    return false;
  }

  bool at_array_end() {
    skip_ws();
    return p < end && *p == ']';
  }

  bool finish_array() {
    if (!consume(']')) return false;
    skip_ws();
    return p == end;
  }

  bool parse_float(float* out) {
    skip_ws();
    auto r = fast_float::from_chars(p, end, *out);
    if (r.ec == std::errc{} && r.ptr != p) { p = r.ptr; return true; }
    // Accept a leading '+' (fast_float's `general` format rejects it), to match
    // the scalar FastFloatParse path. The writer never emits one, so this only widens
    // accepted hand-authored input -- byte-identical on all oracles.
    if (p < end && *p == '+') {
      r = fast_float::from_chars(p + 1, end, *out);
      if (r.ec == std::errc{} && r.ptr != p + 1) { p = r.ptr; return true; }
    }
    return false;
  }

  bool parse_double(double* out) {
    skip_ws();
    auto r = fast_float::from_chars(p, end, *out);
    if (r.ec == std::errc{} && r.ptr != p) { p = r.ptr; return true; }
    if (p < end && *p == '+') {
      r = fast_float::from_chars(p + 1, end, *out);
      if (r.ec == std::errc{} && r.ptr != p + 1) { p = r.ptr; return true; }
    }
    return false;
  }

  // Decimal `[+-]?digits` integer parse with in-loop overflow detection. USD
  // integer literals are decimal; this is our own atoi (modeled on
  // src/tiny-string.cc str::parse_int64) so the parser depends on neither libc
  // strtoll (locale-aware, base-0 octal/hex, and the hottest symbol on mesh
  // scenes) nor std stdio — important for WASM/WASI. Overflow returns false,
  // matching strtoll's ERANGE rejection.
  bool parse_i64(int64_t* out) {
    skip_ws();
    const char* q = p;
    if (q >= end) return false;
    bool neg = false;
    if (*q == '+' || *q == '-') { neg = (*q == '-'); ++q; }
    if (q >= end || *q < '0' || *q > '9') {
      // Possibly a bare-fraction float literal (`.5`): pxr coerces float
      // literals to integer types by truncating toward zero.
      if (q < end && *q == '.') return coerce_float_to_i64(out);
      return false;
    }
    // Largest magnitude: INT64_MAX, or INT64_MAX+1 for a negative value.
    const uint64_t limit = neg ? (static_cast<uint64_t>(INT64_MAX) + 1u)
                               : static_cast<uint64_t>(INT64_MAX);
    uint64_t val = 0;
    while (q < end && *q >= '0' && *q <= '9') {
      const uint64_t d = static_cast<uint64_t>(*q - '0');
      if (val > (limit - d) / 10u) return false;  // would overflow
      val = val * 10u + d;
      ++q;
    }
    if (q < end && (*q == '.' || *q == 'e' || *q == 'E')) {
      // Float literal (`1.5`, `3e2`): pxr coerces to integer types with
      // truncation toward zero; out of range after truncation is an error.
      return coerce_float_to_i64(out);
    }
    // 0u - val gives the two's-complement negation, correct even at INT64_MIN.
    *out = neg ? static_cast<int64_t>(0u - val) : static_cast<int64_t>(val);
    p = q;
    return true;
  }

  // Re-parse from the current position as a double and truncate toward zero
  // (pxr float->int coercion). Callers narrow to their element range after.
  bool coerce_float_to_i64(int64_t* out) {
    double v = 0.0;
    if (!parse_double(&v)) return false;
    const double d = std::trunc(v);
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
      return false;  // NaN/inf land here too
    }
    *out = static_cast<int64_t>(d);
    return true;
  }

  bool coerce_float_to_u64(uint64_t* out) {
    double v = 0.0;
    if (!parse_double(&v)) return false;
    const double d = std::trunc(v);
    if (!(d >= 0.0 && d < 18446744073709551616.0)) return false;
    *out = static_cast<uint64_t>(d);
    return true;
  }

  bool parse_u64(uint64_t* out) {
    skip_ws();
    const char* q = p;
    if (q >= end) return false;
    if (*q == '-') {
      // pxr admits negative float literals that truncate INTO range
      // (`uint u = -0.5` -> 0); a bare negative integer stays an error.
      return coerce_float_to_u64(out);
    }
    if (*q == '+') ++q;
    if (q >= end || *q < '0' || *q > '9') {
      if (q < end && *q == '.') return coerce_float_to_u64(out);
      return false;
    }
    uint64_t val = 0;
    while (q < end && *q >= '0' && *q <= '9') {
      const uint64_t d = static_cast<uint64_t>(*q - '0');
      if (val > (UINT64_MAX - d) / 10u) return false;  // would overflow
      val = val * 10u + d;
      ++q;
    }
    if (q < end && (*q == '.' || *q == 'e' || *q == 'E')) {
      return coerce_float_to_u64(out);
    }
    *out = val;
    p = q;
    return true;
  }

  bool parse_bool(bool* out) {
    skip_ws();
    if (p + 4 <= end && std::memcmp(p, "true", 4) == 0 &&
        (p + 4 == end || !SliceIsIdentifierContinue(*(p + 4)))) {
      p += 4;
      *out = true;
      return true;
    }
    if (p + 5 <= end && std::memcmp(p, "false", 5) == 0 &&
        (p + 5 == end || !SliceIsIdentifierContinue(*(p + 5)))) {
      p += 5;
      *out = false;
      return true;
    }
    // pxr accepts ONLY the integer literals 0 and 1 for bool (no numeric
    // truthiness). Match the scalar ParseBool: a bare '0'/'1' digit not
    // followed by more number characters.
    if (p < end && (*p == '0' || *p == '1')) {
      const char* q = p + 1;
      const bool more_number =
          q < end && ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' ||
                      *q == 'E');
      if (!more_number) {
        *out = (*p == '1');
        p = q;
        return true;
      }
    }
    return false;
  }

  bool parse_string(std::string* out) {
    skip_ws();
    if (p >= end || (*p != '"' && *p != '\'')) return false;
    const char quote = *p;
    ++p;
    bool triple = false;
    if (p + 1 < end && p[0] == quote && p[1] == quote) {
      triple = true;
      p += 2;
    }
    out->clear();
    while (p < end) {
      char c = *p++;
      if (triple) {
        if (c == quote && p + 1 < end && p[0] == quote && p[1] == quote) {
          p += 2;
          return true;
        }
      } else if (c == quote) {
        return true;
      }
      if (c == '\\' && p < end) {
        char esc = *p++;
        auto hexval = [](char h) -> int {
          if (h >= '0' && h <= '9') return h - '0';
          if (h >= 'a' && h <= 'f') return h - 'a' + 10;
          if (h >= 'A' && h <= 'F') return h - 'A' + 10;
          return -1;
        };
        switch (esc) {
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'a': out->push_back('\a'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'v': out->push_back('\v'); break;
          case '"': out->push_back('"'); break;
          case '\'': out->push_back('\''); break;
          case '\\': out->push_back('\\'); break;
          case 'x': {
            // \xNN — up to two hex digits (matches the scalar lexer decode)
            int v = 0, n = 0;
            while (n < 2 && p < end && hexval(*p) >= 0) {
              v = v * 16 + hexval(*p);
              ++p;
              ++n;
            }
            if (n == 0) { out->push_back('\\'); out->push_back('x'); }
            else out->push_back(static_cast<char>(v));
            break;
          }
          case '0': case '1': case '2': case '3':
          case '4': case '5': case '6': case '7': {
            // Octal \NNN — first digit is `esc`, up to two more
            int v = esc - '0', n = 1;
            while (n < 3 && p < end && *p >= '0' && *p <= '7') {
              v = v * 8 + (*p - '0');
              ++p;
              ++n;
            }
            out->push_back(static_cast<char>(v & 0xff));
            break;
          }
          case 'u':
          case 'U': {
            // \uXXXX / \UXXXXXXXX -> UTF-8 (matches the scalar lexer decode)
            const int want = (esc == 'u') ? 4 : 8;
            uint32_t cp = 0;
            int n = 0;
            while (n < want && p < end && hexval(*p) >= 0) {
              cp = cp * 16 + static_cast<uint32_t>(hexval(*p));
              ++p;
              ++n;
            }
            if (n != want) {
              out->push_back('\\');
              out->push_back(esc);
            } else if (cp < 0x80) {
              out->push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
              out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
              out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
              out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
              out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
              out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
          }
          default: out->push_back(esc); break;
        }
      } else {
        out->push_back(c);
      }
    }
    return false;
  }

  bool parse_identifier(std::string* out) {
    skip_ws();
    if (p >= end || !SliceIsIdentifierStart(*p)) return false;
    const char* start = p;
    ++p;
    while (p < end && SliceIsIdentifierContinue(*p)) ++p;
    out->assign(start, size_t(p - start));
    return true;
  }

  bool parse_token(std::string* out) {
    return parse_string(out) || parse_identifier(out);
  }
};

// Defense-in-depth ceiling on the number of scalars accumulated for one array
// (parity with the mature ASCII parser's kMaxArrayElements). USDA arrays are
// otherwise only input-length-bounded; this caps a single pathological array so
// it can't drive a multi-GB vector even from a large/streamed input.
constexpr size_t kMaxArrayScalars = size_t(1) << 30;  // ~1.07e9 scalars

template <class T, class ParseOne>
bool ParseScalarArray(SliceParser* sp, std::vector<T>* out, ParseOne parse_one) {
  if (!sp->consume('[')) return false;
  out->clear();
  if (sp->at_array_end()) return sp->finish_array();
  while (true) {
    T v{};
    if (!parse_one(sp, &v)) return false;
    out->push_back(v);
    if (out->size() > kMaxArrayScalars) return false;
    if (sp->maybe_consume(',')) {
      // Trailing comma before ']' is valid USDA ([0, 1, 2,]).
      if (sp->at_array_end()) return sp->finish_array();
      continue;
    }
    return sp->finish_array();
  }
}

template <class ScalarT, uint32_t N, class ParseOne>
bool ParseTupleArray(SliceParser* sp, std::vector<ScalarT>* out,
                     ParseOne parse_one) {
  if (!sp->consume('[')) return false;
  out->clear();
  if (sp->at_array_end()) return sp->finish_array();
  while (true) {
    if (!sp->consume('(')) return false;
    for (uint32_t i = 0; i < N; i++) {
      ScalarT v{};
      if (!parse_one(sp, &v)) return false;
      out->push_back(v);
      if (i + 1 < N && !sp->consume(',')) return false;
    }
    if (!sp->consume(')')) return false;
    if (out->size() > kMaxArrayScalars) return false;
    if (sp->maybe_consume(',')) {
      if (sp->at_array_end()) return sp->finish_array();  // trailing comma
      continue;
    }
    return sp->finish_array();
  }
}

template <class ScalarT, uint32_t N, class ParseOne>
bool ParseMatrixArray(SliceParser* sp, std::vector<ScalarT>* out,
                      ParseOne parse_one) {
  if (!sp->consume('[')) return false;
  out->clear();
  if (sp->at_array_end()) return sp->finish_array();
  while (true) {
    if (!sp->consume('(')) return false;
    for (uint32_t r = 0; r < N; r++) {
      if (!sp->consume('(')) return false;
      for (uint32_t c = 0; c < N; c++) {
        ScalarT v{};
        if (!parse_one(sp, &v)) return false;
        out->push_back(v);
        if (c + 1 < N && !sp->consume(',')) return false;
      }
      if (!sp->consume(')')) return false;
      if (r + 1 < N && !sp->consume(',')) return false;
    }
    if (!sp->consume(')')) return false;
    if (out->size() > kMaxArrayScalars) return false;
    if (sp->maybe_consume(',')) {
      if (sp->at_array_end()) return sp->finish_array();  // trailing comma
      continue;
    }
    return sp->finish_array();
  }
}

// ============================================================
// Parallel numeric array parse (large "simple" arrays)
// ============================================================
//
// Distributes the per-element fast_float work across worker threads for big
// numeric arrays. Each worker parses a byte sub-range that contains WHOLE
// elements (boundaries snapped to a top-level separator comma), into a private
// vector; the main thread concatenates them in order. The result is byte-
// identical to the serial parse and independent of the thread count: every token
// is produced by the same fast_float, and element order is preserved. Only used
// when the captured array is "simple" (no comments/strings/asset-refs/nested
// brackets), so every comma/paren is a pure separator. On any worker failure
// (a boundary edge case), the caller falls back to a serial parse — so the
// parallel path can only ever be an optimization, never a correctness risk.
// Min array text size to bother spawning threads, and min bytes per worker chunk.
// Conservative: only clearly-large arrays parallelize (the gain is memory-bandwidth
// bound, so a lower threshold only adds thread-spawn overhead on medium arrays for
// no extra throughput).
constexpr size_t kParallelArrayMinBytes = size_t(4) << 20;  // 4 MiB
constexpr size_t kParallelChunkMinBytes = size_t(1) << 20;  // 1 MiB

// Parse one byte range [b,e) of comma-separated elements (N scalars each; N>1 are
// paren-wrapped vector tuples) into `local`. Returns false on any malformed token.
// A non-final chunk ends right after a boundary separator comma (expected); the
// final chunk (`is_last`) ends at the ']' and must reject a trailing comma, to
// stay byte-for-byte equivalent to the serial parse on malformed input.
template <class T, uint32_t N, class ParseOne>
bool ParseChunkFlat(const char* b, const char* e, bool is_last,
                    std::vector<T>* local, ParseOne parse_one) {
  (void)is_last;  // trailing comma before ']' is valid on any chunk
  SliceParser sp{b, e};
  // Inline whitespace skip. `simple` arrays (the only ones reaching here) contain
  // no `#` comments, so this matches SliceParser::skip_ws on this input. Hoisting
  // it out of parse_one/maybe_consume collapses the 3 skip_ws + the maybe_consume
  // call per element down to one ws scan per separator — the per-element overhead
  // dominates for short-token arrays (indices, 0/1 values) where each fast_float
  // call is tiny.
  const char* const end = sp.end;
  auto skipws = [&] {
    const char* p = sp.p;
    while (p < end) {
      const char c = *p;
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++p;
      else break;
    }
    sp.p = p;
  };
  skipws();
  if (sp.p >= end) return true;  // empty chunk
  while (true) {
    if (N == 1) {
      T v{};
      if (!parse_one(&sp, &v)) return false;  // parse_one's own skip_ws is a no-op here
      local->push_back(v);
    } else {
      if (sp.p >= end || *sp.p != '(') return false;
      ++sp.p;
      for (uint32_t i = 0; i < N; i++) {
        T v{};
        if (!parse_one(&sp, &v)) return false;  // parse_one skips leading ws
        local->push_back(v);
        skipws();
        if (i + 1 < N) {
          if (sp.p >= end || *sp.p != ',') return false;
          ++sp.p;
        }
      }
      if (sp.p >= end || *sp.p != ')') return false;
      ++sp.p;
    }
    skipws();
    if (sp.p >= end) return true;     // last element of chunk, no trailing comma
    if (*sp.p != ',') return false;   // adjacent elements with no separator
    ++sp.p;                            // consume the separator comma
    skipws();
    if (sp.p >= end) {
      // Trailing separator: the normal inter-chunk boundary; on the FINAL
      // chunk it is a trailing comma before ']', which is valid USDA.
      return true;
    }
  }
}

// Snap a target offset forward to a top-level element-separator boundary, returning
// the position just AFTER the separator comma (or `end` if none remains).
template <uint32_t N>
const char* SnapElementBoundary(const char* t, const char* end) {
  if (N > 1) {
    // Vector tuple: the first ')' at/after t closes a tuple (vectors don't nest),
    // after which the next ',' is a top-level separator.
    const char* rp = static_cast<const char*>(std::memchr(t, ')', size_t(end - t)));
    if (!rp) return end;
    t = rp + 1;
  }
  const char* c = static_cast<const char*>(std::memchr(t, ',', size_t(end - t)));
  return c ? c + 1 : end;
}

// ---------------------------------------------------------------------------
// USDA lazy-array count helpers
// ---------------------------------------------------------------------------

// Forward declaration for lazy materialization path.
ParseResult ParseArrayValueFromSlice(const char* data, size_t len, bool simple,
                                    int nt_hint, TypeId element_type);

template <class T, class ParseOne>
bool CountScalarArray(const char* data, size_t len, size_t max_elements,
                     size_t* out_count,
                     ParseOne parse_one) {
  if (len < 2 || data[0] != '[' || data[len - 1] != ']') return false;
  SliceParser sp{data, data + len};
  size_t count = 0;
  if (!sp.consume('[')) return false;
  if (sp.at_array_end()) {
    if (out_count) *out_count = 0;
    return sp.finish_array();
  }

  while (true) {
    T v{};
    if (!parse_one(&sp, &v)) {
      return false;
    }
    ++count;
    if (count > max_elements) return false;
    if (sp.maybe_consume(',')) {
      continue;
    }
    return sp.finish_array() && (out_count != nullptr) &&
           (*out_count = count, true);
  }
}

template <class T, uint32_t N, class ParseOne>
bool CountTupleArray(const char* data, size_t len, size_t max_elements,
                     size_t* out_count, ParseOne parse_one) {
  if (len < 2 || data[0] != '[' || data[len - 1] != ']') return false;
  SliceParser sp{data, data + len};
  if (!sp.consume('[')) return false;
  if (sp.at_array_end()) {
    if (out_count) *out_count = 0;
    return sp.finish_array();
  }

  size_t count = 0;
  while (true) {
    if (!sp.consume('(')) return false;
    for (uint32_t i = 0; i < N; i++) {
      T v{};
      if (!parse_one(&sp, &v)) return false;
      if (i + 1 < N && !sp.consume(',')) return false;
    }
    if (!sp.consume(')')) return false;
    ++count;
    if (count > max_elements) return false;
    if (sp.maybe_consume(',')) {
      continue;
    }
    return sp.finish_array() && (out_count != nullptr) && (*out_count = count, true);
  }
}

template <class T, uint32_t N, class ParseOne>
bool CountMatrixArray(const char* data, size_t len, size_t max_elements,
                      size_t* out_count,
                      ParseOne parse_one) {
  if (len < 2 || data[0] != '[' || data[len - 1] != ']') return false;
  SliceParser sp{data, data + len};
  if (!sp.consume('[')) return false;
  if (sp.at_array_end()) {
    if (out_count) *out_count = 0;
    return sp.finish_array();
  }

  size_t count = 0;
  while (true) {
    if (!sp.consume('(')) return false;
    for (uint32_t r = 0; r < N; r++) {
      if (!sp.consume('(')) return false;
      for (uint32_t c = 0; c < N; c++) {
        T v{};
        if (!parse_one(&sp, &v)) return false;
        if (c + 1 < N && !sp.consume(',')) return false;
      }
      if (!sp.consume(')')) return false;
      if (r + 1 < N && !sp.consume(',')) return false;
    }
    if (!sp.consume(')')) return false;
    ++count;
    if (count > max_elements) return false;
    if (sp.maybe_consume(',')) {
      continue;
    }
    return sp.finish_array() && (out_count != nullptr) && (*out_count = count, true);
  }
}

bool ParseUsdaArrayElementCount(const char* data, size_t len,
                               size_t max_elements, TypeId element_type,
                               size_t* out_count) {
  if (!out_count) return false;
  if (max_elements == 0) {
    max_elements = std::numeric_limits<size_t>::max();
  }
  *out_count = 0;

  auto parse_f = [](SliceParser* s, float* v) { return s->parse_float(v); };
  auto parse_d = [](SliceParser* s, double* v) { return s->parse_double(v); };
  auto parse_b = [](SliceParser* s, bool* v) { return s->parse_bool(v); };
  auto parse_i32 = [](SliceParser* s, int32_t* v) {
    int64_t tmp = 0;
    if (!s->parse_i64(&tmp) || tmp < std::numeric_limits<int32_t>::min() ||
        tmp > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    *v = static_cast<int32_t>(tmp);
    return true;
  };
  auto parse_u32 = [](SliceParser* s, uint32_t* v) {
    uint64_t tmp = 0;
    if (!s->parse_u64(&tmp) || tmp > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    *v = static_cast<uint32_t>(tmp);
    return true;
  };
  auto parse_i64 = [](SliceParser* s, int64_t* v) { return s->parse_i64(v); };
  auto parse_u64 = [](SliceParser* s, uint64_t* v) { return s->parse_u64(v); };

  switch (element_type) {
    case TypeId::Float:
    case TypeId::Half:
    case TypeId::TimeCode:
      return CountScalarArray<float>(data, len, max_elements, out_count,
                                    parse_f);
    case TypeId::Double:
      return CountScalarArray<double>(data, len, max_elements, out_count,
                                     parse_d);
    case TypeId::Bool:
      return CountScalarArray<bool>(data, len, max_elements, out_count,
                                   parse_b);
    case TypeId::Int:
      return CountScalarArray<int32_t>(data, len, max_elements, out_count,
                                      parse_i32);
    case TypeId::UInt:
      return CountScalarArray<uint32_t>(data, len, max_elements, out_count,
                                       parse_u32);
    case TypeId::Int64:
      return CountScalarArray<int64_t>(data, len, max_elements, out_count,
                                      parse_i64);
    case TypeId::UInt64:
      return CountScalarArray<uint64_t>(data, len, max_elements, out_count,
                                       parse_u64);

    case TypeId::Int2:
    case TypeId::Int3:
    case TypeId::Int4:
      if (element_type == TypeId::Int2) {
        return CountTupleArray<int32_t, 2>(data, len, max_elements, out_count,
                                          parse_i32);
      } else if (element_type == TypeId::Int3) {
        return CountTupleArray<int32_t, 3>(data, len, max_elements, out_count,
                                          parse_i32);
      }
      return CountTupleArray<int32_t, 4>(data, len, max_elements, out_count,
                                        parse_i32);
    case TypeId::UInt2:
    case TypeId::UInt3:
    case TypeId::UInt4:
      if (element_type == TypeId::UInt2) {
        return CountTupleArray<uint32_t, 2>(data, len, max_elements, out_count,
                                           parse_u32);
      } else if (element_type == TypeId::UInt3) {
        return CountTupleArray<uint32_t, 3>(data, len, max_elements, out_count,
                                           parse_u32);
      }
      return CountTupleArray<uint32_t, 4>(data, len, max_elements, out_count,
                                         parse_u32);

    case TypeId::Float2:
    case TypeId::Float3:
    case TypeId::Float4:
    case TypeId::Half2:
    case TypeId::Half3:
    case TypeId::Half4:
    case TypeId::Point3h:
    case TypeId::Point3f:
    case TypeId::Vector3h:
    case TypeId::Vector3f:
    case TypeId::Normal3h:
    case TypeId::Normal3f:
    case TypeId::Color3h:
    case TypeId::Color3f:
    case TypeId::Texcoord2h:
    case TypeId::Texcoord2f:
    case TypeId::Texcoord3h:
    case TypeId::Texcoord3f:
    case TypeId::Quath:
    case TypeId::Quatf:
    case TypeId::Color4h:
    case TypeId::Color4f:
      if (IsFloat2Like(element_type)) {
        return CountTupleArray<float, 2>(data, len, max_elements, out_count,
                                        parse_f);
      }
      if (IsFloat3Like(element_type)) {
        return CountTupleArray<float, 3>(data, len, max_elements, out_count,
                                        parse_f);
      }
      return CountTupleArray<float, 4>(data, len, max_elements, out_count,
                                      parse_f);

    case TypeId::Double2:
    case TypeId::Double3:
    case TypeId::Double4:
    case TypeId::Point3d:
    case TypeId::Vector3d:
    case TypeId::Normal3d:
    case TypeId::Color3d:
    case TypeId::Texcoord2d:
    case TypeId::Texcoord3d:
    case TypeId::Quatd:
      if (IsDouble2Like(element_type)) {
        return CountTupleArray<double, 2>(data, len, max_elements, out_count,
                                         parse_d);
      }
      if (IsDouble3Like(element_type)) {
        return CountTupleArray<double, 3>(data, len, max_elements, out_count,
                                         parse_d);
      }
      return CountTupleArray<double, 4>(data, len, max_elements, out_count,
                                        parse_d);

    case TypeId::Matrix2f:
      return CountMatrixArray<float, 2>(data, len, max_elements, out_count,
                                        parse_f);
    case TypeId::Matrix3f:
      return CountMatrixArray<float, 3>(data, len, max_elements, out_count,
                                        parse_f);
    case TypeId::Matrix4f:
      return CountMatrixArray<float, 4>(data, len, max_elements, out_count,
                                        parse_f);
    case TypeId::Matrix2d:
      return CountMatrixArray<double, 2>(data, len, max_elements, out_count,
                                         parse_d);
    case TypeId::Matrix3d:
      return CountMatrixArray<double, 3>(data, len, max_elements, out_count,
                                         parse_d);
    case TypeId::Matrix4d:
      return CountMatrixArray<double, 4>(data, len, max_elements, out_count,
                                         parse_d);
    default:
      return false;
  }
}

#if defined(LIGHTUSD_ENABLE_THREAD)

// Resolve the worker count from a ParseOptions::num_threads hint (forwarded via
// Lexer::num_threads): 1 = serial, >1 = that many, <=0 = auto (min(hw, 8)). No
// process-environment input -- the caller passes the hint explicitly.
static int ResolveParseThreads(int requested) {
  if (requested == 1) return 1;
  if (requested > 1) return requested;
  int hw = static_cast<int>(std::thread::hardware_concurrency());
  if (hw < 1) hw = 1;
  return std::min(hw, 8);
}

// Returns true and fills *out on success; false => caller does a serial fallback.
template <class T, uint32_t N, class ParseOne>
bool ParseNumericArrayParallel(const char* data, size_t len, int nt_hint,
                               std::vector<T>* out, ParseOne parse_one) {
  // Captured span includes the outer '[' .. ']'.
  if (len < 2 || data[0] != '[' || data[len - 1] != ']') return false;
  const char* content_begin = data + 1;
  const char* content_end = data + len - 1;
  const size_t content_len = size_t(content_end - content_begin);
  if (content_len < kParallelArrayMinBytes) return false;

  const int nt = ResolveParseThreads(nt_hint);
  if (nt <= 1) return false;
  size_t nchunks = std::min<size_t>(size_t(nt), content_len / kParallelChunkMinBytes);
  if (nchunks < 2) return false;

  std::vector<const char*> cut(nchunks + 1);
  cut[0] = content_begin;
  cut[nchunks] = content_end;
  for (size_t k = 1; k < nchunks; k++) {
    const char* target = content_begin + (content_len * k) / nchunks;
    const char* c = SnapElementBoundary<N>(target, content_end);
    if (c < cut[k - 1]) c = cut[k - 1];  // keep monotonic
    cut[k] = c;
  }

  std::vector<std::vector<T>> locals(nchunks);
  std::vector<char> ok(nchunks, 1);
  std::vector<std::thread> threads;
  threads.reserve(nchunks);
  for (size_t k = 0; k < nchunks; k++) {
    const char* b = cut[k];
    const char* e = cut[k + 1];
    if (b >= e) continue;  // empty chunk
    const bool is_last = (e == content_end);
    threads.emplace_back([b, e, is_last, k, &locals, &ok, parse_one]() {
      ok[k] = ParseChunkFlat<T, N>(b, e, is_last, &locals[k], parse_one) ? 1 : 0;
    });
  }
  for (auto& th : threads) th.join();
  for (char c : ok) {
    if (!c) return false;  // boundary edge -> serial fallback
  }

  size_t total = 0;
  for (const auto& l : locals) total += l.size();
  out->clear();
  out->reserve(total);
  for (auto& l : locals) out->insert(out->end(), l.begin(), l.end());
  return true;
}

#endif  // LIGHTUSD_ENABLE_THREAD

// Parse a scalar numeric array, using the parallel path for large simple arrays
// and falling back to the serial SliceParser otherwise.
// Serial parse of a whole simple array span ([ .. ]) through the tight
// ParseChunkFlat inner loop — the same low-per-element-overhead path the parallel
// workers use, so sub-threshold (and threads-off) arrays get the same speedup.
template <class T, uint32_t N, class ParseOne>
bool ParseSimpleArraySerial(const char* data, size_t len, std::vector<T>* out,
                            ParseOne parse_one) {
  if (len < 2 || data[0] != '[' || data[len - 1] != ']') return false;
  out->clear();
  return ParseChunkFlat<T, N>(data + 1, data + len - 1, /*is_last=*/true, out,
                              parse_one);
}

template <class T, class ParseOne>
bool ParseScalarArrayMaybeParallel(const char* data, size_t len, bool simple,
                                   int nt_hint, std::vector<T>* out,
                                   ParseOne parse_one) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (simple &&
      ParseNumericArrayParallel<T, 1>(data, len, nt_hint, out, parse_one)) {
    return true;
  }
#else
  (void)nt_hint;
#endif
  if (simple && ParseSimpleArraySerial<T, 1>(data, len, out, parse_one)) {
    return true;
  }
  SliceParser sp{data, data + len};
  return ParseScalarArray<T>(&sp, out, parse_one);
}

// Same, for paren-wrapped single-level vector tuples (float3, double2, ...).
template <class T, uint32_t N, class ParseOne>
bool ParseTupleArrayMaybeParallel(const char* data, size_t len, bool simple,
                                  int nt_hint, std::vector<T>* out,
                                  ParseOne parse_one) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (simple &&
      ParseNumericArrayParallel<T, N>(data, len, nt_hint, out, parse_one)) {
    return true;
  }
#else
  (void)nt_hint;
#endif
  if (simple && ParseSimpleArraySerial<T, N>(data, len, out, parse_one)) {
    return true;
  }
  SliceParser sp{data, data + len};
  return ParseTupleArray<T, N>(&sp, out, parse_one);
}

ParseResult ParseArrayValueFromSlice(const char* data, size_t len, bool simple,
                                    int nt_hint, TypeId element_type) {
  SliceParser sp{data, data + len};

  auto parse_float = [](SliceParser* s, float* v) { return s->parse_float(v); };
  auto parse_double = [](SliceParser* s, double* v) { return s->parse_double(v); };

  if (element_type == TypeId::Float) {
    std::vector<float> values;
    if (!ParseScalarArrayMaybeParallel<float>(data, len, simple, nt_hint,
                                             &values, parse_float)) {
      return ParseResult::Error("Failed to parse float array");
    }
    return ParseResult::Ok(Value::MakeFloatArray(std::move(values)));
  }
  if (element_type == TypeId::Double) {
    std::vector<double> values;
    if (!ParseScalarArrayMaybeParallel<double>(data, len, simple, nt_hint,
                                              &values, parse_double)) {
      return ParseResult::Error("Failed to parse double array");
    }
    return ParseResult::Ok(Value::MakeDoubleArray(std::move(values)));
  }
  if (element_type == TypeId::Half) {
    std::vector<float> values;
    if (!ParseScalarArrayMaybeParallel<float>(data, len, simple, nt_hint,
                                             &values, parse_float)) {
      return ParseResult::Error("Failed to parse half array");
    }
    QuantizeHalfValues(&values);
    return ParseResult::Ok(Value::MakeFloatCompArray(std::move(values), element_type, 1));
  }
  if (element_type == TypeId::TimeCode) {
    std::vector<double> values;
    if (!ParseScalarArrayMaybeParallel<double>(data, len, simple, nt_hint,
                                              &values, parse_double)) {
      return ParseResult::Error("Failed to parse timecode array");
    }
    return ParseResult::Ok(Value::MakeDoubleCompArray(std::move(values), element_type, 1));
  }
  if (element_type == TypeId::Bool) {
    std::vector<bool> values;
    auto parse_bool = [](SliceParser* s, bool* v) { return s->parse_bool(v); };
    if (!ParseScalarArray<bool>(&sp, &values, parse_bool)) {
      return ParseResult::Error("Failed to parse bool array");
    }
    return ParseResult::Ok(Value::MakeBoolArray(values));
  }
  if (element_type == TypeId::Int) {
    std::vector<int32_t> values;
    auto parse_i32 = [](SliceParser* s, int32_t* v) {
      int64_t tmp = 0;
      if (!s->parse_i64(&tmp) || tmp < std::numeric_limits<int32_t>::min() ||
          tmp > std::numeric_limits<int32_t>::max()) return false;
      *v = static_cast<int32_t>(tmp);
      return true;
    };
    if (!ParseScalarArrayMaybeParallel<int32_t>(data, len, simple, nt_hint,
                                               &values, parse_i32)) {
      return ParseResult::Error("Failed to parse int array");
    }
    return ParseResult::Ok(Value::MakeIntArray(std::move(values)));
  }
  if (element_type == TypeId::UInt) {
    std::vector<uint32_t> values;
    auto parse_u32 = [](SliceParser* s, uint32_t* v) {
      uint64_t tmp = 0;
      if (!s->parse_u64(&tmp) || tmp > std::numeric_limits<uint32_t>::max()) return false;
      *v = static_cast<uint32_t>(tmp);
      return true;
    };
    if (!ParseScalarArrayMaybeParallel<uint32_t>(data, len, simple, nt_hint,
                                                &values, parse_u32)) {
      return ParseResult::Error("Failed to parse uint array");
    }
    return ParseResult::Ok(Value::MakeUIntArray(std::move(values)));
  }
  if (element_type == TypeId::UChar) {
    // uchar[]: widened into the uint32 array storage (element type UChar).
    std::vector<uint32_t> values;
    auto parse_u8 = [](SliceParser* s, uint32_t* v) {
      uint64_t tmp = 0;
      if (!s->parse_u64(&tmp) || tmp > 255) return false;
      *v = static_cast<uint32_t>(tmp);
      return true;
    };
    if (!ParseScalarArrayMaybeParallel<uint32_t>(data, len, simple, nt_hint,
                                                &values, parse_u8)) {
      return ParseResult::Error("Failed to parse uchar array");
    }
    return ParseResult::Ok(
        Value::MakeUIntCompArray(std::move(values), TypeId::UChar, 1));
  }
  if (element_type == TypeId::Int64) {
    std::vector<int64_t> values;
    auto parse_i64 = [](SliceParser* s, int64_t* v) { return s->parse_i64(v); };
    if (!ParseScalarArrayMaybeParallel<int64_t>(data, len, simple, nt_hint,
                                               &values, parse_i64)) {
      return ParseResult::Error("Failed to parse int64 array");
    }
    return ParseResult::Ok(Value::MakeInt64Array(std::move(values)));
  }
  if (element_type == TypeId::UInt64) {
    std::vector<uint64_t> values;
    auto parse_u64 = [](SliceParser* s, uint64_t* v) { return s->parse_u64(v); };
    if (!ParseScalarArrayMaybeParallel<uint64_t>(data, len, simple, nt_hint,
                                               &values, parse_u64)) {
      return ParseResult::Error("Failed to parse uint64 array");
    }
    return ParseResult::Ok(Value::MakeUInt64Array(std::move(values)));
  }
  if (element_type == TypeId::Token || element_type == TypeId::String ||
      element_type == TypeId::PathExpression) {
    // pathExpression elements are quoted strings, like string[].
    std::vector<std::string> values;
    auto parse_stringish = [](SliceParser* s, std::string* v) {
      return s->parse_token(v);
    };
    auto parse_string = [](SliceParser* s, std::string* v) {
      return s->parse_string(v);
    };
    // Decay both captureless lambdas to the same function-pointer type before
    // the conditional operator. GCC/Clang form this common type implicitly, but
    // MSVC does not (C2446: "no conversion from <lambda> to <lambda>").
    using ParseStringFn = bool (*)(SliceParser*, std::string*);
    if (!ParseScalarArray<std::string>(&sp, &values,
                                       element_type == TypeId::Token
                                           ? static_cast<ParseStringFn>(parse_stringish)
                                           : static_cast<ParseStringFn>(parse_string))) {
      return ParseResult::Error("Failed to parse string/token array");
    }
    return ParseResult::Ok(Value::MakeStringLikeArray(std::move(values), element_type));
  }
  if (element_type == TypeId::AssetPath) {
    // Asset path array: @path@ literals.
    auto parse_asset = [](SliceParser* s, std::string* v) {
      s->skip_ws();
      if (s->p >= s->end) return false;
      // pxr also accepts quoted STRING literals as asset paths ('a.jpg',
      // "b.png", """c.exr"""), the same acceptance as the scalar path (the
      // lexer turns both spellings into a String token there).
      if (*s->p == '"' || *s->p == '\'') return s->parse_string(v);
      if (*s->p != '@') return false;
      // Triple-@ delimited form (path contains '@'): @@@path@@@ with \@@@
      // escaping inside.
      const bool triple =
          (s->end - s->p) >= 3 && s->p[1] == '@' && s->p[2] == '@';
      s->p += triple ? 3 : 1;
      v->clear();
      if (triple) {
        while (s->p < s->end) {
          if (*s->p == '\\' && (s->end - s->p) >= 4 && s->p[1] == '@' &&
              s->p[2] == '@' && s->p[3] == '@') {
            v->append("@@@");
            s->p += 4;
            continue;
          }
          if ((s->end - s->p) >= 3 && s->p[0] == '@' && s->p[1] == '@' &&
              s->p[2] == '@') {
            s->p += 3;
            return true;
          }
          v->push_back(*s->p++);
        }
        return false;
      }
      while (s->p < s->end && *s->p != '@') v->push_back(*s->p++);
      if (s->p >= s->end) return false;
      ++s->p;  // closing '@'
      return true;
    };
    std::vector<std::string> values;
    if (!ParseScalarArray<std::string>(&sp, &values, parse_asset)) {
      return ParseResult::Error("Failed to parse asset array");
    }
    return ParseResult::Ok(Value::MakeStringLikeArray(std::move(values), element_type));
  }

  // Int-vector tuple arrays (int2[]/int3[]/int4[]): flat int32 buffer.
  if (element_type == TypeId::Int2 || element_type == TypeId::Int3 ||
      element_type == TypeId::Int4) {
    auto parse_i32 = [](SliceParser* s, int32_t* v) {
      int64_t tmp = 0;
      if (!s->parse_i64(&tmp) || tmp < std::numeric_limits<int32_t>::min() ||
          tmp > std::numeric_limits<int32_t>::max()) return false;
      *v = static_cast<int32_t>(tmp);
      return true;
    };
    std::vector<int32_t> values;
    bool ok = false;
    if (element_type == TypeId::Int2) {
      ok = ParseTupleArrayMaybeParallel<int32_t, 2>(
          data, len, simple, nt_hint, &values, parse_i32);
    } else if (element_type == TypeId::Int3) {
      ok = ParseTupleArrayMaybeParallel<int32_t, 3>(
          data, len, simple, nt_hint, &values, parse_i32);
    } else {
      ok = ParseTupleArrayMaybeParallel<int32_t, 4>(
          data, len, simple, nt_hint, &values, parse_i32);
    }
    if (!ok) return ParseResult::Error("Failed to parse int-vector array");
    const uint32_t comps = (element_type == TypeId::Int2)   ? 2u
                           : (element_type == TypeId::Int3) ? 3u
                                                            : 4u;
    return ParseResult::Ok(Value::MakeIntCompArray(std::move(values), element_type, comps));
  }

  if (element_type == TypeId::UInt2 || element_type == TypeId::UInt3 ||
      element_type == TypeId::UInt4) {
    auto parse_u32 = [](SliceParser* s, uint32_t* v) {
      uint64_t tmp = 0;
      if (!s->parse_u64(&tmp) || tmp > std::numeric_limits<uint32_t>::max())
        return false;
      *v = static_cast<uint32_t>(tmp);
      return true;
    };
    std::vector<uint32_t> values;
    bool ok = false;
    if (element_type == TypeId::UInt2) {
      ok = ParseTupleArrayMaybeParallel<uint32_t, 2>(
          data, len, simple, nt_hint, &values, parse_u32);
    } else if (element_type == TypeId::UInt3) {
      ok = ParseTupleArrayMaybeParallel<uint32_t, 3>(
          data, len, simple, nt_hint, &values, parse_u32);
    } else {
      ok = ParseTupleArrayMaybeParallel<uint32_t, 4>(
          data, len, simple, nt_hint, &values, parse_u32);
    }
    if (!ok) return ParseResult::Error("Failed to parse uint-vector array");
    const uint32_t comps = (element_type == TypeId::UInt2)   ? 2u
                           : (element_type == TypeId::UInt3) ? 3u
                                                             : 4u;
    return ParseResult::Ok(Value::MakeUIntCompArray(std::move(values), element_type, comps));
  }

  if (IsFloat2Like(element_type)) {
    std::vector<float> values;
    if (!ParseTupleArrayMaybeParallel<float, 2>(data, len, simple, nt_hint,
                                               &values, parse_float)) {
      return ParseResult::Error("Failed to parse float2 array");
    }
    if (IsHalfLike(element_type)) QuantizeHalfValues(&values);
    return ParseResult::Ok(Value::MakeFloatCompArray(std::move(values), element_type, 2));
  }
  if (IsFloat3Like(element_type)) {
    std::vector<float> values;
    if (!ParseTupleArrayMaybeParallel<float, 3>(data, len, simple, nt_hint,
                                               &values, parse_float)) {
      return ParseResult::Error("Failed to parse float3 array");
    }
    if (IsHalfLike(element_type)) QuantizeHalfValues(&values);
    return ParseResult::Ok(Value::MakeFloatCompArray(std::move(values), element_type, 3));
  }
  if (IsFloat4Like(element_type)) {
    std::vector<float> values;
    if (!ParseTupleArrayMaybeParallel<float, 4>(data, len, simple, nt_hint,
                                               &values, parse_float)) {
      return ParseResult::Error("Failed to parse float4 array");
    }
    if (IsHalfLike(element_type)) QuantizeHalfValues(&values);
    return ParseResult::Ok(Value::MakeFloatCompArray(std::move(values), element_type, 4));
  }
  if (IsDouble2Like(element_type)) {
    std::vector<double> values;
    if (!ParseTupleArrayMaybeParallel<double, 2>(data, len, simple, nt_hint,
                                                &values, parse_double)) {
      return ParseResult::Error("Failed to parse double2 array");
    }
    return ParseResult::Ok(Value::MakeDoubleCompArray(std::move(values), element_type, 2));
  }
  if (IsDouble3Like(element_type)) {
    std::vector<double> values;
    if (!ParseTupleArrayMaybeParallel<double, 3>(data, len, simple, nt_hint,
                                                &values, parse_double)) {
      return ParseResult::Error("Failed to parse double3 array");
    }
    return ParseResult::Ok(Value::MakeDoubleCompArray(std::move(values), element_type, 3));
  }
  if (IsDouble4Like(element_type)) {
    std::vector<double> values;
    if (!ParseTupleArrayMaybeParallel<double, 4>(data, len, simple, nt_hint,
                                                &values, parse_double)) {
      return ParseResult::Error("Failed to parse double4 array");
    }
    return ParseResult::Ok(Value::MakeDoubleCompArray(std::move(values), element_type, 4));
  }
  if (element_type == TypeId::Matrix2f) {
    std::vector<float> values;
    if (!ParseMatrixArray<float, 2>(&sp, &values, parse_float)) {
      return ParseResult::Error("Failed to parse matrix2f array");
    }
    return ParseResult::Ok(Value::MakeFloatCompArray(std::move(values), element_type, 4));
  }
  if (element_type == TypeId::Matrix3f) {
    std::vector<float> values;
    if (!ParseMatrixArray<float, 3>(&sp, &values, parse_float)) {
      return ParseResult::Error("Failed to parse matrix3f array");
    }
    return ParseResult::Ok(Value::MakeFloatCompArray(std::move(values), element_type, 9));
  }
  if (element_type == TypeId::Matrix4f) {
    std::vector<float> values;
    if (!ParseMatrixArray<float, 4>(&sp, &values, parse_float)) {
      return ParseResult::Error("Failed to parse matrix4f array");
    }
    return ParseResult::Ok(Value::MakeFloatCompArray(std::move(values), element_type, 16));
  }
  if (element_type == TypeId::Matrix2d) {
    std::vector<double> values;
    if (!ParseMatrixArray<double, 2>(&sp, &values, parse_double)) {
      return ParseResult::Error("Failed to parse matrix2d array");
    }
    return ParseResult::Ok(Value::MakeDoubleCompArray(std::move(values), element_type, 4));
  }
  if (element_type == TypeId::Matrix3d) {
    std::vector<double> values;
    if (!ParseMatrixArray<double, 3>(&sp, &values, parse_double)) {
      return ParseResult::Error("Failed to parse matrix3d array");
    }
    return ParseResult::Ok(Value::MakeDoubleCompArray(std::move(values), element_type, 9));
  }
  if (element_type == TypeId::Matrix4d) {
    std::vector<double> values;
    if (!ParseMatrixArray<double, 4>(&sp, &values, parse_double)) {
      return ParseResult::Error("Failed to parse matrix4d array");
    }
    return ParseResult::Ok(Value::MakeDoubleCompArray(std::move(values), element_type, 16));
  }

  return ParseResult::Error("No optimized array parser for type");
}

bool IsUsdaArrayLazyCandidate(TypeId element_type) {
  switch (element_type) {
    case TypeId::Float:
    case TypeId::Double:
    case TypeId::Half:
    case TypeId::TimeCode:
    case TypeId::Bool:
    case TypeId::Int:
    case TypeId::UInt:
    case TypeId::UChar:
    case TypeId::Int64:
    case TypeId::UInt64:
    case TypeId::Int2:
    case TypeId::Int3:
    case TypeId::Int4:
    case TypeId::UInt2:
    case TypeId::UInt3:
    case TypeId::UInt4:
    case TypeId::Float2:
    case TypeId::Float3:
    case TypeId::Float4:
    case TypeId::Half2:
    case TypeId::Half3:
    case TypeId::Half4:
    case TypeId::Point3h:
    case TypeId::Point3f:
    case TypeId::Vector3h:
    case TypeId::Vector3f:
    case TypeId::Normal3h:
    case TypeId::Normal3f:
    case TypeId::Color3h:
    case TypeId::Color3f:
    case TypeId::Texcoord2h:
    case TypeId::Texcoord2f:
    case TypeId::Texcoord3h:
    case TypeId::Texcoord3f:
    case TypeId::Quath:
    case TypeId::Quatf:
    case TypeId::Color4h:
    case TypeId::Color4f:
    case TypeId::Double2:
    case TypeId::Double3:
    case TypeId::Double4:
    case TypeId::Point3d:
    case TypeId::Vector3d:
    case TypeId::Normal3d:
    case TypeId::Color3d:
    case TypeId::Color4d:
    case TypeId::Texcoord2d:
    case TypeId::Texcoord3d:
    case TypeId::Quatd:
    case TypeId::Matrix2f:
    case TypeId::Matrix3f:
    case TypeId::Matrix4f:
    case TypeId::Matrix2d:
    case TypeId::Matrix3d:
    case TypeId::Matrix4d:
      return true;
    default:
      return false;
  }
}

ParseResult ParseArrayValue(Lexer& lexer, TypeId element_type,
                           const ParseArrayContext& context) {
  // USD allows array-valued attributes to author `None` as a blocked value.
  // Preserve it as a block (not an empty placeholder) so the writer re-emits
  // `= None` rather than collapsing it to a declared-only attribute.
  if (lexer.peek().type == TokenType::None) {
    lexer.next();
    return ParseResult::Ok(Value::MakeBlock());
  }

  const bool capture_optimized = IsUsdaArrayLazyCandidate(element_type) &&
                                 context.enable_usda_lazy_arrays &&
                                 context.source;

  if (capture_optimized || IsUsdaArrayLazyCandidate(element_type)) {
    const char* data = nullptr;
    size_t len = 0;
    bool simple = false;
    if (!lexer.capture_bracketed_literal(&data, &len, &simple)) {
      return ParseResult::Error(lexer.error());
    }

    if (capture_optimized && simple) {
      size_t element_count = 0;
      if (ParseUsdaArrayElementCount(
              data, len, context.max_usda_lazy_array_elements,
              element_type, &element_count)) {
        const char* base =
            reinterpret_cast<const char*>(context.source->base());
        const char* end = base + context.source->size();
        if (data >= base && data <= end && data + len <= end) {
          const uint64_t begin =
              static_cast<uint64_t>(data - base);
          LazyArrayRef ref;
          ref.source = context.source;
          ref.rep = ValueRep{};
          ref.block_offset = begin;
          ref.block_len = static_cast<uint64_t>(len);
          ref.element_count = static_cast<uint64_t>(element_count);
          ref.src_elem_stride = static_cast<uint32_t>(GetTypeSize(element_type));
          ref.value_type = element_type;
          ref.is_compressed = false;
          ref.crate_type = CrateTypeId::Invalid;
          return ParseResult::Ok(Value::MakeLazyArray(ref));
        }
      }
    }

    return ParseArrayValueFromSlice(data, len, simple, context.num_threads,
                                   element_type);
  }

  if (element_type == TypeId::Token || element_type == TypeId::String ||
      element_type == TypeId::AssetPath ||
      element_type == TypeId::PathExpression) {
    const char* data = nullptr;
    size_t len = 0;
    bool simple = false;
    if (!lexer.capture_bracketed_literal(&data, &len, &simple)) {
      return ParseResult::Error(lexer.error());
    }
    return ParseArrayValueFromSlice(data, len, simple, context.num_threads,
                                   element_type);
  }

  if (!lexer.expect(TokenType::OpenBracket)) {
    return ParseResult::Error(lexer.error());
  }

  // Handle empty array
  if (lexer.peek().type == TokenType::CloseBracket) {
    lexer.next();
    // Return appropriate empty array based on element type
    if (element_type == TypeId::Float || IsFloat3Like(element_type) ||
        IsFloat2Like(element_type) || IsFloat4Like(element_type)) {
      return ParseResult::Ok(Value::MakeFloatArray(std::vector<float>{}));
    } else if (element_type == TypeId::Int) {
      return ParseResult::Ok(Value::MakeIntArray(std::vector<int32_t>{}));
    } else if (element_type == TypeId::Int2 || element_type == TypeId::Int3 ||
               element_type == TypeId::Int4) {
      const uint32_t comps = (element_type == TypeId::Int2)   ? 2u
                             : (element_type == TypeId::Int3) ? 3u
                                                              : 4u;
      return ParseResult::Ok(
          Value::MakeIntCompArray(std::vector<int32_t>{}, element_type, comps));
    } else if (element_type == TypeId::Token || element_type == TypeId::String ||
               element_type == TypeId::AssetPath ||
               element_type == TypeId::PathExpression) {
      return ParseResult::Ok(
          Value::MakeStringLikeArray(std::vector<std::string>{}, element_type));
    }
    return ParseResult::Ok(Value::MakeFloatArray(std::vector<float>{}));
  }

  // Parse elements
  std::vector<float> float_data;
  std::vector<int32_t> int_data;
  std::vector<std::string> string_data;

  while (true) {
    ParseResult elem = ParseValue(lexer, element_type);
    if (!elem.success) {
      return elem;
    }

    // Collect data based on type
    if (element_type == TypeId::Float) {
      if (const float* f = elem.value.as_float()) {
        float_data.push_back(*f);
      }
    } else if (element_type == TypeId::Int) {
      if (const int32_t* i = elem.value.as_int()) {
        int_data.push_back(*i);
      }
    } else if (IsFloat3Like(element_type)) {
      if (const float* f = elem.value.as_float3()) {
        float_data.push_back(f[0]);
        float_data.push_back(f[1]);
        float_data.push_back(f[2]);
      }
    } else if (IsFloat2Like(element_type)) {
      if (const float* f = elem.value.as_float2()) {
        float_data.push_back(f[0]);
        float_data.push_back(f[1]);
      }
    } else if (IsFloat4Like(element_type)) {
      if (const float* f = elem.value.as_float4()) {
        float_data.push_back(f[0]);
        float_data.push_back(f[1]);
        float_data.push_back(f[2]);
        float_data.push_back(f[3]);
      }
    } else if (element_type == TypeId::Token) {
      if (const std::string* s = elem.value.as_token()) {
        string_data.push_back(*s);
      }
    } else if (element_type == TypeId::String) {
      if (const std::string* s = elem.value.as_string()) {
        string_data.push_back(*s);
      }
    }

    // Defense-in-depth: cap a single pathological array's accumulated scalars.
    if (float_data.size() > kMaxArrayScalars ||
        int_data.size() > kMaxArrayScalars ||
        string_data.size() > kMaxArrayScalars) {
      return ParseResult::Error("Array element count exceeds limit");
    }

    // Check for comma or end
    if (lexer.peek().type == TokenType::Comma) {
      lexer.next();
    } else if (lexer.peek().type == TokenType::CloseBracket) {
      lexer.next();
      break;
    } else {
      return ParseResult::Error("Expected ',' or ']' in array");
    }
  }

  // Return appropriate array type
  if (element_type == TypeId::Float) {
    return ParseResult::Ok(Value::MakeFloatArray(std::move(float_data)));
  } else if (element_type == TypeId::Int) {
    return ParseResult::Ok(Value::MakeIntArray(std::move(int_data)));
  } else if (IsFloat3Like(element_type)) {
    return ParseResult::Ok(Value::MakeFloat3Array(std::move(float_data)));
  } else if (IsFloat2Like(element_type) || IsFloat4Like(element_type)) {
    return ParseResult::Ok(Value::MakeFloatArray(std::move(float_data)));
  } else if (element_type == TypeId::Token || element_type == TypeId::String) {
    return ParseResult::Ok(
        Value::MakeStringLikeArray(std::move(string_data), element_type));
  }

  // Default: try to return float array
  return ParseResult::Ok(Value::MakeFloatArray(std::move(float_data)));
}
