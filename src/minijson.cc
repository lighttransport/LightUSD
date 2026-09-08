// SPDX-License-Identifier: MIT
#include "minijson.hh"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/fast_float/include/fast_float/fast_float.h"
#include "external/dragonbox/dragonbox.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace minijson {

namespace {

// Share Dragonbox's shortest-decimal machinery/tables with the USD writers.
// libc++'s floating to_chars brings a separate large formatter into WASM.
// JSON accepts this locale-independent notation; all finite values round-trip.
void AppendJSONNumber(double value, std::string* out) {
  if (std::signbit(value)) out->push_back('-');
  if (value == 0.0) { out->push_back('0'); return; }
  const auto decimal = jkj::dragonbox::to_decimal(value);
  char digits[24];
  const auto converted = std::to_chars(
      digits, digits + sizeof(digits), decimal.significand);
  const int count = static_cast<int>(converted.ptr - digits);
  const int point = count + decimal.exponent;
  // Match USD's fixed/scientific threshold. In particular, large integral
  // doubles need an exponent so the JSON parser does not treat them as an
  // out-of-range int64/uint64 literal.
  if (point > 0 && point <= 15) {
    if (decimal.exponent >= 0) {
      out->append(digits, static_cast<size_t>(count));
      out->append(static_cast<size_t>(decimal.exponent), '0');
    } else {
      out->append(digits, static_cast<size_t>(point));
      out->push_back('.');
      out->append(digits + point, static_cast<size_t>(count - point));
    }
  } else if (point <= 0 && point > -6) {
    out->append("0.");
    out->append(static_cast<size_t>(-point), '0');
    out->append(digits, static_cast<size_t>(count));
  } else {
    out->push_back(digits[0]);
    if (count > 1) {
      out->push_back('.');
      out->append(digits + 1, static_cast<size_t>(count - 1));
    }
    out->push_back('e');
    char exponent[8];
    const auto result = std::to_chars(exponent, exponent + sizeof(exponent), point - 1);
    out->append(exponent, result.ptr);
  }
}

bool IsSpace(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool AppendUTF8(uint32_t cp, std::string *out) {
  if (!out) return false;
  if (cp <= 0x7f) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7ff) {
    out->push_back(static_cast<char>(0xc0u | (cp >> 6u)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
  } else if (cp <= 0xffff) {
    if (cp >= 0xd800 && cp <= 0xdfff) return false;
    out->push_back(static_cast<char>(0xe0u | (cp >> 12u)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
  } else if (cp <= 0x10ffff) {
    out->push_back(static_cast<char>(0xf0u | (cp >> 18u)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
  } else {
    return false;
  }
  return true;
}

bool ReadUTF8(const char *data, size_t size, size_t *pos, std::string *out) {
  if (!data || !pos || *pos >= size) return false;
  const uint8_t c0 = static_cast<uint8_t>(data[*pos]);

  if (c0 <= 0x7f) {
    if (c0 < 0x20) return false;
    if (out) out->push_back(static_cast<char>(c0));
    (*pos)++;
    return true;
  }

  uint32_t cp = 0;
  size_t n = 0;
  if (c0 >= 0xc2 && c0 <= 0xdf) {
    cp = c0 & 0x1fu;
    n = 2;
  } else if (c0 >= 0xe0 && c0 <= 0xef) {
    cp = c0 & 0x0fu;
    n = 3;
  } else if (c0 >= 0xf0 && c0 <= 0xf4) {
    cp = c0 & 0x07u;
    n = 4;
  } else {
    return false;
  }

  if (*pos > size || n > (size - *pos)) return false;

  for (size_t i = 1; i < n; i++) {
    uint8_t cx = static_cast<uint8_t>(data[*pos + i]);
    if ((cx & 0xc0u) != 0x80u) return false;
    cp = (cp << 6u) | (cx & 0x3fu);
  }

  if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) return false;
  if (cp >= 0xd800 && cp <= 0xdfff) return false;
  if (cp > 0x10ffff) return false;

  if (out) out->append(data + *pos, n);
  *pos += n;
  return true;
}

void SetError(Error *err, const std::string &message, size_t offset) {
  if (err) {
    err->message = message;
    err->offset = offset;
  }
}

class Parser {
 public:
  Parser(const char *data, size_t size, const ParseOptions &options)
      : data_(data), size_(size), options_(options) {}

  bool ParseRoot(Value *out, Error *err) {
    if (!out) {
      SetError(err, "output pointer is null", 0);
      return false;
    }
    if (!data_ && size_ > 0) {
      SetError(err, "input pointer is null", 0);
      return false;
    }
    if (size_ > options_.max_input_bytes) {
      SetError(err, "JSON input exceeds maximum size", 0);
      return false;
    }

    SkipSpaces();
    if (pos_ == size_) {
      SetError(err, "empty JSON input", pos_);
      return false;
    }
    Value result;
    if (!ParseValue(0, &result, err)) return false;
    SkipSpaces();
    if (pos_ != size_) {
      SetError(err, "trailing characters after JSON value", pos_);
      return false;
    }
    *out = std::move(result);
    return true;
  }

 private:
  void SkipSpaces() {
    while (pos_ < size_ && IsSpace(data_[pos_])) pos_++;
  }

  bool MatchLiteral(const char *literal) {
    const size_t n = std::strlen(literal);
    if (n > (size_ - pos_)) return false;
    if (std::memcmp(data_ + pos_, literal, n) != 0) return false;
    pos_ += n;
    return true;
  }

  bool ParseValue(size_t depth, Value *out, Error *err) {
    if (depth > options_.max_depth) {
      SetError(err, "JSON nesting depth exceeds limit", pos_);
      return false;
    }
    SkipSpaces();
    if (pos_ >= size_) {
      SetError(err, "unexpected end of JSON input", pos_);
      return false;
    }

    const char c = data_[pos_];
    if (c == 'n') {
      if (!MatchLiteral("null")) {
        SetError(err, "invalid literal", pos_);
        return false;
      }
      *out = nullptr;
      return true;
    }
    if (c == 't') {
      if (!MatchLiteral("true")) {
        SetError(err, "invalid literal", pos_);
        return false;
      }
      *out = true;
      return true;
    }
    if (c == 'f') {
      if (!MatchLiteral("false")) {
        SetError(err, "invalid literal", pos_);
        return false;
      }
      *out = false;
      return true;
    }
    if (c == '"') {
      std::string s;
      if (!ParseString(&s, err)) return false;
      *out = std::move(s);
      return true;
    }
    if (c == '[') return ParseArray(depth, out, err);
    if (c == '{') return ParseObject(depth, out, err);
    if (c == '-' || IsDigit(c)) return ParseNumber(out, err);

    SetError(err, "invalid JSON token", pos_);
    return false;
  }

  bool ParseHex4(uint32_t *cp, Error *err) {
    if (pos_ + 4 > size_) {
      SetError(err, "truncated unicode escape", pos_);
      return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < 4; i++) {
      int h = HexValue(data_[pos_ + i]);
      if (h < 0) {
        SetError(err, "invalid unicode escape", pos_ + i);
        return false;
      }
      v = (v << 4u) | static_cast<uint32_t>(h);
    }
    pos_ += 4;
    *cp = v;
    return true;
  }

  bool ParseString(std::string *out, Error *err) {
    const size_t string_begin = pos_;
    pos_++;
    if (!out) {
      SetError(err, "output string pointer is null", pos_);
      return false;
    }
    std::string s;
    while (pos_ < size_) {
      const char c = data_[pos_];
      if (c == '"') {
        pos_++;
        *out = std::move(s);
        return true;
      }
      if (c == '\\') {
        pos_++;
        if (pos_ >= size_) {
          SetError(err, "truncated string escape", pos_);
          return false;
        }
        const char esc = data_[pos_++];
        switch (esc) {
          case '"': s.push_back('"'); break;
          case '\\': s.push_back('\\'); break;
          case '/': s.push_back('/'); break;
          case 'b': s.push_back('\b'); break;
          case 'f': s.push_back('\f'); break;
          case 'n': s.push_back('\n'); break;
          case 'r': s.push_back('\r'); break;
          case 't': s.push_back('\t'); break;
          case 'u': {
            uint32_t cp1 = 0;
            if (!ParseHex4(&cp1, err)) return false;
            uint32_t cp = cp1;
            if (cp1 >= 0xd800 && cp1 <= 0xdbff) {
              if (pos_ + 2 > size_ || data_[pos_] != '\\' ||
                  data_[pos_ + 1] != 'u') {
                SetError(err, "high surrogate must be followed by low surrogate", pos_);
                return false;
              }
              pos_ += 2;
              uint32_t cp2 = 0;
              if (!ParseHex4(&cp2, err)) return false;
              if (cp2 < 0xdc00 || cp2 > 0xdfff) {
                SetError(err, "invalid low surrogate", pos_);
                return false;
              }
              cp = 0x10000u + (((cp1 - 0xd800u) << 10u) | (cp2 - 0xdc00u));
            } else if (cp1 >= 0xdc00 && cp1 <= 0xdfff) {
              SetError(err, "low surrogate without high surrogate", pos_);
              return false;
            }
            if (!AppendUTF8(cp, &s)) {
              SetError(err, "invalid unicode codepoint", pos_);
              return false;
            }
            break;
          }
          default:
            SetError(err, "invalid string escape", pos_ - 1);
            return false;
        }
      } else {
        if (!ReadUTF8(data_, size_, &pos_, &s)) {
          SetError(err, "invalid UTF-8 or control character in string", pos_);
          return false;
        }
      }
      if (s.size() > options_.max_string_bytes) {
        SetError(err, "JSON string exceeds maximum size", string_begin);
        return false;
      }
    }
    SetError(err, "unterminated string", string_begin);
    return false;
  }

  bool ParseArray(size_t depth, Value *out, Error *err) {
    pos_++;
    Value a = Value::array();
    SkipSpaces();
    if (pos_ < size_ && data_[pos_] == ']') {
      pos_++;
      *out = std::move(a);
      return true;
    }
    while (pos_ < size_) {
      if (a.size() >= options_.max_array_elements) {
        SetError(err, "JSON array exceeds element limit", pos_);
        return false;
      }
      Value element;
      if (!ParseValue(depth + 1, &element, err)) return false;
      a.push_back(std::move(element));
      SkipSpaces();
      if (pos_ >= size_) break;
      if (data_[pos_] == ']') {
        pos_++;
        *out = std::move(a);
        return true;
      }
      if (data_[pos_] != ',') {
        SetError(err, "expected ',' or ']'", pos_);
        return false;
      }
      pos_++;
      SkipSpaces();
    }
    SetError(err, "unterminated array", pos_);
    return false;
  }

  bool ParseObject(size_t depth, Value *out, Error *err) {
    pos_++;
    Value obj = Value::object();
    SkipSpaces();
    if (pos_ < size_ && data_[pos_] == '}') {
      pos_++;
      *out = std::move(obj);
      return true;
    }
    while (pos_ < size_) {
      if (obj.size() >= options_.max_object_members) {
        SetError(err, "JSON object exceeds member limit", pos_);
        return false;
      }
      std::string key;
      if (!ParseString(&key, err)) return false;
      if (options_.reject_duplicate_keys && obj.find(key)) {
        SetError(err, "duplicate object key", pos_);
        return false;
      }
      SkipSpaces();
      if (pos_ >= size_ || data_[pos_] != ':') {
        SetError(err, "expected ':' after object key", pos_);
        return false;
      }
      pos_++;
      Value value;
      if (!ParseValue(depth + 1, &value, err)) return false;
      obj.set(std::move(key), std::move(value));
      SkipSpaces();
      if (pos_ >= size_) break;
      if (data_[pos_] == '}') {
        pos_++;
        *out = std::move(obj);
        return true;
      }
      if (data_[pos_] != ',') {
        SetError(err, "expected ',' or '}'", pos_);
        return false;
      }
      pos_++;
      SkipSpaces();
    }
    SetError(err, "unterminated object", pos_);
    return false;
  }

  bool ParseUnsignedRange(size_t begin, size_t end, uint64_t *out) {
    uint64_t v = 0;
    for (size_t i = begin; i < end; i++) {
      const uint64_t d = static_cast<uint64_t>(data_[i] - '0');
      if (v > ((std::numeric_limits<uint64_t>::max)() - d) / 10u) {
        return false;
      }
      v = v * 10u + d;
    }
    *out = v;
    return true;
  }

  bool ParseNumber(Value *out, Error *err) {
    const size_t begin = pos_;
    bool negative = false;
    if (data_[pos_] == '-') {
      negative = true;
      pos_++;
      if (pos_ >= size_) {
        SetError(err, "truncated number", begin);
        return false;
      }
    }

    const size_t int_begin = pos_;
    if (data_[pos_] == '0') {
      pos_++;
      if (pos_ < size_ && IsDigit(data_[pos_])) {
        SetError(err, "leading zero in number", begin);
        return false;
      }
    } else if (data_[pos_] >= '1' && data_[pos_] <= '9') {
      while (pos_ < size_ && IsDigit(data_[pos_])) pos_++;
    } else {
      SetError(err, "invalid number", begin);
      return false;
    }

    bool is_float = false;
    if (pos_ < size_ && data_[pos_] == '.') {
      is_float = true;
      pos_++;
      if (pos_ >= size_ || !IsDigit(data_[pos_])) {
        SetError(err, "fraction requires at least one digit", begin);
        return false;
      }
      while (pos_ < size_ && IsDigit(data_[pos_])) pos_++;
    }
    if (pos_ < size_ && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
      is_float = true;
      pos_++;
      if (pos_ < size_ && (data_[pos_] == '+' || data_[pos_] == '-')) pos_++;
      if (pos_ >= size_ || !IsDigit(data_[pos_])) {
        SetError(err, "exponent requires at least one digit", begin);
        return false;
      }
      while (pos_ < size_ && IsDigit(data_[pos_])) pos_++;
    }

    if (!is_float) {
      uint64_t u = 0;
      if (!ParseUnsignedRange(int_begin, pos_, &u)) {
        SetError(err, "integer exceeds uint64 range", begin);
        return false;
      }
      if (negative) {
        const uint64_t max_abs =
            static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) + 1u;
        if (u > max_abs) {
          SetError(err, "integer exceeds int64 range", begin);
          return false;
        }
        if (u == max_abs) {
          *out = (std::numeric_limits<int64_t>::min)();
        } else {
          *out = -static_cast<int64_t>(u);
        }
      } else {
        *out = u;
      }
      return true;
    }

    double d = 0.0;
    auto r = fast_float::from_chars(data_ + begin, data_ + pos_, d,
                                    fast_float::chars_format::general);
    if (r.ec != std::errc() || r.ptr != data_ + pos_ || !std::isfinite(d)) {
      SetError(err, "invalid floating-point number", begin);
      return false;
    }
    *out = d;
    return true;
  }

  const char *data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  ParseOptions options_;
};

bool ValidateUTF8String(const std::string &s) {
  size_t pos = 0;
  while (pos < s.size()) {
    const uint8_t c0 = static_cast<uint8_t>(s[pos]);
    if (c0 <= 0x7f) {
      pos++;
      continue;
    }
    const size_t before = pos;
    if (!ReadUTF8(s.data(), s.size(), &pos, nullptr)) return false;
    if (pos <= before) return false;
  }
  return true;
}

bool SerializeString(const std::string &s, std::string *out) {
  if (!out || !ValidateUTF8String(s)) return false;
  out->push_back('"');
  static const char kHex[] = "0123456789abcdef";
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20) {
          out->append("\\u00");
          out->push_back(kHex[c >> 4u]);
          out->push_back(kHex[c & 0x0fu]);
        } else {
          out->push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out->push_back('"');
  return true;
}

bool SerializeValue(const Value &v, std::string *out, Error *err,
                    const SerializeOptions &options, size_t depth) {
  if (!out) return false;
  if (depth > options.max_depth) {
    SetError(err, "JSON nesting depth exceeds serialization limit", 0);
    return false;
  }
  const auto newline_indent = [&](size_t d) {
    if (options.indent >= 0) {
      out->push_back('\n');
      out->append(d * static_cast<size_t>(options.indent), ' ');
    }
  };

  switch (v.type()) {
    case Type::Null:
      out->append("null");
      return true;
    case Type::Boolean: {
      bool b = false;
      v.as_bool(&b);
      out->append(b ? "true" : "false");
      return true;
    }
    case Type::SignedInteger: {
      int64_t i = 0;
      v.as_int64(&i);
      char buffer[32];
      auto result = std::to_chars(buffer, buffer + sizeof(buffer), i);
      if (result.ec != std::errc()) {
        SetError(err, "failed to serialize signed integer", 0);
        return false;
      }
      out->append(buffer, result.ptr);
      return true;
    }
    case Type::UnsignedInteger: {
      uint64_t u = 0;
      v.as_uint64(&u);
      char buffer[32];
      auto result = std::to_chars(buffer, buffer + sizeof(buffer), u);
      if (result.ec != std::errc()) {
        SetError(err, "failed to serialize unsigned integer", 0);
        return false;
      }
      out->append(buffer, result.ptr);
      return true;
    }
    case Type::Number: {
      double d = 0.0;
      v.as_double(&d);
      if (!std::isfinite(d)) {
        SetError(err, "non-finite number cannot be serialized as JSON", 0);
        return false;
      }
      AppendJSONNumber(d, out);
      return true;
    }
    case Type::String: {
      const std::string *s = v.string_ptr();
      if (!s || !SerializeString(*s, out)) {
        SetError(err, "invalid UTF-8 string cannot be serialized", 0);
        return false;
      }
      return true;
    }
    case Type::Array: {
      const auto *items = v.array_items();
      out->push_back('[');
      if (items && !items->empty()) {
        for (size_t i = 0; i < items->size(); i++) {
          if (i > 0) out->push_back(',');
          newline_indent(depth + 1);
          if (!SerializeValue((*items)[i], out, err, options, depth + 1)) {
            return false;
          }
        }
        newline_indent(depth);
      }
      out->push_back(']');
      return true;
    }
    case Type::Object: {
      const auto *items = v.object_items();
      out->push_back('{');
      if (items && !items->empty()) {
        bool needs_sort = options.sort_keys;
        if (needs_sort) {
          // Most builders add fields in a stable order already. Avoid the
          // temporary index vector and comparison sort in that common case.
          for (size_t i = 1; i < items->size(); i++) {
            if ((*items)[i].key < (*items)[i - 1].key) {
              break;
            }
            if (i + 1 == items->size()) needs_sort = false;
          }
          if (items->size() < 2) needs_sort = false;
        }
        if (needs_sort) {
          std::vector<size_t> order;
          order.reserve(items->size());
          for (size_t i = 0; i < items->size(); i++) order.push_back(i);
          std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return (*items)[a].key < (*items)[b].key;
          });
          for (size_t n = 0; n < order.size(); n++) {
            if (n > 0) out->push_back(',');
            newline_indent(depth + 1);
            const ObjectMember &m = (*items)[order[n]];
            if (!SerializeString(m.key, out)) {
              SetError(err, "invalid UTF-8 object key cannot be serialized", 0);
              return false;
            }
            out->push_back(':');
            if (options.indent >= 0) out->push_back(' ');
            if (!SerializeValue(m.value(), out, err, options, depth + 1)) {
              return false;
            }
          }
        } else {
          for (size_t n = 0; n < items->size(); n++) {
            if (n > 0) out->push_back(',');
            newline_indent(depth + 1);
            const ObjectMember &m = (*items)[n];
            if (!SerializeString(m.key, out)) {
              SetError(err, "invalid UTF-8 object key cannot be serialized", 0);
              return false;
            }
            out->push_back(':');
            if (options.indent >= 0) out->push_back(' ');
            if (!SerializeValue(m.value(), out, err, options, depth + 1)) {
              return false;
            }
          }
        }
        newline_indent(depth);
      }
      out->push_back('}');
      return true;
    }
  }
  SetError(err, "unknown JSON value type", 0);
  return false;
}

}  // namespace

ObjectMember::ObjectMember() : value_ptr(new Value()) {}

ObjectMember::ObjectMember(const std::string &k, const Value &v)
    : key(k), value_ptr(new Value(v)) {}

ObjectMember::ObjectMember(const char *k, const Value &v)
    : key(k ? k : ""), value_ptr(new Value(v)) {}

ObjectMember::ObjectMember(const ObjectMember &rhs)
    : key(rhs.key), value_ptr(new Value(rhs.value())) {}

ObjectMember::ObjectMember(ObjectMember &&rhs) noexcept
    : key(std::move(rhs.key)), value_ptr(rhs.value_ptr) {
  rhs.value_ptr = nullptr;
}

ObjectMember::~ObjectMember() { delete value_ptr; }

ObjectMember &ObjectMember::operator=(const ObjectMember &rhs) {
  if (this != &rhs) {
    key = rhs.key;
    if (!value_ptr) {
      value_ptr = new Value(rhs.value());
    } else {
      *value_ptr = rhs.value();
    }
  }
  return *this;
}

ObjectMember &ObjectMember::operator=(ObjectMember &&rhs) noexcept {
  if (this != &rhs) {
    delete value_ptr;
    key = std::move(rhs.key);
    value_ptr = rhs.value_ptr;
    rhs.value_ptr = nullptr;
  }
  return *this;
}

Value &ObjectMember::value() {
  if (!value_ptr) value_ptr = new Value();
  return *value_ptr;
}

const Value &ObjectMember::value() const {
  static const Value kNull;
  return value_ptr ? *value_ptr : kNull;
}

Value::Value() = default;
Value::Value(std::nullptr_t) {}
Value::Value(bool v) { *this = v; }
Value::Value(int v) { *this = static_cast<int64_t>(v); }
Value::Value(int64_t v) { *this = v; }
Value::Value(unsigned int v) { *this = static_cast<uint64_t>(v); }
#if defined(__EMSCRIPTEN__) || defined(__APPLE__) || defined(_WIN32)
Value::Value(unsigned long v) { *this = static_cast<uint64_t>(v); }
#endif
Value::Value(uint64_t v) { *this = v; }
Value::Value(float v) { *this = static_cast<double>(v); }
Value::Value(double v) { *this = v; }
Value::Value(const char *v) { *this = v; }
Value::Value(const std::string &v) { *this = v; }
Value::Value(std::string &&v) { *this = std::move(v); }
Value::Value(std::initializer_list<ObjectMember> members) {
  type_ = Type::Object;
  object_value_.assign(members.begin(), members.end());
}

Value Value::array() {
  Value v;
  v.type_ = Type::Array;
  return v;
}

Value Value::array(std::initializer_list<Value> values) {
  Value v = Value::array();
  v.array_value_.assign(values.begin(), values.end());
  return v;
}

Value Value::object() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

bool Value::empty() const {
  if (type_ == Type::Array) return array_value_.empty();
  if (type_ == Type::Object) return object_value_.empty();
  if (type_ == Type::String) return string_value_.empty();
  return type_ == Type::Null;
}

size_t Value::size() const {
  if (type_ == Type::Array) return array_value_.size();
  if (type_ == Type::Object) return object_value_.size();
  if (type_ == Type::String) return string_value_.size();
  return 0;
}

bool Value::as_bool(bool *out) const {
  if (!out || type_ != Type::Boolean) return false;
  *out = bool_value_;
  return true;
}

bool Value::as_int64(int64_t *out) const {
  if (!out) return false;
  if (type_ == Type::SignedInteger) {
    *out = int_value_;
    return true;
  }
  if (type_ == Type::UnsignedInteger &&
      uint_value_ <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
    *out = static_cast<int64_t>(uint_value_);
    return true;
  }
  return false;
}

bool Value::as_uint64(uint64_t *out) const {
  if (!out) return false;
  if (type_ == Type::UnsignedInteger) {
    *out = uint_value_;
    return true;
  }
  if (type_ == Type::SignedInteger && int_value_ >= 0) {
    *out = static_cast<uint64_t>(int_value_);
    return true;
  }
  return false;
}

bool Value::as_size_t(size_t *out) const {
  if (!out) return false;
  uint64_t u = 0;
  if (!as_uint64(&u)) return false;
#if SIZE_MAX < UINT64_MAX
  if (u > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
    return false;
  }
#endif
  *out = static_cast<size_t>(u);
  return true;
}

bool Value::as_double(double *out) const {
  if (!out) return false;
  if (type_ == Type::Number) {
    *out = number_value_;
    return true;
  }
  if (type_ == Type::SignedInteger) {
    *out = static_cast<double>(int_value_);
    return true;
  }
  if (type_ == Type::UnsignedInteger) {
    *out = static_cast<double>(uint_value_);
    return true;
  }
  return false;
}

bool Value::as_string(std::string *out) const {
  if (!out || type_ != Type::String) return false;
  *out = string_value_;
  return true;
}

bool Value::get_bool() const {
  bool ret = false;
  as_bool(&ret);
  return ret;
}

int Value::get_int() const {
  int64_t ret = 0;
  as_int64(&ret);
  return static_cast<int>(ret);
}

int64_t Value::get_int64() const {
  int64_t ret = 0;
  as_int64(&ret);
  return ret;
}

uint64_t Value::get_uint64() const {
  uint64_t ret = 0;
  as_uint64(&ret);
  return ret;
}

double Value::get_double() const {
  double ret = 0.0;
  as_double(&ret);
  return ret;
}

std::string Value::get_string() const {
  std::string ret;
  as_string(&ret);
  return ret;
}

const std::string *Value::string_ptr() const {
  return type_ == Type::String ? &string_value_ : nullptr;
}

const Value::array_type *Value::array_items() const {
  return type_ == Type::Array ? &array_value_ : nullptr;
}

Value::array_type *Value::array_items() {
  return type_ == Type::Array ? &array_value_ : nullptr;
}

const Value::object_type *Value::object_items() const {
  return type_ == Type::Object ? &object_value_ : nullptr;
}

Value::object_type *Value::object_items() {
  return type_ == Type::Object ? &object_value_ : nullptr;
}

bool Value::contains(const std::string &key) const { return find(key) != nullptr; }

const Value *Value::find(const std::string &key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto &m : object_value_) {
    if (m.key == key) return &m.value();
  }
  return nullptr;
}

Value *Value::find(const std::string &key) {
  if (type_ != Type::Object) return nullptr;
  for (auto &m : object_value_) {
    if (m.key == key) return &m.value();
  }
  return nullptr;
}

Value &Value::operator[](const std::string &key) {
  make_object();
  if (Value *v = find(key)) return *v;
  object_value_.emplace_back(key, Value());
  return object_value_.back().value();
}

Value &Value::operator[](std::string &&key) {
  make_object();
  if (Value *v = find(key)) return *v;
  object_value_.emplace_back();
  object_value_.back().key = std::move(key);
  return object_value_.back().value();
}

Value &Value::operator[](const char *key) {
  return (*this)[std::string(key ? key : "")];
}

const Value &Value::operator[](const std::string &key) const {
  const Value *v = find(key);
  return v ? *v : null_value();
}

const Value &Value::operator[](const char *key) const {
  return (*this)[std::string(key ? key : "")];
}

const Value &Value::at(const std::string &key) const {
  return (*this)[key];
}

Value &Value::at(const std::string &key) {
  return (*this)[key];
}

const Value &Value::operator[](size_t idx) const {
  if (type_ != Type::Array || idx >= array_value_.size()) return null_value();
  return array_value_[idx];
}

Value &Value::operator[](size_t idx) {
  make_array();
  if (idx >= array_value_.size()) array_value_.resize(idx + 1);
  return array_value_[idx];
}

void Value::push_back(const Value &v) {
  make_array();
  array_value_.push_back(v);
}

void Value::push_back(Value &&v) {
  make_array();
  array_value_.push_back(std::move(v));
}

void Value::reserve(size_t capacity) {
  if (type_ == Type::Array) {
    array_value_.reserve(capacity);
  } else if (type_ == Type::Object) {
    object_value_.reserve(capacity);
  }
}

void Value::set(const std::string &key, const Value &v) { (*this)[key] = v; }

void Value::set(const std::string &key, Value &&v) { (*this)[key] = std::move(v); }

void Value::set(std::string &&key, Value &&v) {
  make_object();
  if (Value *existing = find(key)) {
    *existing = std::move(v);
    return;
  }
  object_value_.emplace_back();
  object_value_.back().key = std::move(key);
  object_value_.back().value() = std::move(v);
}

Value &Value::operator=(std::nullptr_t) {
  type_ = Type::Null;
  bool_value_ = false;
  int_value_ = 0;
  uint_value_ = 0;
  number_value_ = 0.0;
  string_value_.clear();
  array_value_.clear();
  object_value_.clear();
  return *this;
}

Value &Value::operator=(bool v) {
  *this = nullptr;
  type_ = Type::Boolean;
  bool_value_ = v;
  return *this;
}

Value &Value::operator=(int v) { return (*this = static_cast<int64_t>(v)); }

Value &Value::operator=(int64_t v) {
  *this = nullptr;
  type_ = Type::SignedInteger;
  int_value_ = v;
  return *this;
}

Value &Value::operator=(unsigned int v) {
  return (*this = static_cast<uint64_t>(v));
}

#if defined(__EMSCRIPTEN__) || defined(__APPLE__) || defined(_WIN32)
Value &Value::operator=(unsigned long v) {
  return (*this = static_cast<uint64_t>(v));
}
#endif

Value &Value::operator=(uint64_t v) {
  *this = nullptr;
  type_ = Type::UnsignedInteger;
  uint_value_ = v;
  return *this;
}

Value &Value::operator=(float v) { return (*this = static_cast<double>(v)); }

Value &Value::operator=(double v) {
  *this = nullptr;
  type_ = Type::Number;
  number_value_ = v;
  return *this;
}

Value &Value::operator=(const char *v) {
  *this = nullptr;
  type_ = Type::String;
  string_value_ = v ? v : "";
  return *this;
}

Value &Value::operator=(const std::string &v) {
  *this = nullptr;
  type_ = Type::String;
  string_value_ = v;
  return *this;
}

Value &Value::operator=(std::string &&v) {
  *this = nullptr;
  type_ = Type::String;
  string_value_ = std::move(v);
  return *this;
}

Value::iterator Value::begin() {
  make_array();
  return array_value_.begin();
}

Value::iterator Value::end() {
  make_array();
  return array_value_.end();
}

Value::const_iterator Value::begin() const {
  return type_ == Type::Array ? array_value_.begin() : empty_array().begin();
}

Value::const_iterator Value::end() const {
  return type_ == Type::Array ? array_value_.end() : empty_array().end();
}

std::string Value::dump(int indent) const {
  std::string out;
  SerializeOptions options;
  options.indent = indent;
  Serialize(*this, &out, nullptr, options);
  return out;
}

void Value::make_array() {
  if (type_ != Type::Array) {
    *this = nullptr;
    type_ = Type::Array;
  }
}

void Value::make_object() {
  if (type_ != Type::Object) {
    *this = nullptr;
    type_ = Type::Object;
  }
}

const Value::array_type &Value::empty_array() {
  static const array_type kEmpty;
  return kEmpty;
}

const Value &Value::null_value() {
  static const Value kNull;
  return kNull;
}

bool Parse(const char *data, size_t size, Value *out, Error *err,
           const ParseOptions &options) {
  Parser parser(data, size, options);
  return parser.ParseRoot(out, err);
}

bool Parse(const std::string &text, Value *out, Error *err,
           const ParseOptions &options) {
  return Parse(text.data(), text.size(), out, err, options);
}

bool Serialize(const Value &value, std::string *out, Error *err,
               const SerializeOptions &options) {
  if (!out) {
    SetError(err, "output pointer is null", 0);
    return false;
  }
  out->clear();
  return SerializeValue(value, out, err, options, 0);
}

const char *TypeName(Type type) {
  switch (type) {
    case Type::Null: return "null";
    case Type::Boolean: return "boolean";
    case Type::SignedInteger: return "signed integer";
    case Type::UnsignedInteger: return "unsigned integer";
    case Type::Number: return "number";
    case Type::String: return "string";
    case Type::Array: return "array";
    case Type::Object: return "object";
  }
  return "unknown";
}

}  // namespace minijson
}  // namespace lightusd
