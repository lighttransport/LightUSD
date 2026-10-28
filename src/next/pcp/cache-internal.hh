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
#include "../memory-trim.hh"
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <atomic>
#include <condition_variable>
#include <mutex>
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
  // Layer stacks are append-only deques, and their identifier vectors are
  // complete before publication. Borrow that immutable string instead of
  // allocating the same identifier for every positive spec-cache entry.
  // Worker spec caches remain worker-local; MergeSources resets their memos.
  const std::string *layer_identifier = nullptr;
  const std::string &layer_id() const {
    if (layer_identifier) return *layer_identifier;
    static const std::string empty;
    return empty;
  }
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
                               std::vector<SpecRef> &&v) ;

  void clear() ;

  // Keep resolved spec vectors address-stable for opinion fill while dropping
  // their now-unused lookup keys/index. This cache becomes empty and reusable.
  std::deque<std::vector<SpecRef>> take_values();

  // Read-only access for merging a worker's cache into the main cache.
  size_t entry_count() const { return count_; }
  const std::deque<std::string> &keys() const { return keys_; }
  const std::deque<std::vector<SpecRef>> &vals() const { return vals_; }
 private:
  void place(uint64_t h, uint32_t vi) ;
  void rehash() ;
};

// Exact-safe negative lookup filter for Specs().
//
// Most ExpandArcs calls ask for a site that NO layer in the stack authors
// (measured 85.6% on a full-LOD benchmark scene district). FindSpecs() answers each of
// those with one `std::unordered_map` probe per layer, hashing the (long) path
// string every time. This Bloom filter over the union of the stack's authored
// prim paths answers "definitely absent" with ONE cheap probe.
//
// A false positive falls through to the existing per-layer probes, so the set
// of specs found is unchanged: the filter can only add a probe, never remove a
// spec. Built lazily after enough cache misses (at least kBuildAfterMisses),
// capped per stack and in total, and cleared with StackSpecCache on layer
// invalidation. Layers are read-only during composition; a layer mutated
// through an authoring API without Cache::InvalidateLayer would already defeat
// StackSpecCache too.
struct PathHashFilter {
  static constexpr size_t kBitsPerPath = 10;
  static constexpr size_t kMaxStackBits = size_t(1) << 24;  // 2 MiB
  static constexpr size_t kMaxTotalBits = size_t(1) << 28;  // 32 MiB
  // Minimum negative cache misses before building; raised per stack when the
  // union scan would not amortize (see Cache::Impl::Specs).
  static constexpr uint32_t kBuildAfterMisses = 24;

  std::vector<uint64_t> words;
  uint64_t bit_mask = 0;
  uint32_t misses = 0;
  // Negative cache misses required before building. Starts at
  // kBuildAfterMisses and is raised for stacks whose path-union scan would
  // cost more than the lookups it saves (see Specs()).
  uint32_t build_after = kBuildAfterMisses;
  bool ready = false;
  bool disabled = false;

  size_t BitCount() const { return words.size() * 64; }

  // Bit budget for `paths` entries, clamped to kMaxStackBits.
  static size_t BitsFor(size_t paths) {
    size_t bits = 64;
    const size_t want =
        paths > kMaxStackBits / kBitsPerPath ? kMaxStackBits
                                             : paths * kBitsPerPath;
    while (bits < want) bits <<= 1;
    return bits;
  }

  void Reset(size_t bits) {
    bit_mask = static_cast<uint64_t>(bits - 1);
    words.assign(bits / 64, 0);
    misses = 0;
    ready = true;
    disabled = false;
  }

  void Disable() {
    words.clear();
    words.shrink_to_fit();
    bit_mask = 0;
    ready = true;
    disabled = true;
  }

  static uint64_t SecondHash(uint64_t h) { return (h >> 33) | 1; }

  void Add(uint64_t h) {
    const size_t i0 = static_cast<size_t>(h) & static_cast<size_t>(bit_mask);
    const size_t i1 = static_cast<size_t>(h + SecondHash(h)) &
                      static_cast<size_t>(bit_mask);
    words[i0 >> 6] |= uint64_t(1) << (i0 & 63);
    words[i1 >> 6] |= uint64_t(1) << (i1 & 63);
  }

  bool MightContain(uint64_t h) const {
    if (!ready || disabled) return true;
    const size_t i0 = static_cast<size_t>(h) & static_cast<size_t>(bit_mask);
    if (!(words[i0 >> 6] & (uint64_t(1) << (i0 & 63)))) return false;
    const size_t i1 = static_cast<size_t>(h + SecondHash(h)) &
                      static_cast<size_t>(bit_mask);
    return (words[i1 >> 6] & (uint64_t(1) << (i1 & 63))) != 0;
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
  // noexcept: keeps Src nothrow-movable, so a growing std::vector<Src>
  // moves its elements instead of copying each site string and context.
  SpecsMemo(SpecsMemo &&) noexcept {}
  SpecsMemo &operator=(SpecsMemo &&) noexcept {
    p = nullptr;
    return *this;
  }
};

// Whether Specs(stack_idx, site) is known to be empty or non-empty for a Src.
// Unlike SpecsMemo this is a value, not a pointer into an Impl's cache, so it
// is valid across Impls (a worker's layer stacks are adopted by content into
// the main Impl) and survives a MOVE: vector compaction, the warm-cache steal
// in MergeSources, ExpandList's reorders. It resets on COPY, because a copy
// may be re-sited by whoever makes it; such a Src simply re-resolves. The
// serial structure walk asks for the specs of ~100M sources per large stage
// and ~90% of them have none, so answering "none" from this byte instead of
// hashing the site string is the point.
// BuildStage records the Layer index of every prim RegisterInstance accepted
// (prototypes and instances), so the flatten passes can address group members
// without building a path->index map over the whole composed layer.
// (declared on Impl via cache-specs-instances.inc)
struct SpecState {
  enum : uint8_t { kUnknown = 0, kNone = 1, kSome = 2 };
  mutable uint8_t v = kUnknown;
  SpecState() = default;
  SpecState(const SpecState &) {}
  SpecState &operator=(const SpecState &) {
    v = kUnknown;
    return *this;
  }
  SpecState(SpecState &&o) noexcept : v(o.v) {}
  SpecState &operator=(SpecState &&o) noexcept {
    v = o.v;
    return *this;
  }
};

// A single composition source (one node's worth of provenance).
// The arc context a source carries -- time offset, expression variables,
// arc chain, arc-site trail. Every source derived through the same arc shares
// it (full-LOD benchmark scene: 24.7M sources, 2.9M distinct contexts), so it lives
// behind one shared pointer instead of 64 inline bytes per Src. Immutable
// while shared: Src's setters copy it first (copy-on-write).
struct SrcContext {
  LayerOffset offset;
  // Composed expression-variable context visible at this source.
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
};

struct Src {
  uint32_t stack_idx = 0;
  // Index into the owning Impl's nm_pool_ (0 == identity). The namespace mapping
  // is created once per arc and SHARED by every Src in the subtree below it, so
  // storing an index (and copying it on child-build) avoids re-copying the two
  // path strings of a non-identity NamespaceMapping for all 140K+ descendants --
  // the dominant path-string churn in compose. Fills stack_idx's padding hole.
  uint32_t map_idx = 0;
  std::string site;            // prim path within the layer stack
  // For Variant sources: the selected variant's inline opinions (lives inside a
  // shared layer's PrimMetadata, so the pointer is stable). null otherwise.
  const PrimVariantData *variant = nullptr;
  // For IMPLIED class sources (and their subtree): the layer stack whose
  // strength position this source composes at. pxr expresses an implied
  // class in each ancestor stack of the introducing arc chain, and its
  // opinions sit immediately after that stack's own positional opinions —
  // NOT at the introducing reference's position (see ExpandList's reorder).
  // UINT32_MAX == not an implied source.
  uint32_t implied_anchor = UINT32_MAX;
  ArcType arc_kind = ArcType::Root;  // arc this source arrived through
  // True when the arc was introduced at a namespace ANCESTOR (the source was
  // derived by DeriveChildSources rather than by this prim's own arc
  // expansion). pxr PcpNodeRef::IsDueToAncestor analogue; specifier
  // resolution treats a class from a direct inherit as weaker than any other
  // defining specifier, but an ancestral one composes in plain strength order.
  bool ancestral = false;
  // Relocation-source anchor whose OWN spec opinions are ignored (pxr:
  // "opinions at relocation source" are a composition error). The Src stays
  // in the list because child derivation descends through it (chained
  // relocations address content via this raw site).
  bool suppress_site_specs = false;
  // Known emptiness of Specs(stack_idx, site); see SpecState.
  SpecState spec_state;
  // Memoized Specs(stack_idx, site) result for this exact (stack_idx, site), so
  // the 2-3 read-side lookups per composed prim (ComposeChildNames,
  // IsInstanceable, instance-key, ComposeOpinions) skip re-hashing `site`.
  // Points into the owning Impl's spec_cache_by_stack_ (stable storage). Resets
  // itself on any copy/move of the Src (see SpecsMemo).
  SpecsMemo specs_;

  // Arc context (see SrcContext).
  const LayerOffset &offset() const { return ctx().offset; }
  const std::shared_ptr<const Value> &expression_variables() const {
    return ctx().expression_variables;
  }
  const std::shared_ptr<const ArcChain> &arc_chain() const {
    return ctx().arc_chain;
  }
  const std::shared_ptr<const std::vector<std::pair<uint32_t, std::string>>> &
  arc_sites() const {
    return ctx().arc_sites;
  }
  // Setters leave a shared context alone when the value does not change.
  void set_offset(const LayerOffset &o) {
    const LayerOffset &cur = offset();
    if (cur.offset == o.offset && cur.scale == o.scale) return;
    mutable_ctx().offset = o;
  }
  void set_expression_variables(std::shared_ptr<const Value> v) {
    if (expression_variables() == v) return;
    mutable_ctx().expression_variables = std::move(v);
  }
  void set_arc_chain(std::shared_ptr<const ArcChain> c) {
    if (arc_chain() == c) return;
    mutable_ctx().arc_chain = std::move(c);
  }
  void set_arc_sites(
      std::shared_ptr<const std::vector<std::pair<uint32_t, std::string>>> t) {
    if (arc_sites() == t) return;
    mutable_ctx().arc_sites = std::move(t);
  }
  // The whole context, shared (e.g. remapped once per context in the merge).
  const std::shared_ptr<SrcContext> &context() const { return ctx_; }
  void set_context(std::shared_ptr<SrcContext> c) { ctx_ = std::move(c); }

 private:
  const SrcContext &ctx() const {
    static const SrcContext kDefault;
    return ctx_ ? *ctx_ : kDefault;
  }
  SrcContext &mutable_ctx() {
    if (!ctx_) ctx_ = std::make_shared<SrcContext>();
    else if (ctx_.use_count() > 1) ctx_ = std::make_shared<SrcContext>(*ctx_);
    return *ctx_;
  }
  std::shared_ptr<SrcContext> ctx_;
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
static_assert(std::is_nothrow_move_constructible<Src>::value,
              "std::vector<Src> growth must move, not copy");

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
  static constexpr uint8_t kLive = 1;
  static constexpr uint8_t kSiteKey = 2;
  std::vector<uint8_t> live_;              // live and optional first-Src key bits
  size_t count_ = 0;                       // live entries
  size_t occupied_ = 0;                    // used + tombstone slots

  size_t size() const { return count_; }

  void clear() ;

  // Transfer the address-stable vectors to the fill pass, dropping lookup
  // keys/indexes immediately. References into the vectors remain valid until
  // the returned owner is destroyed; this cache becomes empty and reusable.
  std::deque<std::vector<Src>> take_values();

  // A resolved entry can reuse its first Src's identical site string during
  // the read-only namespace walk. Mutable access restores an owned key first.
  size_t borrow_site_keys();

  std::vector<Src> *find(const std::string &k) ;
  const std::vector<Src> *find(const std::string &k) const ;
  bool contains(const std::string &k) const { return find(k) != nullptr; }

  // find-or-insert; returns a reference that stays valid across later inserts.
  // `created`, when given, reports whether this call inserted the entry (one
  // probe instead of a contains() + get_or_create() pair per merged key).
  std::vector<Src> &get_or_create(const std::string &k, bool *created = nullptr) ;
  // Same with a precomputed Hash(k) and a key the caller gives up (the warm
  // merge moves worker keys in instead of hashing and copying them again).
  std::vector<Src> &get_or_create_hashed(std::string &&k, uint64_t h,
                                         bool *created) ;
  // Start loading the first probe slot for hash `h`.
  void prefetch(uint64_t h) const {
#if defined(__GNUC__) || defined(__clang__)
    if (!slots_.empty())
      __builtin_prefetch(&slots_[static_cast<size_t>(h) & (slots_.size() - 1)]);
#else
    (void)h;
#endif
  }
  // Visit every live entry in index order as fn(key&&, hash, value&, next_hash)
  // using the hashes already stored in the slots (no rehash of the key);
  // `next_hash` is the following live entry's hash (0 at the end), for
  // prefetching. Keys are left moved-from: only for a cache about to be
  // discarded.
  template <typename Fn>
  void drain_with_hash(Fn fn) {
    std::vector<uint64_t> hv(live_.size(), 0);
    for (const Slot &s : slots_)
      if (s.state == 1 && s.idx < hv.size()) hv[s.idx] = s.hash;
    uint32_t vi = 0;
    while (vi < live_.size() && !live_[vi]) ++vi;
    while (vi < live_.size()) {
      uint32_t nx = vi + 1;
      while (nx < live_.size() && !live_[nx]) ++nx;
      own_key(vi);
      fn(std::move(keys_[vi]), hv[vi], vals_[vi],
         nx < live_.size() ? hv[nx] : uint64_t(0));
      vi = nx;
    }
  }
  // Pre-size the index for `expected` live entries so a bulk merge does not
  // rehash its way up from the seed size.
  void reserve(size_t expected) ;

  // Merge helpers (warm merge): the live entry indices in index order with
  // their stored hashes; a read-only probe with a precomputed hash; key
  // access by entry index; taking an entry's key (cache being discarded).
  void live_with_hash(std::vector<uint32_t> *idx,
                      std::vector<uint64_t> *hash) const;
  const std::vector<Src> *find_hashed(const std::string &k, uint64_t h) const;
  const std::string &key_ref(uint32_t index) const { return key_at(index); }
  std::string take_key(uint32_t index) {
    own_key(index);
    return std::move(keys_[index]);
  }

  // Insert only if absent (merge path). Consumes v when it inserts.
  void emplace_if_absent(const std::string &k, std::vector<Src> &&v) ;

  void erase(const std::string &k) ;

  template <typename Pred>
  void erase_if(Pred pred) {
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (live_[vi] && pred(key_at(vi))) erase(key_at(vi));
    }
  }

  template <typename Fn>
  void for_each(Fn fn) {
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (live_[vi]) {
        own_key(vi);
        fn(const_cast<const std::string &>(keys_[vi]), vals_[vi]);
      }
    }
  }

  template <typename Fn>
  void for_each(Fn fn) const {
    for (uint32_t vi = 0; vi < live_.size(); ++vi) {
      if (live_[vi]) fn(key_at(vi), vals_[vi]);
    }
  }

 private:
  const std::string &key_at(size_t index) const;
  void own_key(size_t index);
  std::vector<Src> &emplace_at(size_t slot, uint64_t h, std::string k,
                               bool was_tomb) ;
  void rehash() ;
};

// Fill-only compaction after namespace/instancing work and lookup retirement.
// Retain vector object addresses and resolved spec memos for the fill's readers.
size_t CompactOpinionSources(std::vector<Src> &sources);

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

// Namespace scope of one batch of a batched (low-memory) build. Only the
// subtrees under `roots` are composed, plus their ancestors (`spine`); a spine
// prim's other children are left out, and its full child-name list is
// recorded so the writer can still emit the prim's primChildren. Instancing
// carries across batches: `seeded_protos` (instance key -> prototype path,
// from earlier batches) makes every later member an instance, and a
// prototype in `holders_with_peers` is a Holder even when its peers are all in
// other batches.
struct BatchScope {
  std::unordered_set<std::string> spine;  // proper ancestors of the roots
  std::unordered_set<std::string> roots;
  std::vector<std::pair<std::string, std::string>> seeded_protos;
  std::unordered_set<std::string> seeded_keys;
  std::unordered_set<std::string> seeded_proto_paths;
  const std::unordered_set<std::string> *holders_with_peers = nullptr;
  // Layers (registry ids) no later batch reads (pass 2, from pass 1's last
  // use): the opinion fill may release them as the default build does. Null:
  // the fill releases no layer.
  const std::unordered_set<std::string> *releasable_layers = nullptr;
  // Output: spine prim path -> its full child names (walk threads add here).
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::mutex full_children_mu;
#endif
  std::unordered_map<std::string, std::vector<std::string>> full_children;

  bool Masks(const std::string &parent) const { return spine.count(parent) != 0; }
  // Keeps the children that are spine prims or batch roots; records the full
  // list first. `parent` is a spine prim (Masks(parent)).
  void Filter(const std::string &parent, std::vector<std::string> *children) {
    {
#if defined(LIGHTUSD_ENABLE_THREAD)
      std::lock_guard<std::mutex> lk(full_children_mu);
#endif
      full_children.emplace(parent, *children);
    }
    std::string child;
    size_t kept = 0;
    for (size_t i = 0; i < children->size(); ++i) {
      child.assign(parent == "/" ? std::string() : parent);
      child += '/';
      child += (*children)[i];
      if (spine.count(child) || roots.count(child)) {
        if (kept != i) (*children)[kept] = std::move((*children)[i]);
        ++kept;
      }
    }
    children->resize(kept);
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
  // Set for one batch of a batched build (see BatchScope); null otherwise.
  BatchScope *batch_scope_ = nullptr;
  // BuildStage with release_layers_during_fill: returns freed pages to the OS
  // off the critical path (see MemoryTrimmer); null otherwise.
  MemoryTrimmer *trimmer_ = nullptr;
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
  uint32_t InternPath(const std::string &p) ;

  std::map<std::string, std::unique_ptr<PrimIndex>> index_cache;
  SrcCache sources_cache;  // path -> expanded sources (open-addressed, stable values)
  // Parallel warm workers borrow the main cache as an immutable seed and keep
  // only newly resolved entries in sources_cache. This avoids deep-copying the
  // full source cache once per worker.
  const SrcCache *source_seed_cache = nullptr;
  const std::vector<Src> *FindCachedSources(const std::string &key) const ;
  bool HasCachedSources(const std::string &key) const {
    return FindCachedSources(key) != nullptr;
  }
  std::vector<Src> &GetOrCreateCachedSources(const std::string &key) ;
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
  uint32_t InternMapping(NamespaceMapping m) ;

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
  // Every registered prim's path appears in prototype_of, its group's member
  // list and instance_prim_index_, and each member repeats its prototype's
  // path: they share refcounted PathStr nodes instead (full-LOD benchmark scene:
  // 1.06M instances, ~7K prototypes). prototype_of is keyed by a view into
  // its entry's own path node, so lookups by std::string need no allocation.
  // Hashed: the in-order structure commit looks every instance key up here
  // (long keys; ordered-map compares were a serial cost). Iteration order is
  // never relied on.
  std::unordered_map<std::string, PathStr> prototype_by_key;
  // Instance keys in DISCOVERY order (first registration during the stage
  // walk): pxr numbers flattened prototypes by this order, not by path.
  std::vector<std::string> instance_key_order;
  struct PrototypeOfEntry {
    PathStr path;   // the prim (backs the map key)
    PathStr proto;  // its prototype
  };
  std::unordered_map<std::string_view, PrototypeOfEntry> prototype_of;  // prim -> prototype
  std::unordered_map<std::string, std::vector<PathStr>> instances_by_prototype;
  // prototype_of[prim] = proto, sharing `prim`'s node as the key.
  void SetPrototypeOf(const PathStr &prim, const PathStr &proto) {
    auto it = prototype_of.find(prim.str());
    if (it != prototype_of.end()) {
      it->second.proto = proto;
      return;
    }
    PrototypeOfEntry e{prim, proto};
    const std::string_view key(e.path.str());
    prototype_of.emplace(key, std::move(e));
  }
  // The shared node for `path` when prototype_of has one, else a new one.
  PathStr PathNode(const std::string &path) const {
    auto it = prototype_of.find(path);
    return it != prototype_of.end() ? it->second.path : PathStr(path);
  }

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
