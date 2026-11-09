#include "core/native-io.hh"
#include "escape-string.hh"
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDA Writer Implementation

#include "usda-writer.hh"
#include "usda-writer-print.hh"
#include "layer/layer.hh"
#include "value-printer.hh"
#include "stream-writer.hh"
#include "dtoa.hh"
#include "usda-format-utils.hh"
#include "crate/lazy-array.hh"
#include "crate/crate-timing.hh"
#include "strfmt.hh"
#include "layer/property-index.hh"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <deque>
#include <unordered_set>
#include <vector>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace lightusd {

namespace {

// A non-borrowable (compressed / component-reordered lazy) array that is still
// worth splitting across workers. It is decoded ONCE, on first use, by whichever
// worker reaches one of its chunks first (the others wait on the once-flag), and
#if defined(LIGHTUSD_ENABLE_THREAD)

// Resolve the effective worker count from the option (1 = serial; <=0 = auto;
// >1 = exactly that many). The parallel writer formats balanced segments (one
// giant array no longer pins a single worker), so throughput scales with cores;
// the auto default is capped at kAutoCap to stay reasonable on shared machines
// while still feeding many cores. An explicit request is honored as-is so callers
// can override on unusual hardware. Measured on a 16-core/32-thread box (Island
// flatten, 7.5 GB): 16 -> 32 workers cut the write ~12% (1.87 -> 1.65 s) for
// ~+300 MB of in-flight buffers; formatting is float-to-text bound, so SMT
// siblings add less than real cores.
int ResolveWriteThreads(int requested) {
  if (requested == 1) return 1;
  if (requested > 1) return requested;
  constexpr int kAutoCap = 32;
  int hw = static_cast<int>(std::thread::hardware_concurrency());
  if (hw < 1) hw = 1;
  return std::min(hw, kAutoCap);
}

// Array-affecting subset of the write options (the value printer only consults
// the precision/limit/compact fields). Mirrors the inline construction in
// WriteProperty so frontier analysis sees the same chunkability decision.
PrintOptions MakeArrayPrintOpts(const USDAWriteOptions& opts) {
  PrintOptions p;
  p.float_precision = opts.float_precision;
  p.double_precision = opts.double_precision;
  p.max_array_elements = opts.max_elements_per_line;
  p.compact = opts.compact;
  return p;
}

// Parallel stage body, byte-identical to the serial writer for any thread count.
//
//  1. Skeleton (serial, cheap): one pre-order walk records every prim's
//     open/close event -- the same WalkPrimSubtree the streaming writer uses,
//     so visit order and cycle/depth guards are identical. The event list is
//     cut into contiguous, cost-balanced RANGES.
//  2. Build (parallel): a worker replays one range into its own document-ordered
//     task list: prim headers, property lines and small arrays become Text,
//     larger arrays are offloaded as WholeValue/Chunk tasks (see SegmentSink).
//     A prim's text depends only on the prim and its depth, so the ranges'
//     outputs concatenate to exactly the serial document. The built task list
//     is cut into byte-balanced SEGMENTS.
//  3. Format (parallel): a worker formats one whole segment (its Text plus the
//     arrays it references) into a pooled buffer.
//  4. Write (main thread): segment buffers are written strictly in document
//     order.
// Building and formatting are pipelined over one worker pool: a worker formats
// the oldest published segment when the in-order window has room, otherwise it
// builds the next range. Ranges are published (their segments appended to the
// global order) strictly in range order. Two bounds keep memory flat: at most
// W segments are in flight past the writer, and ranges are built at most
// kBuildAhead ranges past the one being written, so neither the structure text
// nor decoded array buffers ever accumulate for the whole document.
void WriteStageBodyParallel(StreamWriter& os, const Layer& layer,
                            const LayerMeta& meta, const USDAWriteOptions& opts,
                            int nthreads) {
  const PrintOptions po = MakeArrayPrintOpts(opts);
  CratePhaseTimer timer(opts.enable_timing, "next_usda_write");
  using Clock = std::chrono::steady_clock;
  const auto now = [&]() {
    return opts.enable_timing ? Clock::now() : Clock::time_point{};
  };
  const auto elapsed = [&](Clock::time_point start) {
    return opts.enable_timing
               ? std::chrono::duration<double, std::milli>(Clock::now() - start).count()
               : 0.0;
  };

  // Pre-warm the global interning table on the main thread (its lazy init is the
  // only non-reentrant spot on the write path; reads are thread-safe after).
  (void)GetPropNameTable();

  // The layer header is tiny: write it straight through.
  WriteLayerMeta(os, meta, opts);
  WriteRootPrimOrder(os, meta);

  // ---- 1. Skeleton walk + range cut ----
  enum : uint8_t { kEvOpenRoot, kEvOpenChild, kEvClose, kEvRootEnd };
  struct WalkEvent {
    const PrimSpec* prim;
    int32_t depth;
    uint8_t kind;
  };
  std::vector<WalkEvent> events;
  std::vector<uint32_t> ev_cost;  // per event: relative build cost estimate
  for (uint32_t root_idx : layer.root_indices()) {
    const PrimSpec* root = layer.prim(root_idx);
    if (!root) continue;
    WalkPrimSubtree(
        *root, layer, 0, opts,
        [&](const PrimSpec& prim, int d, bool is_child) {
          events.push_back(WalkEvent{&prim, d,
                                     is_child ? uint8_t(kEvOpenChild)
                                              : uint8_t(kEvOpenRoot)});
          // ~one unit per property line plus the header.
          ev_cost.push_back(
              4u + static_cast<uint32_t>(prim.properties().slots().size()));
        },
        [&](const PrimSpec& prim, int d) {
          events.push_back(WalkEvent{&prim, d, uint8_t(kEvClose)});
          ev_cost.push_back(0);
        });
    events.push_back(WalkEvent{root, 0, uint8_t(kEvRootEnd)});
    ev_cost.push_back(0);
  }
  if (events.empty()) return;

  // Many more ranges than workers, so dynamic claiming balances uneven prims
  // and the first range (which gates all formatting) is quick to build.
  std::vector<size_t> range_begin;  // event index where each range starts; +[n]
  {
    uint64_t total_cost = 0;
    for (uint32_t c : ev_cost) total_cost += c;
    const uint64_t r_target = static_cast<uint64_t>(nthreads) * 16u;
    const uint64_t per_range = std::max<uint64_t>(1, total_cost / r_target);
    range_begin.push_back(0);
    uint64_t acc = 0;
    for (size_t i = 0; i < events.size(); ++i) {
      acc += ev_cost[i];
      if (acc >= per_range && i + 1 < events.size()) {
        range_begin.push_back(i + 1);
        acc = 0;
      }
    }
    range_begin.push_back(events.size());
  }
  std::vector<uint32_t>().swap(ev_cost);
  const size_t R = range_begin.size() - 1;
  timer.lap("skeleton");

  // ---- Segment sizing ----
  // Estimated output bytes of a task (~8 chars per scalar component).
  auto task_bytes = [](const WriteTask& t) -> uint64_t {
    if (t.kind == WriteTask::Kind::Text) return t.text.size();
    size_t comps = GetComponentCount(t.value->type_id());
    if (comps < 1) comps = 1;
    const uint64_t elems = (t.kind == WriteTask::Kind::Chunk)
                               ? uint64_t(t.hi - t.lo)
                               : uint64_t(ArrayElementCount(*t.value));
    return elems * uint64_t(comps) * 8u + 2u;
  };
  // Very large compressed int arrays are formatted by the writer thread
  // straight into the output so they never need a full text buffer.
  auto direct_stream_task = [&](const WriteTask& t) -> bool {
    constexpr uint64_t kDirectStreamBytes = 64ull << 20;
    return t.kind == WriteTask::Kind::WholeValue && t.value &&
           IsCompressedLazyIntArray(*t.value) &&
           task_bytes(t) >= kDirectStreamBytes;
  };
  // ~Chunk-sized segments: large enough that per-segment overhead vanishes,
  // small enough to balance and to keep the in-flight window's buffers small.
  constexpr uint64_t kSegTargetBytes = 4ull << 20;

  struct Segment {
    size_t lo = 0, hi = 0;             // task range within its build range
    bool direct = false;
    std::vector<DecodeSlot*> frees;    // decodes to release once written
  };
  struct RangeOut {
    std::vector<WriteTask> tasks;
    std::deque<DecodeSlot> holder;     // decode slots of split arrays
    std::vector<Segment> segs;
  };
  std::vector<RangeOut> range_out(R);

  auto build_range = [&](size_t r) {
    RangeOut& out = range_out[r];
    {
      std::string cur;
      StreamWriter sw(&cur);
      SegmentSink sink;
      sink.tasks = &out.tasks;
      sink.cur = &cur;
      sink.po = &po;
      sink.holder = &out.holder;
      for (size_t i = range_begin[r]; i < range_begin[r + 1]; ++i) {
        const WalkEvent& ev = events[i];
        switch (ev.kind) {
          case kEvOpenChild:
            sw << "\n";
            WritePrimSpecOpen(sw, *ev.prim, layer, ev.depth, opts, &sink);
            break;
          case kEvOpenRoot:
            WritePrimSpecOpen(sw, *ev.prim, layer, ev.depth, opts, &sink);
            break;
          case kEvClose:
            WriteIndent(sw, ev.depth, opts.indent);
            sw << "}\n";
            break;
          case kEvRootEnd:
            sw << "\n";
            break;
        }
      }
      sink.flush_text();
    }
    // Cut into segments. A decode slot is released with the segment holding
    // its closing chunk: every earlier chunk sits in an earlier (or the same)
    // segment, so all of them have been formatted by then.
    const std::vector<WriteTask>& tasks = out.tasks;
    const size_t m = tasks.size();
    size_t start = 0;
    uint64_t acc = 0;
    for (size_t i = 0; i < m; ++i) {
      if (direct_stream_task(tasks[i])) {
        if (start != i) out.segs.push_back(Segment{start, i, false, {}});
        out.segs.push_back(Segment{i, i + 1, true, {}});
        start = i + 1;
        acc = 0;
        continue;
      }
      acc += task_bytes(tasks[i]);
      if (acc >= kSegTargetBytes) {
        out.segs.push_back(Segment{start, i + 1, false, {}});
        start = i + 1;
        acc = 0;
      }
    }
    if (start != m) out.segs.push_back(Segment{start, m, false, {}});
    for (Segment& sg : out.segs) {
      for (size_t i = sg.lo; i < sg.hi; ++i) {
        if (tasks[i].slot && tasks[i].close) sg.frees.push_back(tasks[i].slot);
      }
    }
  };

  auto format_segment_to = [&](RangeOut& ro, const Segment& sg,
                               StreamWriter& sw) {
    for (size_t i = sg.lo; i < sg.hi; ++i) {
      WriteTask& t = ro.tasks[i];
      switch (t.kind) {
        case WriteTask::Kind::Text:
          sw.write(t.text.data(), t.text.size());
          std::string().swap(t.text);  // free structure bytes once copied
          break;
        case WriteTask::Kind::Chunk:
          PrintArrayRangeToStream(sw, t.slot ? t.slot->get() : *t.value, po,
                                  t.lo, t.hi, t.open, t.close);
          break;
        case WriteTask::Kind::WholeValue:
          PrintValue(sw, *t.value, po);  // worker emits the full `[ ... ]`
          break;
      }
    }
  };

  // ---- 2-4. Pipelined build / format / in-order write ----
  // Scheduler state, all guarded by `mu`. `order` lists published segments in
  // document order; a std::deque so appends never move existing slots.
  struct SegSlot {
    size_t range;
    size_t index;       // into range_out[range].segs
    uint8_t state = 0;  // 0 pending, 1 buffer ready, 2 direct (writer formats)
    std::string buf;
  };
  std::mutex mu;
  std::condition_variable cv_work;    // workers: new segment / window / range
  std::condition_variable cv_writer;  // writer: segment ready / range published
  std::deque<SegSlot> order;
  std::vector<uint8_t> built(R, 0);
  size_t next_build = 0;      // next range to claim for building
  size_t next_pub = 0;        // ranges [0, next_pub) are published into `order`
  size_t next_fmt = 0;        // next `order` index to claim for formatting
  size_t consumed = 0;        // `order` entries already written
  size_t writing_range = 0;   // range the writer is currently in
  const size_t W = static_cast<size_t>(nthreads) * 2 + 2;
  const size_t kBuildAhead = static_cast<size_t>(nthreads) * 2 + 2;

  // Segment buffers are recycled through a small pool (guarded by `mu`): the
  // writer hands a written buffer back (capacity kept) instead of freeing it,
  // so workers reuse already-faulted pages rather than growing a fresh
  // multi-MB string by doubling for every segment (that realloc/page-fault
  // churn was most of the writer's kernel time). An outlier-sized buffer (a
  // giant unsplittable value) is released instead of pooled.
  std::vector<std::string> buf_pool;
  constexpr size_t kMaxPooledCapacity = 4u * (4u << 20);
  // Worker sums overlap in time. Update under the scheduler's existing lock.
  double build_work_ms = 0.0, format_work_ms = 0.0;
  double writer_wait_ms = 0.0, direct_ms = 0.0;
  size_t buffered_bytes = 0, peak_buffered_bytes = 0;

  auto worker = [&]() {
    std::unique_lock<std::mutex> lk(mu);
    for (;;) {
      if (next_fmt < order.size() && next_fmt < consumed + W) {
        SegSlot& slot = order[next_fmt++];
        RangeOut& ro = range_out[slot.range];
        const Segment& sg = ro.segs[slot.index];
        if (sg.direct) {
          slot.state = 2;
          cv_writer.notify_one();
          continue;
        }
        std::string buf;
        if (!buf_pool.empty()) {
          buf = std::move(buf_pool.back());
          buf_pool.pop_back();
        }
        lk.unlock();
        const auto format_start = now();
        buf.clear();
        {
          StreamWriter sw(&buf);
          format_segment_to(ro, sg, sw);
        }
        const double format_ms = elapsed(format_start);
        lk.lock();
        if (opts.enable_timing) {
          format_work_ms += format_ms;
          buffered_bytes += buf.size();
          peak_buffered_bytes = std::max(peak_buffered_bytes, buffered_bytes);
        }
        slot.buf = std::move(buf);
        slot.state = 1;
        cv_writer.notify_one();
        continue;
      }
      if (next_build < R && next_build < writing_range + kBuildAhead) {
        const size_t r = next_build++;
        lk.unlock();
        const auto build_start = now();
        build_range(r);
        const double build_ms = elapsed(build_start);
        lk.lock();
        build_work_ms += build_ms;
        built[r] = 1;
        bool published = false;
        while (next_pub < R && built[next_pub]) {
          const size_t nseg = range_out[next_pub].segs.size();
          for (size_t j = 0; j < nseg; ++j) {
            SegSlot s;
            s.range = next_pub;
            s.index = j;
            order.push_back(std::move(s));
          }
          ++next_pub;
          published = true;
        }
        if (published) {
          cv_work.notify_all();
          cv_writer.notify_one();
        }
        continue;
      }
      if (next_build >= R && next_pub >= R && next_fmt >= order.size()) break;
      cv_work.wait(lk);
    }
  };

  const int nw = std::max(1, nthreads);
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(nw));
  for (int t = 0; t < nw; ++t) pool.emplace_back(worker);

  size_t g = 0;  // global `order` index being written
  for (size_t r = 0; r < R; ++r) {
    RangeOut& ro = range_out[r];
    {
      const auto wait_start = now();
      std::unique_lock<std::mutex> lk(mu);
      cv_writer.wait(lk, [&]() { return next_pub > r; });
      writer_wait_ms += elapsed(wait_start);
    }
    const size_t nseg = ro.segs.size();
    for (size_t j = 0; j < nseg; ++j, ++g) {
      uint8_t state = 0;
      std::string buf;
      {
        const auto wait_start = now();
        std::unique_lock<std::mutex> lk(mu);
        cv_writer.wait(lk, [&]() { return order[g].state != 0; });
        writer_wait_ms += elapsed(wait_start);
        state = order[g].state;
        if (state == 1) {
          buf = std::move(order[g].buf);
          if (opts.enable_timing) buffered_bytes -= buf.size();
        }
      }
      const Segment& sg = ro.segs[j];
      if (state == 2) {
        const auto direct_start = now();
        format_segment_to(ro, sg, os);
        direct_ms += elapsed(direct_start);
      } else {
        os.write(buf.data(), buf.size());
      }
      for (DecodeSlot* ds : sg.frees) ds->decoded = Value{};  // release decodes
      {
        std::lock_guard<std::mutex> lk(mu);
        if (state == 1 && buf.capacity() <= kMaxPooledCapacity) {
          buf.clear();
          buf_pool.push_back(std::move(buf));  // recycle (capacity kept)
        }
        consumed = g + 1;
      }
      cv_work.notify_all();
    }
    // Every segment of range r is written, so no worker references it any
    // more: release its tasks and decode slots, and let building advance.
    std::vector<WriteTask>().swap(ro.tasks);
    std::deque<DecodeSlot>().swap(ro.holder);
    std::vector<Segment>().swap(ro.segs);
    {
      std::lock_guard<std::mutex> lk(mu);
      writing_range = r + 1;
    }
    cv_work.notify_all();
  }

  for (auto& th : pool) th.join();
  timer.lap("pipeline");
  if (opts.enable_timing) {
    LIGHTUSD_LOG_I("[next_usda_write] build_work=" << build_work_ms
                   << "ms format_work=" << format_work_ms
                   << "ms writer_wait=" << writer_wait_ms
                   << "ms direct=" << direct_ms << "ms ranges=" << R
                   << " segments=" << g << " ready_buffer_peak_bytes="
                   << peak_buffered_bytes);
  }
}

#endif  // LIGHTUSD_ENABLE_THREAD

}  // anonymous namespace

// ============================================================
// Public API implementations
// ============================================================

// Core writer: targets a StreamWriter (any backend — stdio, file, string, or a
// host-supplied WASM/WASI sink). The std::ostream / file / string entry points
// below all wrap this.
USDAWriteResult WriteUSDA(StreamWriter& os, const Stage& stage,
                          const USDAWriteOptions& options) {
  USDAWriteResult result;

  const Layer* root_layer = stage.GetRootLayer();
  if (!root_layer) {
    result.error = "Stage has no root layer";
    return result;
  }

  // Write layer header using stage metadata
  LayerMeta meta;
  const StageMeta& stage_meta = stage.GetMeta();
  meta.defaultPrim = stage_meta.defaultPrim;
  meta.defaultPrim_set = stage_meta.defaultPrim_set;
  meta.upAxis = stage_meta.upAxis;
  meta.metersPerUnit = stage_meta.metersPerUnit;
  meta.timeCodesPerSecond = stage_meta.timeCodesPerSecond;
  meta.startTimeCode = stage_meta.startTimeCode;
  meta.endTimeCode = stage_meta.endTimeCode;
  meta.upAxis_set = stage_meta.upAxis_set;
  meta.metersPerUnit_set = stage_meta.metersPerUnit_set;
  meta.timeCodesPerSecond_set = stage_meta.timeCodesPerSecond_set;
  meta.startTimeCode_set = stage_meta.startTimeCode_set;
  meta.endTimeCode_set = stage_meta.endTimeCode_set;
  meta.framesPerSecond = stage_meta.framesPerSecond;
  meta.framesPerSecond_set = stage_meta.framesPerSecond_set;
  meta.hasOwnedSubLayers = root_layer->meta().hasOwnedSubLayers;
  meta.hasOwnedSubLayers_set = root_layer->meta().hasOwnedSubLayers_set;
  meta.kilogramsPerUnit = stage_meta.kilogramsPerUnit;
  meta.kilogramsPerUnit_set = stage_meta.kilogramsPerUnit_set;
  meta.colorConfiguration = stage_meta.colorConfiguration;
  meta.colorManagementSystem = stage_meta.colorManagementSystem;
  meta.colorConfiguration_set = stage_meta.colorConfiguration_set;
  meta.colorManagementSystem_set = stage_meta.colorManagementSystem_set;
  meta.renderSettingsPrimPath = stage_meta.renderSettingsPrimPath;
  meta.renderSettingsPrimPath_set = stage_meta.renderSettingsPrimPath_set;
  meta.doc = stage_meta.doc;
  meta.comment = stage_meta.comment;
  meta.owner = stage_meta.owner;
  meta.autoPlay = stage_meta.autoPlay;
  meta.autoPlay_set = stage_meta.autoPlay_set;
  meta.playbackMode = stage_meta.playbackMode;
  meta.playbackMode_set = stage_meta.playbackMode_set;
  meta.doc_set = stage_meta.doc_set;
  meta.comment_set = stage_meta.comment_set;
  meta.owner_set = stage_meta.owner_set;
  // Dictionary-valued stage metadata and sublayer paths are not mirrored on
  // StageMeta; take them from the composed root layer directly.
  meta.customLayerData = root_layer->meta().customLayerData;
  meta.expressionVariables = root_layer->meta().expressionVariables;
  meta.customLayerData_set = root_layer->meta().customLayerData_set;
  meta.expressionVariables_set = root_layer->meta().expressionVariables_set;
  // subLayers/relocates are composition directives; when this stage came from
  // a compose they are CONSUMED and a flattened layer must not re-state them
  // (pxr UsdStage::Flatten drops them as well). For plain-load stages they
  // are still live opinions and must round-trip.
  if (!options.composed_stage_output) {
    meta.subLayers = root_layer->meta().subLayers;
    meta.subLayers_set = root_layer->meta().subLayers_set;
    meta.subLayerOffsets = root_layer->meta().subLayerOffsets;
    meta.relocates = root_layer->meta().relocates;
    meta.relocates_set = root_layer->meta().relocates_set;
  }
  meta.unknownMeta = root_layer->meta().unknownMeta;
  // Typed unregistered layer fields (the crate reader's store — parallel to
  // the parser's raw-text unknownMeta). Omitting them silently dropped
  // layer-level unknown metadata from USDC inputs on the Stage write path
  // (AOUSD-META-001 lossless preservation), while USDA inputs survived via
  // unknownMeta.
  meta.unknownFields = root_layer->meta().unknownFields;
  meta.rootPrimOrder = root_layer->meta().rootPrimOrder;
  meta.rootPrimOrder_set = root_layer->meta().rootPrimOrder_set;

#if defined(LIGHTUSD_ENABLE_THREAD)
  const int nthreads = ResolveWriteThreads(options.num_threads);
  if (nthreads > 1) {
    WriteStageBodyParallel(os, *root_layer, meta, options, nthreads);
  } else {
    WriteStageBodySerial(os, *root_layer, meta, options);
  }
#else
  WriteStageBodySerial(os, *root_layer, meta, options);
#endif

  os.flush();
  if (os.good()) {
    result.success = true;
    result.bytes_written = static_cast<size_t>(os.bytes_written());
  } else {
    result.error = "Stream write error";
  }
  return result;
}

USDAWriteResult WriteUSDA(std::ostream& os, const Stage& stage,
                           const USDAWriteOptions& options) {
  StreamWriter w(OstreamSink(os));
  return WriteUSDA(w, stage, options);
}

std::string WriteUSDAToString(const Stage& stage, const USDAWriteOptions& options) {
  std::string out;
  StreamWriter w(&out);
  WriteUSDA(w, stage, options);
  return out;
}

USDAWriteResult WriteUSDAToFile(const std::string& filename, const Stage& stage,
                                 const USDAWriteOptions& options) {
  return WriteUSDAToFile(filename.c_str(), stage, options);
}

USDAWriteResult WriteUSDAToFile(const char* filename, const Stage& stage,
                                 const USDAWriteOptions& options) {
  if (!::lightusd::NativeIOEnabled()) { USDAWriteResult r; r.error = ::lightusd::NativeIOUnavailableMessage(); return r; }

  USDAWriteResult result;

  if (!filename) {
    result.error = "Null filename";
    return result;
  }

  // Native default backend: C stdio with a large (StreamWriter) buffer + blocked
  // writes, so library file output reaches disk at device speed.
  std::FILE* fp = std::fopen(filename, "wb");
  if (!fp) {
    result.error = "Failed to open file for writing: ";
    result.error += filename;
    return result;
  }
  {
    StreamWriter w(StdioSink(fp));
    result = WriteUSDA(w, stage, options);  // flushes the buffer before returning
  }
  if (std::fclose(fp) != 0 && result.success) {
    result.success = false;
    result.error = "Error writing to file";
  }
  return result;
}

std::string WriteLayerToString(const Layer& layer, const USDAWriteOptions& options) {
  std::string out;
  StreamWriter w(&out);
  WriteLayer(w, layer, options);
  return out;
}

USDAWriteResult WriteLayer(StreamWriter& os, const Layer& layer,
                            const USDAWriteOptions& options) {
  USDAWriteResult result;

  // Serialize the layer (header meta + root prims) using the same dispatch as
  // WriteUSDA(Stage): parallel subtree serialization when threads are enabled
  // and >1 worker is requested, else the serial streaming path. The Layer's own
  // metadata is used directly (no Stage->LayerMeta synthesis). Output is
  // byte-identical regardless of thread count.
#if defined(LIGHTUSD_ENABLE_THREAD)
  const int nthreads = ResolveWriteThreads(options.num_threads);
  if (nthreads > 1) {
    WriteStageBodyParallel(os, layer, layer.meta(), options, nthreads);
  } else {
    WriteStageBodySerial(os, layer, layer.meta(), options);
  }
#else
  WriteStageBodySerial(os, layer, layer.meta(), options);
#endif

  os.flush();
  if (os.good()) {
    result.success = true;
    result.bytes_written = static_cast<size_t>(os.bytes_written());
  } else {
    result.error = "Stream write error";
  }
  return result;
}

USDAWriteResult WriteLayer(std::ostream& os, const Layer& layer,
                           const USDAWriteOptions& options) {
  StreamWriter w(OstreamSink(os));
  return WriteLayer(w, layer, options);
}

USDAWriteResult WriteLayerToFile(const std::string& filename, const Layer& layer,
                                  const USDAWriteOptions& options) {
  if (!::lightusd::NativeIOEnabled()) { USDAWriteResult r; r.error = ::lightusd::NativeIOUnavailableMessage(); return r; }

  USDAWriteResult result;

  std::FILE* fp = std::fopen(filename.c_str(), "wb");
  if (!fp) {
    result.error = "Failed to open file for writing: " + filename;
    return result;
  }
  {
    StreamWriter w(StdioSink(fp));
    result = WriteLayer(w, layer, options);
  }
  if (std::fclose(fp) != 0 && result.success) {
    result.success = false;
    result.error = "Error writing to file";
  }
  return result;
}

}  // namespace lightusd
