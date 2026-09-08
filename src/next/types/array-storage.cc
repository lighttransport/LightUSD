// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "array-storage.hh"
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

namespace lightusd {
namespace next {
namespace detail {

struct ArrayHandle::Control {
  enum Kind { Float, Int, Double, Int64, UInt, UInt64, Bool, Token };
  std::atomic<size_t> references{1};
  Kind kind;
  union Payload {
    std::vector<float> a_float;
    std::vector<int32_t> a_int;
    std::vector<double> a_double;
    std::vector<int64_t> a_int64;
    std::vector<uint32_t> a_uint;
    std::vector<uint64_t> a_uint64;
    std::vector<uint8_t> a_bool;
    std::vector<std::string> a_token;
    Payload() {}
    ~Payload() {}
  } payload;
  explicit Control(Kind k) : kind(k) {}
};

ArrayHandle::ArrayHandle(const std::vector<float>& data) : control_(new Control(Control::Float)) {
  new (&control_->payload.a_float) std::vector<float>(data);
}
ArrayHandle::ArrayHandle(std::vector<float>&& data) : control_(new Control(Control::Float)) {
  new (&control_->payload.a_float) std::vector<float>(std::move(data));
}
ArrayHandle::ArrayHandle(const std::vector<int32_t>& data) : control_(new Control(Control::Int)) {
  new (&control_->payload.a_int) std::vector<int32_t>(data);
}
ArrayHandle::ArrayHandle(std::vector<int32_t>&& data) : control_(new Control(Control::Int)) {
  new (&control_->payload.a_int) std::vector<int32_t>(std::move(data));
}
ArrayHandle::ArrayHandle(const std::vector<double>& data) : control_(new Control(Control::Double)) {
  new (&control_->payload.a_double) std::vector<double>(data);
}
ArrayHandle::ArrayHandle(std::vector<double>&& data) : control_(new Control(Control::Double)) {
  new (&control_->payload.a_double) std::vector<double>(std::move(data));
}
ArrayHandle::ArrayHandle(const std::vector<int64_t>& data) : control_(new Control(Control::Int64)) {
  new (&control_->payload.a_int64) std::vector<int64_t>(data);
}
ArrayHandle::ArrayHandle(std::vector<int64_t>&& data) : control_(new Control(Control::Int64)) {
  new (&control_->payload.a_int64) std::vector<int64_t>(std::move(data));
}
ArrayHandle::ArrayHandle(const std::vector<uint32_t>& data) : control_(new Control(Control::UInt)) {
  new (&control_->payload.a_uint) std::vector<uint32_t>(data);
}
ArrayHandle::ArrayHandle(std::vector<uint32_t>&& data) : control_(new Control(Control::UInt)) {
  new (&control_->payload.a_uint) std::vector<uint32_t>(std::move(data));
}
ArrayHandle::ArrayHandle(const std::vector<uint64_t>& data) : control_(new Control(Control::UInt64)) {
  new (&control_->payload.a_uint64) std::vector<uint64_t>(data);
}
ArrayHandle::ArrayHandle(std::vector<uint64_t>&& data) : control_(new Control(Control::UInt64)) {
  new (&control_->payload.a_uint64) std::vector<uint64_t>(std::move(data));
}
ArrayHandle::ArrayHandle(const std::vector<uint8_t>& data) : control_(new Control(Control::Bool)) {
  new (&control_->payload.a_bool) std::vector<uint8_t>(data);
}
ArrayHandle::ArrayHandle(std::vector<uint8_t>&& data) : control_(new Control(Control::Bool)) {
  new (&control_->payload.a_bool) std::vector<uint8_t>(std::move(data));
}
ArrayHandle::ArrayHandle(const std::vector<std::string>& data) : control_(new Control(Control::Token)) {
  new (&control_->payload.a_token) std::vector<std::string>(data);
}
ArrayHandle::ArrayHandle(std::vector<std::string>&& data) : control_(new Control(Control::Token)) {
  new (&control_->payload.a_token) std::vector<std::string>(std::move(data));
}

ArrayHandle::~ArrayHandle() { release(); }
ArrayHandle::ArrayHandle(const ArrayHandle& other) noexcept : control_(other.control_) {
  if (control_ && control_->references.fetch_add(1, std::memory_order_relaxed) ==
                      (std::numeric_limits<size_t>::max)()) {
    std::abort();  // An overflowing count must never free live storage.
  }
}
ArrayHandle::ArrayHandle(ArrayHandle&& other) noexcept : control_(other.control_) {
  other.control_ = nullptr;
}
ArrayHandle& ArrayHandle::operator=(ArrayHandle other) noexcept {
  std::swap(control_, other.control_);
  return *this;
}
void ArrayHandle::release() {
  if (!control_ || control_->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  switch (control_->kind) {
    case Control::Float: control_->payload.a_float.~vector(); break;
    case Control::Int: control_->payload.a_int.~vector(); break;
    case Control::Double: control_->payload.a_double.~vector(); break;
    case Control::Int64: control_->payload.a_int64.~vector(); break;
    case Control::UInt: control_->payload.a_uint.~vector(); break;
    case Control::UInt64: control_->payload.a_uint64.~vector(); break;
    case Control::Bool: control_->payload.a_bool.~vector(); break;
    case Control::Token: control_->payload.a_token.~vector(); break;
  }
  delete control_;
}
void ArrayHandle::detach() {
  if (!control_ || control_->references.load(std::memory_order_acquire) == 1) return;
  // Construct the replacement before releasing the original backing.
  switch (control_->kind) {
    case Control::Float: *this = ArrayHandle(control_->payload.a_float); break;
    case Control::Int: *this = ArrayHandle(control_->payload.a_int); break;
    case Control::Double: *this = ArrayHandle(control_->payload.a_double); break;
    case Control::Int64: *this = ArrayHandle(control_->payload.a_int64); break;
    case Control::UInt: *this = ArrayHandle(control_->payload.a_uint); break;
    case Control::UInt64: *this = ArrayHandle(control_->payload.a_uint64); break;
    case Control::Bool: *this = ArrayHandle(control_->payload.a_bool); break;
    case Control::Token: *this = ArrayHandle(control_->payload.a_token); break;
  }
}
size_t ArrayHandle::size() const {
  if (!control_) return 0;
  switch (control_->kind) {
    case Control::Float: return control_->payload.a_float.size();
    case Control::Int: return control_->payload.a_int.size();
    case Control::Double: return control_->payload.a_double.size();
    case Control::Int64: return control_->payload.a_int64.size();
    case Control::UInt: return control_->payload.a_uint.size();
    case Control::UInt64: return control_->payload.a_uint64.size();
    case Control::Bool: return control_->payload.a_bool.size();
    case Control::Token: return control_->payload.a_token.size();
  }
  return 0;
}
void* ArrayHandle::data() const {
  if (!control_) return nullptr;
  switch (control_->kind) {
    case Control::Float: return control_->payload.a_float.empty() ? nullptr : control_->payload.a_float.data();
    case Control::Int: return control_->payload.a_int.empty() ? nullptr : control_->payload.a_int.data();
    case Control::Double: return control_->payload.a_double.empty() ? nullptr : control_->payload.a_double.data();
    case Control::Int64: return control_->payload.a_int64.empty() ? nullptr : control_->payload.a_int64.data();
    case Control::UInt: return control_->payload.a_uint.empty() ? nullptr : control_->payload.a_uint.data();
    case Control::UInt64: return control_->payload.a_uint64.empty() ? nullptr : control_->payload.a_uint64.data();
    case Control::Bool: return control_->payload.a_bool.empty() ? nullptr : control_->payload.a_bool.data();
    case Control::Token: return control_->payload.a_token.empty() ? nullptr : control_->payload.a_token.data();
  }
  return nullptr;
}
void* ArrayHandle::vector_object() const {
  if (!control_) return nullptr;
  switch (control_->kind) {
    case Control::Float: return &control_->payload.a_float;
    case Control::Int: return &control_->payload.a_int;
    case Control::Double: return &control_->payload.a_double;
    case Control::Int64: return &control_->payload.a_int64;
    case Control::UInt: return &control_->payload.a_uint;
    case Control::UInt64: return &control_->payload.a_uint64;
    case Control::Bool: return &control_->payload.a_bool;
    case Control::Token: return &control_->payload.a_token;
  }
  return nullptr;
}

}  // namespace detail
}  // namespace next
}  // namespace lightusd
