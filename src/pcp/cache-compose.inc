// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache opinion composition and lazy prim/index composition.
// Included inside Cache::Impl.

  std::vector<std::string> ComposeInto(const std::vector<Src> &srcs,
                                        PrimSpec *out) ;
  // ComposeInto's Pass 1 (opinion merge), factored out so the parallel compose
  // can fill PrimSpec opinion slots independently after a serial structure pass
  // has fixed every prim's index/parent/child-order. Writes only into `out`
  // (and interns property names into the shared, locked PropNameTable), so
  // distinct slots fill concurrently without interfering.
  void ComposeOpinions(const std::vector<Src> &srcs, PrimSpec *out);

  // Resolved `active` for a prim's sources: strongest authored opinion wins
  // (fill-absent, mirroring ComposeOpinions' metadata merge — a selected
  // variant authors active only when it deactivates). Used by BuildStage to
  // prune the subtree of a deactivated prim WITHOUT composing its opinions
  // (those are deferred to the parallel fill pass).
  bool ComposedActive(const std::vector<Src> &srcs) ;

  // Composed child-name ORDER for a prim's sources (weak->strong): iterate sources
  // and their layer-stack specs in reverse (weakest first); within a single layer
  // the primChildren are in authored (forward) order. Append unseen names. This is
  // ComposeInto's Pass 2, factored out so the parallel source-warming pre-pass
  // discovers exactly the same child namespace the serial compose walks.
  std::vector<std::string> ComposeChildNames(const std::vector<Src> &srcs) ;

  // --- lazy per-prim composition (Phase 10) -------------------------------

  // Compose just `prim_path` on first access (cached), reusing the exact
  // source-resolution + opinion-merge of BuildStageRec's per-prim step, but
  // without walking/emitting the whole namespace. Returns nullptr if the prim
  // authors no opinions. Assumes api_mu_ is held.
  const PrimSpec *ComposePrim_locked(const Path &prim_path, std::string *warn,
                                     std::string *err) ;

  // --- ComputePrimIndex ---------------------------------------------------

  // Public entry: takes the lock once, then runs the lock-free worker.
  const PrimIndex *ComputePrimIndex(const Path &prim_path, std::string *warn,
                                    std::string *err) ;

  // Assumes api_mu_ is already held. Internal callers (LoadPayload /
  // UnloadPayload / the serial PrewarmPrimIndices path) use this directly so the
  // lock is never re-entered (F6: the mutex is non-recursive).
  const PrimIndex *ComputePrimIndex_locked(const Path &prim_path,
                                           std::string *warn, std::string *err) ;
