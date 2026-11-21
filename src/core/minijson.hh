// SPDX-License-Identifier: MIT
// Minimal hardened JSON parser/serializer for LightUSD.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <vector>

namespace lightusd {
namespace minijson {

enum class Type {
  Null,
  Boolean,
  SignedInteger,
  UnsignedInteger,
  Number,
  String,
  Array,
  Object,
};

struct ParseOptions {
  size_t max_depth = 256;
  size_t max_input_bytes = 128 * 1024 * 1024;
  size_t max_string_bytes = 64 * 1024 * 1024;
  size_t max_array_elements = 16 * 1024 * 1024;
  size_t max_object_members = 16 * 1024 * 1024;
  bool reject_duplicate_keys = true;
};

struct SerializeOptions {
  int indent = -1;
  bool sort_keys = false;
  // Protect callers that build JSON programmatically from unbounded nesting.
  size_t max_depth = 256;
};

struct Error {
  std::string message;
  size_t offset = 0;
};

class Value;

struct ObjectMember {
  std::string key;
  Value *value_ptr = nullptr;

  ObjectMember();
  ObjectMember(const std::string &k, const Value &v);
  ObjectMember(const char *k, const Value &v);
  ObjectMember(const ObjectMember &rhs);
  ObjectMember(ObjectMember &&rhs) noexcept;
  ~ObjectMember();
  ObjectMember &operator=(const ObjectMember &rhs);
  ObjectMember &operator=(ObjectMember &&rhs) noexcept;

  Value &value();
  const Value &value() const;
};

class Value {
 public:
  using array_type = std::vector<Value>;
  using object_type = std::vector<ObjectMember>;
  using const_iterator = array_type::const_iterator;
  using iterator = array_type::iterator;

  Value();
  Value(std::nullptr_t);
  Value(bool v);
  Value(int v);
  Value(int64_t v);
  Value(unsigned int v);
#if defined(__EMSCRIPTEN__) || defined(__APPLE__) || defined(_WIN32)
  Value(unsigned long v);
#endif
  Value(uint64_t v);
  Value(float v);
  Value(double v);
  Value(const char *v);
  Value(const std::string &v);
  Value(std::string &&v);
  Value(std::initializer_list<ObjectMember> members);

  static Value array();
  static Value array(std::initializer_list<Value> values);
  static Value object();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_boolean() const { return type_ == Type::Boolean; }
  bool is_number() const {
    return type_ == Type::SignedInteger || type_ == Type::UnsignedInteger ||
           type_ == Type::Number;
  }
  bool is_number_integer() const {
    return type_ == Type::SignedInteger || type_ == Type::UnsignedInteger;
  }
  bool is_number_unsigned() const { return type_ == Type::UnsignedInteger; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool empty() const;
  size_t size() const;

  bool as_bool(bool *out) const;
  bool as_int64(int64_t *out) const;
  bool as_uint64(uint64_t *out) const;
  bool as_size_t(size_t *out) const;
  bool as_double(double *out) const;
  bool as_string(std::string *out) const;
  // Non-template convenience accessors keep the small JSON API friendly to
  // C++ callers without forcing template instantiations into every user.
  bool get_bool() const;
  int get_int() const;
  int64_t get_int64() const;
  uint64_t get_uint64() const;
  double get_double() const;
  std::string get_string() const;
  const std::string *string_ptr() const;
  const array_type *array_items() const;
  array_type *array_items();
  const object_type *object_items() const;
  object_type *object_items();

  bool contains(const std::string &key) const;
  const Value *find(const std::string &key) const;
  Value *find(const std::string &key);

  Value &operator[](const std::string &key);
  Value &operator[](std::string &&key);
  Value &operator[](const char *key);
  const Value &operator[](const std::string &key) const;
  const Value &operator[](const char *key) const;
  const Value &at(const std::string &key) const;
  Value &at(const std::string &key);
  const Value &operator[](size_t idx) const;
  const Value &operator[](int idx) const {
    return idx < 0 ? null_value() : (*this)[static_cast<size_t>(idx)];
  }
  Value &operator[](size_t idx);
  Value &operator[](int idx) {
    return (*this)[static_cast<size_t>(idx < 0 ? 0 : idx)];
  }

  template <typename T>
  T get() const {
    T result{};
    if constexpr (std::is_same<T, bool>::value) {
      as_bool(&result);
    } else if constexpr (std::is_same<T, std::string>::value) {
      as_string(&result);
    } else if constexpr (std::is_floating_point<T>::value) {
      double value = 0.0;
      as_double(&value);
      result = static_cast<T>(value);
    } else if constexpr (std::is_integral<T>::value &&
                         std::is_signed<T>::value) {
      int64_t value = 0;
      if (as_int64(&value)) result = static_cast<T>(value);
    } else if constexpr (std::is_integral<T>::value &&
                         std::is_unsigned<T>::value) {
      uint64_t value = 0;
      if (as_uint64(&value)) result = static_cast<T>(value);
    }
    return result;
  }

  template <typename T>
  T value(const std::string &key, const T &fallback) const {
    const Value *found = find(key);
    return found ? found->get<T>() : fallback;
  }

  void push_back(const Value &v);
  void push_back(Value &&v);
  // Reserve storage for array elements or object members. Calling reserve on
  // another value kind is a no-op; this keeps builder code allocation-aware
  // without exposing the underlying containers.
  void reserve(size_t capacity);
  void set(const std::string &key, const Value &v);
  void set(const std::string &key, Value &&v);
  void set(std::string &&key, Value &&v);

  Value &operator=(std::nullptr_t);
  Value &operator=(bool v);
  Value &operator=(int v);
  Value &operator=(int64_t v);
  Value &operator=(unsigned int v);
#if defined(__EMSCRIPTEN__) || defined(__APPLE__) || defined(_WIN32)
  Value &operator=(unsigned long v);
#endif
  Value &operator=(uint64_t v);
  Value &operator=(float v);
  Value &operator=(double v);
  Value &operator=(const char *v);
  Value &operator=(const std::string &v);
  Value &operator=(std::string &&v);

  iterator begin();
  iterator end();
  const_iterator begin() const;
  const_iterator end() const;

  std::string dump(int indent = -1) const;

 private:
  Type type_ = Type::Null;
  bool bool_value_ = false;
  int64_t int_value_ = 0;
  uint64_t uint_value_ = 0;
  double number_value_ = 0.0;
  std::string string_value_;
  array_type array_value_;
  object_type object_value_;

  void make_array();
  void make_object();
  static const array_type &empty_array();
  static const Value &null_value();
};

bool Parse(const char *data, size_t size, Value *out, Error *err = nullptr,
           const ParseOptions &options = ParseOptions());
bool Parse(const std::string &text, Value *out, Error *err = nullptr,
           const ParseOptions &options = ParseOptions());
bool Serialize(const Value &value, std::string *out, Error *err = nullptr,
               const SerializeOptions &options = SerializeOptions());

const char *TypeName(Type type);

}  // namespace minijson
}  // namespace lightusd
