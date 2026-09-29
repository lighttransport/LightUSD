// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lightusd {
namespace next {
namespace detail {

// One ownership implementation for every materialized array. The transitional
// vector accessors preserve existing callers and zero-copy vector adoption;
// no virtual classes or per-element shared_ptr control blocks are involved.
class ArrayHandle {
 public:
  /// Empty handle (no storage). Only used as a placeholder by deferred-array
  /// fill handles; a Value never holds an empty handle.
  ArrayHandle() noexcept = default;
  explicit ArrayHandle(const std::vector<float>& data);
  explicit ArrayHandle(std::vector<float>&& data);
  explicit ArrayHandle(const std::vector<int32_t>& data);
  explicit ArrayHandle(std::vector<int32_t>&& data);
  explicit ArrayHandle(const std::vector<double>& data);
  explicit ArrayHandle(std::vector<double>&& data);
  explicit ArrayHandle(const std::vector<int64_t>& data);
  explicit ArrayHandle(std::vector<int64_t>&& data);
  explicit ArrayHandle(const std::vector<uint32_t>& data);
  explicit ArrayHandle(std::vector<uint32_t>&& data);
  explicit ArrayHandle(const std::vector<uint64_t>& data);
  explicit ArrayHandle(std::vector<uint64_t>&& data);
  explicit ArrayHandle(const std::vector<uint8_t>& data);
  explicit ArrayHandle(std::vector<uint8_t>&& data);
  explicit ArrayHandle(const std::vector<std::string>& data);
  explicit ArrayHandle(std::vector<std::string>&& data);
  ~ArrayHandle();
  ArrayHandle(const ArrayHandle& other) noexcept;
  ArrayHandle(ArrayHandle&& other) noexcept;
  ArrayHandle& operator=(ArrayHandle other) noexcept;
  void detach();
  size_t size() const;
  void* data() const;
  // Internal only: the active std::vector object, selected by Value's TypeId.
  void* vector_object() const;
  /// Exchange the payload vectors of two handles of the same storage kind in
  /// place (every holder of either handle observes the swap). Returns false,
  /// changing nothing, when either handle is empty or the kinds differ.
  bool swap_payload(ArrayHandle& other);
 private:
  struct Control;
  Control* control_ = nullptr;
  void release();
};

}  // namespace detail
}  // namespace next
}  // namespace lightusd
