// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl token/string/path table helpers.
// Included inside CrateWriter::Impl private section.

  // ============================================================
  // Table building
  // ============================================================

  uint32_t InternToken(std::string_view s) {
    if (frozen_tok_) {  // parallel worker: read-only lookup in the shared table
      auto it = frozen_tok_->find(s);
      if (it != frozen_tok_->end()) return it->second;
      frozen_miss_ = true;  // pre-pass incomplete -> driver falls back to serial
      return 0;
    }
    if (WorkerCtx* w = WorkerTLS()) {
      // Workers never probe the live global tables (the commit thread mutates
      // them); every miss logs one ordered event, replayed exactly once in
      // chunk order at commit. Local ids start at the phase-start token count.
      auto it = w->tok_map.find(s);
      if (it != w->tok_map.end()) return it->second;
      w->tok_store.emplace_back(s);
      const std::string_view sv(w->tok_store.back());
      const uint32_t id = w_base_tok + (w->ntok++);
      w->tok_map.emplace(sv, id);
      w->events.push_back(InternEvent{sv, id, 0});
      return id;
    }
    auto it = token_to_index_.find(s);
    if (it != token_to_index_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(tokens_.size());
    tokens_.emplace_back(s);
    token_to_index_.emplace(std::string_view(tokens_.back()), idx);
    return idx;
  }

  uint32_t InternString(std::string_view s) {
    if (frozen_str_) {  // parallel worker: read-only lookup
      auto it = frozen_str_->find(s);
      if (it != frozen_str_->end()) return it->second;
      frozen_miss_ = true;
      return 0;
    }
    if (WorkerCtx* w = WorkerTLS()) {
      // Token first (serial InternString interns the token too), then the
      // string event -- the replay reproduces both tables' first-use order.
      auto it = w->str_map.find(s);
      if (it != w->str_map.end()) return it->second;
      (void)InternToken(s);
      w->str_store.emplace_back(s);
      const std::string_view sv(w->str_store.back());
      const uint32_t id = w_base_str + (w->nstr++);
      w->str_map.emplace(sv, id);
      w->events.push_back(InternEvent{sv, id, 1});
      return id;
    }
    auto it = string_to_index_.find(s);
    if (it != string_to_index_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(string_indices_.size());

    // String values in USDC: index into string_indices, which maps to tokens
    uint32_t token_idx = InternToken(s);
    string_indices_.push_back(token_idx);
    string_to_index_.emplace(std::string_view(tokens_[token_idx]), idx);
    return idx;
  }

  // ---- Path table -------------------------------------------------------
  //
  // A path is kept as a canonical (stem, name) split of its text: `name` is
  // what follows the property '.' of the last element (PropDotPos), `stem`
  // the rest; the full path is stem + "." + name, or just stem for a prim
  // path. Property specs intern (prim path, property name) directly -- no
  // per-property string concatenation or allocation -- and the PATHS section
  // derives parents and element names from the split.

  // Position of the property '.' separator in a path, or npos. A '.' inside a
  // bracketed variant-selection element ("{set=sel.2}") is part of the
  // element name, NOT a property separator: when the last element starts
  // with '{', the property dot can only come after its '}'.
  static size_t PropDotPos(std::string_view p) {
    const size_t slash = p.rfind('/');
    size_t i = (slash == std::string_view::npos) ? 0 : slash + 1;
    if (i < p.size() && p[i] == '{') {
      const size_t close = p.find('}', i);
      if (close == std::string_view::npos) return std::string_view::npos;
      i = close + 1;
    }
    return p.find('.', i);
  }
  static PathRec SplitPath(std::string_view full) {
    const size_t dot = PropDotPos(full);
    if (dot == std::string_view::npos) return PathRec{full, std::string_view()};
    return PathRec{full.substr(0, dot), full.substr(dot + 1)};
  }
  static uint64_t HashPathText(std::string_view s, uint64_t h) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
  }
  static uint64_t HashStem(std::string_view stem) {
    return HashPathText(stem, 14695981039346656037ull);
  }
  static uint64_t HashPathKey(uint64_t stem_hash, std::string_view name) {
    if (name.empty()) return stem_hash;
    // Continue the stem hash across a '.' separator: equal to hashing the
    // full text, so both interning routes agree for any canonical split.
    return HashPathText(name, (stem_hash ^ '.') * 1099511628211ull);
  }

  static constexpr size_t kNoPathSlot = static_cast<size_t>(-1);

  static size_t PathShardOf(uint64_t h) {
    return static_cast<size_t>(h >> 28) & (kPathShards - 1);
  }
  static uint64_t PathSlotTag(uint64_t h) { return h & 0xFFFFFFFF00000000ull; }
  // Slot capacity keeps each shard's load factor <= 3/4: a probe chain is
  // walked in the slot array alone (fingerprints), so a fuller table costs a
  // few more adjacent slots, not more misses, and the 64-bit slots then take
  // no more memory than 32-bit ones at load <= 1/2.
  static bool PathSlotsFull(size_t n, size_t cap) { return n * 4 > cap * 3; }

  // Touch the slot a coming probe will read. The path table is far larger
  // than any cache on a big scene, so each intern otherwise starts with a
  // stalled random load; the emit loops know the next path one step ahead.
  void PrefetchPathSlot(uint64_t h) const {
    if (WorkerTLS()) return;  // the global slot arrays may rehash concurrently
#if defined(__GNUC__) || defined(__clang__)
    const std::vector<uint64_t>& slots = path_shards_[PathShardOf(h)].slots;
    if (!slots.empty())
      __builtin_prefetch(&slots[static_cast<size_t>(h) & (slots.size() - 1)]);
#else
    (void)h;
#endif
  }

  // Probe for (stem, name). On a miss, *empty_slot (if given) receives the
  // empty slot ending the chain in the key's shard, for AddPathRec.
  uint32_t FindPathKey(std::string_view stem, std::string_view name,
                       uint64_t h, size_t* empty_slot = nullptr) const {
    const std::vector<uint64_t>& slots = path_shards_[PathShardOf(h)].slots;
    if (slots.empty()) return UINT32_MAX;
    const size_t mask = slots.size() - 1;
    const uint64_t tag = PathSlotTag(h);
    for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
      const uint64_t slot = slots[i];
      if (slot == 0) {
        if (empty_slot) *empty_slot = i;
        return UINT32_MAX;
      }
      if ((slot & 0xFFFFFFFF00000000ull) != tag) continue;
      const uint32_t idx = static_cast<uint32_t>(slot) - 1;
      if (path_fingerprints_[idx] != static_cast<uint16_t>(h)) continue;
      if (paths_[idx].stem() == stem && paths_[idx].name() == name) {
        return idx;
      }
    }
  }
  static void PlaceInShard(std::vector<uint64_t>& slots, uint64_t h,
                           uint32_t idx) {
    const size_t mask = slots.size() - 1;
    size_t i = static_cast<size_t>(h) & mask;
    while (slots[i] != 0) i = (i + 1) & mask;
    slots[i] = PathSlotTag(h) | (uint64_t(idx) + 1);
  }
  // Grow one shard to hold at least `n` records (re-places only its own
  // entries, read back from the old slots; touches no other shard).
  void ReservePathShard(PathShard& sh, size_t n) {
    size_t cap = 16;
    while (PathSlotsFull(n, cap)) cap *= 2;
    if (cap <= sh.slots.size()) return;
    std::vector<uint64_t> old;
    old.swap(sh.slots);
    sh.slots.assign(cap, 0);
    for (const uint64_t slot : old) {
      if (slot == 0) continue;
      const uint32_t idx = static_cast<uint32_t>(slot) - 1;
      const PathRec& rec = paths_[idx];
      const uint64_t hash = HashPathKey(HashStem(rec.stem()), rec.name());
      PlaceInShard(sh.slots, hash, idx);
    }
  }
  void ReservePaths(size_t n) {
    paths_.reserve(n);
    path_fingerprints_.reserve(n);
    // Hash bits split records evenly; 1/8 slack covers the spread.
    const size_t per = n / kPathShards;
    for (PathShard& sh : path_shards_)
      ReservePathShard(sh, per + per / 8 + 64);
  }
  // Insert record `idx` (hash h) into its shard, growing it when full;
  // `slot` is a FindPathKey empty-slot hint in that shard (or kNoPathSlot).
  void InsertPathSlot(uint64_t h, uint32_t idx, size_t slot = kNoPathSlot) {
    PathShard& sh = path_shards_[PathShardOf(h)];
    if (PathSlotsFull(sh.count + 1, sh.slots.size())) {
      ReservePathShard(sh, std::max<size_t>(64, (sh.count + 1) * 2));
      slot = kNoPathSlot;  // rehash invalidates the failed lookup's slot
    }
    if (slot == kNoPathSlot) PlaceInShard(sh.slots, h, idx);
    else sh.slots[slot] = PathSlotTag(h) | (uint64_t(idx) + 1);
    ++sh.count;
  }
  // Append a new record (views must outlive the write).
  uint32_t AddPathRec(std::string_view stem, std::string_view name,
                      uint64_t h, size_t slot = kNoPathSlot) {
    const uint32_t idx = static_cast<uint32_t>(paths_.size());
    paths_.push_back(PathRec{stem, name});
    path_fingerprints_.push_back(static_cast<uint16_t>(h));
    InsertPathSlot(h, idx, slot);
    return idx;
  }

  // Append a new (or log-local) path record. Worker mode logs a PathEvent
  // (the commit replays probe+insert in event order = serial first-use
  // order) and mirrors it in the worker's private table. When `owned_full`
  // is non-empty the caller's text may be transient (scratch built for one
  // prim): duplicate it into the ctx's owned storage and use stable views for
  // both the private table and the commit event.
  uint32_t WorkerInternPathKey(WorkerCtx* w, std::string_view stem,
                               std::string_view name, uint64_t h,
                               std::string_view owned_full) {
    size_t slot = kNoPathSlot;
    const uint32_t hit = w->ptab.Find(stem, name, h, &slot);
    if (hit != UINT32_MAX) return w_base_path + hit;
    PathEvent ev;
    ev.hash = h;
    ev.stem = stem;
    ev.name = name;
    ev.store = kStableText;
    if (!owned_full.empty()) {
      ev.store = static_cast<uint32_t>(w->path_store.size());
      w->path_store.emplace_back(owned_full);
      const PathRec owned = SplitPath(w->path_store.back());
      ev.stem = owned.stem();
      ev.name = owned.name();
    }
    ev.local = w_base_path + static_cast<uint32_t>(w->ptab.recs.size());
    w->ptab.Add(ev.stem, ev.name, h, slot);
    w->pevents.push_back(ev);
    return ev.local;
  }

  // Intern a path whose text is transient: copied (once) when new.
  uint32_t InternPath(const std::string& path) {
    const PathRec key = SplitPath(path);
    const uint64_t h = HashPathKey(HashStem(key.stem()), key.name());
    if (WorkerCtx* w = WorkerTLS()) {
      return WorkerInternPathKey(w, key.stem(), key.name(), h,
                                 std::string_view(path));
    }
    size_t slot = kNoPathSlot;
    const uint32_t hit = FindPathKey(key.stem(), key.name(), h, &slot);
    if (hit != UINT32_MAX) return hit;
    path_owned_.push_back(path);
    const PathRec owned = SplitPath(path_owned_.back());
    return AddPathRec(owned.stem(), owned.name(), h, slot);
  }
  // Intern a path whose text outlives the write (e.g. a Layer prim path).
  uint32_t InternPathStable(std::string_view path) {
    const PathRec key = SplitPath(path);
    const uint64_t h = HashPathKey(HashStem(key.stem()), key.name());
    if (WorkerCtx* w = WorkerTLS()) {
      return WorkerInternPathKey(w, key.stem(), key.name(), h, std::string_view());
    }
    size_t slot = kNoPathSlot;
    const uint32_t hit = FindPathKey(key.stem(), key.name(), h, &slot);
    if (hit != UINT32_MAX) return hit;
    return AddPathRec(key.stem(), key.name(), h, slot);
  }
  // Intern "<prim_path>.<name>" for stable views, given HashStem(prim_path).
  // Only valid when that concatenation splits back into exactly
  // (prim_path, name) -- see PropPathSplitsCleanly().
  uint32_t InternPropPath(std::string_view prim_path, uint64_t prim_hash,
                          std::string_view name) {
    const uint64_t h = HashPathKey(prim_hash, name);
    if (WorkerCtx* w = WorkerTLS()) {
      return WorkerInternPathKey(w, prim_path, name, h, std::string_view());
    }
    size_t slot = kNoPathSlot;
    const uint32_t hit = FindPathKey(prim_path, name, h, &slot);
    if (hit != UINT32_MAX) return hit;
    return AddPathRec(prim_path, name, h, slot);
  }
  // True when every "<prim_path>.<name>" (name without '/') canonically
  // splits back into (prim_path, name).
  static bool PropPathSplitsCleanly(std::string_view prim_path) {
    if (prim_path.empty()) return false;
    const size_t slash = prim_path.rfind('/');
    size_t i = (slash == std::string_view::npos) ? 0 : slash + 1;
    if (i < prim_path.size() && prim_path[i] == '{') {
      const size_t close = prim_path.find('}', i);
      if (close == std::string_view::npos) return false;
      i = close + 1;
    }
    return prim_path.find('.', i) == std::string_view::npos;
  }
  uint32_t FindPath(std::string_view path) const {
    const PathRec key = SplitPath(path);
    return FindPathKey(key.stem(), key.name(),
                       HashPathKey(HashStem(key.stem()), key.name()));
  }

  // pxr's compressed path tree requires EVERY ancestor of every path to be
  // present as a node. Connection/relationship TARGET paths can reference
  // prims that have no spec in this layer (e.g. material shader outputs in a
  // not-yet-composed referenced file), so synthesize the missing ancestor
  // chain (tree-only nodes; no spec refers to them), appended in scan order.
  // Ancestors are prefixes of existing stems, so they are stored as views.
  // `a` is `b` or a namespace ancestor of it ("/A/B" vs "/A/B/C").
  static bool IsSlashPrefix(std::string_view a, std::string_view b) {
    if (a.size() > b.size()) return false;
    if (b.compare(0, a.size(), a) != 0) return false;
    return a.size() == b.size() || b[a.size()] == '/';
  }

  void SynthesizeAncestorPaths() {
    auto add = [&](std::string_view v) -> bool {
      const PathRec key = SplitPath(v);
      const uint64_t h = HashPathKey(HashStem(key.stem()), key.name());
      size_t slot = kNoPathSlot;
      if (FindPathKey(key.stem(), key.name(), h, &slot) != UINT32_MAX) return false;
      AddPathRec(key.stem(), key.name(), h, slot);
      return true;
    };
    const size_t orig = paths_.size();
    // Paths of one prim arrive together; a stem already walked needs no
    // second walk (the walk would only re-probe existing ancestors).
    //
    // Known-present range: after a walk, every path from `walked_stem` up to
    // `walked_floor` (inclusive, along its ancestor chain) is in the table --
    // either it was found there or the walk just added it. A later probe of
    // a path inside that range returns "present" and ends the walk, so it is
    // answered here without hashing the (long) stem. Presence only grows, so
    // the range stays valid; the result is identical to probing.
    std::string_view walked_stem;
    std::string_view walked_floor;
    bool have_walked = false;
    auto known_present = [&](std::string_view p) {
      return have_walked && p.size() >= walked_floor.size() &&
             IsSlashPrefix(p, walked_stem);
    };
    // Read-only parallel pre-pass: the walk below stops at the first ancestor
    // already present, so a record can add a path only if its prim part (for a
    // property) or its stem's parent is missing now. Presence only grows, so a
    // record cleared here would add nothing in the ordered walk either.
    std::vector<uint8_t> may_add(orig, uint8_t{0});
    ParallelRanges(orig, 100000, [&](size_t, size_t lo, size_t hi) {
      std::string_view last_stem;
      bool have_last = false, last_add = false;
      for (size_t i = lo; i < hi; ++i) {
        const PathRec& rec = paths_[i];
        if (!(have_last && rec.stem() == last_stem)) {
          const std::string_view stem = rec.stem();
          bool add_any = false;
          if (stem.size() > 1) {
            const size_t sl = stem.rfind('/');
            if (sl != std::string_view::npos && sl != 0 &&
                FindPath(stem.substr(0, sl)) == UINT32_MAX)
              add_any = true;
          }
          last_stem = stem;
          last_add = add_any;
          have_last = true;
        }
        bool add = last_add;
        if (!add && !rec.name().empty() && !rec.stem().empty() &&
            rec.stem() != "/" && FindPath(rec.stem()) == UINT32_MAX)
          add = true;
        may_add[i] = add ? 1 : 0;
      }
    });
    for (size_t i = 0; i < orig; i++) {
      if (!may_add[i]) continue;
      const PathRec rec = paths_[i];
      if (have_walked && rec.stem() == walked_stem) continue;
      std::string_view cur = rec.stem();
      if (!rec.name().empty()) {
        // property path: start from its prim part
        if (!cur.empty() && cur != "/" && !known_present(cur)) add(cur);
      }
      // `floor`: the highest ancestor this walk proves present.
      std::string_view floor = cur;
      while (cur.size() > 1) {
        const size_t sl = cur.rfind('/');
        if (sl == std::string_view::npos || sl == 0) break;
        cur = cur.substr(0, sl);
        if (known_present(cur)) {
          // The previous walk's range continues from here up to its floor.
          floor = walked_floor;
          break;
        }
        floor = cur;
        if (!add(cur)) break;  // rest of the chain already exists
      }
      walked_stem = rec.stem();
      walked_floor = floor;
      have_walked = true;
    }
  }

  // Register every path-element token before the TOKENS section is written
  // (WritePathsSection only looks them up). Components are interned in the
  // same order as splitting each full path text on '/' and '.' (skipping its
  // first character). Interning an already-present token is a no-op, so only
  // components that may be NEW need a lookup; two cheap proofs skip the rest
  // without changing which tokens are added or their order:
  //  - the stem components before the longest '/'-bounded prefix shared with
  //    the previous record's stem were interned when that stem was split;
  //  - property names are views into the PropNameTable, so a repeated
  //    (pointer, length) is a name already split once.
  void RegisterPathElementTokens() {
    auto intern_components = [&](std::string_view s, size_t start) {
      while (start < s.size()) {
        size_t end = s.size();
        const size_t slash = s.find('/', start);
        const size_t dot = s.find('.', start);
        if (slash != std::string_view::npos) end = std::min(end, slash);
        if (dot != std::string_view::npos) end = std::min(end, dot);
        if (end > start) InternToken(s.substr(start, end - start));
        start = end + 1;
      }
    };
    // Open-addressed (pointer, length) set of names already split. Bounded:
    // when full it stops recording and names are split as before.
    struct NameSeen {
      const char* p;
      size_t n;
    };
    std::vector<NameSeen> seen(size_t(1) << 16, NameSeen{nullptr, 0});
    size_t seen_used = 0;
    constexpr size_t kSeenMax = size_t(1) << 22;
    auto seen_insert = [&](std::string_view v) -> bool {  // true if new
      if (!v.data()) return true;
      auto place = [](std::vector<NameSeen>& t, const char* p, size_t n,
                      bool* found) -> NameSeen* {
        const size_t mask = t.size() - 1;
        size_t i = static_cast<size_t>(
                       (reinterpret_cast<uintptr_t>(p) >> 3) *
                       0x9E3779B97F4A7C15ull >> 20) & mask;
        for (;; i = (i + 1) & mask) {
          if (!t[i].p) { *found = false; return &t[i]; }
          if (t[i].p == p && t[i].n == n) { *found = true; return &t[i]; }
        }
      };
      bool found = false;
      NameSeen* slot = place(seen, v.data(), v.size(), &found);
      if (found) return false;
      if ((seen_used + 1) * 2 > seen.size()) {
        if (seen.size() >= kSeenMax) return true;  // stop recording
        std::vector<NameSeen> grown(seen.size() * 2, NameSeen{nullptr, 0});
        for (const NameSeen& e : seen) {
          if (!e.p) continue;
          bool f = false;
          *place(grown, e.p, e.n, &f) = e;
        }
        seen.swap(grown);
        slot = place(seen, v.data(), v.size(), &found);
      }
      *slot = NameSeen{v.data(), v.size()};
      ++seen_used;
      return true;
    };
    // Read-only parallel pre-pass: flag the records with any component not in
    // the token table yet (same split and longest-common-prefix skip as the
    // ordered pass below, with lookups instead of inserts). Presence only
    // grows, so an unflagged record would intern nothing below.
    const size_t npaths = paths_.size();
    std::vector<uint8_t> may_add(npaths, uint8_t{0});
    auto has_token = [&](std::string_view t) {
      return token_to_index_.find(t) != token_to_index_.end();
    };
    auto components_present = [&](std::string_view s, size_t start) {
      while (start < s.size()) {
        size_t end = s.size();
        const size_t slash = s.find('/', start);
        const size_t dot = s.find('.', start);
        if (slash != std::string_view::npos) end = std::min(end, slash);
        if (dot != std::string_view::npos) end = std::min(end, dot);
        if (end > start && !has_token(s.substr(start, end - start)))
          return false;
        start = end + 1;
      }
      return true;
    };
    auto shared_start = [](std::string_view stem, std::string_view prev) {
      // Longest common prefix, then back off to a '/' that is a component
      // boundary in BOTH stems.
      size_t start = 1;
      const size_t lim = std::min(stem.size(), prev.size());
      size_t k = 0;
      while (k < lim && stem[k] == prev[k]) ++k;
      size_t p = std::string_view::npos;
      if (k == prev.size() && k < stem.size() && stem[k] == '/') {
        p = k;  // stem is under prev
      } else if (k > 0) {
        p = stem.rfind('/', k - 1);  // same char in both below k
      }
      if (p != std::string_view::npos && p >= 1) start = p + 1;
      return start;
    };
    ParallelRanges(npaths, 100000, [&](size_t, size_t lo, size_t hi) {
      std::string_view prev;
      bool have = false;
      const char* last_name = nullptr;
      size_t last_name_len = 0;
      for (size_t i = lo; i < hi; ++i) {
        const PathRec& rec = paths_[i];
        if (rec.name().empty() && (rec.stem().empty() || rec.stem() == "/")) {
          may_add[i] = has_token("") ? 0 : 1;
          continue;
        }
        bool add = false;
        if (!(have && rec.stem() == prev)) {
          add = !components_present(rec.stem(),
                                    have ? shared_start(rec.stem(), prev) : 1);
        }
        if (!add && !rec.name().empty() &&
            !(rec.name().data() == last_name && rec.name().size() == last_name_len)) {
          add = !components_present(rec.name(), 0);
          last_name = rec.name().data();
          last_name_len = rec.name().size();
        }
        if (add) {
          // A flagged record ends this slice's chain of verified stems.
          have = false;
          last_name = nullptr;
        } else {
          prev = rec.stem();
          have = true;
        }
        may_add[i] = add ? 1 : 0;
      }
    });

    std::string_view prev_stem;
    bool have_prev = false;
    for (size_t ri = 0; ri < npaths; ++ri) {
      const PathRec& rec = paths_[ri];
      if (rec.name().empty() && (rec.stem().empty() || rec.stem() == "/")) {
        if (may_add[ri]) InternToken("");  // Token 0 = empty string for root path marker
        continue;
      }
      if (!may_add[ri]) {
        // Every component is interned already: nothing to add, and this
        // stem is a valid shared-prefix reference for the next record.
        prev_stem = rec.stem();
        have_prev = true;
        continue;
      }
      if (!(have_prev && rec.stem() == prev_stem)) {
        size_t start = 1;
        if (have_prev) {
          // Longest common prefix, then back off to a '/' that is a
          // component boundary in BOTH stems.
          const size_t lim = std::min(rec.stem().size(), prev_stem.size());
          size_t k = 0;
          while (k < lim && rec.stem()[k] == prev_stem[k]) ++k;
          size_t p = std::string_view::npos;
          if (k == prev_stem.size() && k < rec.stem().size() &&
              rec.stem()[k] == '/') {
            p = k;  // rec.stem is under prev_stem
          } else if (k > 0) {
            p = rec.stem().rfind('/', k - 1);  // same char in both below k
          }
          if (p != std::string_view::npos && p >= 1) start = p + 1;
        }
        intern_components(rec.stem(), start);
      }
      // A property's name follows its stem's '.'; with an empty stem the
      // skipped first character is that '.' itself.
      if (!rec.name().empty() && seen_insert(rec.name()))
        intern_components(rec.name(), 0);
    }
  }

  // First-use-order prim/type/property-name interning, built in parallel.
  //
  // The token and string tables are ordered by first use in the prim walk.
  // Each slice records unique texts in local encounter order; slices cover
  // contiguous prim ranges, so replay in slice order preserves the global
  // order. Per-prim id arrays are filled after the table is immutable.
  //
  // Returns false for small layers or a non-threaded build; the caller then
  // runs the serial loop.
  bool BuildPrimTablesParallel(const Layer& layer, PropNameTable& name_table) {
#if !defined(LIGHTUSD_ENABLE_THREAD)
    (void)layer;
    (void)name_table;
    return false;
#else
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
    // Deferred fill interns each prim's names when it is encoded (its table
    // order differs by design), so there is nothing to pre-collect here.
    if (deferred_filler_) return false;
#endif
    const size_t nprims = layer.prims().size();
    if (nprims < kPrimTablesParallelMinPrims) return false;
    const size_t T = static_cast<size_t>(ResolveThreads());
    if (T < 2) return false;
    const size_t nprop_ids = name_table.size();
    const size_t nprop_words = (nprop_ids + 63) / 64;

    struct Slice {
      std::unordered_set<std::string_view> tok_seen;
      std::unordered_set<std::string_view> str_seen;
      std::vector<std::string_view> tok_events;  // first-use order in this slice
      std::vector<std::string_view> str_events;
      std::vector<uint64_t> prop_seen;  // packed by PropNameId
      // A property id past the snapshot (another layer interned a new name
      // while we scanned): the serial loop must run instead.
      bool prop_overflow = false;
    };
    std::vector<Slice> slices(T);
    for (Slice& s : slices) {
      const size_t token_hint = nprims / T / 4 + 1024;
      s.tok_seen.reserve(token_hint);
      s.tok_events.reserve(token_hint);
      s.str_seen.reserve(64);
      s.str_events.reserve(128);
      s.prop_seen.assign(nprop_words, 0);
    }
    auto note = [](std::unordered_set<std::string_view>& seen,
                   std::vector<std::string_view>& events,
                   std::string_view text) {
      if (seen.insert(text).second) events.push_back(text);
    };
    auto run = [&](size_t t, size_t lo, size_t hi) {
      Slice& sl = slices[t];
      for (size_t i = lo; i < hi; ++i) {
        const PrimSpec& prim = layer.prims()[i];
        const auto tok = [&](std::string_view s) {
          note(sl.tok_seen, sl.tok_events, s);
        };
        // InternString interns the token too, so a string item occupies a
        // position in both tables (same key).
        const auto str = [&](std::string_view s) {
          note(sl.tok_seen, sl.tok_events, s);
          note(sl.str_seen, sl.str_events, s);
        };
        tok(prim.name());
        tok(prim.type_name());
        for (const auto& slot : prim.properties().slots()) {
          const uint32_t id = slot.name_id.id;
          if (id == UINT32_MAX) continue;
          if (id >= nprop_ids) {
            sl.prop_overflow = true;
            continue;
          }
          // Only the first use of a property name occupies a position; the
          // per-slice flag keeps the 51.6 M repeat slots off the hash.
          const uint64_t prop_bit = uint64_t(1) << (id & 63);
          uint64_t& prop_word = sl.prop_seen[id >> 6];
          if (prop_word & prop_bit) continue;
          prop_word |= prop_bit;
          tok(name_table.get(slot.name_id));
        }
        if (prim.meta().doc_authored() || !prim.meta().doc().empty()) {
          str(prim.meta().doc());
        }
        for (const auto& schema : prim.meta().apiSchemas()) tok(schema);
        {
          const StringListOps& edits = prim.meta().apiSchemaEdits();
          const std::vector<std::string>* lists[] = {
              &edits.explicit_items, &edits.added, &edits.prepended,
              &edits.appended, &edits.deleted, &edits.ordered};
          for (const std::vector<std::string>* list : lists)
            for (const std::string& item : *list) tok(item);
        }
        {
          const StringListOps& edits = prim.meta().clipSetEdits();
          const std::vector<std::string>* lists[] = {
              &edits.explicit_items, &edits.added, &edits.prepended,
              &edits.appended, &edits.deleted, &edits.ordered};
          for (const std::vector<std::string>* list : lists)
            for (const std::string& item : *list) tok(item);
        }
        for (const auto& variant_set : prim.meta().variantSets()) {
          str(variant_set.name);
        }
        {
          const StringListOps& edits = prim.meta().variantSetNameEdits();
          const std::vector<std::string>* lists[] = {
              &edits.explicit_items, &edits.added, &edits.prepended,
              &edits.appended, &edits.deleted, &edits.ordered};
          for (const std::vector<std::string>* list : lists)
            for (const std::string& item : *list) str(item);
        }
        for (const auto& ref : prim.meta().references) str(ref);
        for (const auto& inher : prim.meta().inherits) str(inher);
      }
    };
    {
      std::vector<std::thread> pool;
      pool.reserve(T - 1);
      for (size_t t = 1; t < T; ++t) {
        pool.emplace_back([&run, t, nprims, T] {
          run(t, nprims * t / T, nprims * (t + 1) / T);
        });
      }
      run(0, 0, nprims / T);
      for (std::thread& th : pool) th.join();
    }

    for (const Slice& s : slices) {
      if (s.prop_overflow) return false;  // nothing interned yet: safe to bail
    }
    // Each slice visited prims in ascending order, so its event vectors already
    // hold first-use order. The slices cover consecutive prim ranges; replay
    // them in slice order to preserve the global table order. Repeated texts in
    // later slices are harmless because InternToken/InternString deduplicate.
    std::vector<uint64_t> prop_seen(nprop_words, 0);
    for (size_t t = 0; t < T; ++t) {
      for (std::string_view event : slices[t].tok_events) (void)InternToken(event);
      for (size_t word = 0; word < nprop_words; ++word)
        prop_seen[word] |= slices[t].prop_seen[word];
    }
    for (size_t t = 0; t < T; ++t)
      for (std::string_view event : slices[t].str_events) (void)InternString(event);

    // Per-prim / per-property name ids (values are order independent). The
    // token table no longer changes, so the 10 M prim-name lookups may read it
    // concurrently.
    prim_name_tokens_.resize(nprims);
    {
      const auto& table = token_to_index_;
      std::atomic<bool> missing{false};  // cannot happen: the collect covers it
      auto fill = [&](size_t lo, size_t hi) {
        for (size_t i = lo; i < hi; ++i) {
          auto it = table.find(layer.prims()[i].name());
          if (it == table.end()) {
            missing.store(true, std::memory_order_relaxed);
            prim_name_tokens_[i] = 0;
          } else {
            prim_name_tokens_[i] = it->second;
          }
        }
      };
      std::vector<std::thread> pool;
      pool.reserve(T - 1);
      for (size_t t = 1; t < T; ++t) {
        pool.emplace_back([&fill, t, nprims, T] {
          fill(nprims * t / T, nprims * (t + 1) / T);
        });
      }
      fill(0, nprims / T);
      for (std::thread& th : pool) th.join();
      // Defensive only: never leave a wrong token id behind.
      if (missing.load(std::memory_order_relaxed))
        for (size_t i = 0; i < nprims; ++i)
          prim_name_tokens_[i] = InternToken(layer.prims()[i].name());
    }
    if (prop_name_tokens_.size() < nprop_ids)
      prop_name_tokens_.resize(nprop_ids, UINT32_MAX);
    for (size_t word = 0; word < nprop_words; ++word) {
      uint64_t bits = prop_seen[word];
      while (bits) {
#if defined(__GNUC__) || defined(__clang__)
        const unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
#else
        unsigned bit = 0;
        while (((bits >> bit) & 1u) == 0) ++bit;
#endif
        const size_t id = word * 64 + bit;
        if (id < nprop_ids) {
          PropNameId pid;
          pid.id = static_cast<uint32_t>(id);
          prop_name_tokens_[id] = InternToken(name_table.get(pid));
        }
        bits &= bits - 1;
      }
    }
    return true;
#endif
  }

  static constexpr size_t kPrimTablesParallelMinPrims = 65536;

  void BuildTables(const Layer& layer) {
    // Token 0: magic marker expected by pxrUSD's crate reader
    InternToken(";-)");  // software identifier (pxrUSD convention)
    // Token 1: empty string, reserved for root path marker in PATHS
    InternToken("");

    // Common field name tokens
    type_name_token_ = InternToken("typeName");
    specifier_token_ = InternToken("specifier");
    default_token_ = InternToken("default");
    InternToken("active");
    InternToken("hidden");
    InternToken("doc");
    InternToken("documentation");
    InternToken("apiSchemas");
    InternToken("kind");
    InternToken("permission");
    variability_token_ = InternToken("variability");
    InternToken("defaultPrim");
    InternToken("upAxis");
    InternToken("metersPerUnit");
    InternToken("timeCodesPerSecond");
    InternToken("startTimeCode");
    InternToken("endTimeCode");
    InternToken("references");
    InternToken("payload");
    InternToken("inherits");
    InternToken("specializes");
    InternToken("variantSets");
    InternToken("variants");
    InternToken("variantSelection");
    InternToken("customData");
    InternToken("clips");
    InternToken("clipSets");
    InternToken("comment");
    InternToken("subLayers");

    // Common type name tokens
    InternToken("Xform");
    InternToken("Scope");
    InternToken("Mesh");
    InternToken("Material");
    InternToken("Shader");
    InternToken("Camera");
    InternToken("Sphere");
    InternToken("Cube");
    InternToken("Cylinder");
    InternToken("Cone");
    InternToken("Capsule");
    InternToken("Skeleton");
    InternToken("SkelAnimation");
    InternToken("BlendShape");
    InternToken("SkelRoot");

    // Up/down axis tokens
    InternToken("Y");
    InternToken("Z");

    // Intern layer metadata
    if (layer.meta().defaultPrim_set || !layer.meta().defaultPrim.empty()) {
      InternToken(layer.meta().defaultPrim);
    }
    if (layer.meta().doc_set || !layer.meta().doc.empty()) {
      InternString(layer.meta().doc);
    }

    // Prim names, type names, property names. Record the prim-name and
    // property-name tokens: BuildOnePrim re-emits them in `primChildren` and
    // `properties`, and a property name shared by millions of prims is hashed
    // once here instead of once per slot.
    prim_name_tokens_.reserve(layer.prims().size());
    PropNameTable& name_table = GetPropNameTable();
    // Large layers build the same tables from a parallel first-use scan.
    if (BuildPrimTablesParallel(layer, name_table)) return;
    for (const auto& prim : layer.prims()) {
      prim_name_tokens_.push_back(InternToken(prim.name()));
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
      // Deferred fill: only the skeleton exists yet; the rest is interned
      // when each prim is encoded (table order differs, content does not).
      if (deferred_filler_) continue;
#endif
      InternToken(prim.type_name());

      for (const auto& slot : prim.properties().slots()) {
        const uint32_t id = slot.name_id.id;
        if (id == UINT32_MAX) continue;
        if (id >= prop_name_tokens_.size()) {
          prop_name_tokens_.resize(
              std::max<size_t>(id + 1, prop_name_tokens_.size() * 2),
              UINT32_MAX);
        }
        uint32_t& tok = prop_name_tokens_[id];
        if (tok == UINT32_MAX) tok = InternToken(name_table.get(slot.name_id));
      }

      // Metadata strings
      if (prim.meta().doc_authored() || !prim.meta().doc().empty()) {
        InternString(prim.meta().doc());
      }
      for (const auto& schema : prim.meta().apiSchemas()) {
        InternToken(schema);
      }
      {
        const StringListOps& edits = prim.meta().apiSchemaEdits();
        const std::vector<std::string>* lists[] = {
            &edits.explicit_items, &edits.added, &edits.prepended,
            &edits.appended, &edits.deleted, &edits.ordered};
        for (const std::vector<std::string>* list : lists) {
          for (const std::string& item : *list) InternToken(item);
        }
      }
      {
        const StringListOps& edits = prim.meta().clipSetEdits();
        const std::vector<std::string>* lists[] = {
            &edits.explicit_items, &edits.added, &edits.prepended,
            &edits.appended, &edits.deleted, &edits.ordered};
        for (const std::vector<std::string>* list : lists) {
          for (const std::string& item : *list) InternToken(item);
        }
      }
      for (const auto& variant_set : prim.meta().variantSets()) {
        InternString(variant_set.name);
      }
      {
        const StringListOps& edits = prim.meta().variantSetNameEdits();
        const std::vector<std::string>* lists[] = {
            &edits.explicit_items, &edits.added, &edits.prepended,
            &edits.appended, &edits.deleted, &edits.ordered};
        for (const std::vector<std::string>* list : lists) {
          for (const std::string& item : *list) InternString(item);
        }
      }
      for (const auto& ref : prim.meta().references) {
        InternString(ref);
      }
      for (const auto& inher : prim.meta().inherits) {
        InternString(inher);
      }
    }
  }
