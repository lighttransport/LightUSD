// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl token/string/path table helpers.
// Included inside CrateWriter::Impl private section.

  // ============================================================
  // Table building
  // ============================================================

  uint32_t InternToken(const std::string& s) {
    if (frozen_tok_) {  // parallel worker: read-only lookup in the shared table
      auto it = frozen_tok_->find(s);
      if (it != frozen_tok_->end()) return it->second;
      frozen_miss_ = true;  // pre-pass incomplete -> driver falls back to serial
      return 0;
    }
    auto it = token_to_index_.find(s);
    if (it != token_to_index_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(tokens_.size());
    tokens_.push_back(s);
    token_to_index_[s] = idx;
    return idx;
  }

  uint32_t InternString(const std::string& s) {
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
    string_to_index_[s] = idx;
    return idx;
  }

  uint32_t InternPath(const std::string& path) {
    auto it = path_to_index_.find(path);
    if (it != path_to_index_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(paths_.size());
    paths_.push_back(path);
    path_to_index_[path] = idx;
    return idx;
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
