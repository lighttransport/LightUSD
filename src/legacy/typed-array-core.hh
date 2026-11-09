// SPDX-License-Identifier: MIT
// Copyright 2025 - Present, Light Transport Entertainment Inc.

///
/// TypedArray: A type-safe wrapper around std::vector<uint8_t> with
/// nonstd::span views Provides in-place type transformation capabilities for
/// memory-efficient operations
///

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <vector>

#include "core/safe-arithmetic.hh"
#include "nonstd/expected.hpp"
#include "nonstd/span.hpp"
// NOTE: logger.hh is intentionally NOT included here. typed-array.hh uses no
// logging, but it sits under value-types.hh, so pulling logger.hh (the heavy
// TraceManager class + <unordered_map>/<mutex>/<chrono>) dragged a ~70ms parse
// into every value-system TU (~130 of them). DCOUT comes from common-macros.inc;
// direct TraceManager users include logger.hh themselves.

namespace lightusd { namespace legacy {

template <typename T>
class TypedArray {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using iterator = T*;
  using const_iterator = const T*;

  // Default constructor
  TypedArray() = default;

  // Constructor with size
  explicit TypedArray(size_type count) {
    resize(count);
  }

  // Constructor with size and default value
  TypedArray(size_type count, const T& value) {
    resize(count, value);
  }

  // Constructor from initializer list
  TypedArray(std::initializer_list<T> init) {
    reserve(init.size());
    for (const auto& item : init) {
      push_back(item);
    }
  }

  // Constructor from existing data (copies data)
  TypedArray(const T* data, size_type count) {
    if (data && count > 0) {
      size_t nbytes;
      if (safe::mul(count, sizeof(T), &nbytes)) {
        _storage.resize(nbytes);
        std::memcpy(_storage.data(), data, nbytes);
      }
    }
  }

  // Copy constructor
  TypedArray(const TypedArray& other)
      : _storage(other._storage) {
  }

  // Move constructor
  TypedArray(TypedArray&& other) noexcept
      : _storage(std::move(other._storage)) {
  }

  // Copy assignment
  TypedArray& operator=(const TypedArray& other) {
    if (this != &other) {
      _storage = other._storage;
    }
    return *this;
  }

  // Move assignment
  TypedArray& operator=(TypedArray&& other) noexcept {
    if (this != &other) {
      _storage = std::move(other._storage);
    }
    return *this;
  }

  // Destructor
  ~TypedArray() = default;

  // Size operations
  size_type size() const noexcept {
    return _storage.size() / sizeof(T);
  }

  size_type capacity() const noexcept {
    return _storage.capacity() / sizeof(T);
  }

  bool empty() const noexcept {
    return _storage.empty();
  }

  size_type max_size() const noexcept {
    return _storage.max_size() / sizeof(T);
  }

  size_type memory_usage() const noexcept {
    return _storage.capacity();
  }

  // Data access
  pointer data() noexcept {
    return reinterpret_cast<pointer>(_storage.data());
  }

  const_pointer data() const noexcept {
    return reinterpret_cast<const_pointer>(_storage.data());
  }

  // Element access
  reference operator[](size_type index) { return data()[index]; }

  const_reference operator[](size_type index) const { return data()[index]; }

  reference at(size_type index) {
    return data()[index];
  }

  const_reference at(size_type index) const {
    return data()[index];
  }

  reference front() { return data()[0]; }

  const_reference front() const { return data()[0]; }

  reference back() { return data()[size() - 1]; }

  const_reference back() const { return data()[size() - 1]; }

  // Iterators
  iterator begin() noexcept { return data(); }

  const_iterator begin() const noexcept { return data(); }

  const_iterator cbegin() const noexcept { return data(); }

  iterator end() noexcept { return data() + size(); }

  const_iterator end() const noexcept { return data() + size(); }

  const_iterator cend() const noexcept { return data() + size(); }

  // Span views
  nonstd::span<T> span() noexcept { return nonstd::span<T>(data(), size()); }

  nonstd::span<const T> span() const noexcept {
    return nonstd::span<const T>(data(), size());
  }

  nonstd::span<T> subspan(size_type offset,
                          size_type count = static_cast<size_type>(-1)) {
    size_type actual_count =
        (count == static_cast<size_type>(-1)) ? (size() - offset) : count;
    return nonstd::span<T>(data() + offset, actual_count);
  }

  nonstd::span<const T> subspan(
      size_type offset, size_type count = static_cast<size_type>(-1)) const {
    size_type actual_count =
        (count == static_cast<size_type>(-1)) ? (size() - offset) : count;
    return nonstd::span<const T>(data() + offset, actual_count);
  }

  // Span views with bounds checking always enabled (no exceptions needed).
  // Returns an error string on out-of-bounds access; safe in all build configs.
  nonstd::expected<nonstd::span<T>, std::string> subspan_checked(
      size_type offset, size_type count = static_cast<size_type>(-1)) {
    if (offset > size()) {
      return nonstd::make_unexpected("TypedArray::subspan_checked: offset out of range");
    }
    size_type actual_count =
        (count == static_cast<size_type>(-1)) ? (size() - offset) : count;
    if (offset + actual_count > size()) {
      return nonstd::make_unexpected("TypedArray::subspan_checked: count exceeds array bounds");
    }
    return nonstd::span<T>(data() + offset, actual_count);
  }

  nonstd::expected<nonstd::span<const T>, std::string> subspan_checked(
      size_type offset, size_type count = static_cast<size_type>(-1)) const {
    if (offset > size()) {
      return nonstd::make_unexpected("TypedArray::subspan_checked: offset out of range");
    }
    size_type actual_count =
        (count == static_cast<size_type>(-1)) ? (size() - offset) : count;
    if (offset + actual_count > size()) {
      return nonstd::make_unexpected("TypedArray::subspan_checked: count exceeds array bounds");
    }
    return nonstd::span<const T>(data() + offset, actual_count);
  }

  // Modifiers
  void clear() noexcept {
    _storage.clear();
  }

  void resize(size_type count) {
    size_t nbytes;
    if (!safe::mul(count, sizeof(T), &nbytes)) {
      _storage.clear();
      return;
    }
    _storage.resize(nbytes);
  }

  void resize(size_type count, const T& value) {
    size_type old_size = size();
    size_t nbytes;
    if (!safe::mul(count, sizeof(T), &nbytes)) {
      _storage.clear();
      return;
    }
    _storage.resize(nbytes);

    // Initialize new elements with the given value
    for (size_type i = old_size; i < count; ++i) {
      new (data() + i) T(value);
    }
  }

  void reserve(size_type new_capacity) {
    size_t nbytes;
    if (!safe::mul(new_capacity, sizeof(T), &nbytes)) return;
    _storage.reserve(nbytes);
  }

  void shrink_to_fit() {
    _storage.shrink_to_fit();
  }

  void push_back(const T& value) {
    size_type old_size = size();
    resize(old_size + 1);
    data()[old_size] = value;
  }

  void push_back(T&& value) {
    size_type old_size = size();
    resize(old_size + 1);
    data()[old_size] = std::move(value);
  }

  void pop_back() {
    if (!empty()) {
      resize(size() - 1);
    }
  }

  // In-place transform function
  // Transforms from current type T to new type N
  // Requirement: sizeof(T) >= sizeof(N) for in-place operation
  template <typename N, typename Func>
  TypedArray<N> transform(Func func) const {
    static_assert(sizeof(T) >= sizeof(N),
                  "transform: source type size must be >= destination type "
                  "size for in-place operation");
    static_assert(std::is_trivially_copyable<N>::value,
                  "transform: destination type must be trivially copyable");

    TypedArray<N> result;
    if (empty()) {
      return result;
    }

    // Calculate how many elements of type N we can fit
    size_type src_count = size();
    size_type dst_count = (src_count * sizeof(T)) / sizeof(N);

    result.resize(dst_count);

    // Apply transformation
    for (size_type i = 0; i < src_count; ++i) {
      // Calculate how many N elements this T element can produce
      size_type n_elements_per_t = sizeof(T) / sizeof(N);
      for (size_type j = 0;
           j < n_elements_per_t && (i * n_elements_per_t + j) < dst_count;
           ++j) {
        size_type dst_idx = i * n_elements_per_t + j;
        func(data()[i], result.data()[dst_idx]);
      }
    }

    return result;
  }

  // Specialized transform for 1:1 type conversion (e.g., int to float)
  template <typename N, typename Func>
  TypedArray<N> transform_1to1(Func func) const {
    static_assert(
        sizeof(T) >= sizeof(N),
        "transform_1to1: source type size must be >= destination type size");
    static_assert(
        std::is_trivially_copyable<N>::value,
        "transform_1to1: destination type must be trivially copyable");

    TypedArray<N> result;
    if (empty()) {
      return result;
    }

    result.resize(size());

    for (size_type i = 0; i < size(); ++i) {
      func(data()[i], result.data()[i]);
    }

    return result;
  }

  // Enhanced transform supporting sizeof(srcTy) <= sizeof(dstTy) with
  // controlled buffer growth
  template <typename N, typename Func>
  TypedArray<N> transform_expand(Func func) const {
    static_assert(
        std::is_trivially_copyable<N>::value,
        "transform_expand: destination type must be trivially copyable");

    TypedArray<N> result;
    if (empty()) {
      return result;
    }

    size_type src_count = size();

    // Use template meta-programming instead of if constexpr for C++14
    // compatibility
    return transform_expand_impl<N>(
        func, src_count, result,
        std::integral_constant<bool, (sizeof(T) >= sizeof(N))>{});
  }

  // In-place transform with expansion (modifies current array)
  // Only works when sizeof(T) <= sizeof(N) and we have sufficient capacity
  template <typename N, typename Func>
  TypedArray<N> transform_inplace_expand(Func func) {
    static_assert(std::is_trivially_copyable<N>::value,
                  "transform_inplace_expand: destination type must be "
                  "trivially copyable");

    if (empty()) {
      TypedArray<N> result;
      return result;
    }

    size_type src_count = size();
    size_t required_bytes;
    if (!safe::mul(src_count, sizeof(N), &required_bytes)) {
      return TypedArray<N>();
    }

    // Check if we can expand in-place or need reallocation
    if (required_bytes <= _storage.capacity()) {
      // Can expand in-place - transform from end to beginning to avoid
      // overwriting
      _storage.resize(required_bytes);

      // Cast storage to both source and destination types
      T* src_data = reinterpret_cast<T*>(_storage.data());
      N* dst_data = reinterpret_cast<N*>(_storage.data());

      // Transform backwards to avoid overlap issues
      for (size_type i = src_count; i > 0; --i) {
        size_type src_idx = i - 1;
        size_type dst_idx = src_idx;
        func(src_data[src_idx], dst_data[dst_idx]);
      }

      // Return TypedArray<N> that shares the same storage
      TypedArray<N> result;
      result._storage = std::move(_storage);
      _storage.clear();  // Current array is now empty
      return result;
    } else {
      // Need reallocation - use regular transform_expand
      return transform_expand<N>(func);
    }
  }

  // Transform with controlled growth - specify maximum buffer growth factor
  template <typename N, typename Func>
  TypedArray<N> transform_with_limit(Func func,
                                         double max_growth_factor = 2.0) const {
    static_assert(
        std::is_trivially_copyable<N>::value,
        "transform_with_limit: destination type must be trivially copyable");

    TypedArray<N> result;
    if (empty()) {
      return result;
    }

    size_type src_count = size();
    size_type required_bytes = src_count * sizeof(N);
    size_type current_bytes = _storage.size();

    // Check if expansion exceeds the growth limit
    double growth_ratio = static_cast<double>(required_bytes) /
                          static_cast<double>(current_bytes);
    if (growth_ratio > max_growth_factor) {
      return result;
    }

    // Proceed with transformation
    result.resize(src_count);
    for (size_type i = 0; i < src_count; ++i) {
      func(data()[i], result.data()[i]);
    }

    return result;
  }

  // Get raw storage access (advanced usage)
  std::vector<uint8_t>& storage() noexcept { return _storage; }

  const std::vector<uint8_t>& storage() const noexcept { return _storage; }

  // Swap
  void swap(TypedArray& other) noexcept {
    _storage.swap(other._storage);
  }

  // Comparison operators
  bool operator==(const TypedArray& other) const {
    if (size() != other.size()) return false;
    if (size() == 0) return true;
    return std::memcmp(data(), other.data(), size() * sizeof(T)) == 0;
  }

  bool operator!=(const TypedArray& other) const {
    return !(*this == other);
  }

  // Hash function for use with unordered_map
  // Hashes ALL elements for correctness
  struct Hash {
    size_t operator()(const TypedArray<T>& arr) const {
      size_t hash = std::hash<size_t>{}(arr.size());

      // Hash all elements for complete hash coverage
      for (size_t i = 0; i < arr.size(); ++i) {
        // Combine hashes using boost hash_combine technique
        // with golden ratio constant for good mixing
        hash ^= std::hash<T>{}(arr[i]) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
      }
      return hash;
    }
  };

  // Quick hash - samples only first 32 elements for performance
  // Useful for approximate hashing or when full precision isn't needed
  size_t quick_hash() const {
    size_t hash = std::hash<size_t>{}(size());
    const size_t max_sample = 32;
    const size_t num_to_hash = std::min(size(), max_sample);

    for (size_t i = 0; i < num_to_hash; ++i) {
      hash ^=
          std::hash<T>{}((*this)[i]) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
  }

 private:
  std::vector<uint8_t> _storage;

  // Helper method implementations for C++14 compatibility (instead of if
  // constexpr)
  template <typename N, typename Func>
  TypedArray<N> transform_expand_impl(
      Func func, size_type src_count, TypedArray<N>& result,
      std::true_type /* sizeof(T) >= sizeof(N) */) const {
    // Shrinking case - can use in-place approach
    size_type dst_count = (src_count * sizeof(T)) / sizeof(N);
    result.resize(dst_count);

    for (size_type i = 0; i < src_count; ++i) {
      size_type n_elements_per_t = sizeof(T) / sizeof(N);
      for (size_type j = 0;
           j < n_elements_per_t && (i * n_elements_per_t + j) < dst_count;
           ++j) {
        size_type dst_idx = i * n_elements_per_t + j;
        func(data()[i], result.data()[dst_idx]);
      }
    }
    return result;
  }

  template <typename N, typename Func>
  TypedArray<N> transform_expand_impl(
      Func func, size_type src_count, TypedArray<N>& result,
      std::false_type /* sizeof(T) < sizeof(N) */) const {
    // Expanding case - requires buffer growth
    // Buffer grows exactly to needed size: num_items * sizeof(dstTy)
    result.resize(src_count);

    for (size_type i = 0; i < src_count; ++i) {
      func(data()[i], result.data()[i]);
    }
    return result;
  }
};


///
/// TypedArrayView: A lightweight view over typed data using nonstd::span
/// Provides zero-copy access to data stored in various containers
///
template <typename T>
class TypedArrayView {
 public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;
  using iterator = pointer;
  using const_iterator = const_pointer;

  // Default constructor - creates empty view
  TypedArrayView() noexcept : _span() {}

  // Constructor from raw pointer and size
  TypedArrayView(pointer data, size_type count) noexcept : _span(data, count) {}

  // Constructor from raw pointer range
  TypedArrayView(pointer first, pointer last) noexcept : _span(first, last) {}

  // Constructor from C-style array
  template <std::size_t N>
  TypedArrayView(T (&arr)[N]) noexcept : _span(arr, N) {}

  // Constructor from std::vector with type size validation
  template <typename U>
  TypedArrayView(const std::vector<U>& vec) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "TypedArrayView: T must be trivially copyable");
    static_assert(std::is_trivially_copyable<U>::value,
                  "TypedArrayView: source type must be trivially copyable");

    if (sizeof(T) > sizeof(U) || vec.size() * sizeof(U) < sizeof(T)) {
      // Cannot safely view as T - create empty view
      _span = nonstd::span<T>();
    } else {
      // Safe to view as T
      size_type count = (vec.size() * sizeof(U)) / sizeof(T);
      _span = nonstd::span<T>(reinterpret_cast<const T*>(vec.data()), count);
    }
  }

  // Constructor from mutable std::vector with type size validation
  template <typename U>
  TypedArrayView(std::vector<U>& vec) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "TypedArrayView: T must be trivially copyable");
    static_assert(std::is_trivially_copyable<U>::value,
                  "TypedArrayView: source type must be trivially copyable");

    if (sizeof(T) > sizeof(U) || vec.size() * sizeof(U) < sizeof(T)) {
      // Cannot safely view as T - create empty view
      _span = nonstd::span<T>();
    } else {
      // Safe to view as T
      size_type count = (vec.size() * sizeof(U)) / sizeof(T);
      _span = nonstd::span<T>(reinterpret_cast<T*>(vec.data()), count);
    }
  }

  // Constructor from TypedArray with type size validation
  template <typename U>
  TypedArrayView(const TypedArray<U>& typed_array) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "TypedArrayView: T must be trivially copyable");
    static_assert(std::is_trivially_copyable<U>::value,
                  "TypedArrayView: source type must be trivially copyable");

    if (sizeof(T) > sizeof(U) || typed_array.size() * sizeof(U) < sizeof(T)) {
      // Cannot safely view as T - create empty view
      _span = nonstd::span<T>();
    } else {
      // Safe to view as T
      size_type count = (typed_array.size() * sizeof(U)) / sizeof(T);
      _span = nonstd::span<T>(reinterpret_cast<const T*>(typed_array.data()),
                              count);
    }
  }

  // Constructor from mutable TypedArray with type size validation
  template <typename U>
  TypedArrayView(TypedArray<U>& typed_array) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "TypedArrayView: T must be trivially copyable");
    static_assert(std::is_trivially_copyable<U>::value,
                  "TypedArrayView: source type must be trivially copyable");

    if (sizeof(T) > sizeof(U) || typed_array.size() * sizeof(U) < sizeof(T)) {
      // Cannot safely view as T - create empty view
      _span = nonstd::span<T>();
    } else {
      // Safe to view as T
      size_type count = (typed_array.size() * sizeof(U)) / sizeof(T);
      _span = nonstd::span<T>(reinterpret_cast<T*>(typed_array.data()), count);
    }
  }

  // Constructor from nonstd::span
  explicit TypedArrayView(nonstd::span<T> sp) noexcept : _span(sp) {}

  // Copy constructor
  TypedArrayView(const TypedArrayView& other) noexcept = default;

  // Assignment operator
  TypedArrayView& operator=(const TypedArrayView& other) noexcept = default;

  // Size and capacity
  size_type size() const noexcept { return _span.size(); }

  size_type size_bytes() const noexcept { return _span.size_bytes(); }

  bool empty() const noexcept { return _span.empty(); }

  // Data access
  pointer data() noexcept { return _span.data(); }

  const_pointer data() const noexcept { return _span.data(); }

  // Element access
  reference operator[](size_type index) const { return _span[index]; }

  reference at(size_type index) const {
    return _span[index];
  }

  reference front() const { return _span.front(); }

  reference back() const { return _span.back(); }

  // Iterators
  iterator begin() const noexcept { return _span.begin(); }

  iterator end() const noexcept { return _span.end(); }

  const_iterator cbegin() const noexcept { return _span.cbegin(); }

  const_iterator cend() const noexcept { return _span.cend(); }

  // Subviews
  TypedArrayView first(size_type count) const {
    return TypedArrayView(_span.first(count));
  }

  TypedArrayView last(size_type count) const {
    return TypedArrayView(_span.last(count));
  }

  TypedArrayView subspan(size_type offset,
                         size_type count = static_cast<size_type>(-1)) const {
    size_type actual_count =
        (count == static_cast<size_type>(-1)) ? (size() - offset) : count;
    return TypedArrayView(_span.subspan(offset, actual_count));
  }

  // Get underlying span
  nonstd::span<T> span() const noexcept { return _span; }

  // Type reinterpret view (with validation)
  template <typename N>
  TypedArrayView<N> reinterpret_as() const noexcept {
    static_assert(
        std::is_trivially_copyable<N>::value,
        "reinterpret_as: destination type must be trivially copyable");

    if (sizeof(N) > sizeof(T) || size() * sizeof(T) < sizeof(N)) {
      // Cannot safely reinterpret as N - return empty view
      return TypedArrayView<N>();
    }

    // Safe to reinterpret as N
    size_type new_count = (size() * sizeof(T)) / sizeof(N);
    return TypedArrayView<N>(reinterpret_cast<const N*>(data()), new_count);
  }

  // Mutable type reinterpret view (with validation)
  template <typename N>
  TypedArrayView<N> reinterpret_as_mutable() const noexcept {
    static_assert(
        std::is_trivially_copyable<N>::value,
        "reinterpret_as_mutable: destination type must be trivially copyable");
    static_assert(!std::is_const<T>::value,
                  "reinterpret_as_mutable: source type must not be const");

    if (sizeof(N) > sizeof(T) || size() * sizeof(T) < sizeof(N)) {
      // Cannot safely reinterpret as N - return empty view
      return TypedArrayView<N>();
    }

    // Safe to reinterpret as N
    size_type new_count = (size() * sizeof(T)) / sizeof(N);
    return TypedArrayView<N>(
        reinterpret_cast<N*>(
            const_cast<typename std::remove_const<T>::type*>(data())),
        new_count);
  }

  // Check if this view is valid (non-empty and properly aligned)
  bool is_valid() const noexcept {
    return !empty() && data() != nullptr &&
           (reinterpret_cast<std::uintptr_t>(data()) % alignof(T)) == 0;
  }

  // Comparison operators
  bool operator==(const TypedArrayView& other) const noexcept {
    if (size() != other.size()) return false;
    if (data() == other.data()) return true;  // Same memory location
    return std::memcmp(data(), other.data(), size() * sizeof(T)) == 0;
  }

  bool operator!=(const TypedArrayView& other) const noexcept {
    return !(*this == other);
  }

 private:
  nonstd::span<T> _span;
};

// Non-member swap
template <typename T>
void swap(TypedArray<T>& lhs, TypedArray<T>& rhs) noexcept {
  lhs.swap(rhs);
}

// Convenience functions for creating TypedArrayView

template <typename T>
TypedArrayView<T> make_typed_array_view(T* data, std::size_t count) {
  return TypedArrayView<T>(data, count);
}

template <typename T>
TypedArrayView<const T> make_typed_array_view(const T* data,
                                              std::size_t count) {
  return TypedArrayView<const T>(data, count);
}

template <typename T, std::size_t N>
TypedArrayView<T> make_typed_array_view(T (&arr)[N]) {
  return TypedArrayView<T>(arr);
}

template <typename T>
TypedArrayView<T> make_typed_array_view(std::vector<T>& vec) {
  return TypedArrayView<T>(vec);
}

template <typename T>
TypedArrayView<const T> make_typed_array_view(const std::vector<T>& vec) {
  return TypedArrayView<const T>(vec);
}

template <typename T>
TypedArrayView<T> make_typed_array_view(TypedArray<T>& arr) {
  return TypedArrayView<T>(arr);
}

template <typename T>
TypedArrayView<const T> make_typed_array_view(const TypedArray<T>& arr) {
  return TypedArrayView<const T>(arr);
}

template <typename T>
TypedArrayView<T> make_typed_array_view(nonstd::span<T> sp) {
  return TypedArrayView<T>(sp);
}

// Type reinterpret convenience functions
template <typename T, typename U>
TypedArrayView<T> reinterpret_typed_array_view(const TypedArrayView<U>& view) {
  return view.template reinterpret_as<T>();
}

template <typename T, typename U>
TypedArrayView<T> reinterpret_typed_array_view_mutable(
    const TypedArrayView<U>& view) {
  return view.template reinterpret_as_mutable<T>();
}

// Convenience function to create TypedArray from span
template <typename T>
TypedArray<T> make_typed_array(nonstd::span<const T> sp) {
  return TypedArray<T>(sp.data(), sp.size());
}

// ============================================================================
// TypedArray Convenience Functions
// ============================================================================

///
/// Create TypedArray with owned copy of data
/// Copies the data into internal storage.
///
/// Example:
///   float data[] = {1.0f, 2.0f, 3.0f};
///   auto arr = MakeTypedArrayCopy(data, 3);
///
template <typename T>
inline TypedArray<T> MakeTypedArrayCopy(const T* data, size_t count) {
  return TypedArray<T>(data, count);  // Copies data
}

///
/// Create empty TypedArray with specified capacity
/// Reserves memory without initializing elements.
///
/// Example:
///   auto arr = MakeTypedArrayReserved<double>(1000);
///   for (int i = 0; i < 500; ++i) {
///       arr.push_back(i * 1.5);
///   }
///
template <typename T>
inline TypedArray<T> MakeTypedArrayReserved(size_t capacity) {
  TypedArray<T> arr;
  arr.reserve(capacity);
  return arr;
}

///
/// Deep copy an existing TypedArray
/// Creates a new independent copy with its own storage.
///
/// Example:
///   TypedArray<float> original = ...;
///   TypedArray<float> copy = DuplicateTypedArray(original);
///
template <typename T>
inline TypedArray<T> DuplicateTypedArray(const TypedArray<T>& source) {
  if (source.empty()) {
    return TypedArray<T>();
  }
  return TypedArray<T>(source.data(), source.size());
}

}  // namespace legacy
}  // namespace lightusd
