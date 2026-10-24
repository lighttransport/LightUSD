// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - per-phase page-fault attribution.
//
// The composed all-district layer reaches ~75 GiB, so "is this phase compute
// bound or residency bound?" is the first question a phase timing has to
// answer. These counters are only read when an enable_timing option is on.

#pragma once

namespace lightusd {
namespace next {

struct FaultCounters {
  unsigned long minor = 0;
  unsigned long major = 0;
};

#if !defined(__EMSCRIPTEN__)
}  // namespace next
}  // namespace lightusd

#include <sys/resource.h>

namespace lightusd {
namespace next {
#endif

inline FaultCounters ReadFaultCounters() {
  FaultCounters out;
#if !defined(__EMSCRIPTEN__)
  struct rusage usage {};
  if (::getrusage(RUSAGE_SELF, &usage) == 0) {
    out.minor = static_cast<unsigned long>(usage.ru_minflt);
    out.major = static_cast<unsigned long>(usage.ru_majflt);
  }
#endif
  return out;
}

}  // namespace next
}  // namespace lightusd
