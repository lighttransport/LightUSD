// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - private USDA ASCII parser implementation details.

#pragma once

#include "ascii-parser.hh"
#include "lexer.hh"
#include "value-parser.hh"
#include "../crate/lazy-array.hh"
#include "../layer/layer.hh"

#include <memory>
#include <string>
#include <vector>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <atomic>
#include <condition_variable>
#include <mutex>
#endif

namespace lightusd {
namespace next {

#if defined(LIGHTUSD_ENABLE_THREAD)
/// One dispatched run of sibling prim subtrees, parsed by a worker into its
/// own Layer fragment (one root per distinct sibling) and spliced into the
/// main layer after the join. Owned via unique_ptr in the state's fragment list (stable address);
/// a worker writes only its own fragment.
struct SubtreeFragment {
  std::unique_ptr<Layer> layer;
  // Warnings the sub-parser emitted, spliced into the main list at the
  // position the serial parse would have produced them.
  std::vector<std::string> warnings;
  // Main-parser warnings_.size() and main layer prim_count() at dispatch.
  size_t warning_pos = 0;
  size_t prim_pos = 0;
  // Absolute paths of the fragment roots (duplicate-sibling check at join).
  std::vector<std::string> root_paths;
};

/// Shared state of the parallel prim-subtree parse, owned by the root parser.
/// `fragments` is touched by the main thread only (dispatch order = authored
/// order); workers touch only their own SubtreeFragment plus the counters.
struct SubtreeParseState {
  std::mutex mu;
  std::condition_variable cv;
  size_t inflight = 0;
  size_t max_inflight = 0;
  size_t max_block_bytes = 0;  // capture ceiling (larger prims parse inline)
  std::atomic<bool> failed{false};
  std::vector<std::unique_ptr<SubtreeFragment>> fragments;
};
#endif

class AsciiParser::Impl {
public:
  explicit Impl(const ParseOptions& options) : options_(options) {}

  bool Parse(const char* data, size_t length);
  bool ParseOwned(std::string&& data);
  bool ParseWithSource(const char* data, size_t length,
                      std::shared_ptr<LazyArraySource> source);
  bool ParseFile(const char* filename);

  Stage TakeStage() { return std::move(stage_); }
  const std::vector<ParseError>& GetErrors() const { return errors_; }
  bool HasErrors() const { return !errors_.empty(); }
  const std::vector<std::string>& GetWarnings() const { return warnings_; }
  bool UsedFastPath() const { return used_fast_path_; }

private:
  // One full parse attempt. `fast` enables the batched deferred-array parse
  // and the parallel prim-subtree parse; any failure of a fast attempt is
  // re-run with fast=false so every diagnostic comes from the serial parser.
  bool ParseAttempt(const char* data, size_t length, bool fast);

  ParseOptions options_;
  bool used_fast_path_ = false;
  Stage stage_;
  std::vector<ParseError> errors_;
  std::vector<std::string> warnings_;
  std::shared_ptr<LazyArraySource> source_;
  size_t parse_length_ = 0;

  // Parsing state
  std::unique_ptr<Lexer> lexer_;
  std::unique_ptr<Layer> layer_;
  std::unique_ptr<LayerBuilder> builder_;
  size_t depth_ = 0;
  // The layer prim blocks may be dispatched into (variant bodies swap layer_
  // to a content layer; prims there always parse inline).
  Layer* dispatch_layer_ = nullptr;
  // Capture attempts are skipped below this input offset: the enclosing prim
  // block was measured smaller than the dispatch floor, so every descendant
  // is smaller still.
  size_t no_capture_until_ = 0;
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Batched deferred numeric-array parse (ParseOptions::async_arrays). Shared
  // with prim-subtree sub-parsers (Enqueue is thread-safe); drained before
  // finalize and on every exit path (spans point into the input).
  std::shared_ptr<DeferredArrayScheduler> deferred_arrays_;
  // Parallel prim-subtree parse (ParseOptions::parallel_prims). Root parser
  // only; null in sub-parsers.
  std::shared_ptr<SubtreeParseState> subtree_state_;
  // Parse one captured prim block into `fragment` (worker or inline).
  static void RunSubtreeParse(const ParseOptions& options,
                              std::shared_ptr<DeferredArrayScheduler> arrays,
                              SubtreeParseState* st, const char* data,
                              size_t len, size_t start_line,
                              size_t start_column, size_t base_depth,
                              const std::string& parent_path,
                              SubtreeFragment* fragment);
  // Wait for every dispatched subtree; false if any failed.
  bool WaitSubtreeTasks();
  // Splice fragments/warnings into the main layer. False on any
  // inconsistency (caller re-parses serially).
  bool StitchSubtreeFragments();
#endif
  // ParsePrim, or capture the block and dispatch it to a worker when the
  // parallel subtree parse is active and the block is in the size window.
  bool ParsePrimMaybeParallel();
  // Array value for attribute defaults / timeSamples: deferred to the worker
  // pool when possible (*out_deferred = true), else parsed synchronously.
  ParseResult ParseArrayAttributeValue(TypeId type_id, bool* out_deferred);

  // Parsing methods
  bool ParseStageMetadata();
  bool ParsePrim();
  bool ParsePrimContents();
  bool ParseAttribute(
      PrimSpec::RelationshipListOp connection_op =
          PrimSpec::RelationshipListOp::Append,
      bool explicit_connection = true,
      const std::string& preconsumed_type = std::string());
  bool ParseRelationship(PrimSpec::RelationshipListOp op =
                             PrimSpec::RelationshipListOp::Append,
                         bool explicit_list = true, uint16_t flags = 0);
  bool ParseMetadataBlock();
  bool ParseTimeSamples(const std::string& prop_name, TypeId type_id,
                        bool is_array);
  bool ParseVariantSetBody(const std::string& variant_set_name);
  bool ParseVariantSetBodyInto(const std::string& variant_set_name,
                               std::vector<VariantSetData>& target, int depth);
  bool ParseVariantOption(VariantData* out, int depth);
  // Read one composition-arc reference into canonical "@asset@</prim>" /
  // "</prim>" form (with an optional `?layerOffset=off:scale` suffix). Shared by
  // prim-metadata arcs and variant-option arcs. Returns false if no arc token.
  bool ReadArcRef(std::string* out);
  bool ParseNamespacedName(std::string* out, const char* what);
  bool ParseOrderList(std::vector<std::string>* out);
  bool SkipBalancedBlock(TokenType open, TokenType close, size_t depth = 0);
  bool SkipValueLike();
  void SkipPropertyMetadata();
  void ParsePropertyMetadata(const std::string& prop_name);

  void AddError(const std::string& message);
  void AddWarning(const std::string& message);
  bool ReportProgress(const char* phase, size_t current, size_t total);

  // Token helpers
  bool Match(TokenType type);
  bool Check(TokenType type);
  bool AtEnd();
};

bool IsNameToken(const Token& tok);

}  // namespace next
}  // namespace lightusd
