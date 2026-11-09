// SPDX-License-Identifier: Apache 2.0
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// RAII Memory Budget Manager for LightUSD
//
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <limits>

#include "nonstd/expected.hpp"

namespace lightusd {

// Counters are atomic: budget reserve/release runs concurrently during
// parallel USDC prim reconstruction (relaxed ordering — the budget is an
// approximate soft cap, not a synchronization point).
class MemoryBudgetManager {
 public:
  explicit MemoryBudgetManager(uint64_t max_budget = (std::numeric_limits<uint32_t>::max)())
      : max_budget_(max_budget), current_usage_(0), peak_usage_(0) {}

  bool CheckAndReserve(uint64_t requested_bytes) {
    // Guard against uint64_t overflow in the addition.
    uint64_t cur = current_usage_.load(std::memory_order_relaxed);
    do {
      if (requested_bytes > max_budget_ - cur) {
        return false;
      }
    } while (!current_usage_.compare_exchange_weak(
        cur, cur + requested_bytes, std::memory_order_relaxed));

    const uint64_t now = cur + requested_bytes;
    uint64_t peak = peak_usage_.load(std::memory_order_relaxed);
    while (now > peak && !peak_usage_.compare_exchange_weak(
                             peak, now, std::memory_order_relaxed)) {
    }
    return true;
  }

  // Non-RAII version: reserves budget and keeps it until Release() is called.
  // Use this with explicit Release() calls (via REDUCE_MEMORY_USAGE macro).
  bool Reserve(uint64_t requested_bytes) {
    return CheckAndReserve(requested_bytes);
  }

  void Release(uint64_t bytes_to_release) {
    uint64_t cur = current_usage_.load(std::memory_order_relaxed);
    uint64_t next;
    do {
      next = (cur >= bytes_to_release) ? (cur - bytes_to_release) : 0;
    } while (!current_usage_.compare_exchange_weak(cur, next,
                                                   std::memory_order_relaxed));
  }

  uint64_t GetCurrentUsage() const {
    return current_usage_.load(std::memory_order_relaxed);
  }
  uint64_t GetPeakUsage() const {
    return peak_usage_.load(std::memory_order_relaxed);
  }
  uint64_t GetMaxBudget() const { return max_budget_; }
  uint64_t GetRemainingBudget() const {
    const uint64_t cur = GetCurrentUsage();
    return (cur <= max_budget_) ? (max_budget_ - cur) : 0;
  }

  size_t GetUsageInMB() const { return size_t(GetCurrentUsage() / (1024 * 1024)); }
  size_t GetPeakUsageInMB() const { return size_t(GetPeakUsage() / (1024 * 1024)); }

  void Reset() {
    current_usage_.store(0, std::memory_order_relaxed);
    peak_usage_.store(0, std::memory_order_relaxed);
  }

  class ScopedReservation {
   public:
    ScopedReservation(MemoryBudgetManager& manager, uint64_t bytes)
        : manager_(manager), bytes_(bytes), reserved_(false) {
      reserved_ = manager_.CheckAndReserve(bytes);
    }

    ~ScopedReservation() {
      if (reserved_) {
        manager_.Release(bytes_);
      }
    }

    bool IsReserved() const { return reserved_; }

    ScopedReservation(const ScopedReservation&) = delete;
    ScopedReservation& operator=(const ScopedReservation&) = delete;

    ScopedReservation(ScopedReservation&& other) noexcept
        : manager_(other.manager_), bytes_(other.bytes_), reserved_(other.reserved_) {
      other.reserved_ = false;
    }

    ScopedReservation& operator=(ScopedReservation&& other) noexcept {
      if (this != &other) {
        if (reserved_) {
          manager_.Release(bytes_);
        }
        bytes_ = other.bytes_;
        reserved_ = other.reserved_;
        other.reserved_ = false;
      }
      return *this;
    }

   private:
    MemoryBudgetManager& manager_;
    uint64_t bytes_;
    bool reserved_;
  };

  ScopedReservation ReserveScoped(uint64_t bytes) {
    return ScopedReservation(*this, bytes);
  }

 private:
  uint64_t max_budget_;
  std::atomic<uint64_t> current_usage_;
  std::atomic<uint64_t> peak_usage_;
};

template <typename ReturnType>
class MemoryBudgetGuard {
 public:
  MemoryBudgetGuard(MemoryBudgetManager& manager, uint64_t bytes,
                    const std::string& error_tag = "")
      : reservation_(manager.ReserveScoped(bytes)), error_tag_(error_tag) {}

  template <typename T>
  nonstd::expected<T, std::string> CheckAndReturn(T&& value) {
    if (!reservation_.IsReserved()) {
      return nonstd::make_unexpected(error_tag_.empty()
        ? "Reached maximum memory budget"
        : error_tag_ + ": Reached maximum memory budget");
    }
    return std::forward<T>(value);
  }

  bool IsValid() const { return reservation_.IsReserved(); }

 private:
  MemoryBudgetManager::ScopedReservation reservation_;
  std::string error_tag_;
};

// Non-RAII check: reserves budget persistently until a matching
// REDUCE_MEMORY_USAGE / Release() call. Fixes the original RAII-guard
// version which released the reservation immediately (before the actual
// allocation), providing no real protection.
#define MEMORY_BUDGET_CHECK(manager, bytes, error_tag) \
  do { \
    if (!(manager).Reserve(bytes)) { \
      PUSH_ERROR_AND_RETURN_TAG(error_tag, "Reached maximum memory budget"); \
    } \
  } while(0)

#define MEMORY_BUDGET_CHECK_AND_RETURN(manager, bytes, error_tag, return_type) \
  MemoryBudgetGuard<return_type>(manager, bytes, error_tag)

}  // namespace lightusd
