// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - PCP cache lock policy.

#pragma once

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#if defined(LIGHTUSD_FINE_LOCKS)
#include <shared_mutex>
#endif

namespace lightusd {
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
//  * LIGHTUSD_FINE_LOCKS: a std::shared_timed_mutex. Pure reads take a
//    shared lock and builds/writers take the exclusive lock.
//
//  * LIGHTUSD_RECURSIVE_LOCK: a bring-up escape hatch for new re-entrant
//    paths.
#if defined(LIGHTUSD_FINE_LOCKS)
using PcpMutex = std::shared_timed_mutex;
#elif defined(LIGHTUSD_RECURSIVE_LOCK)
using PcpMutex = std::recursive_mutex;
#else
using PcpMutex = std::mutex;
#endif

}  // namespace pcp
}  // namespace lightusd

#if defined(LIGHTUSD_FINE_LOCKS)
#define LIGHTUSD_PCP_READ_LOCK(m) std::shared_lock<lightusd::pcp::PcpMutex> _pcp_rlk(m)
#define LIGHTUSD_PCP_WRITE_LOCK(m) std::unique_lock<lightusd::pcp::PcpMutex> _pcp_wlk(m)
#elif defined(LIGHTUSD_RECURSIVE_LOCK)
#define LIGHTUSD_PCP_READ_LOCK(m) std::lock_guard<lightusd::pcp::PcpMutex> _pcp_lk(m)
#define LIGHTUSD_PCP_WRITE_LOCK(m) std::lock_guard<lightusd::pcp::PcpMutex> _pcp_lk(m)
#else
#define LIGHTUSD_PCP_READ_LOCK(m) std::lock_guard<lightusd::pcp::PcpMutex> _pcp_lk(m)
#define LIGHTUSD_PCP_WRITE_LOCK(m) std::lock_guard<lightusd::pcp::PcpMutex> _pcp_lk(m)
#endif
#define LIGHTUSD_PCP_LOCK(m) LIGHTUSD_PCP_WRITE_LOCK(m)

#else
#define LIGHTUSD_PCP_READ_LOCK(m) (void)0
#define LIGHTUSD_PCP_WRITE_LOCK(m) (void)0
#define LIGHTUSD_PCP_LOCK(m) (void)0
#endif
