// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#pragma once

#include <cstdint>

namespace lightusd {
namespace next {

// Stable, allocation-free operation outcome shared by loading/composition and
// render conversion APIs.
enum class OperationStatus : uint8_t {
  Ok = 0,
  InvalidArgument,
  InvalidData,
  Unsupported,
  ResourceLimit,
  IntegerOverflow,
  AllocationFailure,
  Cancelled,
  Busy,
  StaleRevision,
  SinkRejected,
};

}  // namespace next
}  // namespace lightusd
