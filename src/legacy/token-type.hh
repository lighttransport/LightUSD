// SPDX-License-Identifier: Apache 2.0
// `token` type
#pragma once

//
// a class for `token` type.
//
// `token` is primarily used for a short-length string.
//
// Unlike pxrUSD, `Token` class does not acquire a lock by default. This means
// there is a potential hash collision for the hash value of `Token` string, but
// LightUSD does not require token(string) hashes are unique inside of LightUSD
// library. If you need pxrUSD-like behavior of `Token` class(i.e, you want a
// token hash with no collision), you can compile LightUSD with
// LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE.
// (Also you need to include foonathan/string_id c++ files(Please see <lightusd>/CMakeLists.txt) to your project)
//
// LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE
//   - Use foonathan/string_id to implement Token class.
//   - database(token storage) is accessed with mutex so an application should
//   not frequently construct Token class among threads.
//
// ---
//
//

#include <string>
#include <utility>

#include "compiler-features.hh"
#include "nonstd/optional.hpp"

#if defined(LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE)

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// external
#include "external/string_id/database.hpp"
#include "external/string_id/string_id.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#else  // LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE
#include <functional>
#endif  // LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE

namespace lightusd {

#if defined(LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE)

namespace sid = foonathan::string_id;

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif

// Singleton
class TokenStorage {
 public:
  TokenStorage(const TokenStorage &) = delete;
  TokenStorage &operator=(const TokenStorage &) = delete;
  TokenStorage(TokenStorage &&) = delete;
  TokenStorage &operator=(TokenStorage &&) = delete;

  static sid::default_database &GetInstance() {
    static sid::default_database s_database;
    return s_database;
  }

 private:
  TokenStorage() = default;
  ~TokenStorage() = default;
};

#ifdef __clang__
#pragma clang diagnostic pop
#endif

class Token {
 public:
  Token() {}

  explicit Token(const std::string &str) {
    str_ = sid::string_id(str.c_str(), TokenStorage::GetInstance());
  }

  explicit Token(std::string &&str) {
    str_ = sid::string_id(str.c_str(), TokenStorage::GetInstance());
  }

  Token(Token &&str) = default;


  explicit Token(const char *str) {
    str_ = sid::string_id(str, TokenStorage::GetInstance());
  }

  Token(const char *str, size_t len) {
    std::string tmp(str, len);
    str_ = sid::string_id(tmp.c_str(), TokenStorage::GetInstance());
  }

  const std::string str() const {
    if (!str_) {
      return std::string();
    }
    return str_.value().string();
  }

  const char *c_str() const {
    return str_ ? str_.value().string() : "";
  }

  uint64_t hash() const {
    if (!str_) {
      return 0;
    }

    // Assume non-zero hash value for non-empty string.
    return str_.value().hash_code();
  }

  bool valid() const {
    if (!str_) {
      return false;
    }

    if (str_.value().string().empty()) {
      return false;
    }

    return true;
  }

 private:
  nonstd::optional<sid::string_id> str_;
};

inline bool operator==(const Token &tok, const std::string &rhs) {
  return tok.str().compare(rhs) == 0;
}

struct TokenHasher {
  inline size_t operator()(const Token &tok) const {
    return size_t(tok.hash());
  }
};

struct TokenKeyEqual {
  bool operator()(const Token &lhs, const Token &rhs) const {
    return lhs.str() == rhs.str();
  }
};

#else  // LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE

class Token {
 public:
  Token() {}

  explicit Token(const std::string &str) { str_ = str; }

  explicit Token(std::string &&str) : str_(std::move(str)) {}

  explicit Token(const char *str) { str_ = str; }

  Token(const char *str, size_t len) : str_(str, len) {}

  const std::string &str() const LIGHTUSD_LIFETIMEBOUND { return str_; }

  const char *c_str() const { return str_.c_str(); }

  bool valid() const {
    if (str().empty()) {
      return false;
    }

    return true;
  }

  // No string hash for LightUSD

 private:
  std::string str_;
};

struct TokenHasher {
  inline size_t operator()(const Token &tok) const {
    return std::hash<std::string>()(tok.str());
  }
};

struct TokenKeyEqual {
  bool operator()(const Token &lhs, const Token &rhs) const {
    return lhs.str() == rhs.str();
  }
};

#endif  // LIGHTUSD_USE_STRING_ID_FOR_TOKEN_TYPE

inline bool operator==(const Token &lhs, const Token &rhs) {
  return TokenKeyEqual()(lhs, rhs);
}

inline bool operator!=(const Token &lhs, const Token &rhs) {
  return !TokenKeyEqual()(lhs, rhs);
}

inline bool operator<(const Token &lhs, const Token &rhs) {
  return lhs.str() < rhs.str();
}

}  // namespace lightusd
