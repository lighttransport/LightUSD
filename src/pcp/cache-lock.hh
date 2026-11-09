// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP cache lock policy.

#pragma once

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#if defined(LIGHTUSD_NEXT_FINE_LOCKS)
#include <shared_mutex>
#endif

namespace lightusd {
namespace next {
namespace pcp {

// Serializes the Cache's shared mutable state so it is safe to use
// (ComputePrimIndex / BuildStage / queries / payload edits) from multiple
// threads. Compiles to nothing in non-threaded builds.
//
// Three lock policies, selected at compile time:
//
//  * default: a single non-recursive std::mutex. Public entry points take the
//    lock exactly once and delegate to lock-free `*_locked` internals.
//
//  * LIGHTUSD_NEXT_FINE_LOCKS: a std::shared_timed_mutex. Pure reads take a
//    shared lock and builds/writers take the exclusive lock.
//
//  * LIGHTUSD_NEXT_RECURSIVE_LOCK: a bring-up escape hatch for new re-entrant
//    paths.
#if defined(LIGHTUSD_NEXT_FINE_LOCKS)
using PcpMutex = std::shared_timed_mutex;
#elif defined(LIGHTUSD_NEXT_RECURSIVE_LOCK)
using PcpMutex = std::recursive_mutex;
#else
using PcpMutex = std::mutex;
#endif

}  // namespace pcp
}  // namespace next
}  // namespace lightusd

#if defined(LIGHTUSD_NEXT_FINE_LOCKS)
#define NEXT_PCP_READ_LOCK(m) std::shared_lock<lightusd::next::pcp::PcpMutex> _pcp_rlk(m)
#define NEXT_PCP_WRITE_LOCK(m) std::unique_lock<lightusd::next::pcp::PcpMutex> _pcp_wlk(m)
#elif defined(LIGHTUSD_NEXT_RECURSIVE_LOCK)
#define NEXT_PCP_READ_LOCK(m) std::lock_guard<lightusd::next::pcp::PcpMutex> _pcp_lk(m)
#define NEXT_PCP_WRITE_LOCK(m) std::lock_guard<lightusd::next::pcp::PcpMutex> _pcp_lk(m)
#else
#define NEXT_PCP_READ_LOCK(m) std::lock_guard<lightusd::next::pcp::PcpMutex> _pcp_lk(m)
#define NEXT_PCP_WRITE_LOCK(m) std::lock_guard<lightusd::next::pcp::PcpMutex> _pcp_lk(m)
#endif
#define NEXT_PCP_LOCK(m) NEXT_PCP_WRITE_LOCK(m)

#else
#define NEXT_PCP_READ_LOCK(m) (void)0
#define NEXT_PCP_WRITE_LOCK(m) (void)0
#define NEXT_PCP_LOCK(m) (void)0
#endif
