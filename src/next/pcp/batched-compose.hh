// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - batched composition (low-memory flatten).
//
// Composes a stage in namespace batches instead of all at once. The composed
// namespace is cut at a frontier of subtree roots found top-down; each batch
// composes a run of consecutive frontier roots (in namespace pre-order) plus
// their ancestors, with a fresh Cache over one shared LayerRegistry, so the
// batch's composition sources and its composed prims can be freed before the
// next batch. Instancing carries across batches: a prototype claimed in an
// earlier batch makes every later member an instance, and a pre-pass records
// which Holder prototypes have peers in other batches. Concatenating the
// batches (minus repeated ancestors) gives the same prims, in the same order,
// as one ComposeStageFromFile.

#pragma once

#include "cache.hh"
#include "../crate/crate-writer.hh"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lightusd {
namespace next {
namespace pcp {

struct BatchedComposeOptions {
  // The top-down discovery expands whole namespace levels (active,
  // non-instanceable prims with children) while the next level stays within
  // this many frontier roots.
  size_t max_frontier = 65536;
  // Pass 1 sizes its batches from the previous batch's prims per root; pass 2
  // cuts them from pass 1's exact counts.
  // 400 K keeps full45 (10.2 M prims) near 12 GiB; smaller batches lower
  // the peak little (the writer's tables dominate) and cost time.
  size_t target_batch_prims = 400000;
  // Fixed frontier roots per batch instead (testing); 0 = by prim target.
  size_t roots_per_batch = 0;
  // Run the per-batch callback (the writer) on its own thread while the next
  // batch composes; one more batch is alive at a time. Threaded builds only.
  bool pipeline = true;
};

// One composed batch, in namespace pre-order.
struct ComposedBatch {
  size_t index = 0;
  // The batch's ancestors ("spine" prims) followed by its subtrees. A spine
  // prim links only the children inside this batch.
  std::unique_ptr<Layer> layer;
  // Per prim of `layer`: a spine prim, and a spine prim some earlier batch
  // already produced.
  std::vector<uint8_t> spine;
  std::vector<uint8_t> repeated;
  // Spine prim path -> its full child names (some may be in other batches).
  std::unordered_map<std::string, std::vector<std::string>> full_children;
};

class BatchedComposer {
 public:
  BatchedComposer();
  ~BatchedComposer();
  BatchedComposer(const BatchedComposer &) = delete;
  BatchedComposer &operator=(const BatchedComposer &) = delete;

  // Loads the root layer and discovers the frontier.
  bool Open(const std::string &filename, AssetResolver &resolver,
            const CompositionOptions &options,
            const BatchedComposeOptions &batch_options, std::string *warn,
            std::string *err);

  // True when the stage is not split (no composition work, or a mode the
  // batching does not cover): Run() yields one batch holding the whole stage.
  bool whole() const;
  size_t frontier_size() const;
  size_t batch_count() const;
  // Loaded source identifiers, for dependency manifests after Plan().
  std::vector<std::string> source_identifiers() const;

  // Pass 1: composes every batch and discards it, recording the cross-batch
  // instancing facts (Holder prototypes with peers elsewhere). `on_batch`
  // (optional) sees each batch before it is freed.
  bool Plan(std::string *warn, std::string *err,
            const std::function<bool(ComposedBatch &)> &on_batch = {});

  // Pass 2: composes every batch again, in order, and hands each to
  // `on_batch`, which may move from it. Requires Plan().
  bool Run(const std::function<bool(ComposedBatch &)> &on_batch,
           std::string *warn, std::string *err);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// Debug/verification helper: Plan + Run, concatenating the batches into one
// composed stage. Same result as ComposeStageFromFile (no memory saving).
bool ComposeStageBatched(const std::string &filename, AssetResolver &resolver,
                         Stage *out_stage, const CompositionOptions &options,
                         const BatchedComposeOptions &batch_options,
                         std::string *warn = nullptr, std::string *err = nullptr);

// Low-memory flatten to a USDC file: composes in batches (Plan, then Run)
// and streams each batch into one CrateWriter (CrateWriter::AddBatchTables /
// WriteBatch), so neither the whole composed stage nor its composition
// sources are ever resident at once. The file is byte-identical to
// ComposeStageFromFile + WriteLayerToFile. `result` gets the writer's result.
bool FlattenToUSDCFileBatched(const std::string &filename,
                              AssetResolver &resolver,
                              const std::string &out_path,
                              const CompositionOptions &options,
                              const BatchedComposeOptions &batch_options,
                              const CrateWriteOptions &write_options,
                              CrateWriteResult *result,
                              std::string *warn = nullptr,
                              std::string *err = nullptr);

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
