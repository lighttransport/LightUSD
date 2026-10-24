// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache stage materialization and instance flattening.
// Included inside Cache::Impl.

  // --- BuildStage ---------------------------------------------------------

  // `out_path` is where the composed prim is placed and where its sources are
  // looked up (the walk keys sources by composed path). `src_path` is unused
  // by the walk and left empty for children.
  struct BuildStageWork {
    Path src_path;
    Path out_path;
    uint32_t parent_idx = 0;
    bool is_root = false;
    uint32_t depth = 0;
  };

  // Temporary internal-breakdown profiling accumulators (ns), summed across all
  // root subtrees; printed by the top-level BuildStage under LIGHTUSD_NEXT_TIMING.
  mutable uint64_t prof_sources_ns_ = 0;
  mutable uint64_t prof_compose_ns_ = 0;
  mutable uint64_t prof_reg_ns_ = 0;
  // Walk-only attribution (temporary, LIGHTUSD_NEXT_TIMING): child-name
  // composition, instance registration + prototype lookup, prim publication and
  // child-work push.
  mutable uint64_t prof_childnames_ns_ = 0;
  mutable uint64_t prof_protomap_ns_ = 0;
  mutable uint64_t prof_addprim_ns_ = 0;
  mutable uint64_t prof_push_ns_ = 0;
  mutable uint64_t prof_source_hits_ = 0;
  mutable uint64_t prof_source_misses_ = 0;
  mutable uint64_t prof_structure_nodes_ = 0;

  void BuildStage(Layer *out, const BuildStageWork &root_work,
                  std::string *warn, std::string *err) ;
