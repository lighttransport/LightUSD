// SPDX-License-Identifier: MIT
// Copyright 2024 - Present : Syoyo Fujita
//
// Simple stack container class for custom vector/string.
// Inspired from
// - https://github.com/p-ranav/small_vector
// - https://chromium.googlesource.com/chromium/chromium/+/master/base/stack_container.h

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace lightusd {

// Simple small vector optimization implementation.
// Pre-allocates N elements on the stack, falls back to heap for larger sizes.
template <typename T, size_t N>
class StackVector {
 public:
  StackVector() : _size(0), _capacity(N), _use_heap(false), _heap() {}

  ~StackVector() {
    clear();
  }

  StackVector(const StackVector &rhs) : _size(0), _capacity(N), _use_heap(false), _heap() {
    reserve(rhs._size);
    for (size_t i = 0; i < rhs._size; ++i) {
      emplace_back(rhs[i]);
    }
  }

  StackVector &operator=(const StackVector &rhs) {
    if (this != &rhs) {
      clear();
      reserve(rhs._size);
      for (size_t i = 0; i < rhs._size; ++i) {
        emplace_back(rhs[i]);
      }
    }
    return *this;
  }

  // Move constructor
  StackVector(StackVector &&rhs) noexcept : _size(0), _capacity(N), _use_heap(false), _heap() {
    if (rhs._use_heap) {
      _heap = std::move(rhs._heap);
      _use_heap = true;
      _size = rhs._size;
      _capacity = rhs._capacity;
      rhs._size = 0;
      rhs._capacity = N;
      rhs._use_heap = false;
    } else {
      for (size_t i = 0; i < rhs._size; ++i) {
        emplace_back(std::move(rhs._stack_data()[i]));
      }
      rhs.clear();
    }
  }

  // Move assignment
  StackVector &operator=(StackVector &&rhs) noexcept {
    if (this != &rhs) {
      clear();
      if (rhs._use_heap) {
        _heap = std::move(rhs._heap);
        _use_heap = true;
        _size = rhs._size;
        _capacity = rhs._capacity;
        rhs._size = 0;
        rhs._capacity = N;
        rhs._use_heap = false;
      } else {
        for (size_t i = 0; i < rhs._size; ++i) {
          emplace_back(std::move(rhs._stack_data()[i]));
        }
        rhs.clear();
      }
    }
    return *this;
  }

  void reserve(size_t new_cap) {
    if (_use_heap) {
      _heap.reserve(new_cap);
      _capacity = _heap.capacity();
    } else if (new_cap > N) {
      // Move to heap
      _heap.reserve(new_cap);
      for (size_t i = 0; i < _size; ++i) {
        _heap.emplace_back(std::move(_stack_data()[i]));
        _stack_data()[i].~T();
      }
      _use_heap = true;
      _capacity = _heap.capacity();
    }
    // If new_cap <= N and still using stack, nothing to do
  }

  template <typename... Args>
  void emplace_back(Args &&... args) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
    if (_use_heap) {
      _heap.emplace_back(std::forward<Args>(args)...);
      _size = _heap.size();
      _capacity = _heap.capacity();
    } else if (_size < N) {
      new (&_stack_data()[_size]) T(std::forward<Args>(args)...);
      ++_size;
    } else {
      // Need to move to heap
      _heap.reserve(N * 2);
      for (size_t i = 0; i < _size; ++i) {
        _heap.emplace_back(std::move(_stack_data()[i]));
        _stack_data()[i].~T();
      }
      _heap.emplace_back(std::forward<Args>(args)...);
      _use_heap = true;
      _size = _heap.size();
      _capacity = _heap.capacity();
    }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  }

  void push_back(const T &val) {
    emplace_back(val);
  }

  void push_back(T &&val) {
    emplace_back(std::move(val));
  }

  void pop_back() {
    if (_size == 0) return;
    if (_use_heap) {
      _heap.pop_back();
      _size = _heap.size();
    } else {
      --_size;
      _stack_data()[_size].~T();
    }
  }

  void clear() {
    if (_use_heap) {
      _heap.clear();
      _size = 0;
    } else {
      for (size_t i = 0; i < _size; ++i) {
        _stack_data()[i].~T();
      }
      _size = 0;
    }
  }

  T &back() {
    if (_use_heap) {
      return _heap.back();
    } else {
      return _stack_data()[_size - 1];
    }
  }

  const T &back() const {
    if (_use_heap) {
      return _heap.back();
    } else {
      return _stack_data()[_size - 1];
    }
  }

  T &operator[](size_t i) {
    if (_use_heap) {
      return _heap[i];
    } else {
      return _stack_data()[i];
    }
  }

  const T &operator[](size_t i) const {
    if (_use_heap) {
      return _heap[i];
    } else {
      return _stack_data()[i];
    }
  }

  size_t size() const { return _size; }
  size_t capacity() const { return _capacity; }
  bool empty() const { return _size == 0; }

  T *data() {
    if (_use_heap) {
      return _heap.data();
    } else {
      return _stack_data();
    }
  }

  const T *data() const {
    if (_use_heap) {
      return _heap.data();
    } else {
      return _stack_data();
    }
  }

  T *begin() { return data(); }
  const T *begin() const { return data(); }
  T *end() { return data() + _size; }
  const T *end() const { return data() + _size; }

 private:
  T *_stack_data() { return reinterpret_cast<T *>(_stack_buf); }
  const T *_stack_data() const { return reinterpret_cast<const T *>(_stack_buf); }

  alignas(T) char _stack_buf[sizeof(T) * N];
  size_t _size;
  size_t _capacity;
  bool _use_heap;
  std::vector<T> _heap;
};

}  // namespace lightusd
