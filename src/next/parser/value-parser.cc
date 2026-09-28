// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Value Parser implementation

#include "value-parser.hh"
#include "value-parser-numeric.hh"
#include "../strfmt.hh"
#include "lexer.hh"
#include "../crate/crate-format.hh"
#include "../crate/lazy-array.hh"
#include "../types/type-info.hh"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <limits>
#include <unordered_map>
#include <vector>

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#endif

namespace lightusd {
namespace next {

namespace {

// ============================================================
// Value parsing functions
// ============================================================

using value_parser_detail::DecimalToI64;
using value_parser_detail::DecimalToI32;
using value_parser_detail::IsDecimalIntToken;
using value_parser_detail::DecimalToU32;
using value_parser_detail::DecimalToU64;
using value_parser_detail::FastFloatParse;
using value_parser_detail::FastFloatParseToken;

#if defined(LIGHTUSD_ENABLE_THREAD)
// Persistent worker pool shared by the deferred-array batches and the
// parallel prim-subtree parse. Tasks never block on other pool work, so a
// saturated pool degrades to inline parsing on the producer, never deadlock.
class ValueWorkerPool {
 public:
  explicit ValueWorkerPool(size_t nthreads) {
    if (nthreads < 1) nthreads = 1;
    workers_.reserve(nthreads);
    for (size_t i = 0; i < nthreads; i++) {
      workers_.emplace_back([this]() { WorkerLoop(); });
    }
  }

  ~ValueWorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
  }

  ValueWorkerPool(const ValueWorkerPool&) = delete;
  ValueWorkerPool& operator=(const ValueWorkerPool&) = delete;

  void Submit(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

  size_t size() const { return workers_.size(); }

 private:
  void WorkerLoop() {
    while (true) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() { return stop_ || !jobs_.empty(); });
        if (stop_ && jobs_.empty()) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  std::vector<std::thread> workers_;
  bool stop_ = false;
};

// One process-wide pool per requested size. Pools are created on demand and
// never torn down while the process runs (concurrent parses — e.g.
// composition's parallel layer loads — may hold differently-sized requests;
// replacing a pool in use would join workers mid-parse). The set of distinct
// sizes is tiny (auto + explicit hints).
ValueWorkerPool* GetValueWorkerPool(int requested_threads) {
  int nthreads = requested_threads;
  if (nthreads <= 0) {
    nthreads = static_cast<int>(std::thread::hardware_concurrency());
    if (nthreads < 1) nthreads = 1;
    nthreads = std::min(nthreads, 8);
  }
  if (nthreads <= 1) return nullptr;

  static std::mutex pool_mu;
  static std::vector<std::pair<int, ValueWorkerPool*>>* pools =
      new std::vector<std::pair<int, ValueWorkerPool*>>();  // intentionally leaked
  std::lock_guard<std::mutex> lock(pool_mu);
  for (const auto& entry : *pools) {
    if (entry.first == nthreads) return entry.second;
  }
  ValueWorkerPool* pool = new ValueWorkerPool(static_cast<size_t>(nthreads));
  pools->emplace_back(nthreads, pool);
  return pool;
}
#endif  // LIGHTUSD_ENABLE_THREAD

#include "value-parser-scalars.inc"

}  // anonymous namespace

// ============================================================
// Public API
// ============================================================

ParseResult ParseValue(Lexer& lexer, TypeId expected_type) {
  // Handle None: an authored value block, not "no value". Preserve it as a block
  // so the writer re-emits `= None` (round-trips USDC ValueBlock + USDA `= None`).
  if (lexer.peek().type == TokenType::None) {
    lexer.next();
    return ParseResult::Ok(Value::MakeBlock());
  }

  // Dictionaries have a dedicated recursive parser (no flat ParseFn entry).
  if (expected_type == TypeId::Dictionary ||
      lexer.peek().type == TokenType::OpenBrace) {
    return ParseDict(lexer);
  }

  ParseFn fn = GetParseFunction(expected_type);
  if (!fn) {
    // GetTypeName returns nullptr for ids without TypeInfo (e.g. semantic ids
    // a malformed file maps onto) — std::string(nullptr) is UB/abort.
    const char* tn = GetTypeName(expected_type);
    return ParseResult::Error("No parser for type " +
                              (tn ? std::string(tn)
                                  : "#" + IntToStr(int(expected_type))));
  }

  ParseResult result = fn(lexer);

  // If parsing succeeded but type has a semantic distinction, fix the type ID
  if (result.success) {
    // For semantic types, the parse function returns the base type
    // We need to update to the actual requested type
    switch (expected_type) {
      case TypeId::Point3f:
      case TypeId::Point3h:
      case TypeId::Vector3f:
      case TypeId::Vector3h:
      case TypeId::Normal3f:
      case TypeId::Normal3h:
      case TypeId::Color3f:
      case TypeId::Color3h:
      case TypeId::Point3d:
      case TypeId::Vector3d:
      case TypeId::Normal3d:
      case TypeId::Color3d:
      case TypeId::Color4f:
      case TypeId::Color4h:
      case TypeId::Color4d:
      case TypeId::Texcoord2f:
      case TypeId::Texcoord2h:
      case TypeId::Texcoord2d:
      case TypeId::Texcoord3f:
      case TypeId::Texcoord3h:
      case TypeId::Texcoord3d:
        result.value = Value::MakeFromRaw(expected_type, result.value.raw_data());
        break;
      default:
        break;
    }
  }

  return result;
}

#include "value-parser-arrays.inc"

#if defined(LIGHTUSD_ENABLE_THREAD)

bool SubmitPoolTask(int num_threads, std::function<void()> task) {
  ValueWorkerPool* pool = GetValueWorkerPool(num_threads);
  if (!pool) return false;
  pool->Submit(std::move(task));
  return true;
}

int ResolveUsdaParseThreads(int requested) {
  return ResolveParseThreads(requested);
}

namespace {
// Batch shaping: enough text per task that the pool's mutex/condvar and
// std::function overhead vanish against the parse work, while staying small
// enough that batches spread evenly across workers.
constexpr size_t kDeferredBatchTargetBytes = size_t(512) << 10;  // 512 KiB
constexpr size_t kDeferredBatchMaxItems = 1024;
}  // namespace

struct DeferredArrayScheduler::Shared {
  std::mutex mu;
  std::condition_variable cv;
  size_t inflight = 0;
  std::atomic<bool> failed{false};

  // Parse every item of one batch; after the first failure the rest are
  // skipped (the whole layer is re-parsed serially anyway). Every item's fill
  // reference is dropped either way.
  void RunBatch(std::vector<DeferredArrayItem>& items) {
    for (DeferredArrayItem& item : items) {
      if (failed.load(std::memory_order_relaxed)) {
        item.fill.Release();
        continue;
      }
      if (!CompleteDeferredArrayItem(item)) {
        failed.store(true, std::memory_order_relaxed);
      }
    }
  }
};

DeferredArrayScheduler::DeferredArrayScheduler(int num_threads)
    : shared_(std::make_shared<Shared>()), num_threads_(num_threads) {}

DeferredArrayScheduler::~DeferredArrayScheduler() {
  Drain();  // never leave workers writing into payloads after the parse
}

std::unique_ptr<DeferredArrayScheduler> DeferredArrayScheduler::Create(
    int num_threads) {
  ValueWorkerPool* pool = GetValueWorkerPool(num_threads);
  if (!pool) return nullptr;
  std::unique_ptr<DeferredArrayScheduler> s(
      new DeferredArrayScheduler(num_threads));
  // Bound how far the producers may run ahead: enough batches to keep every
  // worker busy plus a queue, without unbounded pending memory.
  s->max_inflight_ = 4 * pool->size();
  return s;
}

bool DeferredArrayScheduler::failed() const {
  return shared_->failed.load(std::memory_order_relaxed);
}

void DeferredArrayScheduler::DispatchBatch(
    std::vector<DeferredArrayItem> batch) {
  if (batch.empty()) return;

  {
    std::unique_lock<std::mutex> lock(shared_->mu);
    if (shared_->inflight >= max_inflight_) {
      // Backpressure: workers are saturated — parse this batch inline on the
      // producer thread instead of blocking (self-balancing).
      lock.unlock();
      shared_->RunBatch(batch);
      return;
    }
    shared_->inflight++;
  }

  std::shared_ptr<Shared> st = shared_;
  auto batch_ptr =
      std::make_shared<std::vector<DeferredArrayItem>>(std::move(batch));
  const bool submitted = SubmitPoolTask(num_threads_, [st, batch_ptr]() {
    st->RunBatch(*batch_ptr);
    std::lock_guard<std::mutex> lock(st->mu);
    st->inflight--;
    if (st->inflight == 0) st->cv.notify_all();
  });
  if (!submitted) {
    shared_->RunBatch(*batch_ptr);
    std::lock_guard<std::mutex> lock(shared_->mu);
    shared_->inflight--;
    if (shared_->inflight == 0) shared_->cv.notify_all();
  }
}

void DeferredArrayScheduler::Enqueue(DeferredArrayItem item) {
  std::vector<DeferredArrayItem> batch;
  {
    std::lock_guard<std::mutex> lock(enqueue_mu_);
    current_bytes_ += item.len;
    current_.push_back(std::move(item));
    if (current_bytes_ < kDeferredBatchTargetBytes &&
        current_.size() < kDeferredBatchMaxItems) {
      return;
    }
    batch.swap(current_);
    current_bytes_ = 0;
  }
  DispatchBatch(std::move(batch));  // outside the producer lock
}

bool DeferredArrayScheduler::Drain() {
  std::vector<DeferredArrayItem> batch;
  {
    std::lock_guard<std::mutex> lock(enqueue_mu_);
    batch.swap(current_);
    current_bytes_ = 0;
  }
  DispatchBatch(std::move(batch));
  std::unique_lock<std::mutex> lock(shared_->mu);
  shared_->cv.wait(lock, [this]() { return shared_->inflight == 0; });
  return !shared_->failed.load(std::memory_order_relaxed);
}

#endif  // LIGHTUSD_ENABLE_THREAD

ParseResult ParseGenericValue(Lexer& lexer, TypeId& out_type) {
  const Token& tok = lexer.peek();

  if (tok.type == TokenType::True || tok.type == TokenType::False) {
    out_type = TypeId::Bool;
    return ParseBool(lexer);
  }

  if (tok.type == TokenType::Number) {
    // Try to determine if it's int or float
    if (tok.value.find('.') != std::string::npos ||
        tok.value.find('e') != std::string::npos ||
        tok.value.find('E') != std::string::npos) {
      out_type = TypeId::Double;
      return ParseDouble(lexer);
    } else {
      out_type = TypeId::Int;
      return ParseInt(lexer);
    }
  }

  if (tok.type == TokenType::String) {
    out_type = TypeId::String;
    return ParseString(lexer);
  }

  if (tok.type == TokenType::OpenParen) {
    // Count elements in the tuple to determine arity before parsing.
    // Save byte position of '(' so we can reset the lexer after the scan pass.
    size_t saved_pos = lexer.token_start();
    lexer.next();  // consume '('
    size_t count = 1;
    size_t depth = 1;
    bool all_int = true;
    while (depth > 0) {
      const Token& t = lexer.peek();
      if (t.type == TokenType::Eof) break;
      if (t.type == TokenType::OpenParen) {
        depth++;
      } else if (t.type == TokenType::CloseParen) {
        depth--;
        if (depth == 0) break;
      } else if (t.type == TokenType::Comma && depth == 1) {
        count++;
      } else if (IsNumberToken(t)) {
        if (t.value.find('.') != std::string::npos ||
            t.value.find('e') != std::string::npos ||
            t.value.find('E') != std::string::npos) {
          all_int = false;
        }
      }
      lexer.next();
    }
    if (depth != 0) {
      return ParseResult::Error("Unmatched '(' in tuple");
    }

    // Reset lexer to saved_pos and re-parse with the correct arity.
    lexer.set_position(saved_pos);

    if (!all_int) {
      switch (count) {
        case 2: {
          out_type = TypeId::Float2;
          return ParseFloat2(lexer);
        }
        case 3: {
          out_type = TypeId::Float3;
          return ParseFloat3(lexer);
        }
        case 4: {
          out_type = TypeId::Float4;
          return ParseFloat4(lexer);
        }
        default:
          break;
      }
    } else {
      switch (count) {
        case 2: {
          out_type = TypeId::Int2;
          return ParseInt2(lexer);
        }
        case 3: {
          out_type = TypeId::Int3;
          return ParseInt3(lexer);
        }
        case 4: {
          out_type = TypeId::Int4;
          return ParseInt4(lexer);
        }
        default:
          break;
      }
    }

    out_type = TypeId::Invalid;
    return ParseResult::Error("Unsupported tuple arity " + std::to_string(count));
  }

  if (tok.type == TokenType::None) {
    out_type = TypeId::Invalid;
    lexer.next();
    return ParseResult::Ok(Value());
  }

  out_type = TypeId::Invalid;
  return ParseResult::Error("Cannot infer type from token");
}

#include "value-parser-dict.inc"


}  // namespace next
}  // namespace lightusd
