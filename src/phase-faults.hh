// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - per-phase page-fault attribution.
//
// The composed all-district layer reaches ~75 GiB, so "is this phase compute
// bound or residency bound?" is the first question a phase timing has to
// answer. These counters are only read when an enable_timing option is on.

#pragma once
#include "core/host-services.hh"

namespace lightusd {

using FaultCounters = HostFaultCounters;

inline FaultCounters ReadFaultCounters(
    const std::shared_ptr<const HostServices>& services = {}) {
  return services && services->fault_counters
      ? services->fault_counters() : FaultCounters{};
}

}  // namespace lightusd
