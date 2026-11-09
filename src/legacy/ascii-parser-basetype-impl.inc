// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Light Transport Entertainment Inc.
//
// Shared helpers + member-template definitions for the ascii basic-type parser.
// Included (inside namespace lightusd::ascii) by ascii-parser-basetype.cc and by
// ascii-parser-basetype-inst.cc (which emits the explicit instantiations).
// Keeping the bodies here lets the heavy explicit instantiations compile in a
// separate, parallel TU.

// ---- file-local parse helpers (were an anonymous namespace) ----

// parseInt
// 0 = success
// -1 = bad input
// -2 = overflow
// -3 = underflow
inline int parseInt(const std::string &s, int *out_result) {
  size_t n = s.size();
  const char *c = s.c_str();

  if ((c == nullptr) || (*c) == '\0') {
    return -1;
  }

  size_t idx = 0;
  bool negative = c[0] == '-';
  if ((c[0] == '+') | (c[0] == '-')) {
    idx = 1;
    if (n == 1) {
      // sign char only
      return -1;
    }
  }

  int64_t result = 0;

  // allow zero-padded digits(e.g. "003")
  while (idx < n) {
    if ((c[idx] >= '0') && (c[idx] <= '9')) {
      int digit = int(c[idx] - '0');

      // Check for overflow before multiplication
      if (result > (std::numeric_limits<int>::max)() / 10) {
        return negative ? -3 : -2;
      }

      result = result * 10 + digit;
    } else {
      // bad input
      return -1;
    }

    if (negative) {
      if ((-result) < (std::numeric_limits<int>::min)()) {
        return -3;
      }
    } else {
      if (result > (std::numeric_limits<int>::max)()) {
        return -2;
      }
    }

    idx++;
  }

  if (negative) {
    (*out_result) = -int(result);
  } else {
    (*out_result) = int(result);
  }

  return 0;  // OK
}

inline nonstd::expected<float, std::string> ParseFloat(const std::string &s) {

  // Parse with fast_float
  float result;
  auto ans = fast_float::from_chars(s.data(), s.data() + s.size(), result);
  if (ans.ec != std::errc()) {
    // Current `fast_float` implementation does not report detailed parsing err.
    return nonstd::make_unexpected("Parse failed.");
  }

  return result;
}

inline nonstd::expected<double, std::string> ParseDouble(const std::string &s) {

  // Parse with fast_float
  double result;
  auto ans = fast_float::from_chars(s.data(), s.data() + s.size(), result);
  if (ans.ec != std::errc()) {
    // Current `fast_float` implementation does not report detailed parsing err.
    return nonstd::make_unexpected("Parse failed.");
  }

  return result;
}

inline bool SkipQuotedStringForArrayEnd(const char **p, const char *end) {
  if (!p || *p >= end) {
    return false;
  }

  const char quote = **p;
  if (quote != '"' && quote != '\'') {
    return false;
  }
  (*p)++;

  const bool triple =
      ((*p + 1) < end && (*p)[0] == quote && (*p)[1] == quote);
  if (triple) {
    *p += 2;
    while (*p < end) {
      if (**p == '\\') {
        if ((*p + 3) < end && (*p)[1] == quote && (*p)[2] == quote &&
            (*p)[3] == quote) {
          *p += 4;
          continue;
        }
        (*p)++;
        continue;
      }

      if ((*p + 2) < end && (*p)[0] == quote && (*p)[1] == quote &&
          (*p)[2] == quote) {
        *p += 3;
        return true;
      }
      (*p)++;
    }
    return false;
  }

  while (*p < end) {
    if (**p == '\\' && (*p + 1) < end &&
        ((*p)[1] == '\'' || (*p)[1] == '"')) {
      *p += 2;
      continue;
    }

    if (**p == quote) {
      (*p)++;
      return true;
    }

    (*p)++;
  }

  return false;
}

inline bool FindArrayLiteralEnd(const uint8_t *bytes, size_t size, uint64_t start_loc,
                         uint64_t *end_loc) {
  if (!bytes || !end_loc || start_loc >= size) {
    return false;
  }

  const char *begin = reinterpret_cast<const char *>(bytes);
  const char *p = begin + start_loc;
  const char *end = begin + size;

  if (*p != '[') {
    return false;
  }
  p++;

  int bracket_depth = 1;
  while (p < end && bracket_depth > 0) {
    const char c = *p;

    if (c == '#') {
      while (p < end && *p != '\n' && *p != '\r') {
        p++;
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      if (!SkipQuotedStringForArrayEnd(&p, end)) {
        return false;
      }
      continue;
    }

    p++;
    if (c == '[') {
      bracket_depth++;
    } else if (c == ']') {
      bracket_depth--;
    }
  }

  if (bracket_depth != 0) {
    return false;
  }

  *end_loc = uint64_t(p - begin);
  return true;
}


// ---- member-template definitions ----
template <typename T, size_t N>
bool AsciiParser::ParseTupleArray(
    std::vector<nonstd::optional<std::array<T, N>>> *result) {

  if (!Expect('[')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  // Empty array?
  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == ']') {
      result->clear();
      return true;
    }

    Rewind(1);
  }

  if (!SepBy1TupleType<T, N>(',', result)) {
    return false;
  }

  if (!Expect(']')) {
    return false;
  }

  return true;
}

template <typename T, size_t N>
bool AsciiParser::ParseTupleArray(std::vector<std::array<T, N>> *result) {
  (void)result;

  if (!Expect('[')) {
    return false;
  }

  if (!SepBy1TupleType<T, N>(',', result)) {
    return false;
  }

  if (!Expect(']')) {
    return false;
  }

  return true;
}

template <typename T>
bool AsciiParser::SepBy1BasicType(const char sep,
                                  std::vector<nonstd::optional<T>> *result) {
  result->clear();

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  {
    nonstd::optional<T> value;
    if (!ReadBasicType(&value)) {
      PushError("Not starting with the value of requested type.\n");
      return false;
    }

    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  while (!Eof()) {
    constexpr size_t kMaxArrayElements = 1024ull * 1024ull * 128ull;
    if (result->size() >= kMaxArrayElements) {
      PushError(fmt::format("Array element count exceeds limit ({}).\n",
                            kMaxArrayElements));
      return false;
    }

    // sep
    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c != sep) {
      // end
      _sr->seek_from_current(-1);  // unwind single char
      break;
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    nonstd::optional<T> value;
    if (!ReadBasicType(&value)) {
      break;
    }

    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  if (result->empty()) {
    PushError("Empty array.\n");
    return false;
  }

  return true;
}

template <typename T>
bool AsciiParser::SepBy1BasicType(const char sep, std::vector<T> *result) {
  result->clear();

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  {
    T value;
    if (!ReadBasicType(&value)) {
      PushError("Not starting with the value of requested type.\n");
      return false;
    }

    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  constexpr size_t kMaxArrayElements = 1024ull * 1024ull * 128ull; // 128M elements max

  while (!Eof()) {
    // sep
    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c != sep) {
      // end
      _sr->seek_from_current(-1);  // unwind single char
      break;
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    T value;
    if (!ReadBasicType(&value)) {
      break;
    }

    if (result->size() >= kMaxArrayElements) {
      PushError(fmt::format("Array element count exceeds limit ({}).\n", kMaxArrayElements));
      return false;
    }
    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  if (result->empty()) {
    PushError("Empty array.\n");
    return false;
  }

  return true;
}

template <typename T>
bool AsciiParser::SepBy1BasicType(const char sep, const char end_symbol, std::vector<T> *result) {
  result->clear();

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  {
    T value;
    if (!ReadBasicType(&value)) {
      PushError("Not starting with the value of requested type.\n");
      return false;
    }

    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  constexpr size_t kMaxArrayElements = 1024ull * 1024ull * 128ull; // 128M elements max

  while (!Eof()) {
    if (!SkipCommentAndWhitespaceAndNewline()) {
      return false;
    }

    // sep
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == sep) {
      // Look next token
      if (!SkipCommentAndWhitespaceAndNewline()) {
        return false;
      }

      char nc;
      if (!LookChar1(&nc)) {
        return false;
      }

      if (nc == end_symbol) {
        // end
        break;
      }
    }

    if (c != sep) {
      // end
      _sr->seek_from_current(-1);  // unwind single char
      break;
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    T value;
    if (!ReadBasicType(&value)) {
      break;
    }

    if (result->size() >= kMaxArrayElements) {
      PushError(fmt::format("Array element count exceeds limit ({}).\n", kMaxArrayElements));
      return false;
    }
    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);

  }

  if (result->empty()) {
    PushError("Empty array.\n");
    return false;
  }

  return true;
}

template <typename T, size_t N>
bool AsciiParser::SepBy1TupleType(
    const char sep, std::vector<nonstd::optional<std::array<T, N>>> *result) {
  result->clear();

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  if (MaybeNone()) {
    result->push_back(nonstd::nullopt);
  } else {
    std::array<T, N> value;
    if (!ParseBasicTypeTuple<T, N>(&value)) {
      PushError("Not starting with the tuple value of requested type.\n");
      return false;
    }

    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  while (!Eof()) {
    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c != sep) {
      // end
      _sr->seek_from_current(-1);  // unwind single char
      break;
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    constexpr size_t kMaxArrayElements = 1024ull * 1024ull * 128ull;
    if (result->size() >= kMaxArrayElements) {
      PushError(fmt::format("Tuple array element count exceeds limit ({}).\n", kMaxArrayElements));
      return false;
    }
    if (MaybeNone()) {
      result->push_back(nonstd::nullopt);
    } else {
      std::array<T, N> value;
      if (!ParseBasicTypeTuple<T, N>(&value)) {
        break;
      }
      CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
    }
  }

  if (result->empty()) {
    PushError("Empty array.\n");
    return false;
  }

  return true;
}

template <typename T, size_t N>
bool AsciiParser::SepBy1TupleType(const char sep,
                                  std::vector<std::array<T, N>> *result) {
  result->clear();

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  {
    std::array<T, N> value;
    if (!ParseBasicTypeTuple<T, N>(&value)) {
      PushError("Not starting with the tuple value of requested type.\n");
      return false;
    }

    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  while (!Eof()) {
    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c != sep) {
      // end
      _sr->seek_from_current(-1);  // unwind single char
      break;
    }

    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    std::array<T, N> value;
    if (!ParseBasicTypeTuple<T, N>(&value)) {
      break;
    }

    constexpr size_t kMaxArrayElements = 1024ull * 1024ull * 128ull;
    if (result->size() >= kMaxArrayElements) {
      PushError(fmt::format("Tuple array element count exceeds limit ({}).\n", kMaxArrayElements));
      return false;
    }
    CHECK_MEMORY_USAGE(sizeof(nonstd::optional<T>) + sizeof(T));
    result->push_back(value);
  }

  if (result->empty()) {
    PushError("Empty array.\n");
    return false;
  }

  return true;
}

template <typename T>
bool AsciiParser::ParseBasicTypeArray(
    std::vector<nonstd::optional<T>> *result) {
  if (!Expect('[')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  // Empty array?
  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == ']') {
      result->clear();
      return true;
    }

    Rewind(1);
  }

  if (!SepBy1BasicType<T>(',', ']', result)) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  if (!Expect(']')) {
    return false;
  }

  return true;
}

template <typename T>
bool AsciiParser::ParseBasicTypeArray(std::vector<T> *result) {
  if (!Expect('[')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  // Empty array?
  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == ']') {
      result->clear();
      return true;
    }

    Rewind(1);
  }

  if (!SepBy1BasicType<T>(',', ']', result)) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  if (!Expect(']')) {
    return false;
  }
  return true;
}

template <typename T>
bool AsciiParser::ParseBasicTypeArray(TypedArray<T> *result) {
  if (!Expect('[')) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  // Empty array?
  {
    char c;
    if (!Char1(&c)) {
      return false;
    }

    if (c == ']') {
      result->clear();
      return true;
    }

    Rewind(1);
  }

  // Parse elements into a temporary vector first
  std::vector<T> temp_result;
  if (!SepBy1BasicType<T>(',', ']', &temp_result)) {
    return false;
  }

  if (!SkipCommentAndWhitespaceAndNewline()) {
    return false;
  }

  if (!Expect(']')) {
    return false;
  }

  // Transfer to TypedArray for memory optimization
  result->clear();
  result->reserve(temp_result.size());
  for (const auto& item : temp_result) {
    result->push_back(item);
  }

  return true;
}

template <typename T, size_t N>
bool AsciiParser::ParseBasicTypeTuple(std::array<T, N> *result) {
  if (!Expect('(')) {
    return false;
  }

  for (size_t i = 0; i < N; i++) {
    if (!SkipWhitespaceAndNewline()) {
      return false;
    }

    T value;
    if (!ReadBasicType(&value)) {
      PushError("Failed to parse tuple element " + std::to_string(i) + ".\n");
      return false;
    }

    (*result)[i] = value;

    if ((i + 1) < N) {
      if (!SkipWhitespaceAndNewline()) {
        return false;
      }

      if (!Expect(',')) {
        return false;
      }
    }
  }

  if (!SkipWhitespaceAndNewline()) {
    return false;
  }

  if (!Expect(')')) {
    return false;
  }

  return true;
}

template <typename T, size_t N>
bool AsciiParser::ParseBasicTypeTuple(
    nonstd::optional<std::array<T, N>> *result) {
  if (MaybeNone()) {
    (*result) = nonstd::nullopt;
    return true;
  }

  std::array<T, N> ret;
  if (!ParseBasicTypeTuple<T, N>(&ret)) {
    return false;
  }

  (*result) = ret;

  return true;
}

template <typename T>
bool AsciiParser::MaybeNonFinite(T *out) {
  auto loc = CurrLoc();

  // Check that what follows the candidate token is not an identifier-
  // continuation character (so we don't consume the `inf` prefix of an
  // identifier like `info`).
  auto trailing_is_word_char = [](char c) -> bool {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
  };

  // Try matching "inf"/"nan" (3 chars) or "-inf" (4 chars). Advance
  // past the candidate, then peek the next char; if it's an identifier
  // continuation we reject (so `info` / `infrared` doesn't false-match
  // as `inf`).
  auto try_match = [&](const char *expected, size_t n,
                       T result_value) -> bool {
    std::array<char, 4> buf{};
    if (!CharN(n, &buf[0])) {
      SeekTo(loc);
      return false;
    }
    for (size_t i = 0; i < n; i++) {
      if (buf[i] != expected[i]) {
        SeekTo(loc);
        return false;
      }
    }
    char next;
    if (LookChar1(&next) && trailing_is_word_char(next)) {
      SeekTo(loc);
      return false;
    }
    (*out) = result_value;
    return true;
  };

  if (try_match("inf", 3, std::numeric_limits<T>::infinity())) return true;
  if (try_match("nan", 3, std::numeric_limits<T>::quiet_NaN())) return true;
  if (try_match("-inf", 4, -std::numeric_limits<T>::infinity())) return true;
  // NOTE: pxrUSD does not support `-nan`; skip it.

  return false;
}

template <typename T>
bool AsciiParser::ReadBasicType(std::vector<T> *value) {
  return ParseBasicTypeArray(value);
}

template <typename T>
bool AsciiParser::ReadBasicType(nonstd::optional<std::vector<T>> *value) {
  if (MaybeNone()) {
    (*value) = nonstd::nullopt;
    return true;
  }

  std::vector<T> v;
  if (ParseBasicTypeArray(&v)) {
    (*value) = v;
    return true;
  }

  return false;
}
