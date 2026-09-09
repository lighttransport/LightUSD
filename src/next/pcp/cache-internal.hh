// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP Cache implementation
// Phase 1: sublayers + references.  Phase 2: ancestral opinions + deferred payloads.

#pragma once
#include "cache.hh"
#include "../execution.hh"

#include "cache-lock.hh"
#include "cache-utils.hh"
#include "../composition/composition.hh"  // reuse ParseReference / ParsePayload / CopyLocalOpinions
#include "../strfmt.hh"                    // IntToStr / UIntToStr
#include "../../logger.hh"                 // lightusd::logging LIGHTUSD_LOG_*

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <atomic>
#include <thread>
#endif

namespace lightusd {
namespace next {
namespace pcp {

namespace cache_detail {

// The layer stacks an arc chain crossed to reach a source, as (stack, map_idx)
// pairs from the root stack down to the source's own stack -- one entry per
// composition node. Implied class arcs are re-expressed in each of these
// namespaces, so the chain must survive the re-rooting a child prim does; an
// INTERMEDIATE stack's opinion on the class path is otherwise lost (bug69932).
// Shared, so propagating it to a whole subtree costs one refcount.
using ArcChain = std::vector<std::pair<uint32_t, uint32_t>>;

// One resolved spec: a PrimSpec + its owning layer (identity). File scope (was
// Cache::Impl::SpecRef) so Src can cache a pointer to a Specs() result.
struct SpecRef {
  const PrimSpec *spec = nullptr;
  const Layer *layer = nullptr;
  std::string layer_id;
  // Time offset of the owning layer within its stack (sublayer offsets),
  // relative to the stack root. Identity for the root layer.
  LayerOffset layer_offset;
};

// Per-layer-stack memo of FindSpecs(site) results (the compose hot path:
// Specs() is the single largest self-cost during BuildStage). Replaces a
// std::unordered_map keyed by a (stack, site) struct: an open-addressed
// hash->value-index table (cache-friendly, no per-entry node alloc) over
// STABLE value storage (a deque, so a returned `const vector<SpecRef>&`
// stays valid across later inserts — required, callers and Src::specs_ hold
// it). A fast 8-byte-chunked mix hash replaces std::hash's per-byte path.
struct StackSpecCache {
  static uint64_t HashSite(const char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
      uint64_t k;
      std::memcpy(&k, p + i, 8);
      h = (h ^ k) * 0x100000001b3ull;
      h = (h << 27) | (h >> 37);
    }
    for (; i < n; ++i) h = (h ^ static_cast<uint8_t>(p[i])) * 0x100000001b3ull;
    h ^= h >> 33;
    return h ? h : 1;  // reserve 0 for empty slot
  }

  struct Slot {
    uint64_t hash = 0;  // 0 = empty
    uint32_t val_idx = 0;
  };
  std::vector<Slot> slots_;
  std::deque<std::string> keys_;              // keys_[i] backs vals_[i]
  std::deque<std::vector<SpecRef>> vals_;     // stable value storage
  size_t count_ = 0;

  std::vector<SpecRef> *find(const std::string &site, uint64_t h) {
    if (slots_.empty()) return nullptr;
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    while (slots_[i].hash != 0) {
      if (slots_[i].hash == h) {
        const uint32_t vi = slots_[i].val_idx;
        const std::string &k = keys_[vi];
        if (k.size() == site.size() &&
            std::memcmp(k.data(), site.data(), site.size()) == 0) {
          return &vals_[vi];
        }
      }
      i = (i + 1) & mask;
    }
    return nullptr;
  }

  std::vector<SpecRef> &insert(const std::string &site, uint64_t h,
                               std::vector<SpecRef> &&v) {
    if (slots_.empty() || (count_ + 1) * 2 > slots_.size()) rehash();
    const uint32_t vi = static_cast<uint32_t>(vals_.size());
    keys_.emplace_back(site);
    vals_.emplace_back(std::move(v));
    place(h, vi);
    ++count_;
    return vals_[vi];
  }

  void clear() {
    slots_.clear();
    keys_.clear();
    vals_.clear();
    count_ = 0;
  }

 private:
  void place(uint64_t h, uint32_t vi) {
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    while (slots_[i].hash != 0) i = (i + 1) & mask;
    slots_[i].hash = h;
    slots_[i].val_idx = vi;
  }
  void rehash() {
    const size_t new_cap = slots_.empty() ? 16 : slots_.size() * 2;
    std::vector<Slot> old = std::move(slots_);
    slots_.assign(new_cap, Slot{});
    for (const Slot &s : old) {
      if (s.hash != 0) place(s.hash, s.val_idx);
    }
  }
};

// Memoized Specs() pointer that RESETS on copy/move. A Src copied on
// child-build gets a different site, a Src seeded into (or merged back from) a
// parallel-warm worker crosses Impls, and an Impl's spec cache is dropped on
// InvalidateLayer — in every case a carried-over pointer would be stale or
// semantically wrong, so any copy/move starts unresolved and SpecsFor()
// re-resolves on first use (one extra hash, not a correctness risk).
// CAVEAT: moving a whole std::vector<Src> is a buffer steal — element
// copy/move (and this reset) never runs — so a cross-Impl vector move must
// null specs_ explicitly (see MergeSources).
struct SpecsMemo {
  mutable const std::vector<SpecRef> *p = nullptr;
  SpecsMemo() = default;
  SpecsMemo(const SpecsMemo &) {}
  SpecsMemo &operator=(const SpecsMemo &) {
    p = nullptr;
    return *this;
  }
  SpecsMemo(SpecsMemo &&) {}
  SpecsMemo &operator=(SpecsMemo &&) {
    p = nullptr;
    return *this;
  }
};

// A single composition source (one node's worth of provenance).
struct Src {
  uint32_t stack_idx = 0;
  // Index into the owning Impl's nm_pool_ (0 == identity). The namespace mapping
  // is created once per arc and SHARED by every Src in the subtree below it, so
  // storing an index (and copying it on child-build) avoids re-copying the two
  // path strings of a non-identity NamespaceMapping for all 140K+ descendants --
  // the dominant path-string churn in compose. Fills stack_idx's padding hole.
  uint32_t map_idx = 0;
  std::string site;            // prim path within the layer stack
  LayerOffset offset;
  ArcType arc_kind = ArcType::Root;  // arc this source arrived through
  // True when the arc was introduced at a namespace ANCESTOR (the source was
  // derived by DeriveChildSources rather than by this prim's own arc
  // expansion). pxr PcpNodeRef::IsDueToAncestor analogue; specifier
  // resolution treats a class from a direct inherit as weaker than any other
  // defining specifier, but an ancestral one composes in plain strength order.
  bool ancestral = false;
  // For Variant sources: the selected variant's inline opinions (lives inside a
  // shared layer's PrimSpecMeta, so the pointer is stable). null otherwise.
  const VariantData *variant = nullptr;
  // Composed expression-variable context visible at this source. Shared so
  // propagating a source to thousands of descendants stays cheap.
  std::shared_ptr<const Value> expression_variables;
  // The arc chain this source was reached through (null == the root stack).
  std::shared_ptr<const ArcChain> arc_chain;
  // Parallel to arc_chain, but records each crossed arc's TARGET (stack, site)
  // in the target stack's own namespace. The live ExpansionFrame cycle chain
  // resets at every prim during the BuildStage namespace walk, so an ANCESTRAL
  // reference cycle (a child whose arc re-targets a site crossed to reach an
  // ancestor) is invisible to it. Re-seeding the cycle-detection frame from
  // this persisted trail restores pxr's behavior: the cyclic arc is dropped and
  // the re-entrant prim materializes once as `over` (ErrorArcCycle).
  std::shared_ptr<const std::vector<std::pair<uint32_t, std::string>>> arc_sites;
  // For IMPLIED class sources (and their subtree): the layer stack whose
  // strength position this source composes at. pxr expresses an implied
  // class in each ancestor stack of the introducing arc chain, and its
  // opinions sit immediately after that stack's own positional opinions —
  // NOT at the introducing reference's position (see ExpandList's reorder).
  // UINT32_MAX == not an implied source.
  uint32_t implied_anchor = UINT32_MAX;
  // Relocation-source anchor whose OWN spec opinions are ignored (pxr:
  // "opinions at relocation source" are a composition error). The Src stays
  // in the list because child derivation descends through it (chained
  // relocations address content via this raw site).
  bool suppress_site_specs = false;
  // Memoized Specs(stack_idx, site) result for this exact (stack_idx, site), so
  // the 2-3 read-side lookups per composed prim (ComposeChildNames,
  // IsInstanceable, instance-key, ComposeOpinions) skip re-hashing `site`.
  // Points into the owning Impl's spec_cache_by_stack_ (stable storage). Resets
  // itself on any copy/move of the Src (see SpecsMemo).
  SpecsMemo specs_;
};

// path -> expanded composition sources. The compose structure pass hammers
// this (SourcesForPath: one find + one get-or-create per composed prim, plus
// a parent read). Was std::unordered_map<string, vector<Src>>: node alloc,
// pointer chase, per-byte std::hash. This is an open-addressed hash->index
// table over STABLE value storage (a deque of vector<Src>): the loop in
// SourcesForSite reads a parent slot's ref, then inserts the child slot, and
// returns a slot ref to its caller — all of which must survive later inserts
// (ExpandList re-enters recursively), so values live in a deque (append never
// moves) and rehash rebuilds only the index slots. get_or_create default-
// constructs an absent value exactly like unordered_map::operator[], which
// the in-progress re-entry semantics rely on. Erase is invalidation-only
// (never on the flatten hot path): tombstones keep probe chains valid; dead
// deque entries are reclaimed on clear(). Default copy deep-copies (worker
// seeding; Src's SpecsMemo resets on that element-wise copy).
struct SrcCache {
  static uint64_t Hash(const std::string &s) {
    return StackSpecCache::HashSite(s.data(), s.size());
  }
  struct Slot {
    uint64_t hash = 0;
    uint32_t idx = 0;
    uint8_t state = 0;  // 0 empty, 1 used, 2 tombstone
  };
  std::vector<Slot> slots_;
  std::deque<std::string> keys_;           // keys_[i] backs vals_[i]
  std::deque<std::vector<Src>> vals_;      // STABLE value storage
  std::vector<uint8_t> live_;              // parallel to vals_ (1 = live)
  size_t count_ = 0;                       // live entries
  size_t occupied_ = 0;                    // used + tombstone slots

  size_t size() const { return count_; }

  void clear() {  // full free (drops the path-string keys too)
    std::vector<Slot>().swap(slots_);
    std::deque<std::string>().swap(keys_);
    std::deque<std::vector<Src>>().swap(vals_);
    std::vector<uint8_t>().swap(live_);
    count_ = 0;
    occupied_ = 0;
  }

  std::vector<Src> *find(const std::string &k) {
    if (slots_.empty()) return nullptr;
    const uint64_t h = Hash(k);
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    for (;;) {
      const Slot &s = slots_[i];
      if (s.state == 0) return nullptr;
      if (s.state == 1 && s.hash == h) {
        const std::string &key = keys_[s.idx];
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          return &vals_[s.idx];
        }
      }
      i = (i + 1) & mask;
    }
  }
  const std::vector<Src> *find(const std::string &k) const {
    if (slots_.empty()) return nullptr;
    const uint64_t h = Hash(k);
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    for (;;) {
      const Slot &s = slots_[i];
      if (s.state == 0) return nullptr;
      if (s.state == 1 && s.hash == h) {
        const std::string &key = keys_[s.idx];
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          return &vals_[s.idx];
        }
      }
      i = (i + 1) & mask;
    }
  }
  bool contains(const std::string &k) const { return find(k) != nullptr; }

  // find-or-insert; returns a reference that stays valid across later inserts.
  std::vector<Src> &get_or_create(const std::string &k) {
    const uint64_t h = Hash(k);
    if (slots_.empty() || (occupied_ + 1) * 2 > slots_.size()) rehash();
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    size_t first_tomb = SIZE_MAX;
    for (;;) {
      Slot &s = slots_[i];
      if (s.state == 0) {
        const size_t at = (first_tomb == SIZE_MAX) ? i : first_tomb;
        return emplace_at(at, h, k, /*was_tomb=*/slots_[at].state == 2);
      }
      if (s.state == 2) {
        if (first_tomb == SIZE_MAX) first_tomb = i;
      } else if (s.hash == h) {
        const std::string &key = keys_[s.idx];
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          return vals_[s.idx];
        }
      }
      i = (i + 1) & mask;
    }
  }

  // Insert only if absent (merge path). Consumes v when it inserts.
  void emplace_if_absent(const std::string &k, std::vector<Src> &&v) {
    if (contains(k)) return;
    get_or_create(k) = std::move(v);
  }

  void erase(const std::string &k) {
    if (slots_.empty()) return;
    const uint64_t h = Hash(k);
    const size_t mask = slots_.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    for (;;) {
      Slot &s = slots_[i];
      if (s.state == 0) return;
      if (s.state == 1 && s.hash == h) {
        const std::string &key = keys_[s.idx];
        if (key.size() == k.size() &&
            std::memcmp(key.data(), k.data(), k.size()) == 0) {
          live_[s.idx] = 0;
          std::string().swap(keys_[s.idx]);
          std::vector<Src>().swap(vals_[s.idx]);
          s.state = 2;  // tombstone: keeps the probe chain valid
          --count_;
          return;
        }
      }
      i = (i + 1) & mask;
    }
  }

  template <typename Pred>
  void erase_if(Pred pred) {
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (live_[vi] && pred(keys_[vi])) erase(keys_[vi]);
    }
  }

  template <typename Fn>
  void for_each(Fn fn) {
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (live_[vi]) fn(const_cast<const std::string &>(keys_[vi]), vals_[vi]);
    }
  }

 private:
  std::vector<Src> &emplace_at(size_t slot, uint64_t h, const std::string &k,
                               bool was_tomb) {
    uint32_t vi;
    if (was_tomb) {
      vi = static_cast<uint32_t>(live_.size());
      for (uint32_t di = 0; di < live_.size(); ++di) {
        if (!live_[di]) { vi = di; break; }
      }
      if (vi < live_.size()) {
        keys_[vi] = k;
        vals_[vi].clear();
        live_[vi] = 1;
      } else {
        keys_.emplace_back(k);
        vals_.emplace_back();
        live_.push_back(1);
      }
    } else {
      vi = static_cast<uint32_t>(vals_.size());
      keys_.emplace_back(k);
      vals_.emplace_back();
      live_.push_back(1);
    }
    slots_[slot].hash = h;
    slots_[slot].idx = vi;
    slots_[slot].state = 1;
    ++count_;
    if (!was_tomb) ++occupied_;
    return vals_[vi];
  }
  void rehash() {
    const size_t new_cap = slots_.empty() ? 16 : slots_.size() * 2;
    slots_.assign(new_cap, Slot{});
    occupied_ = 0;
    const size_t mask = new_cap - 1;
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (!live_[vi]) continue;
      const uint64_t h = Hash(keys_[vi]);
      size_t i = static_cast<size_t>(h) & mask;
      while (slots_[i].state != 0) i = (i + 1) & mask;
      slots_[i].hash = h;
      slots_[i].idx = vi;
      slots_[i].state = 1;
      ++occupied_;
    }
  }
};

// Reverse-dependency key: which (layer, prim-path) an index read from.
struct Site {
  std::string layer_id;
  std::string prim_path;
  bool operator==(const Site &o) const {
    return layer_id == o.layer_id && prim_path == o.prim_path;
  }
};
struct SiteHash {
  size_t operator()(const Site &s) const {
    return std::hash<std::string>()(s.layer_id) ^
           (std::hash<std::string>()(s.prim_path) << 1);
  }
};

// One level of arc expansion, linked up the C++ call stack (OpenUSD
// PcpPrimIndex_StackFrame analogue). Replaces the per-arc copied
// std::set<std::string> cycle keys: cycle detection walks the chain comparing
// (stack_idx, site), and depth gives the max-depth backstop for free. The
// `site` pointer targets a string that outlives the recursion (a Src member or
// a ProcessArc local).
struct ExpansionFrame {
  uint32_t stack_idx;
  uint32_t map_idx;
  const std::string *site;
  const ExpansionFrame *prev;
  uint32_t depth;  // number of frames above the seed (seed == 0)

  bool Contains(uint32_t stack, const std::string &s) const {
    for (const ExpansionFrame *f = this; f; f = f->prev) {
      if (f->stack_idx == stack && *f->site == s) return true;
    }
    return false;
  }
};

}  // namespace cache_detail
using namespace cache_detail;

struct Cache::Impl {
  // Compile container construction/destruction once, including worker caches.
  Impl();
  ~Impl();

  AssetResolver *resolver = nullptr;
  CompositionOptions options;
  LayerRegistry registry;          // owned by this Impl
  LayerRegistry *reg_ = &registry;  // borrowed pointer: worker Impls point this
                                    // at the *main* Impl's registry (parse-once
                                    // shared across the parallel build).
#if defined(LIGHTUSD_ENABLE_THREAD)
  mutable PcpMutex api_mu_;  // guards all shared state below (F6: non-recursive)

  // --- parallel-build worker hooks (see PrewarmPrimIndices) ---------------
  // When set, RegisterInstance stashes its (instanceable, key) into
  // pending_instance_ instead of mutating the prototype maps, so the merge can
  // assign prototypes deterministically in input order.
  bool defer_instances_ = false;
  std::unordered_map<std::string, std::pair<bool, std::string>> pending_instance_;
#endif

  std::shared_ptr<Layer> root_layer;  // kept alive (also layer_stacks[0].layers[0])
  std::string root_identifier;

  // Append-only shared tables backing every PrimIndex (Phase 9 F3). std::deque
  // gives stable element addresses: a PrimIndex handed to another thread keeps
  // resolving its nodes' layer-stacks/sites even as a concurrent build appends
  // new entries here (a std::vector would reallocate and dangle them).
  std::deque<LayerStack> layer_stacks;              // table; [0] == root stack
  std::map<std::string, uint32_t> stack_by_id;      // dedup by resolved identifier

  // Memoized EXTERNAL-arc target resolution. A heavily-referenced scene runs
  // ProcessArc for millions of arc INSTANCES that name the same few hundred
  // (authoring layer, asset path) pairs; each un-memoized instance re-paid
  // RealAnchorOf + resolver.Resolve (twice: GetOrLoad + ResolvePath) + the
  // expression-vars fingerprint build + the stack_by_id std::map walk (long
  // near-identical path keys -> memcmp storm). The resolved target depends
  // only on (anchor source, evaluated asset path, referencing expression-vars
  // identity) plus per-Impl constants, so successful resolutions are memoized;
  // failures re-run so per-instance diagnostics are unchanged. Cleared with
  // the spec cache on InvalidateLayer (a dropped layer re-resolves).
  struct ArcTargetEntry {
    std::shared_ptr<Layer> layer;
    std::string arc_id;  // resolved identifier (diagnostics + salted-earth)
    uint32_t stack_idx = UINT32_MAX;
  };
  struct ArcMemoHash {
    size_t operator()(const std::string &s) const {
      return static_cast<size_t>(
          StackSpecCache::HashSite(s.data(), s.size()));
    }
  };
  std::unordered_map<std::string, ArcTargetEntry, ArcMemoHash> arc_target_memo_;

  // Interned prim-path table shared by all PrimIndex nodes (dedup across indices).
  std::deque<std::string> path_table;
  std::unordered_map<std::string, uint32_t> path_intern;
  uint32_t InternPath(const std::string &p) {
    auto it = path_intern.find(p);
    if (it != path_intern.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(path_table.size());
    path_table.push_back(p);
    path_intern.emplace(p, idx);
    return idx;
  }

  std::map<std::string, std::unique_ptr<PrimIndex>> index_cache;
  SrcCache sources_cache;  // path -> expanded sources (open-addressed, stable values)
  // Parallel warm workers borrow the main cache as an immutable seed and keep
  // only newly resolved entries in sources_cache. This avoids deep-copying the
  // full source cache once per worker.
  const SrcCache *source_seed_cache = nullptr;
  const std::vector<Src> *FindCachedSources(const std::string &key) const {
    if (const std::vector<Src> *local = sources_cache.find(key)) {
      return local;
    }
    if (source_seed_cache) {
      return source_seed_cache->find(key);
    }
    return nullptr;
  }
  bool HasCachedSources(const std::string &key) const {
    return FindCachedSources(key) != nullptr;
  }
  std::vector<Src> &GetOrCreateCachedSources(const std::string &key) {
    if (std::vector<Src> *local = sources_cache.find(key)) return *local;
    if (source_seed_cache) {
      if (const std::vector<Src> *seed = source_seed_cache->find(key)) {
        // This path is only used when a caller is about to populate or mutate
        // an entry that was found through the immutable seed.
        return sources_cache.get_or_create(key) = *seed;
      }
    }
    return sources_cache.get_or_create(key);
  }
  std::set<std::string> sources_in_progress;
  // Reentrancy guard for SourcesForRelocatedContent: derives a relocate
  // arrival's CONTENT from the source's COMPOSED parent (SourcesForSite), which
  // can chain back into another arrival re-deriving the same child (shapes like
  // Path->Anim/Path then Anim->AnimScope). Keyed by the arrival dst composed
  // path; on re-entry the helper falls back to the isolated relocate-source
  // walk instead of recursing (see cache-arc-expansion.inc).
  std::set<std::string> reloc_content_in_progress;
  // >0 while inside an isolated SourcesForRelocateSource walk. The composed-
  // parent relocated-content derivation only applies at the true composed
  // (stack-0) level; nested calls from within the isolated walk (chained
  // relocates resolved in a stack's own namespace) must use the isolated path.
  size_t isolated_reloc_depth_ = 0;
  const std::vector<Src> empty_sources_;

  // Pool of namespace mappings shared by Src.map_idx. Index 0 is identity, so a
  // default Src (and the bulk of subtree children) needs no pool entry. New
  // mappings are appended only in the SERIAL arc expansion (ProcessArc / variant
  // ExpandArcs); the parallel opinion fill only READS the pool (Apply), and a
  // std::deque never relocates existing entries -> lock-free concurrent reads.
  std::deque<NamespaceMapping> nm_pool_{NamespaceMapping{}};
  const NamespaceMapping &Mapping(uint32_t idx) const { return nm_pool_[idx]; }
  uint32_t InternMapping(NamespaceMapping m) {
    if (m.is_identity()) return 0;
    uint32_t idx = static_cast<uint32_t>(nm_pool_.size());
    nm_pool_.push_back(std::move(m));
    return idx;
  }

  // Phase 10: lazily-composed per-prim specs + their composed child names,
  // keyed by prim path. Populated by ComposePrim, dropped by Invalidate.
  std::unordered_map<std::string, std::unique_ptr<PrimSpec>> composed_cache_;
  std::unordered_map<std::string, std::vector<std::string>> composed_children_;
  std::unordered_map<Site, std::set<std::string>, SiteHash> site_to_indices;
  std::unordered_map<std::string, std::vector<Site>> index_to_sites;

  // Deferred payload state.
  std::set<std::string> deferred_payload_prims;    // root-space prim paths
  LoadRules load_rules_;  // per-subtree payload overrides (on top of base policy)

  // Instancing: instance key -> prototype prim path; and the groupings.
  std::map<std::string, std::string> prototype_by_key;
  // Instance keys in DISCOVERY order (first registration during the stage
  // walk): pxr numbers flattened prototypes by this order, not by path.
  std::vector<std::string> instance_key_order;
  std::unordered_map<std::string, std::string> prototype_of;           // prim -> prototype
  std::unordered_map<std::string, std::vector<std::string>> instances_by_prototype;

  // Relocates are a per-LAYER-STACK property (LayerStack::relocates, built in
  // InternLayerStack) and are applied while deriving a prim's composition
  // sources (DeriveChildSources), so a relocate authored in a referenced or
  // payloaded layer stack renames prims inside THAT stack's namespace and the
  // rename maps through the arc into root space.



  // --- typed composition diagnostics (Phase 7 E4) -------------------------
  // Accumulated typed issues. Recorded regardless of whether a caller passed an
  // `err` pointer; when present, the rendered message is mirrored into *err so
  // the existing string-based API is unchanged.
  std::vector<CompositionIssue> issues_;

  #include "cache-layer-stack.inc"

  #include "cache-specs-instances.inc"

  #include "cache-arc-listops.inc"

  #include "cache-arc-expansion.inc"

  #include "cache-compose.inc"

  #include "cache-stage-build.inc"

  #include "cache-stage-fill.inc"

#if defined(LIGHTUSD_ENABLE_THREAD)
  #include "cache-parallel-merge.inc"
#endif

  #include "cache-parallel-warm.inc"

  #include "cache-invalidation.inc"


};

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
