// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Value Parser
// Parses USD values from tokenized input

#pragma once

#include "../types/value.hh"
#include <memory>
#include <string>
#include <vector>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <functional>
#include <mutex>
#endif

namespace lightusd {
namespace next {

class Lexer;
class LazyArraySource;
struct ParseArrayContext {
  /// Retained USDA source backing lazy arrays.
  std::shared_ptr<LazyArraySource> source;

  /// Copy of parser options relevant to lazy USDA array behavior.
  bool enable_usda_lazy_arrays = false;
  /// Hard cap for lazy USDA array element count; 0 means no cap.
  size_t max_usda_lazy_array_elements = (static_cast<size_t>(1) << 30);
  int num_threads = 0;
};

/// Result of a parse operation
struct ParseResult {
  bool success = false;
  Value value;
  std::string error;

  /// Construct success result
  static ParseResult Ok(Value v) {
    ParseResult r;
    r.success = true;
    r.value = std::move(v);
    return r;
  }

  /// Construct error result
  static ParseResult Error(const std::string& msg) {
    ParseResult r;
    r.success = false;
    r.error = msg;
    return r;
  }
};

/// Parse a value of the expected type from the lexer
/// The lexer should be positioned at the start of the value
ParseResult ParseValue(Lexer& lexer, TypeId expected_type);

/// Parse an array value of the expected element type
/// The lexer should be positioned at the opening bracket
ParseResult ParseArrayValue(Lexer& lexer, TypeId element_type,
                            const ParseArrayContext& context = {});

class DeferredArrayScheduler;

/// Array-value parse that may DEFER the numeric conversion: for a simple
/// numeric array literal whose element count can be predicted exactly from the
/// capture (comma tally), returns a committed-now Value::MakeDeferredArray
/// placeholder and hands the captured span to `scheduler` (*out_deferred =
/// true); the payload is filled by a pool worker before the scheduler's drain
/// barrier. Everything else — and every call with a null scheduler or with
/// lazy USDA arrays enabled — behaves exactly like ParseArrayValue.
ParseResult ParseArrayValueMaybeDeferred(Lexer& lexer, TypeId element_type,
                                         const ParseArrayContext& context,
                                         DeferredArrayScheduler* scheduler,
                                         bool* out_deferred);

#if defined(LIGHTUSD_ENABLE_THREAD)

/// Submit a task to the shared USDA parser worker pool (the pool the deferred
/// array batches run on). Returns false — task NOT taken — when the pool is
/// unavailable (num_threads == 1 / no hardware parallelism). Tasks must never
/// block on other pool work.
bool SubmitPoolTask(int num_threads, std::function<void()> task);

/// Resolved worker count for a ParseOptions::num_threads hint (0 = auto =
/// min(hardware_concurrency, 8); 1 = serial).
int ResolveUsdaParseThreads(int requested);

/// One captured simple array awaiting deferred (worker-pool) parsing. `data`
/// points into the parser's input buffer (alive until the parse's drain
/// barrier); `fill` completes the pre-committed Value's payload.
struct DeferredArrayItem {
  const char* data = nullptr;     // span incl. the outer '[' and ']'
  size_t len = 0;
  size_t expected_scalars = 0;    // exact for a well-formed simple array
  TypeId type_id = TypeId::Invalid;
  Value::DeferredArrayFill fill;
};

/// Batches deferred array parses onto the shared parser worker pool so the
/// numeric conversion overlaps lexing. Per-array tasks are far too fine
/// (millions of ~KB arrays); items are accumulated into multi-hundred-KB
/// batches before submission. Enqueue is thread-safe (the parser main thread
/// AND parallel prim-subtree sub-parsers produce concurrently); Drain() is the
/// join barrier.
class DeferredArrayScheduler {
 public:
  /// Returns nullptr when the worker pool is unavailable.
  static std::unique_ptr<DeferredArrayScheduler> Create(int num_threads);
  ~DeferredArrayScheduler();

  DeferredArrayScheduler(const DeferredArrayScheduler&) = delete;
  DeferredArrayScheduler& operator=(const DeferredArrayScheduler&) = delete;

  void Enqueue(DeferredArrayItem item);

  /// Submit any partial batch and wait for all in-flight batches. Returns
  /// false if any deferred array failed to parse or did not match its
  /// predicted shape (the caller then re-parses serially, which reproduces
  /// the exact synchronous diagnostics). Idempotent; call only when no
  /// producer can still Enqueue.
  bool Drain();

  /// Cheap failure probe so producers can stop deferring early.
  bool failed() const;

  struct Shared;

 private:
  explicit DeferredArrayScheduler(int num_threads);
  void DispatchBatch(std::vector<DeferredArrayItem> batch);

  std::shared_ptr<Shared> shared_;
  int num_threads_ = 0;
  size_t max_inflight_ = 0;
  // Producer-side accumulation, guarded for concurrent producers. The lock is
  // held only for the push/swap (never across a batch parse or pool submit).
  std::mutex enqueue_mu_;
  std::vector<DeferredArrayItem> current_;
  size_t current_bytes_ = 0;
};

#endif  // LIGHTUSD_ENABLE_THREAD

/// Parse a generic value, inferring the type from syntax
/// Returns the value and sets type_id to the inferred type
ParseResult ParseGenericValue(Lexer& lexer, TypeId& out_type);

/// Parse a USD dictionary `{ [type] key = value ... }` into a Dictionary Value.
/// The lexer should be positioned at the opening brace. Nested dictionaries and
/// array-valued entries are supported.
ParseResult ParseDict(Lexer& lexer);

/// Get TypeId from a USD type name string
/// Handles both simple types ("float") and array types ("float[]")
TypeId ParseTypeName(const std::string& type_name, bool& is_array);

}  // namespace next
}  // namespace lightusd
