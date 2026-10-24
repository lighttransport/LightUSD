// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Crate Reader stage reconstruction

#include "crate-reader-internal.hh"
#include "crate-timing.hh"
#include "../layer/layer.hh"
#include "../execution.hh"

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lightusd {
namespace next {

using crate_stage_detail::kSkipEntry;
#if defined(LIGHTUSD_ENABLE_THREAD)
// Below these sizes the stage build stays serial: task setup and thread
// wake-up would cost more than the decode saves. Only referenced from the
// threaded paths, so keep them out of non-threaded builds where they would
// otherwise trip -Wunused-const-variable.
constexpr size_t kParallelBuildMinSpecs = 4096;
constexpr size_t kParallelBuildMinPrims = 512;
#endif

void CrateReader::Impl::ReorderPrimsByPath(std::vector<PrimEntry>& prim_entries,
                      [[maybe_unused]] int build_threads) {
    std::vector<uint32_t> perm(prim_entries.size());
    for (size_t i = 0; i < perm.size(); ++i) perm[i] = static_cast<uint32_t>(i);
    auto entry_less = [&prim_entries](uint32_t a, uint32_t b) {
      const int c = prim_entries[a].full_path.compare(prim_entries[b].full_path);
      return c != 0 ? c < 0 : a < b;
    };
    bool sorted = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (build_threads > 1 && perm.size() >= 65536) {
      size_t runs = 1;
      while (runs * 2 <= static_cast<size_t>(build_threads)) runs *= 2;
      std::vector<size_t> bound(runs + 1);
      for (size_t r = 0; r <= runs; ++r) bound[r] = perm.size() * r / runs;
      TaskArena& arena = BuildArena();
      arena.Run(runs, [&](size_t r) {
        std::sort(perm.begin() + bound[r], perm.begin() + bound[r + 1],
                  entry_less);
      });
      for (size_t width = 1; width < runs; width *= 2) {
        const size_t merges = runs / (width * 2);
        arena.Run(merges, [&](size_t m) {
          const size_t lo = bound[m * width * 2];
          const size_t mid = bound[m * width * 2 + width];
          const size_t hi = bound[m * width * 2 + width * 2];
          std::inplace_merge(perm.begin() + lo, perm.begin() + mid,
                             perm.begin() + hi, entry_less);
        });
      }
      sorted = true;
    }
#endif
    if (!sorted) std::sort(perm.begin(), perm.end(), entry_less);
    std::vector<PrimEntry> ordered;
    ordered.reserve(prim_entries.size());
    for (uint32_t i : perm) ordered.push_back(std::move(prim_entries[i]));
    prim_entries.swap(ordered);
}

static void BuildPrimHierarchy(const std::vector<PrimEntry>& prim_entries,
                        LayerBuilder& builder, Layer& layer,
                        std::vector<uint32_t>& entry_prim,
                        bool& irregular_entries) {
    size_t open_depth = 0;
    for (size_t entry_index = 0; entry_index < prim_entries.size();
         ++entry_index) {
      const auto& entry = prim_entries[entry_index];
      // Compute depth of this prim (number of '/' in path)
      const size_t depth =
          std::count(entry.full_path.begin(), entry.full_path.end(), '/');
      if (depth == 0) continue;  // warned about in the body pass (in order)

      // Pop stack until we're at the correct parent level
      while (open_depth > depth - 1) {
        builder.end_prim();
        --open_depth;
      }

      // Begin this prim
      const size_t prims_before = layer.prim_count();
      entry_prim[entry_index] =
          builder.begin_prim(entry.name, entry.type_name, entry.specifier);
      // An entry that re-opened an earlier prim (duplicate path), or whose
      // depth-stack path differs from its crate path (a sibling name sorting
      // between a prim and its children), makes the entry -> prim mapping
      // irregular: bodies and reorders then run serially, in entry order,
      // exactly as the interleaved build did.
      if (layer.prim_count() == prims_before ||
          layer.prim(entry_prim[entry_index])->path().str() !=
              entry.full_path) {
        irregular_entries = true;
      }
      ++open_depth;
    }
    // Close remaining prims
    while (open_depth > 0) {
      builder.end_prim();
      --open_depth;
    }
}

void CrateReader::Impl::ApplyChildrenOrder(
    Layer& layer, const std::vector<PrimEntry>& prim_entries,
    std::vector<uint32_t>& entry_prim,
    std::vector<std::vector<std::string>>& children_order,
    const std::unordered_map<std::string, std::vector<std::string>>&
        prim_children_order,
    bool irregular_entries,
    [[maybe_unused]] int build_threads) {
  auto reorder = [&](const std::vector<uint32_t>& current,
                     const std::vector<std::string>& names) {
    std::vector<uint32_t> out;
    out.reserve(current.size());
    std::vector<bool> used(current.size(), false);
    // Map each child name to a FIFO of its positions in `current` (preserving
    // original order for duplicate names). Reordering is then O(N+M) instead
    // of O(N*M): a malformed file with N root prims and an N-name primChildren
    // list previously cost O(N^2) string compares (a ~O(N) file could hang).
    std::unordered_map<std::string, std::deque<size_t>> by_name;
    for (size_t i = 0; i < current.size(); ++i) {
      const PrimSpec* c = layer.prim(current[i]);
      if (c) by_name[c->name()].push_back(i);
    }
    for (const std::string& nm : names) {
      auto it = by_name.find(nm);
      if (it == by_name.end() || it->second.empty()) continue;
      size_t idx = it->second.front();
      it->second.pop_front();
      used[idx] = true;
      out.push_back(current[idx]);
    }
    for (size_t i = 0; i < current.size(); ++i) {
      if (!used[i]) out.push_back(current[i]);
    }
    return out;
  };
  // The common case -- children already in the authored order (the sorted
  // hierarchy build matches it) -- needs no reorder at all.
  auto already_ordered = [&](const std::vector<uint32_t>& current,
                             const std::vector<std::string>& names) {
    if (current.size() != names.size()) return false;
    for (size_t i = 0; i < current.size(); ++i) {
      const PrimSpec* c = layer.prim(current[i]);
      if (!c || c->name() != names[i]) return false;
    }
    return true;
  };
  auto apply_children_order = [&](PrimSpec* p,
                                  const std::vector<std::string>& names) {
    if (!p || already_ordered(p->child_indices(), names)) return;
    std::vector<uint32_t> co = reorder(p->child_indices(), names);
    p->set_child_indices(std::move(co));
  };
  for (auto& kv : prim_children_order) {
    if (kv.first == "/") {
      if (!already_ordered(layer.root_indices(), kv.second)) {
        std::vector<uint32_t> ro = reorder(layer.root_indices(), kv.second);
        layer.set_root_indices(std::move(ro));
      }
      continue;
    }
    apply_children_order(layer.prim_at_path_mutable(kv.first), kv.second);
  }
  if (!irregular_entries) {
    // Every entry owns a distinct prim, so the per-prim reorders are
    // independent (each writes only its own child list and reads immutable
    // names): run them as tasks when there are many.
    auto reorder_range = [&](size_t begin, size_t end) {
      for (size_t i = begin; i < end; ++i) {
        if (children_order[i].empty() || entry_prim[i] == kSkipEntry) continue;
        apply_children_order(layer.prim(entry_prim[i]), children_order[i]);
      }
    };
    bool reordered = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (build_threads > 1 && prim_entries.size() >= kParallelBuildMinPrims) {
      const size_t task_size = std::max<size_t>(
          1024, prim_entries.size() / (static_cast<size_t>(build_threads) * 4));
      const size_t ntasks = (prim_entries.size() + task_size - 1) / task_size;
      TaskArena& arena = BuildArena();
      arena.Run(ntasks, [&](size_t t) {
        reorder_range(t * task_size,
                      std::min(prim_entries.size(), (t + 1) * task_size));
      });
      reordered = true;
    }
#endif
    if (!reordered) reorder_range(0, prim_entries.size());
  }
}


bool CrateReader::Impl::BuildStage() {
  CratePhaseTimer timer(options_.enable_timing, "next_crate_stage");
  auto lap = [&](const char* phase) { timer.lap(phase); };
  // Create layer and builder
  Layer layer;
  LayerBuilder builder(layer);
  // Large production scenes can contain hundreds of thousands of specs. In
  // WASM every progress report crosses into JavaScript, where the worker also
  // checks its UI throttle. A coarser interval remains comfortably responsive
  // while avoiding thousands of otherwise-no-op JS/WASM transitions.
  constexpr size_t kReportInterval = 8192;
  auto report_stage = [&](const char* phase, size_t current,
                          size_t total) -> bool {
    if (current == 0 || current == total ||
        (current % kReportInterval) == 0) {
      return ReportProgress(phase, current, total);
    }
    return true;
  };

  // First, process the PseudoRoot spec to extract layer metadata
  // Authored child order per prim path (from primChildren fields; "/" = the
  // pseudo-root's list), applied after the sorted hierarchy build.
  std::unordered_map<std::string, std::vector<std::string>> prim_children_order;

  DecodeStageMetadata(layer, prim_children_order);

  // Process prim specs to build prims with hierarchy from paths

  // Variant content lives in the layer as bracketed holder/child prims
  // ("/Prim/{vset=sel}" + descendants); their attributes attach to those prims
  // via attr_map (so the layer fully represents the variants, enabling both the
  // compositor — which reads the selected holder — and unflattened round-trip).
  // We only need to know, per owning prim, which sets are selected.
  //   owning prim path -> { variantSet -> selected variant } (all selections).
  std::unordered_map<std::string, std::map<std::string, std::string>> variant_sel;
  // Variant OPTION names per owning prim/set, recovered from the holder specs
  // ("/Prim/{set=var}"): lets the model expose the full option list (not just
  // the selection) after a crate load.
  std::unordered_map<std::string, std::map<std::string, std::vector<std::string>>>
      variant_opts;

  lap("pseudoroot");
  // Prim / variant-holder specs of [spec_begin, spec_end), in spec order.
  // Variant option and selection records are applied to variant_opts /
  // variant_sel afterwards, in the same order, so the maps end up exactly as
  // an in-loop update would leave them. Shared by the serial and the parallel
  // (per spec-range task) collect; `report` only on the serial path.
  auto collect_prims =
      [&](size_t spec_begin, size_t spec_end, PrimRecords& out,
          std::vector<std::pair<std::string_view, ValueRep>>& raw_field_scratch,
          std::vector<std::pair<std::string_view, Value>>& value_field_scratch,
          bool report) -> bool {
    for (size_t spec_index = spec_begin; spec_index < spec_end; ++spec_index) {
      if (report && spec_index > 0 &&
          !report_stage("stage.prims", spec_index, specs_.size())) {
        return false;
      }
      const auto& spec = specs_[spec_index];
      // Build Prim specs AND variant-namespace holders (Variant/VariantSet specs
      // at "/Prim/{vset=sel}" / "/Prim/{vset=}"). The holders are needed so that
      // variant CHILD prims ("/Prim/{vset=sel}/Geo") get a correct path from the
      // depth-stack builder (begin_prim recomputes path from its parent). Holders
      // and variant sub-prims are grafted onto the owning prim by the compositor
      // on selection and skipped by the writer (their paths contain '{').
      const bool is_prim = spec.spec_type == SpecType::Prim;
      const bool is_variant = spec.spec_type == SpecType::Variant ||
                              spec.spec_type == SpecType::VariantSet;
      if (!is_prim && !is_variant) continue;

      if (spec.path_index.value >= paths_.size()) continue;
      const std::string_view full_path = paths_.view(spec.path_index.value);
      if (full_path.empty() || full_path == "/") continue;
      const bool bracketed = full_path.find('{') != std::string::npos;
      // Only real (non-bracketed) Prim specs carry a variantSelection.
      if (is_variant && !bracketed) continue;  // defensive

      // Record option names from holder specs "<owner>/{set=var}". The owner
      // is the path before the LAST bracketed component and may itself be a
      // holder ("/P/{a=x}/{b=y}" — a nested variant set on option {a=x}).
      if (bracketed && full_path.back() == '}') {
        size_t lb = full_path.rfind("/{");
        if (lb != std::string::npos) {
          const std::string nm(full_path.substr(lb + 2,
                                                full_path.size() - lb - 3));
          size_t eq = nm.find('=');
          if (eq != std::string::npos && eq + 1 < nm.size() &&
              nm.find('{') == std::string::npos) {
            out.opts.push_back(VariantOptRecord{std::string(full_path.substr(0, lb)),
                                                nm.substr(0, eq),
                                                nm.substr(eq + 1)});
          }
        }
      }

      // Decode the prim's variantSelection (a VariantSelectionMap that
      // UnpackValue/ResolveFieldset cannot represent). Captures every selection
      // so prims with multiple variant sets compose correctly. Holder specs
      // carry a variantSelection too when they own NESTED variant sets.
      if (ResolveFieldsetRaw(spec.fieldset_index.value, raw_field_scratch)) {
        for (auto& f : raw_field_scratch) {
          if (f.first == "variantSelection") {
            std::vector<std::pair<std::string, std::string>> sels;
            if (DecodeVariantSelectionMap(f.second, sels)) {
              // Create the entry even for an authored-EMPTY selection map so
              // the authored bit survives the round trip.
              out.sels.push_back(VariantSelRecord{std::string(full_path),
                                                   std::move(sels)});
            }
            break;
          }
        }
      }

      size_t last_slash = full_path.rfind('/');
      std::string prim_name((last_slash != std::string::npos && last_slash < full_path.size() - 1)
                              ? full_path.substr(last_slash + 1) : full_path);
      if (prim_name.empty()) continue;

      PrimEntry entry;
      entry.full_path = full_path;
      entry.name = std::move(prim_name);
      entry.fieldset_index = spec.fieldset_index.value;
      entry.path_index = spec.path_index.value;

      ResolveFieldset(spec.fieldset_index.value, value_field_scratch);

      // Untyped prims stay untyped: composition fills the type from the
      // referenced prim (a forced "Xform" default would mask e.g. a referenced
      // Mesh definition; the writer already skips empty type names).
      entry.type_name.clear();
      // A crate spec with NO `specifier` field composes as `over` in pxr —
      // for variant holder specs (which never carry one) AND for prim specs
      // (prim-with-no-specifier-001; pxr writes the field for every def, so
      // real crates are unaffected). A Def default here wrongly promoted
      // specifier-less specs to def during composition.
      entry.specifier = PrimSpecifier::Over;
      for (auto& f : value_field_scratch) {
        if (f.first == "typeName") {
          if (auto s = f.second.as_token()) entry.type_name = *s;
          // Legacy "no prim type" spelling (pxr composes it as untyped).
          if (entry.type_name == "__AnyType__") entry.type_name.clear();
        } else if (f.first == "specifier") {
          if (auto s = f.second.as_token()) {
            if (*s == "def") entry.specifier = PrimSpecifier::Def;
            else if (*s == "over") entry.specifier = PrimSpecifier::Over;
            else if (*s == "class") entry.specifier = PrimSpecifier::Class;
          }
        }
      }
      out.entries.push_back(std::move(entry));
    }
    return true;
  };

  std::vector<PrimEntry> prim_entries;
#if defined(LIGHTUSD_ENABLE_THREAD)
  const int build_threads = ResolveBuildThreads();
#endif
  if (!report_stage("stage.prims", 0, specs_.size())) return false;
  {
    std::vector<PrimRecords> prim_records;
    bool prims_collected = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (build_threads > 1 && specs_.size() >= kParallelBuildMinSpecs) {
      const size_t task_size = std::max<size_t>(
          1024, specs_.size() / (static_cast<size_t>(build_threads) * 8));
      const size_t ntasks = (specs_.size() + task_size - 1) / task_size;
      prim_records.resize(ntasks);
      std::vector<std::unique_ptr<ThreadDecodeCtx>> ctxs(ntasks);
      TaskArena& arena = BuildArena();
      arena.Run(ntasks, [&](size_t t) {
        ctxs[t].reset(new ThreadDecodeCtx(this, *reader_));
        ScopedThreadDecodeCtx scoped(ctxs[t].get());
        std::vector<std::pair<std::string_view, ValueRep>> raw_scratch;
        std::vector<std::pair<std::string_view, Value>> value_scratch;
        collect_prims(t * task_size,
                      std::min(specs_.size(), (t + 1) * task_size),
                      prim_records[t], raw_scratch, value_scratch,
                      /*report=*/false);
      });
      for (auto& ctx : ctxs) MergeDecodeCtx(*ctx);
      prims_collected = true;
    }
#endif
    if (!prims_collected) {
      prim_records.resize(1);
      std::vector<std::pair<std::string_view, ValueRep>> raw_field_scratch;
      std::vector<std::pair<std::string_view, Value>> value_field_scratch;
      if (!collect_prims(0, specs_.size(), prim_records[0], raw_field_scratch,
                         value_field_scratch, /*report=*/true)) {
        return false;
      }
    }
    size_t total_entries = 0;
    for (const PrimRecords& r : prim_records) total_entries += r.entries.size();
    prim_entries.reserve(total_entries);
    for (PrimRecords& r : prim_records) {
      for (PrimEntry& e : r.entries) prim_entries.push_back(std::move(e));
      for (VariantOptRecord& o : r.opts) {
        variant_opts[o.owner][o.set].push_back(std::move(o.option));
      }
      for (VariantSelRecord& v : r.sels) {
        // Create the entry even for an authored-EMPTY selection map so the
        // authored bit survives the round trip.
        auto& per_prim = variant_sel[v.prim_path];
        for (auto& kv : v.sels) per_prim[kv.first] = kv.second;
      }
    }
  }
  if (!report_stage("stage.prims", specs_.size(), specs_.size())) {
    return false;
  }

  // Collect separate Attribute and Relationship specs. pxrUSD-authored crates
  // store each property as its own spec at "<primpath>/<name>" (rendered with a
  // leading '.' marker by ReadPaths), rather than inline in the prim's
  // fieldset. We capture, for each property: its declared typeName, its
  // `default` value (lazy if an array — no payload decode), any connection
  // targets, relationship targets, and uniform variability. These are attached
  // to the parent prim below so the writer can faithfully re-emit them.
  //
  // Rarely-authored parts live behind lazily allocated pointers: a property
  // record is otherwise ~1.4 KB (PropMeta alone is ~0.9 KB), and a large scene
  // holds millions of them between the collect and the body pass.
  lap("prims");
  // Sort by full path (produces correct depth-first order with parents before
  // children). Done before the property collect so property specs can be
  // resolved to their owning entry while they are decoded. The order is total
  // (duplicate paths keep their spec order), so the parallel sort -- sorted
  // chunks, then pairwise merge rounds -- yields exactly the serial order.
#if defined(LIGHTUSD_ENABLE_THREAD)
  ReorderPrimsByPath(prim_entries, build_threads);
#else
  ReorderPrimsByPath(prim_entries, 1);
#endif
  lap("sort");

  // Owning prim path -> entry index (the first entry on a duplicate path).
  // Property specs attach to the entry whose path equals their prim part;
  // specs of prims with no entry are dropped, as before. Keys view the
  // immutable PATHS pool, which remains alive throughout reconstruction.
  std::unordered_map<std::string_view, uint32_t> entry_by_path;
  entry_by_path.reserve(prim_entries.size());
  for (size_t i = 0; i < prim_entries.size(); ++i) {
    entry_by_path.emplace(prim_entries[i].full_path, static_cast<uint32_t>(i));
  }
  std::vector<std::pair<std::string_view, ValueRep>> property_raw_field_scratch;

  // Split a property spec path ".<primpath>/<name>" into (primpath, name).


  // Collect the Attribute / Relationship specs of [spec_begin, spec_end) into
  // (prim path, info) records in spec order. Shared verbatim by the serial
  // and the parallel (per spec-range task) collect; `report` is set only on
  // the serial path (progress callbacks must stay on the calling thread).
  // Records are (owning entry index, info), in spec order.
  // Fast path: the property's parent slot in the PATHS tree names its prim
  // entry directly; it is taken only when that entry's path equals the split
  // prim path (so the answer is identical to the hash lookup it skips).
  std::vector<uint32_t> slot_to_entry(paths_.size(), UINT32_MAX);
  for (const PrimEntry& e : prim_entries) {
    if (e.path_index < slot_to_entry.size()) {
      slot_to_entry[e.path_index] = entry_by_path.find(e.full_path)->second;
    }
  }


  if (!report_stage("stage.properties", 0, specs_.size())) return false;
  bool properties_collected = false;
  std::vector<AttrRecords*> attr_lists;
  std::vector<RelRecords*> rel_lists;
  std::unique_ptr<AttrRecords> serial_attrs;
  std::unique_ptr<RelRecords> serial_rels;
#if defined(LIGHTUSD_ENABLE_THREAD)
  struct CollectTask {
    size_t begin = 0;
    size_t end = 0;
    AttrRecords attrs;
    RelRecords rels;
    std::unique_ptr<ThreadDecodeCtx> ctx;
  };
  std::vector<CollectTask> tasks;
#endif
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (build_threads > 1 && specs_.size() >= kParallelBuildMinSpecs) {
    // Parallel collect: contiguous spec ranges decode into task-local record
    // vectors (each task with its own stream cursor + diagnostics), merged in
    // range order below -- exactly the serial append order.
    const size_t task_size = std::max<size_t>(
        1024, specs_.size() / (static_cast<size_t>(build_threads) * 8));
    tasks.resize((specs_.size() + task_size - 1) / task_size);
    for (size_t t = 0; t < tasks.size(); ++t) {
      tasks[t].begin = t * task_size;
      tasks[t].end = std::min(specs_.size(), tasks[t].begin + task_size);
    }
    TaskArena& arena = BuildArena();
    arena.Run(tasks.size(), [&](size_t t) {
      CollectTask& task = tasks[t];
      task.ctx.reset(new ThreadDecodeCtx(this, *reader_));
      ScopedThreadDecodeCtx scoped(task.ctx.get());
      std::vector<std::pair<std::string_view, ValueRep>> scratch;
      CollectPropertyRecords(prim_entries, entry_by_path, slot_to_entry,
                           report_stage, task.begin, task.end, task.attrs, task.rels, scratch,
                         /*report=*/false);
    });
    for (CollectTask& task : tasks) {
      MergeDecodeCtx(*task.ctx);
      attr_lists.push_back(&task.attrs);
      rel_lists.push_back(&task.rels);
    }
    properties_collected = true;
  }
#endif
  if (!properties_collected) {
    serial_attrs.reset(new AttrRecords());
    serial_rels.reset(new RelRecords());
    if (!CollectPropertyRecords(prim_entries, entry_by_path, slot_to_entry,
                           report_stage, 0, specs_.size(), *serial_attrs, *serial_rels,
                            property_raw_field_scratch, /*report=*/true)) {
      return false;
    }
    attr_lists.push_back(serial_attrs.get());
    rel_lists.push_back(serial_rels.get());
  }

  lap("properties");
  // Per-entry record lists (CSR over the record vectors, which stay alive
  // until the bodies have moved from them): entry i owns
  // refs[off[i] .. off[i+1]), in spec order.
  auto build_entry_index = [&](auto& lists, std::vector<uint32_t>& off,
                               auto& refs) {
    off.assign(prim_entries.size() + 1, 0);
    size_t total = 0;
    for (auto* list : lists) {
      for (auto& rec : *list) {
        ++off[rec.first + 1];
        ++total;
      }
    }
    for (size_t i = 0; i < prim_entries.size(); ++i) off[i + 1] += off[i];
    refs.resize(total);
    std::vector<uint32_t> cursor(off.begin(), off.end() - 1);
    for (auto* list : lists) {
      for (auto& rec : *list) refs[cursor[rec.first]++] = &rec.second;
    }
  };
  std::vector<uint32_t> attr_off;
  std::vector<uint32_t> rel_off;
  std::vector<AttrInfo*> attr_refs;
  std::vector<RelInfo*> rel_refs;
  build_entry_index(attr_lists, attr_off, attr_refs);
  build_entry_index(rel_lists, rel_off, rel_refs);
  // Every property name was interned during the collect above; publish
  // the name table's lock-free snapshot so the body pass never takes its lock.
  GetPropNameTable().publish_snapshot();
  if (!report_stage("stage.properties", specs_.size(), specs_.size())) {
    return false;
  }

  lap("property_index");
  // Emit, in two passes over the path-sorted entries:
  //  1. structure (serial): the depth-stack begin_prim/end_prim walk creates
  //     every PrimSpec (name/type/specifier/path + hierarchy links) exactly as
  //     the interleaved build did, recording each entry's prim index;
  //  2. bodies: decode each entry's fields/metadata and attach its property
  //     specs into its own PrimSpec. Bodies only touch their own PrimSpec (and
  //     read-only shared tables), so when no entry re-opened an earlier prim
  //     (duplicate path) they run as parallel tasks over contiguous entry
  //     ranges; diagnostics merge back in entry order.
  std::vector<uint32_t> entry_prim(prim_entries.size(), kSkipEntry);
  bool irregular_entries = false;
  if (!report_stage("stage.hierarchy", 0, prim_entries.size())) return false;
  layer.reserve(prim_entries.size());
  BuildPrimHierarchy(prim_entries, builder, layer, entry_prim, irregular_entries);

  lap("structure");
  // Authored primChildren order per entry (merged into prim_children_order
  // after the body pass, in entry order).
  std::vector<std::vector<std::string>> children_order(prim_entries.size());

  PrimBuildState state{layer, builder, prim_entries, entry_by_path,
                       entry_prim, children_order, prim_children_order,
                       irregular_entries, attr_refs, rel_refs, attr_off,
                       rel_off, variant_sel, variant_opts};

  bool bodies_emitted = false;
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (build_threads > 1 && !irregular_entries &&
      prim_entries.size() >= kParallelBuildMinPrims) {
    struct EmitTask {
      size_t begin = 0;
      size_t end = 0;
      std::unique_ptr<ThreadDecodeCtx> ctx;
    };
    const size_t task_size = std::max<size_t>(
        128, prim_entries.size() / (static_cast<size_t>(build_threads) * 16));
    std::vector<EmitTask> tasks((prim_entries.size() + task_size - 1) /
                                task_size);
    for (size_t t = 0; t < tasks.size(); ++t) {
      tasks[t].begin = t * task_size;
      tasks[t].end = std::min(prim_entries.size(), tasks[t].begin + task_size);
    }
    TaskArena& arena = BuildArena();
    arena.Run(tasks.size(), [&](size_t t) {
      EmitTask& task = tasks[t];
      task.ctx.reset(new ThreadDecodeCtx(this, *reader_));
      ScopedThreadDecodeCtx scoped(task.ctx.get());
      std::vector<std::pair<std::string_view, Value>> scratch;
      for (size_t i = task.begin; i < task.end; ++i) EmitPrimBody(state, i, scratch, report_stage);
    });
    for (EmitTask& task : tasks) MergeDecodeCtx(*task.ctx);
    bodies_emitted = true;
  }
#endif
  if (!bodies_emitted) {
    std::vector<std::pair<std::string_view, Value>> prim_value_field_scratch;
    for (size_t entry_index = 0; entry_index < prim_entries.size();
         ++entry_index) {
      if (entry_index > 0 &&
          !report_stage("stage.hierarchy", entry_index, prim_entries.size())) {
        return false;
      }
      EmitPrimBody(state, entry_index, prim_value_field_scratch, report_stage);
    }
  }
  if (irregular_entries) {
    // Several entries share a prim: keep the last authored order per path.
    for (size_t entry_index = 0; entry_index < prim_entries.size();
         ++entry_index) {
      if (!children_order[entry_index].empty()) {
        prim_children_order[std::string(prim_entries[entry_index].full_path)] =
            std::move(children_order[entry_index]);
      }
    }
  }
  if (!report_stage("stage.hierarchy", prim_entries.size(),
                    prim_entries.size())) {
    return false;
  }

  lap("bodies");
  // Finalize
  builder.finalize();
  lap("finalize");

  // Restore authored namespace order from primChildren: reorder each prim's
  // child links (and the root list) to the recorded name order; unlisted
  // children keep their relative (sorted) order at the end.
  // Restore authored namespace order from primChildren: reorder each prim's
  // child links (and the root list) to the recorded name order; unlisted
  // children keep their relative (sorted) order at the end. (The path index
  // used below was built by finalize().)
#if defined(LIGHTUSD_ENABLE_THREAD)
  ApplyChildrenOrder(layer, prim_entries, entry_prim, children_order, prim_children_order, irregular_entries, build_threads);
#else
  ApplyChildrenOrder(layer, prim_entries, entry_prim, children_order, prim_children_order, irregular_entries, 1);
#endif

  lap("reorder");
  // Create stage from layer
  result_.stage.SetRootLayer(std::move(layer));
  lap("set_root");
  return true;
}


}  // namespace next
}  // namespace lightusd
