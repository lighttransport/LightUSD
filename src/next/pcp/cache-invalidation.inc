// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache invalidation, payload loading, and load rules.
// Included inside Cache::Impl.

  std::string ComputeInstanceKey(const Path &path, std::string *warn,
                                 std::string *err) ;
  // --- invalidation -------------------------------------------------------

  void DropIndex(const std::string &key) ;

  void Invalidate(const Path &prim_path) ;

  // Assumes api_mu_ is already held (called by LoadPayload / UnloadPayload).
  void Invalidate_locked(const Path &prim_path) ;

  void InvalidateLayer(const std::string &layer_id) ;

  bool ReloadLayer(const std::string &layer_id, std::string *warn,
                   std::string *err) ;

  // --- payloads -----------------------------------------------------------

  bool LoadPayload(const Path &prim_path, bool with_descendants,
                   std::string *warn, std::string *err) ;

  bool LoadPayloads(const std::vector<Path> &prim_paths,
                    bool with_descendants) ;

  bool UnloadPayload(const Path &prim_path) ;

  void SetLoadRules(const LoadRules &rules) ;

  void SetVariantSelections(
      const CompositionOptions::VariantSelectionMap &selections) ;
