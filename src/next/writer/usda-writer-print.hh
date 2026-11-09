// SPDX-License-Identifier: Apache-2.0
// USDA text-serialization printers + the shared chunking types and the
// subtree-walk helper, split from usda-writer.cc. These turn Layer/PrimSpec
// into USDA text; the parallel write driver in usda-writer.cc feeds them
// through SegmentSink.
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "../layer/layer.hh"
#include "../layer/prim-spec.hh"
#include "../crate/crate-format.hh"   // CrateTypeId
#include "../crate/lazy-array.hh"     // LazyArrayRef
#include "dtoa.hh"          // dtos()
#include "escape-string.hh"  // EscapeUSDAString()
#include "usda-format-utils.hh"  // FormatArcRef()
#include "stream-writer.hh"
#include "usda-writer.hh"
#include "value-printer.hh"

namespace lightusd {
namespace next {

// released after its last chunk has been written -- so a decoded buffer only
// lives while its chunks are inside the formatting window, never for the whole
// write.
struct DecodeSlot {
  const Value* src = nullptr;
  Value decoded;
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::once_flag once;
#else
  bool done = false;
#endif

  const Value& get() {
#if defined(LIGHTUSD_ENABLE_THREAD)
    std::call_once(once, [this]() { decoded = src->materialized_copy(); });
#else
    if (!done) {
      decoded = src->materialized_copy();
      done = true;
    }
#endif
    return decoded;
  }
};

// One unit of parallel write work, emitted in document order by the build pass.
// The build produces structural glue as Text; every array value worth
// offloading becomes a WholeValue task (formatted by one worker) or, for a large
// chunkable array, a run of Chunk tasks (formatted by many workers).
struct WriteTask {
  enum class Kind : uint8_t { Text, WholeValue, Chunk } kind;
  std::string text;             // Text: literal bytes, produced during the walk
  const Value* value = nullptr; // WholeValue/Chunk: borrowed from the Layer
  DecodeSlot* slot = nullptr;   // Chunk: decode-on-first-use source (or null)
  size_t lo = 0;                // Chunk: USD element range [lo, hi)
  size_t hi = 0;
  bool open = false;            // Chunk: emit leading '['
  bool close = false;           // Chunk: emit trailing ']'
};

// Offload tuning. An array property with >= kOffloadMinElems USD elements is
// formatted by a worker rather than inline on the build thread (tiny arrays stay
// inline to avoid task spam). A chunkable array with >= kSplitMinElems elements
// is split into kChunkElems-sized pieces so one giant array spreads across
// workers (a single unsplit array holds up the in-order writer for its whole
// formatting time); kChunkElems also bounds each worker buffer (~3 MB for float3
// at 64Ki). A chunkable-TYPE array that is not borrowable (compressed, or lazy
// with reordered components such as quath) is decoded once into a DecodeSlot
// and then chunked (compressed int arrays only up to kMaxDecodeSplitElems).
constexpr size_t kChunkElems = 64u * 1024u;
constexpr size_t kSplitMinElems = 2u * kChunkElems;
constexpr size_t kOffloadMinElems = 256u;
// Compressed int arrays up to this size are decoded once (<= 64 MiB) and split
// like any other array; beyond it they keep the streaming no-decode path.
constexpr size_t kMaxDecodeSplitElems = 16u * 1024u * 1024u;

inline bool IsCompressedLazyIntArray(const Value& value) {
  if (!value.is_lazy() || value.type_id() != TypeId::Int) return false;
  const LazyArrayRef* ref = value.lazy_ref();
  return ref && ref->is_compressed && ref->crate_type == CrateTypeId::Int;
}

// Collects ordered tasks during a build walk. Structural bytes accrue in `cur`
// (the StreamWriter target); each offloaded array flushes `cur` as a Text task
// and appends WholeValue/Chunk task(s). The worker emits the array's full
// `[...]`, so the build path writes nothing for the value itself. Split
// non-borrowable arrays get a DecodeSlot in `holder` (stable addresses) that
// their chunks share; the decode is freed once consumed.
struct SegmentSink {
  std::vector<WriteTask>* tasks = nullptr;
  std::string* cur = nullptr;
  const PrintOptions* po = nullptr;
  std::deque<DecodeSlot>* holder = nullptr;

  void flush_text() {
    if (cur && !cur->empty()) {
      WriteTask t;
      t.kind = WriteTask::Kind::Text;
      // Copy out an exact-size string and keep `cur`'s capacity for the next
      // run (moving it out would regrow a fresh buffer by doubling each time).
      t.text.assign(cur->data(), cur->size());
      cur->clear();
      tasks->push_back(std::move(t));
    }
  }

  void push_chunks(const Value* src, DecodeSlot* slot, size_t n) {
    for (size_t lo = 0; lo < n; lo += kChunkElems) {
      WriteTask t;
      t.kind = WriteTask::Kind::Chunk;
      t.value = src;
      t.slot = slot;
      t.lo = lo;
      t.hi = (lo + kChunkElems < n) ? (lo + kChunkElems) : n;
      t.open = (lo == 0);
      t.close = (t.hi == n);
      tasks->push_back(std::move(t));
    }
  }

  void offload_array(const Value* v) {
    flush_text();
    const size_t n = ArrayElementCount(*v);
    if (n >= kSplitMinElems && IsChunkableArray(*v, *po)) {
      push_chunks(v, nullptr, n);  // borrowable: chunk in place
    } else if (IsCompressedLazyIntArray(*v) &&
               (n < kSplitMinElems || n > kMaxDecodeSplitElems)) {
      // Streamed by the printer's own block decoder (a giant one directly into
      // the output by the writer thread) -- no whole-array decode buffer.
      WriteTask t;
      t.kind = WriteTask::Kind::WholeValue;
      t.value = v;
      tasks->push_back(std::move(t));
    } else if (n >= kSplitMinElems && holder && IsChunkableType(*v, *po)) {
      holder->emplace_back();  // decoded on first use, then chunked
      holder->back().src = v;
      push_chunks(v, &holder->back(), n);
    } else {
      WriteTask t;
      t.kind = WriteTask::Kind::WholeValue;
      t.value = v;
      tasks->push_back(std::move(t));
    }
  }
};

// Freestanding integer -> decimal string: IntToStr/UIntToStr live in ../strfmt.hh
// (shared with the value printer).

// Get specifier keyword

inline bool IsCompressedLazyIntArray(const Value& value);  // fwd (defined above)

template <typename OpenFn, typename CloseFn>
void WalkPrimSubtree(const PrimSpec& spec, const Layer& layer, int depth,
                     const USDAWriteOptions& opts, OpenFn&& open,
                     CloseFn&& close) {
  // Composed-stage output: pxr usdcat --flatten DROPS deactivated prims
  // (and their subtrees) from the flattened layer entirely. The in-memory
  // stage keeps them (IsActive stays queryable); only the flatten output
  // prunes.
  if (opts.composed_stage_output && !spec.meta().active) return;

  struct Frame {
    const PrimSpec* prim;
    int depth;
    size_t next_child;
    bool opened;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{&spec, depth, 0, false});

  // Cycle guard. The explicit stack removed the C-stack overflow but not the
  // non-termination: a malformed/adversarial USDC whose primChildren links
  // form a cycle (A -> B -> A) otherwise writes output forever until OOM.
  // Composition already assumes child indices can be stale or cyclic —
  // Compositor::GraftSubtree carries the same defense.
  // The `on_stack` set is what actually guarantees termination (a prim already
  // open on this path is never re-entered, so depth is bounded by prim_count).
  // kMaxWriteDepth is only a cheap backstop and must stay far above any
  // legitimate hierarchy — test_deep_stage_writer alone nests 8192 deep.
  std::unordered_set<const PrimSpec*> on_stack;
  on_stack.insert(&spec);
  constexpr int kMaxWriteDepth = 1 << 20;

  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (!frame.opened) {
      open(*frame.prim, frame.depth, /*is_child=*/stack.size() > 1);
      frame.opened = true;
    }

    // When `segsink` is active (the parallel build walk), the same sink
    // propagates so each child's large array values are offloaded too.
    // Bracketed variant HOLDER prims ("{set=var}", crate representation) are
    // not real children: their selections are emitted through variantSets.
    bool descended = false;
    const auto& children = frame.prim->child_indices();
    while (frame.next_child < children.size()) {
      const PrimSpec* child = layer.prim(children[frame.next_child++]);
      if (!child) continue;
      const std::string& name = child->name();
      if (name.size() >= 2 && name.front() == '{' && name.back() == '}') {
        continue;
      }
      if (opts.composed_stage_output && !child->meta().active) continue;

      const int child_depth = frame.depth + 1;
      // Skip a child already open on this path (cycle) or beyond the depth
      // ceiling, rather than recursing into it forever.
      if (child_depth >= kMaxWriteDepth || !on_stack.insert(child).second) {
        continue;
      }
      stack.push_back(Frame{child, child_depth, 0, false});
      descended = true;
      break;
    }
    if (descended) continue;

    close(*frame.prim, frame.depth);
    on_stack.erase(frame.prim);
    stack.pop_back();
  }
}

const char* SpecifierKeyword(PrimSpecifier spec);
void WriteIndent(StreamWriter& os, int depth, const std::string& indent);
std::string IndentContinuation(const std::string& s, const std::string& prefix);
void WriteLayerMeta(StreamWriter& os, const LayerMeta& meta, const USDAWriteOptions& opts);
bool WritePropMeta(StreamWriter& os, const PrimSpec& spec, PropNameId name_id, int depth, const USDAWriteOptions& opts);
void WriteTimeSamples(StreamWriter& os, const std::string& name, PropNameId name_id, const PrimSpec& spec, int depth, const USDAWriteOptions& opts, SegmentSink* segsink);
void WriteProperty(StreamWriter& os, const PropSlot& slot, const PrimSpec& spec, int depth, const USDAWriteOptions& opts, SegmentSink* segsink = nullptr);
void WriteRelationship(StreamWriter& os, const std::string& name, const std::vector<Path>& targets, const PrimSpec& spec, PropNameId name_id, int depth, const USDAWriteOptions& opts);
void WritePrimSpec(StreamWriter& os, const PrimSpec& spec, const Layer& layer, int depth, const USDAWriteOptions& opts, SegmentSink* segsink = nullptr);
void WriteVariantSets(StreamWriter& os, const std::vector<PrimVariantSetData>& sets, const Layer& layer, const std::string& initial_owner_path, int initial_depth, const USDAWriteOptions& opts, SegmentSink* segsink);
void WritePrimSpecOpen(StreamWriter& os, const PrimSpec& spec, const Layer& layer, int depth, const USDAWriteOptions& opts, SegmentSink* segsink);
void WriteRootPrimOrder(StreamWriter& os, const LayerMeta& meta);
void WriteStageBodySerial(StreamWriter& os, const Layer& layer, const LayerMeta& meta, const USDAWriteOptions& opts);

}  // namespace next
}  // namespace lightusd
