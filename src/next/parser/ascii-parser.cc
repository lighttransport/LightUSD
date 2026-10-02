// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA ASCII Parser implementation

#include "ascii-parser-internal.hh"
#include "../safe-file-size.hh"
#include "../strfmt.hh"
#include "usda-lazy-source.hh"
#include "value-parser.hh"
#include "../writer/value-printer.hh"
#include "../../external/fast_float/include/fast_float/fast_float.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <fstream>
#include <system_error>
#include <limits>
#include <utility>

namespace lightusd {
namespace next {
namespace {

ParseOptions NormalizeParseOptions(const ParseOptions& options) {
  ParseOptions normalized = options;
  if (normalized.max_usda_lazy_array_elements == 0) {
    normalized.max_usda_lazy_array_elements =
        std::numeric_limits<size_t>::max();
  }
  if (normalized.num_threads < 0) {
    normalized.num_threads = 0;
  }
  return normalized;
}

}  // namespace

bool IsNameToken(const Token& tok) {
  switch (tok.type) {
    case TokenType::Identifier:
    case TokenType::Def:
    case TokenType::Over:
    case TokenType::Class:
    case TokenType::True:
    case TokenType::False:
    case TokenType::None:
    case TokenType::TimeSamples:
    case TokenType::Custom:
    case TokenType::Uniform:
    case TokenType::Varying:
    case TokenType::Prepend:
    case TokenType::Append:
    case TokenType::Delete:
    case TokenType::Add:
    case TokenType::Reorder:
    case TokenType::Rel:
      return !tok.value.empty();
    default:
      return false;
  }
}

bool AsciiParser::Impl::ReportProgress(const char* phase, size_t current,
                                       size_t total) {
  if (!options_.progress_callback ||
      options_.progress_callback(phase, current, total)) return true;
  AddError(std::string("USDA parse cancelled during ") + phase);
  return false;
}

#if defined(LIGHTUSD_ENABLE_THREAD)
namespace {
// Inputs below this size always take the serial path: nothing in them is big
// enough to dispatch, and the worker pool is not worth waking.
constexpr size_t kFastParseMinBytes = size_t(64) << 10;  // 64 KiB
// Parallel prim-subtree window. Blocks below the floor are cheaper to parse
// inline than to dispatch (fragment + task + splice overhead); blocks above
// the ceiling are descended inline so their CHILDREN are dispatched
// individually (bounded worker granularity, short drain tail).
constexpr size_t kSubtreeMinBytes = size_t(16) << 10;  // 16 KiB
constexpr size_t kSubtreeMaxBytes = size_t(64) << 20;  // 64 MiB
// A run of small sibling blocks below this size is parsed inline (task,
// fragment and splice overhead would exceed the parse itself).
constexpr size_t kSubtreeRunMinBytes = size_t(4) << 10;  // 4 KiB
}  // namespace
#endif

ParseResult AsciiParser::Impl::ParseArrayAttributeValue(TypeId type_id,
                                                        bool* out_deferred) {
  ParseArrayContext array_ctx;
  array_ctx.source = source_;
  array_ctx.enable_usda_lazy_arrays = options_.enable_usda_lazy_arrays;
  array_ctx.max_usda_lazy_array_elements =
      options_.max_usda_lazy_array_elements;
  array_ctx.num_threads = options_.num_threads;
#if defined(LIGHTUSD_ENABLE_THREAD)
  DeferredArrayScheduler* scheduler = deferred_arrays_.get();
#else
  DeferredArrayScheduler* scheduler = nullptr;
#endif
  return ParseArrayValueMaybeDeferred(*lexer_, type_id, array_ctx, scheduler,
                                      out_deferred);
}

#if defined(LIGHTUSD_ENABLE_THREAD)

void AsciiParser::Impl::RunSubtreeParse(
    const ParseOptions& options,
    std::shared_ptr<DeferredArrayScheduler> arrays, SubtreeParseState* st,
    const char* data, size_t len, size_t start_line, size_t start_column,
    size_t base_depth, const std::string& parent_path,
    SubtreeFragment* fragment) {
  Impl sub(options);
  sub.layer_ = std::make_unique<Layer>();
  sub.builder_ = std::make_unique<LayerBuilder>(*sub.layer_);
  if (!parent_path.empty()) sub.builder_->set_path_prefix(parent_path);
  sub.lexer_ = std::make_unique<Lexer>(data, len);
  sub.lexer_->num_threads = options.num_threads;
  sub.lexer_->strict_aousd_conformance = options.strict_aousd_conformance;
  sub.lexer_->set_source_location(start_line, start_column);
  sub.deferred_arrays_ = std::move(arrays);
  sub.depth_ = base_depth;
  sub.parse_length_ = len;

  // The span holds one or more sibling prim blocks separated only by
  // whitespace/comments; parse them exactly as the enclosing prim-contents
  // (or root) loop would.
  bool ok = true;
  while (ok && sub.lexer_->peek().type != TokenType::Eof) {
    ok = sub.ParsePrim();
  }
  // The fragment is only usable when the sub-parse is indistinguishable from
  // what the serial parser would have done with the same bytes: clean parse,
  // exactly the captured span consumed, no recoverable lexer error left
  // behind (it would be sticky in the serial lexer) and no set_position()
  // rewind (which renumbers lines). Anything else fails the fast attempt and
  // the whole layer is re-parsed serially.
  ok = ok && sub.errors_.empty() && !sub.lexer_->has_error() &&
       !sub.lexer_->position_reset() && sub.layer_->prim_count() > 0 &&
       !sub.layer_->root_indices().empty();
  if (ok) {
    for (uint32_t r : sub.layer_->root_indices()) {
      const PrimSpec* root = sub.layer_->prim(r);
      if (!root) {
        ok = false;
        break;
      }
      fragment->root_paths.push_back(root->path().str());
    }
  }
  if (ok) {
    fragment->warnings = std::move(sub.warnings_);
    fragment->layer = std::move(sub.layer_);
  } else {
    st->failed.store(true, std::memory_order_relaxed);
  }
}

bool AsciiParser::Impl::WaitSubtreeTasks() {
  if (!subtree_state_) return true;
  SubtreeParseState* st = subtree_state_.get();
  std::unique_lock<std::mutex> lock(st->mu);
  st->cv.wait(lock, [st]() { return st->inflight == 0; });
  return !st->failed.load(std::memory_order_relaxed);
}

bool AsciiParser::Impl::StitchSubtreeFragments() {
  if (!subtree_state_) return true;
  SubtreeParseState* st = subtree_state_.get();
  if (st->fragments.empty()) return true;

  // A fragment root that duplicates a sibling (in the main layer or another
  // fragment) would have been MERGED into the existing prim by the serial
  // builder: not reproducible here.
  std::unordered_set<std::string> roots;
  roots.reserve(st->fragments.size());
  std::vector<Layer*> layers;
  std::vector<size_t> insert_before;
  layers.reserve(st->fragments.size());
  insert_before.reserve(st->fragments.size());
  for (const auto& f : st->fragments) {
    if (!f->layer) return false;
    for (const std::string& root_path : f->root_paths) {
      if (builder_->contains_path(root_path) ||
          !roots.insert(root_path).second) {
        return false;
      }
    }
    layers.push_back(f->layer.get());
    insert_before.push_back(f->prim_pos);
  }
  if (!layer_->splice_fragments(layers, insert_before)) return false;

  // Warnings: each fragment's go where the serial parse emitted them (the
  // main parser emits nothing while a captured block is skipped).
  size_t extra = 0;
  for (const auto& f : st->fragments) extra += f->warnings.size();
  if (extra > 0) {
    std::vector<std::string> merged;
    merged.reserve(warnings_.size() + extra);
    size_t wi = 0;
    for (const auto& f : st->fragments) {
      while (wi < f->warning_pos && wi < warnings_.size()) {
        merged.push_back(std::move(warnings_[wi++]));
      }
      for (std::string& w : f->warnings) merged.push_back(std::move(w));
    }
    while (wi < warnings_.size()) merged.push_back(std::move(warnings_[wi++]));
    warnings_ = std::move(merged);
  }
  st->fragments.clear();
  return true;
}

#endif  // LIGHTUSD_ENABLE_THREAD

bool AsciiParser::Impl::ParsePrimMaybeParallel() {
#if defined(LIGHTUSD_ENABLE_THREAD)
  SubtreeParseState* st = subtree_state_.get();
  if (st && layer_.get() == dispatch_layer_ &&
      lexer_->position() >= no_capture_until_ && !lexer_->has_error() &&
      !st->failed.load(std::memory_order_relaxed)) {
    // Capture a RUN of consecutive sibling prim blocks (only whitespace /
    // comments between them) until it reaches the dispatch size: wide
    // parents with many small children would otherwise leave all of those
    // children to the main thread.
    const Lexer::SavedState run_start = lexer_->save_state();
    const char* block = nullptr;
    size_t len = 0;
    size_t line = 0;
    size_t column = 0;
    size_t first_end = 0;
    size_t nblocks = 0;
    while (true) {
      const char* b = nullptr;
      size_t l = 0, bl = 0, bc = 0;
      bool too_big = false;
      if (!lexer_->capture_prim_block(0, st->max_block_bytes, &b, &l, &bl, &bc,
                                      &too_big)) {
        break;
      }
      if (nblocks == 0) {
        block = b;
        line = bl;
        column = bc;
        first_end = static_cast<size_t>((b + l) - lexer_->input_data());
      }
      ++nblocks;
      len = static_cast<size_t>((b + l) - block);
      if (len >= kSubtreeMinBytes) break;
      const TokenType next = lexer_->peek().type;
      if (lexer_->has_error() ||
          (next != TokenType::Def && next != TokenType::Over &&
           next != TokenType::Class)) {
        break;
      }
    }
    if (nblocks > 0 && len < kSubtreeRunMinBytes) {
      // Too little work to be worth a task: rewind and parse the first block
      // inline. Its descendants are smaller still — skip their captures.
      lexer_->restore_state(run_start);
      if (first_end > no_capture_until_) no_capture_until_ = first_end;
      nblocks = 0;
    }
    if (nblocks > 0) {
      const uint32_t frag_id = static_cast<uint32_t>(st->fragments.size());
      auto owned = std::make_unique<SubtreeFragment>();
      owned->warning_pos = warnings_.size();
      owned->prim_pos = layer_->prim_count();
      // Placeholder in authored position (resolved by the splice).
      std::string parent_path;
      if (PrimSpec* parent = builder_->current()) {
        parent->add_child_index(Layer::kPendingIndexBit | frag_id);
        parent_path = parent->path().str();
      } else {
        layer_->add_root_pending(frag_id);
      }
      SubtreeFragment* frag = owned.get();
      st->fragments.push_back(std::move(owned));

      bool run_inline = false;
      {
        std::lock_guard<std::mutex> lock(st->mu);
        if (st->inflight >= st->max_inflight) {
          run_inline = true;  // backpressure: bound pending fragment memory
        } else {
          st->inflight++;
        }
      }
      if (run_inline) {
        RunSubtreeParse(options_, deferred_arrays_, st, block, len, line,
                        column, depth_, parent_path, frag);
      } else {
        std::shared_ptr<SubtreeParseState> stp = subtree_state_;
        std::shared_ptr<DeferredArrayScheduler> arrays = deferred_arrays_;
        const ParseOptions opts = options_;
        const size_t base_depth = depth_;
        auto task = [opts, arrays, stp, block, len, line, column, base_depth,
                     parent_path, frag]() {
          RunSubtreeParse(opts, arrays, stp.get(), block, len, line, column,
                          base_depth, parent_path, frag);
          std::lock_guard<std::mutex> lock(stp->mu);
          stp->inflight--;
          if (stp->inflight == 0) stp->cv.notify_all();
        };
        if (!SubmitPoolTask(options_.num_threads, std::move(task))) {
          RunSubtreeParse(options_, deferred_arrays_, st, block, len, line,
                          column, depth_, parent_path, frag);
          std::lock_guard<std::mutex> lock(st->mu);
          st->inflight--;
          if (st->inflight == 0) st->cv.notify_all();
        }
      }
      // Worker failures surface at the join; the attempt is then re-run
      // serially, so every diagnostic still comes from the serial parser.
      return true;
    }
    // Not captured (too big, malformed, block comment, or too small): parse
    // inline; a too-big prim's children get their own capture attempts.
  }
#endif
  return ParsePrim();
}

bool AsciiParser::Impl::ParseWithSource(const char* data, size_t length,
                                       std::shared_ptr<LazyArraySource> source) {
  source_ = std::move(source);
  used_fast_path_ = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  // The fast attempt (batched deferred arrays + parallel prim subtrees) is an
  // optimization only: it either produces exactly the serial result or fails,
  // and a failed attempt is re-run serially so errors, warnings and partial
  // behavior are those of the serial parser. Lazy-array mode and progress
  // callbacks (per-prim, in input order) stay serial.
  const bool fast = (options_.async_arrays || options_.parallel_prims) &&
                    !options_.enable_usda_lazy_arrays && !source_ &&
                    !options_.progress_callback &&
                    length >= kFastParseMinBytes && data != nullptr &&
                    ResolveUsdaParseThreads(options_.num_threads) > 1;
  if (fast && ParseAttempt(data, length, /*fast=*/true)) {
    used_fast_path_ = true;
    return true;
  }
#endif
  return ParseAttempt(data, length, /*fast=*/false);
}

bool AsciiParser::Impl::ParseAttempt(const char* data, size_t length,
                                     bool fast) {
  errors_.clear();
  warnings_.clear();
  depth_ = 0;
  no_capture_until_ = 0;
  parse_length_ = length;

  if (!source_ && length != 0 && !data) {
    AddError("Invalid null USDA input");
    return false;
  }

  if (options_.max_file_size > 0 && length > options_.max_file_size) {
    AddError("File size exceeds maximum allowed");
    return false;
  }

  // Create fresh layer and builder
  layer_ = std::make_unique<Layer>();
  builder_ = std::make_unique<LayerBuilder>(*layer_);
  dispatch_layer_ = layer_.get();

  if (source_) {
    // Keep a shared ownership of the full USDA source while parsing so any lazy
    // array source slices stay valid until the parse/build graph drops them.
    lexer_ = std::make_unique<Lexer>(
        reinterpret_cast<const char*>(source_->base()), length);
  } else {
    lexer_ = std::make_unique<Lexer>(data, length);
  }
  lexer_->num_threads = options_.num_threads;
  lexer_->strict_aousd_conformance = options_.strict_aousd_conformance;

#if defined(LIGHTUSD_ENABLE_THREAD)
  if (fast && options_.async_arrays) {
    deferred_arrays_ = DeferredArrayScheduler::Create(options_.num_threads);
  }
  if (fast && options_.parallel_prims) {
    const int nt = ResolveUsdaParseThreads(options_.num_threads);
    if (nt > 1) {
      subtree_state_ = std::make_shared<SubtreeParseState>();
      subtree_state_->max_inflight = static_cast<size_t>(4 * nt);
      // Blocks above ~1/(4*threads) of the input are descended inline so
      // their children spread over the pool: a mid-size file whose content
      // sits under one root prim would otherwise go to a single worker.
      subtree_state_->max_block_bytes = std::min(
          kSubtreeMaxBytes,
          std::max(length / (4 * static_cast<size_t>(nt)),
                   4 * kSubtreeMinBytes));
    }
  }
  // Deferred array workers hold spans into `data` and fill payloads committed
  // into the layer; subtree workers read `data` and enqueue arrays. EVERY exit
  // from this attempt must therefore join subtree workers FIRST, then drain
  // the array scheduler, before the input buffer or the layer can die (guards
  // run in reverse declaration order).
  struct DrainGuard {
    std::shared_ptr<DeferredArrayScheduler>& scheduler;
    ~DrainGuard() {
      if (scheduler) scheduler->Drain();
      scheduler.reset();
    }
  } drain_guard{deferred_arrays_};
  struct SubtreeJoinGuard {
    Impl* impl;
    ~SubtreeJoinGuard() {
      impl->WaitSubtreeTasks();
      impl->subtree_state_.reset();
    }
  } subtree_join_guard{this};
#else
  (void)fast;
#endif

  if (!ReportProgress("bootstrap", 0, length)) return false;

  // Enforce the `#usda 1.0` magic on the raw bytes BEFORE lexing: the lexer
  // treats '#' as a comment, so a token-level check is dead code and any
  // text starting with `def "x" {}` would "parse" as USDA.
  {
    const char* raw = source_ ? reinterpret_cast<const char*>(source_->base())
                              : data;
    size_t pos = 0;
    while (pos < length &&
           (raw[pos] == ' ' || raw[pos] == '\t' || raw[pos] == '\r' ||
            raw[pos] == '\n')) {
      pos++;
    }
    bool ok = (pos + 5 <= length) && std::memcmp(raw + pos, "#usda", 5) == 0;
    if (ok) {
      pos += 5;
      size_t ws = pos;
      while (ws < length && (raw[ws] == ' ' || raw[ws] == '\t')) ws++;
      // Require whitespace then a version digit ("#usda  1.0" is valid —
      // the legacy parser accepts arbitrary spacing here).
      ok = (ws > pos) && ws < length && raw[ws] >= '0' && raw[ws] <= '9';
    }
    if (!ok) {
      AddError("Missing or invalid '#usda 1.0' header");
      return false;
    }
  }

  // Parse stage metadata (header block)
  if (!ParseStageMetadata()) {
    return false;
  }

  // Parse root prims
  while (!AtEnd()) {
    // Pseudo-root namespace ordering is an authored field and must survive a
    // layer rewrite even when it mentions currently absent prim names.
    if (lexer_->peek().type == TokenType::Reorder) {
      lexer_->next();
      std::string what;
      if (!lexer_->expect(TokenType::Identifier, what)) return false;
      if (what == "rootPrims") {
        if (!ParseOrderList(&layer_->meta().rootPrimOrder)) return false;
        layer_->meta().rootPrimOrder_set = true;
      } else if (options_.strict_aousd_conformance) {
        AddError("Unsupported root reorder field: " + what);
        return false;
      } else {
        if (Match(TokenType::Equals)) SkipValueLike();
        AddWarning("Unknown root reorder field ignored: " + what);
      }
      continue;
    }
    if (!ParsePrimMaybeParallel()) {
      return false;
    }
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  // Join barrier: splice the parallel prim subtrees (authored order and exact
  // serial prim order restored), then wait for every deferred array payload,
  // before anything reads the tree. Any failure fails this (fast) attempt.
  if (subtree_state_) {
    if (!WaitSubtreeTasks() || !StitchSubtreeFragments()) {
      AddError("Parallel USDA prim parse failed");
      return false;
    }
  }
  if (deferred_arrays_ && !deferred_arrays_->Drain()) {
    AddError("Deferred USDA array parse failed");
    return false;
  }
#endif

  // A fatal lexical malformation (unterminated string/path/asset literal,
  // oversized token) must fail the parse even when the token-level grammar
  // happened to recover (e.g. an unterminated single-quoted string cut off by
  // a newline used to be silently accepted).
  if (lexer_->has_fatal_error()) {
    AddError(lexer_->error());
    return false;
  }

  // Finalize the layer
  builder_->finalize();

  if (!ReportProgress("complete", length, length)) return false;

  // Create stage from layer
  stage_ = Stage();
  stage_.SetRootLayer(std::move(*layer_));

  lexer_.reset();
  builder_.reset();
  layer_.reset();

  return errors_.empty();
}

bool AsciiParser::Impl::Parse(const char* data, size_t length) {
  if (options_.enable_usda_lazy_arrays) {
    auto source = UsdaLazyArraySource::AdoptString(
        data ? std::string(data, data + length) : std::string());
    const char* src_data = reinterpret_cast<const char*>(source->base());
    return ParseWithSource(src_data, length, std::move(source));
  }
  return ParseWithSource(data, length, nullptr);
}

bool AsciiParser::Impl::ParseOwned(std::string&& data) {
  const size_t length = data.size();
  if (options_.enable_usda_lazy_arrays) {
    auto source = UsdaLazyArraySource::AdoptString(std::move(data));
    const char* src_data = reinterpret_cast<const char*>(source->base());
    return ParseWithSource(src_data, length, std::move(source));
  }
  const char* src_data = data.empty() ? nullptr : data.data();
  return ParseWithSource(src_data, length, nullptr);
}

bool AsciiParser::Impl::ParseFile(const char* filename) {
  if (!filename || !*filename) {
    AddError("Invalid USDA filename");
    return false;
  }
  std::ifstream file(filename, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    AddError(std::string("Failed to open file: ") + filename);
    return false;
  }

  // Unconditional bound -- max_file_size is optional, and the size_t
  // comparison this replaces was a no-op on LP64. A directory opens fine here
  // and reports LLONG_MAX (see SafeStreamSize).
  size_t size = 0;
  if (!SafeStreamSize(file, static_cast<uint64_t>(options_.max_file_size),
                      &size)) {
    AddError("Invalid or oversized file");
    return false;
  }
  file.seekg(0, std::ios::beg);

  if (options_.enable_usda_lazy_arrays) {
    std::string mmap_error;
    auto mapped = UsdaLazyArraySource::MmapFile(filename, &mmap_error);
    if (mapped) {
      const char* data = reinterpret_cast<const char*>(mapped->base());
      const size_t mapped_size = mapped->size();
      return ParseWithSource(data, mapped_size, std::move(mapped));
    }

    std::string content(size ? size : 0, '\0');
    if (size && !file.read(content.data(), static_cast<std::streamsize>(size))) {
      AddError("Failed to read file contents");
      return false;
    }
    auto src = UsdaLazyArraySource::AdoptString(std::move(content));
    const char* data = reinterpret_cast<const char*>(src->base());
    return ParseWithSource(data, size, std::move(src));
  }

#if LIGHTUSD_NEXT_USDA_LAZY_MMAP
  // Map the file instead of copying it into the heap: the parse only reads
  // the bytes, and a large flattened layer would otherwise cost a full
  // single-threaded read() copy up front. The mapping outlives the parse
  // (including every deferred-array worker, which the parse joins).
  {
    std::string mmap_error;
    auto mapped = UsdaLazyArraySource::MmapFile(filename, &mmap_error);
    if (mapped && mapped->size() == size) {
      const char* data = reinterpret_cast<const char*>(mapped->base());
      return Parse(size ? data : "", size);
    }
  }
#endif

  // Default-init (NOT value-init) the buffer: `new char[]` leaves the bytes
  // uninitialized, so we skip zero-filling hundreds of MB we immediately
  // overwrite with file.read (the zero-fill was ~8% of a big-file parse).
  std::unique_ptr<char[]> content(new char[size ? size : 1]);
  if (size && !file.read(content.get(), static_cast<std::streamsize>(size))) {
    AddError("Failed to read file contents");
    return false;
  }

  return Parse(content.get(), size);
}

// Read one composition-arc reference into canonical "@asset@</prim>" /
// "</prim>" form (the lexer yields @asset@ as a String without '@' and
// </prim> as a PathRef without '<>'). Peeks before consuming so a missing
// optional token does not eat the next one. Returns false if no arc token.
bool AsciiParser::Impl::ReadArcRef(std::string* out) {
  std::string ref;
  if (Check(TokenType::String)) {
    std::string asset;
    lexer_->expect(TokenType::String, asset);
    ref = "@" + asset + "@";
    if (Check(TokenType::PathRef)) {
      std::string pr;
      lexer_->expect(TokenType::PathRef, pr);
      ref += "<" + pr + ">";
    }
  } else if (Check(TokenType::PathRef)) {
    std::string pr;
    lexer_->expect(TokenType::PathRef, pr);
    ref = "<" + pr + ">";
  } else {
    return false;
  }
  // Optional per-arc layer offset: `(offset = N; scale = M)`. Encoded into the
  // canonical ref as `?layerOffset=offset:scale` (Compositor::ParseReference
  // decodes it); composition composes it through the arc chain and bakes it
  // into time-sample times.
  if (Check(TokenType::OpenParen)) {
    Match(TokenType::OpenParen);
    double off = 0.0, scl = 1.0;
    std::string custom_data;
    while (!Check(TokenType::CloseParen) && !AtEnd()) {
      if (Check(TokenType::Identifier)) {
        std::string k;
        lexer_->expect(TokenType::Identifier, k);
        Match(TokenType::Equals);
        if (k == "customData") {
          ParseResult parsed = ParseValue(*lexer_, TypeId::Dictionary);
          if (!parsed.success || !parsed.value.is_dictionary()) {
            AddError("Invalid reference customData dictionary");
            return false;
          }
          PrintOptions print;
          print.float_precision = 9;
          print.double_precision = 17;
          print.sort_dictionary_keys = true;
          if (!parsed.value.as_dictionary()->empty())
            custom_data = PrintValue(parsed.value, print);
        } else if (Check(TokenType::Number)) {
          std::string num;
          lexer_->expect(TokenType::Number, num);
          // Freestanding double parse (fast_float; no libc strtod).
          double v = 0.0;
          {
            const char* b = num.c_str();
            const char* e = b + num.size();
            auto r = fast_float::from_chars(b, e, v);
            if (!(r.ec == std::errc{} && r.ptr == e) && b < e && *b == '+') {
              fast_float::from_chars(b + 1, e, v);
            }
          }
          if (k == "offset") off = v;
          else if (k == "scale") scl = v;
        } else if ((k == "offset" || k == "scale") &&
                   Check(TokenType::Identifier)) {
          // `nan` / `inf` lex as identifiers; keep them as numbers so the
          // finiteness check below sees them instead of the default.
          std::string word;
          lexer_->expect(TokenType::Identifier, word);
          const double v =
              word == "inf" ? std::numeric_limits<double>::infinity()
                            : std::numeric_limits<double>::quiet_NaN();
          if (k == "offset") off = v;
          else scl = v;
        } else {
          // Unknown key with a structured value (customData = { ... } may
          // contain nested parens/braces): balanced skip, or the paren scan
          // terminates INSIDE the value and desyncs the metadata block.
          SkipValueLike();
        }
      } else {
        lexer_->next();  // skip unexpected token (avoid spinning)
      }
      Match(TokenType::Semicolon);
    }
    Match(TokenType::CloseParen);
    // The file format preserves any finite offset/scale (the AOUSD
    // primmetadata.usda baseline keeps `scale = -2.0`); composition maps a
    // non-positive scale to identity when it applies the arc.
    if (!std::isfinite(off) || !std::isfinite(scl)) {
      if (options_.strict_aousd_conformance) {
        AddError("AOUSD composition-arc layer offset and scale must be finite");
        return false;
      }
      AddWarning("Invalid composition-arc layer offset; using identity mapping");
      off = 0.0;
      scl = 1.0;
    }
    if (off != 0.0 || scl != 1.0) {
      ref += "?layerOffset=" + std::to_string(off) + ":" + std::to_string(scl);
    }
    if (!custom_data.empty()) ref += '\x1f' + custom_data;
  }
  *out = ref;
  return true;
}

bool AsciiParser::Impl::ParseMetadataBlock() {
  if (!Match(TokenType::OpenParen)) {
    return false;
  }

  PrimSpec* prim = builder_->current();
  if (!prim) {
    AddError("No current prim for metadata");
    return false;
  }

  // Read an arc value that may be a bracketed list or a single value.
  // List-op qualifier applied to an arc list (prepend/append/delete/explicit).
  enum class ArcQual { Explicit, Add, Prepend, Append, Delete, Reorder };

  enum class ArcField { References, Payloads, Inherits, Specializes };
  auto SelectArc = [](PrimSpecMeta& meta,
                      ArcField f) -> std::vector<std::string>& {
    switch (f) {
      case ArcField::References: return meta.references;
      case ArcField::Payloads: return meta.payloads;
      case ArcField::Inherits: return meta.inherits;
      default: return meta.specializes;
    }
  };
  auto SelectEdit = [](ArcListOpEdits& e, ArcField f) -> ArcEdit& {
    switch (f) {
      case ArcField::References: return e.references;
      case ArcField::Payloads: return e.payloads;
      case ArcField::Inherits: return e.inherits;
      default: return e.specializes;
    }
  };

  // Read the bracketed (or single) arc references, then merge into the inline
  // arc vector honoring the list-op qualifier (explicit/bare replaces, prepend
  // front, append/reorder back, delete removes) -- the within-spec effective
  // list. ALSO record the raw qualifier into the spec's ArcEdit (Phase 7 S5),
  // which cross-layer composition (apply_list_ops) and the writer consume. Even
  // a bare empty list (`references = []`) is authored and must replace weaker
  // opinions.
  auto ReadArcList = [this, &SelectArc, &SelectEdit](
                         PrimSpecMeta& meta, ArcField field, ArcQual qual) {
    std::vector<std::string> items;
    if (Check(TokenType::None)) {
      // `references = None`: an explicit-clear list op. Consume the token
      // (leaving it un-consumed desynchronizes the metadata loop) and record
      // an authored empty explicit edit.
      lexer_->next();
      ArcEdit& e0 = SelectEdit(meta.ensure_arc_edits(), field);
      e0 = ArcEdit();
      e0.authored = true;
      SelectArc(meta, field).clear();
      return;
    }
    if (Match(TokenType::OpenBracket)) {
      while (!Check(TokenType::CloseBracket) && !AtEnd()) {
        std::string ref;
        if (!ReadArcRef(&ref)) break;
        items.push_back(ref);
        Match(TokenType::Comma);
      }
      Match(TokenType::CloseBracket);
    } else {
      std::string ref;
      if (ReadArcRef(&ref)) items.push_back(ref);
    }

    // Record the list-op edit first (copies items for non-bare qualifiers).
    ArcEdit& e = SelectEdit(meta.ensure_arc_edits(), field);
    switch (qual) {
      case ArcQual::Explicit:
        e = ArcEdit();  // explicit replaces: is_explicit=true, lists cleared
        e.authored = true;
        break;
      case ArcQual::Prepend:
        e.authored = true;
        e.is_explicit = false;
        e.prepended.insert(e.prepended.end(), items.begin(), items.end());
        break;
      case ArcQual::Append:
        e.authored = true;
        e.is_explicit = false;
        e.appended.insert(e.appended.end(), items.begin(), items.end());
        break;
      case ArcQual::Add:
        e.authored = true;
        e.is_explicit = false;
        e.added.insert(e.added.end(), items.begin(), items.end());
        break;
      case ArcQual::Delete:
        e.authored = true;
        e.is_explicit = false;
        e.deleted.insert(e.deleted.end(), items.begin(), items.end());
        break;
      case ArcQual::Reorder:
        e.authored = true;
        e.is_explicit = false;
        e.ordered.insert(e.ordered.end(), items.begin(), items.end());
        break;
    }

    std::vector<std::string>* target = &SelectArc(meta, field);
    switch (qual) {
      case ArcQual::Explicit:
        *target = std::move(items);
        break;
      case ArcQual::Prepend:
        target->insert(target->begin(), items.begin(), items.end());
        break;
      case ArcQual::Append:
      case ArcQual::Add:
        target->insert(target->end(), items.begin(), items.end());
        break;
      case ArcQual::Reorder:
        ApplyStringListOrder(items, target);
        break;
      case ArcQual::Delete: {
        // Single O(N+M) pass via a hash set of the deleted entries. A per-entry
        // erase(remove()) is O(items * target) = O(N^2) for a `references=[...]`
        // then `delete references=[...]` block with N distinct refs (a ~O(N)
        // text input could hang).
        const std::unordered_set<std::string> del(items.begin(), items.end());
        target->erase(std::remove_if(target->begin(), target->end(),
                                     [&](const std::string& x) {
                                       return del.count(x) != 0;
                                     }),
                      target->end());
        break;
      }
    }
  };

  while (!Check(TokenType::CloseParen) && !AtEnd()) {
    if (Match(TokenType::Semicolon)) continue;
    // A bare (often triple-quoted) string is the prim COMMENT — pxr's only
    // accepted spelling (26.x rejects `comment = "..."` as a non-metadata
    // field; the bare string maps to `comment`, not `doc`).
    if (Check(TokenType::String)) {
      std::string d;
      lexer_->expect(TokenType::String, d);
      prim->meta().comment() = d;
      prim->meta().set_comment_authored();
      continue;
    }
    // Optional list-op qualifier keyword (prepend/append/delete/reorder)
    // precedes the real key, e.g. `prepend references = [...]`.
    ArcQual arc_qual = ArcQual::Explicit;
    if (Match(TokenType::Prepend)) {
      arc_qual = ArcQual::Prepend;
    } else if (Match(TokenType::Append)) {
      arc_qual = ArcQual::Append;
    } else if (Match(TokenType::Add)) {
      arc_qual = ArcQual::Add;
    } else if (Match(TokenType::Delete)) {
      arc_qual = ArcQual::Delete;
    } else if (Match(TokenType::Reorder)) {
      arc_qual = ArcQual::Reorder;
    }

    std::string key;
    if (!lexer_->expect(TokenType::Identifier, key)) {
      AddError("Expected metadata key");
      return false;
    }

    if (!Match(TokenType::Equals)) {
      AddError("Expected '=' after metadata key");
      return false;
    }

    // Handle known prim metadata
    if (key == "active") {
      ParseResult result = ParseValue(*lexer_, TypeId::Bool);
      if (result.success && result.value.as_bool()) {
        builder_->set_active(*result.value.as_bool());
        if (PrimSpec* cur = builder_->current()) {
          cur->meta().active_authored = true;
        }
      }
    } else if (key == "hidden") {
      ParseResult result = ParseValue(*lexer_, TypeId::Bool);
      if (result.success && result.value.as_bool()) {
        builder_->set_hidden(*result.value.as_bool());
        if (PrimSpec* cur = builder_->current()) {
          cur->meta().hidden_authored = true;
        }
      }
    } else if (key == "permission") {
      // Unquoted token (`permission = private`).
      std::string v;
      if (Check(TokenType::Identifier)) lexer_->expect(TokenType::Identifier, v);
      else lexer_->expect(TokenType::String, v);
      if (!v.empty()) prim->meta().permission() = v;
    } else if (key == "doc" || key == "documentation") {
      std::string doc;
      if (lexer_->expect(TokenType::String, doc)) {
        prim->meta().doc() = doc;
        prim->meta().set_doc_authored();
      }
    } else if (key == "comment") {
      std::string v;
      if (lexer_->expect(TokenType::String, v)) {
        prim->meta().comment() = v;
        prim->meta().set_comment_authored();
      }
    } else if (key == "kind") {
      std::string v;
      if (lexer_->expect(TokenType::String, v)) {
        prim->meta().kind() = v;
        prim->meta().setKindAuthored();
      }
    } else if (key == "displayName") {
      std::string v;
      if (lexer_->expect(TokenType::String, v)) {
        prim->meta().displayName() = v;
        prim->meta().setDisplayNameAuthored();
      }
    } else if (key == "displayGroupOrder") {
      ParseResult r = ParseArrayValue(*lexer_, TypeId::String);
      if (r.success && r.value.as_token_array()) {
        prim->meta().editDisplayGroupOrder() = *r.value.as_token_array();
        prim->meta().setDisplayGroupOrderAuthored();
      }
    } else if (key == "instanceable") {
      ParseResult result = ParseValue(*lexer_, TypeId::Bool);
      if (result.success && result.value.as_bool()) {
        prim->meta().instanceable = *result.value.as_bool();
        prim->meta().instanceable_authored = true;
      }
    } else if (key == "relocates") {
      // relocates = { </old/path>: </new/path>, ... } — namespace renames
      // consumed by pcp (SdfRelocates). Relative paths resolve against the
      // owning prim.
      if (prim) prim->meta().setRelocatesAuthored();
      if (Match(TokenType::OpenBrace)) {
        while (!Check(TokenType::CloseBrace) && !AtEnd()) {
          std::string src, dst;
          if (!lexer_->expect(TokenType::PathRef, src)) break;
          if (!Match(TokenType::Colon)) break;
          if (!lexer_->expect(TokenType::PathRef, dst)) break;
          if (prim) {
            auto abs = [&](const std::string& p) {
              if (p.empty() || p[0] == '/') return p;
              return prim->path().str() + "/" + p;
            };
            prim->meta().relocates().emplace_back(abs(src), abs(dst));
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBrace);
      }
    } else if (key == "apiSchemas") {
      prim->meta().setApiSchemasAuthored();
      std::vector<std::string> schemas;
      if (Check(TokenType::None)) {
        lexer_->next();
      } else if (Match(TokenType::OpenBracket)) {
        while (!Check(TokenType::CloseBracket) && !AtEnd()) {
          // Schema names are authored as quoted strings (`"PhysicsRigidBodyAPI"`);
          // accept bare identifiers too. expect() consumes even on mismatch, so
          // Check the token type first.
          std::string schema;
          if (Check(TokenType::String)
                  ? lexer_->expect(TokenType::String, schema)
                  : lexer_->expect(TokenType::Identifier, schema)) {
            schemas.push_back(schema);
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBracket);
      } else if (Check(TokenType::String)) {
        // Non-bracketed single value (`prepend apiSchemas = "FooAPI"`): must
        // be consumed here, or the metadata loop's bare-string rule assigns
        // it to the prim DOC (double corruption).
        std::string schema;
        if (lexer_->expect(TokenType::String, schema)) {
          schemas.push_back(schema);
        }
      }
      StringListOpEdits& edits = prim->meta().apiSchemaEdits();
      edits.authored = true;
      if (arc_qual != ArcQual::Explicit && edits.is_explicit) {
        edits = StringListOpEdits();
        edits.authored = true;
      }
      auto append = [&](std::vector<std::string>* dst) {
        dst->insert(dst->end(), schemas.begin(), schemas.end());
      };
      switch (arc_qual) {
        case ArcQual::Explicit:
          edits = StringListOpEdits();
          edits.authored = true;
          edits.is_explicit = true;
          edits.explicit_items = schemas;
          prim->meta().apiSchemasQualifier().clear();
          break;
        case ArcQual::Add:
          edits.is_explicit = false;
          append(&edits.added);
          prim->meta().apiSchemasQualifier() = "append";
          break;
        case ArcQual::Prepend:
          edits.is_explicit = false;
          append(&edits.prepended);
          prim->meta().apiSchemasQualifier() = "prepend";
          break;
        case ArcQual::Append:
          edits.is_explicit = false;
          append(&edits.appended);
          prim->meta().apiSchemasQualifier() = "append";
          break;
        case ArcQual::Delete:
          edits.is_explicit = false;
          append(&edits.deleted);
          prim->meta().apiSchemasQualifier() = "delete";
          break;
        case ArcQual::Reorder:
          edits.is_explicit = false;
          append(&edits.ordered);
          break;
      }
      std::vector<std::string>& applied = prim->meta().apiSchemas();
      applied = edits.is_explicit ? edits.explicit_items : edits.added;
      if (!edits.is_explicit) {
        applied.insert(applied.begin(), edits.prepended.begin(),
                       edits.prepended.end());
        applied.insert(applied.end(), edits.appended.begin(),
                       edits.appended.end());
        std::vector<std::string> reordered;
        for (const std::string& schema : edits.ordered) {
          auto it = std::find(applied.begin(), applied.end(), schema);
          if (it != applied.end()) {
            reordered.push_back(*it);
            applied.erase(it);
          }
        }
        reordered.insert(reordered.end(), applied.begin(), applied.end());
        applied = std::move(reordered);
      }
    } else if (key == "references") {
      ReadArcList(prim->meta(), ArcField::References, arc_qual);
    } else if (key == "payload") {
      ReadArcList(prim->meta(), ArcField::Payloads, arc_qual);
    } else if (key == "inherits") {
      ReadArcList(prim->meta(), ArcField::Inherits, arc_qual);
    } else if (key == "specializes") {
      ReadArcList(prim->meta(), ArcField::Specializes, arc_qual);
    } else if (key == "customData") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().customData() = std::move(r.value);
        prim->meta().setCustomDataAuthored();
      }
    } else if (key == "assetInfo") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().assetInfo() = std::move(r.value);
        prim->meta().setAssetInfoAuthored();
      }
    } else if (key == "sdrMetadata") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().sdrMetadata() = std::move(r.value);
        prim->meta().setSdrMetadataAuthored();
      }
    } else if (key == "clips") {
      ParseResult r = ParseDict(*lexer_);
      if (r.success) {
        prim->meta().clips() = std::move(r.value);
        prim->meta().setClipsAuthored();
      }
    } else if (key == "clipSets") {
      std::vector<std::string> names;
      if (Check(TokenType::None)) {
        lexer_->next();
      } else if (Match(TokenType::OpenBracket)) {
        while (!Check(TokenType::CloseBracket) && !AtEnd()) {
          std::string name;
          if (Check(TokenType::String)
                  ? lexer_->expect(TokenType::String, name)
                  : lexer_->expect(TokenType::Identifier, name)) {
            names.push_back(name);
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBracket);
      } else {
        std::string name;
        if (Check(TokenType::String)
                ? lexer_->expect(TokenType::String, name)
                : lexer_->expect(TokenType::Identifier, name)) {
          names.push_back(name);
        }
      }

      StringListOpEdits& edits = prim->meta().clipSetEdits();
      edits.authored = true;
      if (arc_qual != ArcQual::Explicit && edits.is_explicit) {
        edits = StringListOpEdits();
        edits.authored = true;
      }
      auto append = [&](std::vector<std::string>* dst) {
        dst->insert(dst->end(), names.begin(), names.end());
      };
      switch (arc_qual) {
        case ArcQual::Explicit:
          edits = StringListOpEdits();
          edits.authored = true;
          edits.is_explicit = true;
          edits.explicit_items = names;
          break;
        case ArcQual::Add:
          edits.is_explicit = false;
          append(&edits.added);
          break;
        case ArcQual::Prepend:
          edits.is_explicit = false;
          append(&edits.prepended);
          break;
        case ArcQual::Append:
          edits.is_explicit = false;
          append(&edits.appended);
          break;
        case ArcQual::Delete:
          edits.is_explicit = false;
          append(&edits.deleted);
          break;
        case ArcQual::Reorder:
          edits.is_explicit = false;
          append(&edits.ordered);
          break;
      }
    } else if (key == "variantSets") {
      // variantSets = ["setName1", "setName2"]  OR a single bare string
      // (`add variantSets = "shadingVariant"`). Declarations only; no body here.
      std::vector<std::string> authored_names;
      auto add_vs = [&](const std::string& vs_name) {
        authored_names.push_back(vs_name);
      };
      if (Match(TokenType::OpenBracket)) {
        while (!Check(TokenType::CloseBracket) && !AtEnd()) {
          // expect() consumes even on mismatch, so dispatch on the peeked type
          // (the old `expect(String) || expect(Identifier)` dropped identifier
          // names by consuming them in the failed String branch).
          std::string vs_name;
          if (Check(TokenType::String)
                  ? lexer_->expect(TokenType::String, vs_name)
                  : lexer_->expect(TokenType::Identifier, vs_name)) {
            add_vs(vs_name);
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBracket);
      } else {
        std::string vs_name;
        if (Check(TokenType::String)
                ? lexer_->expect(TokenType::String, vs_name)
                : lexer_->expect(TokenType::Identifier, vs_name)) {
          add_vs(vs_name);
        }
      }

      StringListOpEdits& edits = prim->meta().variantSetNameEdits();
      edits.authored = true;
      if (arc_qual != ArcQual::Explicit && edits.is_explicit) {
        edits = StringListOpEdits();
        edits.authored = true;
      }
      auto append = [&](std::vector<std::string>* dst) {
        dst->insert(dst->end(), authored_names.begin(), authored_names.end());
      };
      switch (arc_qual) {
        case ArcQual::Explicit:
          edits = StringListOpEdits();
          edits.authored = true;
          edits.is_explicit = true;
          edits.explicit_items = authored_names;
          break;
        case ArcQual::Add:
          edits.is_explicit = false;
          append(&edits.added);
          break;
        case ArcQual::Prepend:
          edits.is_explicit = false;
          append(&edits.prepended);
          break;
        case ArcQual::Append:
          edits.is_explicit = false;
          append(&edits.appended);
          break;
        case ArcQual::Delete:
          edits.is_explicit = false;
          append(&edits.deleted);
          break;
        case ArcQual::Reorder:
          edits.is_explicit = false;
          append(&edits.ordered);
          break;
      }

      std::vector<std::string> effective = edits.is_explicit
          ? edits.explicit_items
          : edits.added;
      if (!edits.is_explicit) {
        effective.insert(effective.begin(), edits.prepended.begin(),
                         edits.prepended.end());
        effective.insert(effective.end(), edits.appended.begin(),
                         edits.appended.end());
        std::vector<std::string> reordered;
        for (const std::string& name : edits.ordered) {
          auto it = std::find(effective.begin(), effective.end(), name);
          if (it != effective.end()) {
            reordered.push_back(*it);
            effective.erase(it);
          }
        }
        reordered.insert(reordered.end(), effective.begin(), effective.end());
        effective = std::move(reordered);
      }
      std::vector<VariantSetData> old =
          std::move(prim->meta().variantSets());
      std::vector<VariantSetData>& sets = prim->meta().variantSets();
      sets.clear();
      for (const std::string& name : effective) {
        auto it = std::find_if(old.begin(), old.end(),
                               [&](const VariantSetData& set) {
                                 return set.name == name;
                               });
        if (it != old.end()) {
          sets.push_back(std::move(*it));
          old.erase(it);
        } else {
          VariantSetData set;
          set.name = name;
          sets.push_back(std::move(set));
        }
      }
    } else if (key == "variants" || key == "variantSelection") {
      // variants = { string set = "selection"  string set2 = "sel2" }. USD
      // supports a selection per set; record them all in variantSelections()
      // (keeping the legacy single field set to the first for back-compat).
      if (prim) prim->meta().setVariantSelectionsAuthored();
      if (Match(TokenType::OpenBrace)) {
        while (!Check(TokenType::CloseBrace) && !AtEnd()) {
          // Optional leading type name ("string"); the key may be a quoted
          // string or a bare identifier.
          std::string set_name;
          if (Check(TokenType::String)) {
            lexer_->expect(TokenType::String, set_name);
          } else if (Check(TokenType::Identifier)) {
            std::string first;
            lexer_->expect(TokenType::Identifier, first);
            // `string set = ...` -> first is the type name, read the real key.
            if (!Check(TokenType::Equals) &&
                (Check(TokenType::Identifier) || Check(TokenType::String))) {
              if (Check(TokenType::String)) {
                lexer_->expect(TokenType::String, set_name);
              } else {
                lexer_->expect(TokenType::Identifier, set_name);
              }
            } else {
              set_name = first;
            }
          } else {
            break;
          }
          if (!Match(TokenType::Equals)) break;
          std::string sel_name;
          if (!lexer_->expect(TokenType::String, sel_name)) break;
          prim->meta().variantSelections().emplace_back(set_name, sel_name);
          if (prim->meta().variantSelection.empty()) {
            prim->meta().variantSelection = set_name + "=" + sel_name;
          }
          Match(TokenType::Comma);
        }
        Match(TokenType::CloseBrace);
      }
    } else {
      // Unknown (unmodeled) metadata: consume the value structurally, but
      // PRESERVE its raw source text so the writer can re-emit it verbatim
      // (the legacy parser round-trips unknown prim metadata; dropping it
      // loses pipeline-specific opinions like `sceneName`).
      lexer_->peek();  // ensure the value's first token is scanned
      const size_t vstart = lexer_->token_start();
      const bool skipped = SkipValueLike();
      if (skipped) {
        lexer_->peek();  // scan the FOLLOWING token; its start bounds the value
        size_t vend = lexer_->token_start();
        const char* base = lexer_->input_data();
        while (vend > vstart &&
               (base[vend - 1] == ' ' || base[vend - 1] == '\t' ||
                base[vend - 1] == '\r' || base[vend - 1] == '\n')) {
          vend--;
        }
        if (vend > vstart) {
          // A list-op qualifier consumed before the key must ride along or
          // the re-emitted opinion silently changes strength (`prepend
          // weirdKey = [...]` re-emitted bare).
          std::string qual_prefix;
          switch (arc_qual) {
            case ArcQual::Add: qual_prefix = "add "; break;
            case ArcQual::Prepend: qual_prefix = "prepend "; break;
            case ArcQual::Append: qual_prefix = "append "; break;
            case ArcQual::Delete: qual_prefix = "delete "; break;
            case ArcQual::Reorder: qual_prefix = "reorder "; break;
            default: break;
          }
          prim->meta().unknownMeta().emplace_back(
              qual_prefix + key, std::string(base + vstart, vend - vstart));
        }
      }
      AddWarning("Unknown prim metadata (preserved): " + key);
    }
  }

  return Match(TokenType::CloseParen);
}

// ============================================================
// AsciiParser public interface
// ============================================================

AsciiParser::AsciiParser(const ParseOptions& options)
    : impl_(std::make_unique<Impl>(NormalizeParseOptions(options))) {}

AsciiParser::~AsciiParser() = default;

AsciiParser::AsciiParser(AsciiParser&&) noexcept = default;
AsciiParser& AsciiParser::operator=(AsciiParser&&) noexcept = default;

bool AsciiParser::Parse(const char* data, size_t length) {
  return impl_->Parse(data, length);
}

bool AsciiParser::ParseOwned(std::string&& data) {
  return impl_->ParseOwned(std::move(data));
}

bool AsciiParser::ParseFile(const char* filename) {
  return impl_->ParseFile(filename);
}

Stage AsciiParser::TakeStage() {
  return impl_->TakeStage();
}

const std::vector<ParseError>& AsciiParser::GetErrors() const {
  return impl_->GetErrors();
}

bool AsciiParser::HasErrors() const {
  return impl_->HasErrors();
}

const std::vector<std::string>& AsciiParser::GetWarnings() const {
  return impl_->GetWarnings();
}

bool AsciiParser::UsedFastPath() const {
  return impl_->UsedFastPath();
}

}  // namespace next
}  // namespace lightusd
