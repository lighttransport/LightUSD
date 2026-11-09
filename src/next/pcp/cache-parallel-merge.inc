// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache parallel worker merge helpers.
// Included inside Cache::Impl.

  void MergeWorkerIndex(Impl &w, const std::string &key) ;

  void MergeSources(Impl &w, size_t seed_stack_count, size_t seed_pool_count) ;
