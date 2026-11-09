// SPDX-License-Identifier: Apache-2.0
#include "byte-budget.hh"

#include <cassert>
#include <cstdint>
#include <limits>
#include <atomic>

int main() {
  using lusdql::budget_detail::FitsQueueBudget;
  using lusdql::budget_detail::CheckedMulSize;
  using lusdql::budget_detail::SaturatingAdd;
  using lusdql::budget_detail::SaturatingMul;
  using lusdql::budget_detail::TryReserve;
  constexpr uint64_t max = (std::numeric_limits<uint64_t>::max)();

  assert(FitsQueueBudget(8, 2, 10));
  assert(!FitsQueueBudget(8, 3, 10));
  assert(!FitsQueueBudget(11, 0, 10));
  assert(FitsQueueBudget(max - 1, 1, max));
  assert(!FitsQueueBudget(max - 1, 2, max));

  assert(SaturatingAdd(10, 20) == 30);
  assert(SaturatingAdd(max - 1, 2) == max);
  assert(SaturatingMul(7, 9) == 63);
  assert(SaturatingMul(max / 2 + 1, 2) == max);

  size_t product = 0;
  assert(CheckedMulSize(13, 7, &product) && product == 91);
  assert(!CheckedMulSize((std::numeric_limits<size_t>::max)(), 2, &product));

  std::atomic<uint64_t> used{8};
  assert(TryReserve(used, 2, 10));
  assert(used.load() == 10);
  assert(!TryReserve(used, 1, 10));
  used.store(max - 1);
  assert(!TryReserve(used, 2, max));
  assert(used.load() == max - 1);
  return 0;
}
