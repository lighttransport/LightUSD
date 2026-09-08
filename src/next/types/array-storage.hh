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
 private:
  struct Control;
  Control* control_ = nullptr;
  void release();
};

}  // namespace detail
}  // namespace next
}  // namespace lightusd
