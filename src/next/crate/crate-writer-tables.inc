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

  uint32_t FindPathKey(std::string_view stem, std::string_view name,
                       uint64_t h) const {
    if (path_slots_.empty()) return UINT32_MAX;
    const size_t mask = path_slots_.size() - 1;
    for (size_t i = static_cast<size_t>(h) & mask;; i = (i + 1) & mask) {
      const uint32_t slot = path_slots_[i];
      if (slot == 0) return UINT32_MAX;
      const uint32_t idx = slot - 1;
      if (path_hashes_[idx] == h && paths_[idx].stem == stem &&
          paths_[idx].name == name) {
        return idx;
      }
    }
  }
  void PlacePathSlot(uint32_t idx) {
    const size_t mask = path_slots_.size() - 1;
    size_t i = static_cast<size_t>(path_hashes_[idx]) & mask;
    while (path_slots_[i] != 0) i = (i + 1) & mask;
    path_slots_[i] = idx + 1;
  }
  void ReservePaths(size_t n) {
    paths_.reserve(n);
    path_hashes_.reserve(n);
    size_t cap = 16;
    while (cap < n * 2) cap *= 2;
    if (cap > path_slots_.size()) {
      path_slots_.assign(cap, 0);
      for (uint32_t i = 0; i < paths_.size(); ++i) PlacePathSlot(i);
    }
  }
  // Append a new record (views must outlive the write).
  uint32_t AddPathRec(std::string_view stem, std::string_view name,
                      uint64_t h) {
    if ((paths_.size() + 1) * 2 > path_slots_.size()) {
      ReservePaths(std::max<size_t>(64, (paths_.size() + 1) * 2));
    }
    const uint32_t idx = static_cast<uint32_t>(paths_.size());
    paths_.push_back(PathRec{stem, name});
    path_hashes_.push_back(h);
    PlacePathSlot(idx);
    return idx;
  }

  // Intern a path whose text is transient: copied (once) when new.
  uint32_t InternPath(const std::string& path) {
    const PathRec key = SplitPath(path);
    const uint64_t h = HashPathKey(HashStem(key.stem), key.name);
    const uint32_t hit = FindPathKey(key.stem, key.name, h);
    if (hit != UINT32_MAX) return hit;
    path_owned_.push_back(path);
    const PathRec owned = SplitPath(path_owned_.back());
    return AddPathRec(owned.stem, owned.name, h);
  }
  // Intern a path whose text outlives the write (e.g. a Layer prim path).
  uint32_t InternPathStable(std::string_view path) {
    const PathRec key = SplitPath(path);
    const uint64_t h = HashPathKey(HashStem(key.stem), key.name);
    const uint32_t hit = FindPathKey(key.stem, key.name, h);
    if (hit != UINT32_MAX) return hit;
    return AddPathRec(key.stem, key.name, h);
  }
  // Intern "<prim_path>.<name>" for stable views, given HashStem(prim_path).
  // Only valid when that concatenation splits back into exactly
  // (prim_path, name) -- see PropPathSplitsCleanly().
  uint32_t InternPropPath(std::string_view prim_path, uint64_t prim_hash,
                          std::string_view name) {
    const uint64_t h = HashPathKey(prim_hash, name);
    const uint32_t hit = FindPathKey(prim_path, name, h);
    if (hit != UINT32_MAX) return hit;
    return AddPathRec(prim_path, name, h);
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
    return FindPathKey(key.stem, key.name,
                       HashPathKey(HashStem(key.stem), key.name));
  }

  // pxr's compressed path tree requires EVERY ancestor of every path to be
  // present as a node. Connection/relationship TARGET paths can reference
  // prims that have no spec in this layer (e.g. material shader outputs in a
  // not-yet-composed referenced file), so synthesize the missing ancestor
  // chain (tree-only nodes; no spec refers to them), appended in scan order.
  // Ancestors are prefixes of existing stems, so they are stored as views.
  void SynthesizeAncestorPaths() {
    auto add = [&](std::string_view v) -> bool {
      const PathRec key = SplitPath(v);
      const uint64_t h = HashPathKey(HashStem(key.stem), key.name);
      if (FindPathKey(key.stem, key.name, h) != UINT32_MAX) return false;
      AddPathRec(key.stem, key.name, h);
      return true;
    };
    const size_t orig = paths_.size();
    // Paths of one prim arrive together; a stem already walked needs no
    // second walk (the walk would only re-probe existing ancestors).
    std::string_view walked_stem;
    bool have_walked = false;
    for (size_t i = 0; i < orig; i++) {
      const PathRec rec = paths_[i];
      if (have_walked && rec.stem == walked_stem) continue;
      std::string_view cur = rec.stem;
      if (!rec.name.empty()) {
        // property path: start from its prim part
        if (!cur.empty() && cur != "/") add(cur);
      }
      while (cur.size() > 1) {
        const size_t sl = cur.rfind('/');
        if (sl == std::string_view::npos || sl == 0) break;
        cur = cur.substr(0, sl);
        if (!add(cur)) break;  // rest of the chain already exists
      }
      walked_stem = rec.stem;
      have_walked = true;
    }
  }

  // Register every path-element token before the TOKENS section is written
  // (WritePathsSection only looks them up). Components are interned in the
  // same order as splitting each full path text on '/' and '.' (skipping its
  // first character); a stem equal to the previous record's needs no second
  // split, as its components are already interned.
  void RegisterPathElementTokens() {
    std::string comp;
    auto intern_components = [&](std::string_view s, size_t start) {
      while (start < s.size()) {
        size_t end = s.size();
        const size_t slash = s.find('/', start);
        const size_t dot = s.find('.', start);
        if (slash != std::string_view::npos) end = std::min(end, slash);
        if (dot != std::string_view::npos) end = std::min(end, dot);
        if (end > start) {
          comp.assign(s.data() + start, end - start);
          InternToken(comp);
        }
        start = end + 1;
      }
    };
    std::string_view prev_stem;
    bool have_prev = false;
    for (const PathRec& rec : paths_) {
      if (rec.name.empty() && (rec.stem.empty() || rec.stem == "/")) {
        InternToken("");  // Token 0 = empty string for root path marker
        continue;
      }
      if (!(have_prev && rec.stem == prev_stem)) {
        intern_components(rec.stem, 1);
      }
      // A property's name follows its stem's '.'; with an empty stem the
      // skipped first character is that '.' itself.
      if (!rec.name.empty()) intern_components(rec.name, 0);
      prev_stem = rec.stem;
      have_prev = true;
    }
  }

  void BuildTables(const Layer& layer) {
    // Token 0: magic marker expected by pxrUSD's crate reader
    InternToken(";-)");  // software identifier (pxrUSD convention)
    // Token 1: empty string, reserved for root path marker in PATHS
    InternToken("");

    // Common field name tokens
    InternToken("typeName");
    InternToken("specifier");
    InternToken("default");
    InternToken("active");
    InternToken("hidden");
    InternToken("doc");
    InternToken("documentation");
    InternToken("apiSchemas");
    InternToken("kind");
    InternToken("permission");
    InternToken("variability");
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

    // Prim names, type names, property names
    for (const auto& prim : layer.prims()) {
      InternToken(prim.name());
      InternToken(prim.type_name());

      PropNameTable& name_table = GetPropNameTable();
      for (const auto& slot : prim.properties().slots()) {
        InternToken(std::string(name_table.get(slot.name_id)));
      }

      // Metadata strings
      if (prim.meta().doc_authored() || !prim.meta().doc().empty()) {
        InternString(prim.meta().doc());
      }
      for (const auto& schema : prim.meta().apiSchemas()) {
        InternToken(schema);
      }
      {
        const StringListOpEdits& edits = prim.meta().apiSchemaEdits();
        const std::vector<std::string>* lists[] = {
            &edits.explicit_items, &edits.added, &edits.prepended,
            &edits.appended, &edits.deleted, &edits.ordered};
        for (const std::vector<std::string>* list : lists) {
          for (const std::string& item : *list) InternToken(item);
        }
      }
      {
        const StringListOpEdits& edits = prim.meta().clipSetEdits();
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
        const StringListOpEdits& edits = prim.meta().variantSetNameEdits();
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
