// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl fieldset/spec construction.
// Included inside CrateWriter::Impl private section.

  // One prim spec + its property specs. Operates on `this` Impl's tables, so a
  // worker Impl can build a disjoint prim range in parallel. `variant_names` is
  // a read-only prescan shared across workers; `path_to_fs_idx` is per-builder.
  void BuildOnePrim(
      const Layer& layer, size_t i,
      const std::unordered_map<std::string,
          std::map<std::string, std::vector<std::string>>>& variant_names,
      const std::unordered_map<std::string, std::vector<std::string>>& parent_children,
      std::unordered_map<std::string, uint32_t>& path_to_fs_idx) {
      const PrimSpec& prim = layer.prims()[i];
      // Variant-namespace prims ("/Prim/{vset=sel}/..."): emit them ONLY for an
      // unflattened layer (round-trip), detected by their owning prim still
      // carrying variant sets. After flattening, the compositor bakes + clears
      // variantSets, so these become leftover and are dropped.
      {
        const std::string& pp = prim.path().str();
        // Nearest bracketed component owns this prim (nested variants):
        // "/P/{a=x}/{b=y}/Geo" is owned by "/P/{a=x}", not "/P".
        size_t brace = pp.rfind('{');
        if (brace != std::string::npos) {
          size_t owner_end = pp.rfind('/', brace);
          if (owner_end == std::string::npos) return;
          const PrimSpec* owner = layer.prim_at_path(pp.substr(0, owner_end));
          if (!owner || owner->meta().variantSets().empty()) return;
        }
      }
      std::vector<uint32_t> fieldset;

      // A variant-namespace HOLDER ("/Prim/{vset=sel}" or "/Prim/{vset=}") is a
      // Variant/VariantSet spec, which (per pxrUSD) carries neither specifier
      // nor typeName — emitting them makes pxr warn. Variant CHILD prims
      // ("/Prim/{vset=sel}/Geo") have ordinary names and keep both.
      const std::string& prim_nm = prim.name();
      const bool is_variant_holder =
          prim_nm.size() >= 2 && prim_nm.back() == '}' &&
          prim_nm.find('{') != std::string::npos;

      // specifier (pxrUSD convention: specifier first, then typeName)
      if (!is_variant_holder) {
        CrateField f;
        f.token_index.value = InternToken("specifier");
        uint64_t spec_val = static_cast<uint64_t>(prim.specifier());
        f.value_rep = ValueRep::Make(CrateTypeId::Specifier, spec_val, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // typeName
      if (!is_variant_holder && !prim.type_name().empty()) {
        CrateField f;
        f.token_index.value = InternToken("typeName");
        f.value_rep = ValueRep::Make(CrateTypeId::Token,
                                     InternToken(prim.type_name()),
                                     false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // variantChildren on a VariantSet spec ("/Prim/{vset=}"): the names of the
      // variants in that set, so pxrUSD can enumerate them.
      if (is_variant_holder && prim_nm.size() >= 3 &&
          prim_nm[prim_nm.size() - 2] == '=') {  // "{vset=}"
        const std::string& pth = prim.path().str();
        // Owner = path before the LAST bracketed component (nested variants).
        size_t lb = pth.rfind("/{");
        if (lb == std::string::npos && prim_nm.find('{') != std::string::npos) {
          lb = pth.rfind('/');
        }
        std::string owning = (lb == std::string::npos) ? std::string()
                                                       : pth.substr(0, lb);
        size_t brace = prim_nm.find('{');
        std::string vset = prim_nm.substr(brace + 1, prim_nm.size() - brace - 3);
        auto oit = variant_names.find(owning);
        if (oit != variant_names.end()) {
          auto vit = oit->second.find(vset);
          if (vit != oit->second.end() && !vit->second.empty()) {
            fieldset.push_back(static_cast<uint32_t>(fields_.size()));
            fields_.push_back(MakeTokenVectorField("variantChildren", vit->second));
          }
        }
      }

      // active (authored true also round-trips via active_authored)
      if (!prim.meta().active || prim.meta().active_authored) {
        CrateField f;
        f.token_index.value = InternToken("active");
        f.value_rep = ValueRep::Make(CrateTypeId::Bool,
                                     prim.meta().active ? 1 : 0, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // hidden (authored false also round-trips via hidden_authored)
      if (prim.meta().hidden || prim.meta().hidden_authored) {
        CrateField f;
        f.token_index.value = InternToken("hidden");
        f.value_rep = ValueRep::Make(CrateTypeId::Bool,
                                     prim.meta().hidden ? 1 : 0, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // doc
      if (prim.meta().doc_authored() || !prim.meta().doc().empty()) {
        CrateField f;
        f.token_index.value = InternToken("documentation");
        uint32_t str_idx = InternString(prim.meta().doc());
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // apiSchemas — a TokenListOp with one run:
      //   [u8 ListOpHeader][u64 count][u32 token_idx]*count
      // The header encodes the authored qualifier. A BARE `apiSchemas = [...]`
      // is an EXPLICIT list (0x03 = IsExplicit|HasExplicitItems), NOT a
      // prepend: pxr round-trips a bare authoring as explicit, and writing it
      // as prepend flipped the qualifier on every USDC round trip.
      if (prim.meta().apiSchemasAuthored() ||
          !prim.meta().apiSchemas().empty()) {
        if (prim.meta().apiSchemaEdits().authored) {
          fieldset.push_back(static_cast<uint32_t>(fields_.size()));
          fields_.push_back(
              MakeTokenListOpField("apiSchemas", prim.meta().apiSchemaEdits()));
        } else {
        size_t count = prim.meta().apiSchemas().size();
        size_t items_bytes = 0;
        size_t api_data_size = 0;
        if (!safe::mul(count, size_t(4), &items_bytes) ||
            !safe::add(size_t(9), items_bytes, &api_data_size)) {
          result_.error = "apiSchemas payload size overflow";
          return;
        }
        std::vector<uint8_t> api_data(api_data_size);
        // Header bit from the authored qualifier; a bare list is explicit.
        api_data[0] = prim.meta().apiSchemasQualifier() == "prepend"
                          ? 0x20   // kHasPrepended
                      : prim.meta().apiSchemasQualifier() == "append"
                          ? 0x40   // kHasAppended
                      : prim.meta().apiSchemasQualifier() == "delete"
                          ? 0x08   // kHasDeleted (rewriting as prepend
                                   // would INVERT the opinion)
                          : 0x03;  // kIsExplicit | kHasExplicitItems
        uint64_t cnt = count;
        std::memcpy(api_data.data() + 1, &cnt, 8);
        for (size_t i = 0; i < count; ++i) {
          uint32_t tok_idx = InternToken(prim.meta().apiSchemas()[i]);
          std::memcpy(api_data.data() + 1 + 8 + i * 4, &tok_idx, 4);
        }
        uint64_t data_idx = InternBlock({TypeId::Token, std::move(api_data)});

        CrateField f;
        f.token_index.value = InternToken("apiSchemas");
        f.value_rep = ValueRep::Make(CrateTypeId::TokenListOp, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
        }
      }

      // relocates (SdfRelocates, crate >= 0.11):
      //   [u64 count][(u32 srcPathIdx, u32 dstPathIdx)*]
      if (prim.meta().relocatesAuthored() || !prim.meta().relocates().empty()) {
        const auto& rel = prim.meta().relocates();
        std::vector<uint8_t> blob(8 + rel.size() * 8);
        uint64_t cnt = rel.size();
        std::memcpy(blob.data(), &cnt, 8);
        for (size_t i = 0; i < rel.size(); ++i) {
          uint32_t src = InternPath(rel[i].first);
          uint32_t dst = InternPath(rel[i].second);
          std::memcpy(blob.data() + 8 + i * 8, &src, 4);
          std::memcpy(blob.data() + 8 + i * 8 + 4, &dst, 4);
        }
        uint64_t idx = InternBlock({TypeId::Invalid, std::move(blob)});
        CrateField f;
        f.token_index.value = InternToken("relocates");
        f.value_rep =
            ValueRep::Make(CrateTypeId::Relocates, idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
        if (required_version_minor_ < 11) required_version_minor_ = 11;
      }

      // Population-order opinions are distinct from the persistent natural
      // child/property lists below. Preserve the sparse authored fields so a
      // Crate round trip retains traversal semantics.
      if (prim.meta().primOrderAuthored() ||
          !prim.meta().primOrder().empty()) {
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(
            MakeTokenVectorField("primOrder", prim.meta().primOrder()));
      }
      if (prim.meta().propertyOrderAuthored() ||
          !prim.meta().propertyOrder().empty()) {
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(MakeTokenVectorField(
            "propertyOrder", prim.meta().propertyOrder()));
      }
      if (prim.meta().displayGroupOrderAuthored() ||
          !prim.meta().displayGroupOrder().empty()) {
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(MakeStringVectorField(
            "displayGroupOrder", prim.meta().displayGroupOrder()));
      }

      {
        auto cit = parent_children.find(prim.path().str());
        if (cit != parent_children.end() && !cit->second.empty()) {
          fieldset.push_back(static_cast<uint32_t>(fields_.size()));
          fields_.push_back(MakeTokenVectorField("primChildren", cit->second));
        }
      }

      // Properties, relationships, connections and time samples are emitted as
      // separate pxrUSD-style Attribute/Relationship specs (below). Collect the
      // child property names here for the prim's "properties" field.
      std::vector<std::string> prop_names = EmitPrimProperties(prim);
      if (!prim.meta().propertyOrder().empty()) {
        std::vector<std::string> ordered;
        ordered.reserve(prop_names.size());
        for (const std::string& wanted : prim.meta().propertyOrder()) {
          if (std::find(prop_names.begin(), prop_names.end(), wanted) !=
                  prop_names.end() &&
              std::find(ordered.begin(), ordered.end(), wanted) ==
                  ordered.end()) {
            ordered.push_back(wanted);
          }
        }
        for (const std::string& name : prop_names) {
          if (std::find(ordered.begin(), ordered.end(), name) == ordered.end())
            ordered.push_back(name);
        }
        prop_names = std::move(ordered);
      }

      // Composition arcs in pxr-native encodings (pxr rejects the whole layer
      // when a known field like `references` holds a token[] instead of its
      // schema type). The effective (composed) list is written explicitly;
      // authored list-op sublists additionally round-trip via the next-private
      // `<arc>_listOp` companion fields below.
      const ArcListOpEdits* arc_edits = prim.meta().arc_edits();
      auto add_ref_listop_field = [&](const char* name,
                                      const std::vector<std::string>& arcs,
                                      bool is_payload, const ArcEdit* e) {
        if (arcs.empty() && !(e && e->authored)) return;
        uint64_t blk = StoreReferenceListOp(arcs, is_payload, e);
        CrateField f;
        f.token_index.value = InternToken(name);
        f.value_rep = ValueRep::Make(is_payload ? CrateTypeId::PayloadListOp
                                                : CrateTypeId::ReferenceListOp,
                                     blk, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      };
      auto add_path_listop_field = [&](const char* name,
                                       const std::vector<std::string>& arcs,
                                       const ArcEdit* e) {
        if (arcs.empty() && !(e && e->authored)) return;
        uint64_t blk = StorePathArcListOp(arcs, e);
        CrateField f;
        f.token_index.value = InternToken(name);
        f.value_rep =
            ValueRep::Make(CrateTypeId::PathListOp, blk, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      };
      add_ref_listop_field("references", prim.meta().references, false,
                           arc_edits ? &arc_edits->references : nullptr);
      add_ref_listop_field("payload", prim.meta().payloads, true,
                           arc_edits ? &arc_edits->payloads : nullptr);
      // pxr's field name for inherits is "inheritPaths".
      add_path_listop_field("inheritPaths", prim.meta().inherits,
                            arc_edits ? &arc_edits->inherits : nullptr);
      add_path_listop_field("specializes", prim.meta().specializes,
                            arc_edits ? &arc_edits->specializes : nullptr);

      // Authored list-op sublists round-trip natively inside the pxr listop
      // encodings above (no next-private companion fields).
      // comment must be a String rep: pxr's SdfSchema types `comment` as
      // string and silently drops a Token-typed field on read.
      if (prim.meta().comment_authored() || !prim.meta().comment().empty()) {
        CrateField f;
        f.token_index.value = InternToken("comment");
        uint32_t str_idx = InternString(prim.meta().comment());
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // kind (token), displayName (string), instanceable (bool).
      if (prim.meta().kindAuthored() || !prim.meta().kind().empty()) {
        CrateField f;
        f.token_index.value = InternToken("kind");
        f.value_rep = ValueRep::Make(CrateTypeId::Token,
                                     InternToken(prim.meta().kind()), false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      if (!prim.meta().permission().empty()) {
        CrateField f;
        f.token_index.value = InternToken("permission");
        f.value_rep = ValueRep::Make(
            CrateTypeId::Token, InternToken(prim.meta().permission()), false,
            true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      // displayName is string-typed in pxr's schema (Token would be dropped).
      if (prim.meta().displayNameAuthored() ||
          !prim.meta().displayName().empty()) {
        CrateField f;
        f.token_index.value = InternToken("displayName");
        uint32_t str_idx = InternString(prim.meta().displayName());
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      // instanceable=false is a real (blocking) opinion when authored.
      if (prim.meta().instanceable || prim.meta().instanceable_authored) {
        CrateField f;
        f.token_index.value = InternToken("instanceable");
        f.value_rep = ValueRep::Make(CrateTypeId::Bool,
                                     prim.meta().instanceable ? 1 : 0, false,
                                     true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // Dictionary-valued metadata: pxr binary VtDictionary (round-trips through
      // both this reader and OpenUSD).
      auto add_dict_field = [&](const char* name, const Value& v,
                                bool authored) {
        if (!v.is_dictionary() || !v.as_dictionary() ||
            (!authored && v.as_dictionary()->empty())) {
          return;
        }
        CrateField f;
        f.token_index.value = InternToken(name);
        f.value_rep = EncodeValue(v);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      };
      add_dict_field("customData", prim.meta().customData(),
                     prim.meta().customDataAuthored());
      add_dict_field("assetInfo", prim.meta().assetInfo(),
                     prim.meta().assetInfoAuthored());
      add_dict_field("sdrMetadata", prim.meta().sdrMetadata(),
                     prim.meta().sdrMetadataAuthored());
      add_dict_field("clips", prim.meta().clips(),
                     prim.meta().clipsAuthored());
      if (prim.meta().clipSetEdits().authored) {
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(
            MakeTokenListOpField("clipSets", prim.meta().clipSetEdits()));
      }
      // Unknown (unmodeled) prim metadata the parser preserved verbatim
      // (`aa = a`, `autoPlay = false`, custom pipeline keys, ...). Serialize the
      // (key, raw-value-text) pairs into a single length-prefixed string field
      // so they survive USDC; a length prefix keeps keys/values that contain
      // ':' / newlines / quotes exact. Stored under a lightusd-private field
      // name so pxr treats it as (harmless) unregistered metadata rather than
      // misreading a phantom typed field.
      if (!prim.meta().unknownMeta().empty()) {
        std::string blob;
        for (const auto& um : prim.meta().unknownMeta()) {
          blob += std::to_string(um.first.size()) + ":" + um.first;
          blob += std::to_string(um.second.size()) + ":" + um.second;
        }
        CrateField f;
        f.token_index.value = InternToken("__lightusd_unknownMeta");
        uint32_t str_idx = InternString(blob);
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      for (const auto& extension : prim.meta().unknownFields()) {
        CrateField f;
        f.token_index.value = InternToken(extension.name);
        f.value_rep = EncodeExtensionField(extension);
        if (f.value_rep.type_id() == CrateTypeId::Invalid) {
          if (options_.strict_aousd_conformance && result_.error.empty()) {
            result_.error = "Cannot encode extension prim field '" +
                            extension.name + "'";
          }
          continue;
        }
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      // variantSelection: a VariantSelectionMap of each set's selected variant.
      // The reader rebuilds the VariantSetData from this plus the bracketed
      // holder prims, so no separate variantSets token field is emitted. (The
      // bracketed holder/child prims carry the variant CONTENT and are emitted
      // above for unflattened layers.)
      // NOTE: not gated on variantSets(): a prim may only SELECT a variant
      // defined by a referenced asset ("variants = {...}" next to a
      // reference), in which case it has selections but no local sets.
      {
        // Selections can live in three places depending on how the layer was
        // produced: VariantSetData::selected (crate reader / authoring API,
        // the authoritative per-set form), the plural variantSelections()
        // list (USDA parser), or the legacy single "set=sel" string. Merge
        // all three, per-set `selected` first (matches RecordSelections).
        std::vector<std::pair<std::string, std::string>> sels;
        auto add_sel = [&sels](const std::string& set, const std::string& v) {
          if (set.empty()) return;
          for (auto& kv : sels) {
            if (kv.first == set) return;  // first writer wins
          }
          sels.emplace_back(set, v);
        };
        for (const auto& vs : prim.meta().variantSets()) {
          if (!vs.selected.empty()) add_sel(vs.name, vs.selected);
        }
        for (const auto& kv : prim.meta().variantSelections()) {
          add_sel(kv.first, kv.second);
        }
        if (!prim.meta().variantSelection.empty()) {
          size_t eq = prim.meta().variantSelection.find('=');
          if (eq != std::string::npos) {
            add_sel(prim.meta().variantSelection.substr(0, eq),
                    prim.meta().variantSelection.substr(eq + 1));
          }
        }
        if (!sels.empty() || prim.meta().variantSelectionsAuthored()) {
          uint64_t blk = StoreVariantSelectionMap(sels);
          CrateField vf;
          vf.token_index.value = InternToken("variantSelection");
          vf.value_rep =
              ValueRep::Make(CrateTypeId::VariantSelectionMap, blk, false, false);
          fieldset.push_back(static_cast<uint32_t>(fields_.size()));
          fields_.push_back(vf);
        }
        // variantSetChildren: sets that have CONTENT (holder prims present in
        // the layer) — pxr enumerates the child specs from it.
        // Same gate as the holder specs above: after flattening, the
        // compositor clears the owner's variant sets and its leftover holder
        // specs are not emitted, so neither are their set/child lists.
        auto oit = prim.meta().variantSets().empty() ? variant_names.end()
                                                     : variant_names.find(prim.path().str());
        std::vector<std::string> content_sets;
        if (oit != variant_names.end()) {
          content_sets.reserve(oit->second.size());
          for (const auto& kv : oit->second) content_sets.push_back(kv.first);
        }
        if (!content_sets.empty()) {
          fieldset.push_back(static_cast<uint32_t>(fields_.size()));
          fields_.push_back(
              MakeTokenVectorField("variantSetChildren", content_sets));
        }
        // variantSetNames (StringListOp): the AUTHORED `variantSets`
        // declaration — the union of content sets and sets declared without
        // local content (meta().variantSets() carries the parsed
        // `prepend variantSets` names). Emitting it for declaration-only prims
        // keeps `prepend variantSets` through a crate round trip AND is what
        // pxr's Pcp needs to create the variant arcs. A prim that only SELECTS
        // a variant (`variants = {...}` with no declaration) has an empty
        // variantSets(), so nothing is emitted here — the reader then treats it
        // as a dangling selection rather than synthesizing a phantom set.
        std::vector<std::string> decl_sets = content_sets;
        for (const auto& vs : prim.meta().variantSets()) {
          if (std::find(decl_sets.begin(), decl_sets.end(), vs.name) ==
              decl_sets.end()) {
            decl_sets.push_back(vs.name);
          }
        }
        const StringListOpEdits& authored_variant_sets =
            prim.meta().variantSetNameEdits();
        if (authored_variant_sets.authored) {
          StringListOpEdits edits = authored_variant_sets;
          auto contains = [&](const std::string& name) {
            const std::vector<std::string>* lists[] = {
                &edits.explicit_items, &edits.added, &edits.prepended,
                &edits.appended, &edits.deleted, &edits.ordered};
            for (const std::vector<std::string>* list : lists) {
              if (std::find(list->begin(), list->end(), name) != list->end())
                return true;
            }
            return false;
          };
          // Variant holder content implies declaration. Keep an exact raw
          // list-op when it already names every content set; otherwise add the
          // missing holder names without rewriting the authored sublists.
          for (const std::string& name : content_sets) {
            if (contains(name)) continue;
            if (edits.is_explicit) edits.explicit_items.push_back(name);
            else edits.prepended.push_back(name);
          }
          fieldset.push_back(static_cast<uint32_t>(fields_.size()));
          fields_.push_back(
              MakeStringListOpField("variantSetNames", edits));
        } else if (!decl_sets.empty()) {
          fieldset.push_back(static_cast<uint32_t>(fields_.size()));
          fields_.push_back(MakeStringListOpField("variantSetNames", decl_sets));
        }
      }

      // "properties" field: TokenVector listing this prim's child property and
      // relationship names (pxrUSD enumerates properties from it). The next
      // reader rebuilds properties from the separate specs, so it ignores this.
      if (!prop_names.empty()) {
        size_t n = prop_names.size();
        std::vector<uint8_t> raw(8 + n * 4);
        uint64_t cnt = n;
        std::memcpy(raw.data(), &cnt, 8);
        for (size_t i = 0; i < n; ++i) {
          uint32_t tok = InternToken(prop_names[i]);
          std::memcpy(raw.data() + 8 + i * 4, &tok, 4);
        }
        uint64_t data_idx = InternBlock({TypeId::Invalid, std::move(raw)});
        CrateField pf;
        pf.token_index.value = InternToken("properties");
        pf.value_rep =
            ValueRep::Make(CrateTypeId::TokenVector, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(pf);
      }

      // Record fieldset
      uint32_t fs_idx = static_cast<uint32_t>(fieldsets_.size());
      fieldsets_.push_back(std::move(fieldset));
      path_to_fs_idx[prim.path().str()] = fs_idx;

      // Create spec. A prim whose LAST path element is a variant selector
      // ("{vset=sel}") is a Variant spec; "{vset=}" is the VariantSet
      // declaration. (Variant CHILD prims like "/Prim/{vset=sel}/Geo" keep a
      // normal name -> Prim.) Using the right spec type keeps the unflattened
      // output readable by pxrUSD (which rejects a Prim named "{...}").
      CrateSpec spec;
      spec.path_index.value = InternPath(prim.path().str());
      spec.fieldset_index.value = fs_idx;
      spec.spec_type = SpecType::Prim;
      {
        const std::string& nm = prim.name();
        size_t brace = nm.find('{');
        if (nm.size() >= 2 && brace != std::string::npos && nm.back() == '}') {
          spec.spec_type = (nm.find('=', brace + 1) == nm.size() - 2)
                               ? SpecType::VariantSet   // "{vset=}"
                               : SpecType::Variant;     // "{vset=sel}"
        }
      }
      specs_.push_back(spec);
  }

  void BuildFieldsAndSpecs(const Layer& layer) {
    // 1. PseudoRoot spec (index 0)
    {
      std::vector<uint32_t> fieldset;

      // defaultPrim
      if (layer.meta().defaultPrim_set || !layer.meta().defaultPrim.empty()) {
        CrateField f;
        f.token_index.value = InternToken("defaultPrim");
        f.value_rep = ValueRep::Make(CrateTypeId::Token,
                                     InternToken(layer.meta().defaultPrim),
                                     false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // Stage metadata below is gated on the same "differs from fallback"
      // heuristics the usda writer uses, so an unauthored default does not
      // round-trip into a spuriously authored opinion (authored-at-default
      // values are collapsed, matching the usda writer).
      // upAxis
      if (layer.meta().upAxis_set || layer.meta().upAxis != "Y") {
        CrateField f;
        f.token_index.value = InternToken("upAxis");
        f.value_rep = ValueRep::Make(CrateTypeId::Token,
                                     InternToken(layer.meta().upAxis),
                                     false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // metersPerUnit
      if (layer.meta().metersPerUnit_set || layer.meta().metersPerUnit != 0.01) {
        CrateField f;
        f.token_index.value = InternToken("metersPerUnit");
        double v = layer.meta().metersPerUnit;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // timeCodesPerSecond
      if (layer.meta().timeCodesPerSecond_set || layer.meta().timeCodesPerSecond != 24.0) {
        CrateField f;
        f.token_index.value = InternToken("timeCodesPerSecond");
        double v = layer.meta().timeCodesPerSecond;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // startTimeCode
      if (layer.meta().startTimeCode_set ||
          layer.meta().startTimeCode != 0.0 ||
          layer.meta().endTimeCode != 0.0) {
        CrateField f;
        f.token_index.value = InternToken("startTimeCode");
        double v = layer.meta().startTimeCode;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        // Store as Double (pxrUSD/legacy reader reject an uninlined TimeCode).
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // endTimeCode
      if (layer.meta().endTimeCode_set ||
          layer.meta().startTimeCode != 0.0 ||
          layer.meta().endTimeCode != 0.0) {
        CrateField f;
        f.token_index.value = InternToken("endTimeCode");
        double v = layer.meta().endTimeCode;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        // Store as Double (pxrUSD/legacy reader reject an uninlined TimeCode).
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // framesPerSecond / kilogramsPerUnit (only when authored).
      if (layer.meta().framesPerSecond_set) {
        CrateField f;
        f.token_index.value = InternToken("framesPerSecond");
        double v = layer.meta().framesPerSecond;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      if (layer.meta().kilogramsPerUnit_set) {
        CrateField f;
        f.token_index.value = InternToken("kilogramsPerUnit");
        double v = layer.meta().kilogramsPerUnit;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // Dictionary-valued stage metadata (pxr binary VtDictionary).
      auto add_layer_dict = [&](const char* name, const Value& v,
                                bool authored) {
        if (!v.is_dictionary() || !v.as_dictionary() ||
            (!authored && v.as_dictionary()->empty())) {
          return;
        }
        CrateField f;
        f.token_index.value = InternToken(name);
        f.value_rep = EncodeValue(v);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      };
      add_layer_dict("customLayerData", layer.meta().customLayerData,
                     layer.meta().customLayerData_set);
      add_layer_dict("expressionVariables", layer.meta().expressionVariables,
                     layer.meta().expressionVariables_set);

      // doc
      if (layer.meta().doc_set || !layer.meta().doc.empty()) {
        CrateField f;
        f.token_index.value = InternToken("documentation");
        uint32_t str_idx = InternString(layer.meta().doc);
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // comment
      if (layer.meta().comment_set || !layer.meta().comment.empty()) {
        CrateField f;
        f.token_index.value = InternToken("comment");
        uint32_t str_idx = InternString(layer.meta().comment);
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      if (layer.meta().owner_set || !layer.meta().owner.empty()) {
        CrateField f;
        f.token_index.value = InternToken("owner");
        uint32_t str_idx = InternString(layer.meta().owner);
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false,
                                     true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // USDZ playback metadata is a typed boolean field on the pseudo-root.
      if (layer.meta().autoPlay_set) {
        CrateField f;
        f.token_index.value = InternToken("autoPlay");
        f.value_rep = EncodeValue(Value(layer.meta().autoPlay));
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      if (layer.meta().playbackMode_set) {
        CrateField f;
        f.token_index.value = InternToken("playbackMode");
        f.value_rep = ValueRep::Make(
            CrateTypeId::Token, InternToken(layer.meta().playbackMode), false,
            true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // Unknown (unmodeled) LAYER metadata preserved verbatim by the parser
      // (`autoPlay = false`, custom stage keys). Same length-prefixed encoding
      // as the prim-level field (see EmitPrimSpec).
      if (!layer.meta().unknownMeta.empty()) {
        std::string blob;
        for (const auto& um : layer.meta().unknownMeta) {
          blob += std::to_string(um.first.size()) + ":" + um.first;
          blob += std::to_string(um.second.size()) + ":" + um.second;
        }
        CrateField f;
        f.token_index.value = InternToken("__lightusd_unknownMeta");
        uint32_t str_idx = InternString(blob);
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      for (const auto& extension : layer.meta().unknownFields) {
        CrateField f;
        f.token_index.value = InternToken(extension.name);
        f.value_rep = EncodeExtensionField(extension);
        if (f.value_rep.type_id() == CrateTypeId::Invalid) {
          if (options_.strict_aousd_conformance && result_.error.empty()) {
            result_.error = "Cannot encode extension layer field '" +
                            extension.name + "'";
          }
          continue;
        }
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // layerRelocates (SdfRelocates, crate >= 0.11), same wire layout as
      // the prim-level field: [u64 count][(u32 src, u32 dst)*].
      if (layer.meta().relocates_set || !layer.meta().relocates.empty()) {
        const auto& rel = layer.meta().relocates;
        std::vector<uint8_t> blob(8 + rel.size() * 8);
        uint64_t cnt = rel.size();
        std::memcpy(blob.data(), &cnt, 8);
        for (size_t i = 0; i < rel.size(); ++i) {
          uint32_t src = InternPath(rel[i].first);
          uint32_t dst = InternPath(rel[i].second);
          std::memcpy(blob.data() + 8 + i * 8, &src, 4);
          std::memcpy(blob.data() + 8 + i * 8 + 4, &dst, 4);
        }
        uint64_t bidx = InternBlock({TypeId::Invalid, std::move(blob)});
        CrateField f;
        f.token_index.value = InternToken("layerRelocates");
        f.value_rep =
            ValueRep::Make(CrateTypeId::Relocates, bidx, false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
        if (required_version_minor_ < 11) required_version_minor_ = 11;
      }

      if (layer.meta().rootPrimOrder_set ||
          !layer.meta().rootPrimOrder.empty()) {
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(
            MakeTokenVectorField("primOrder", layer.meta().rootPrimOrder));
      }

      // Color management (pxr schema: colorConfiguration is asset,
      // colorManagementSystem is token). Previously dropped by the crate
      // writer while the USDA writer kept them.
      if (layer.meta().colorConfiguration_set ||
          !layer.meta().colorConfiguration.empty()) {
        CrateField f;
        f.token_index.value = InternToken("colorConfiguration");
        f.value_rep = ValueRep::Make(
            CrateTypeId::AssetPath,
            InternToken(layer.meta().colorConfiguration), false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      if (layer.meta().colorManagementSystem_set ||
          !layer.meta().colorManagementSystem.empty()) {
        CrateField f;
        f.token_index.value = InternToken("colorManagementSystem");
        f.value_rep = ValueRep::Make(
            CrateTypeId::Token,
            InternToken(layer.meta().colorManagementSystem), false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }
      if (layer.meta().renderSettingsPrimPath_set ||
          !layer.meta().renderSettingsPrimPath.empty()) {
        CrateField f;
        f.token_index.value = InternToken("renderSettingsPrimPath");
        f.value_rep = ValueRep::Make(
            CrateTypeId::String,
            InternString(layer.meta().renderSettingsPrimPath), false, true);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
      }

      // subLayers (StringVector — pxr's schema type; a token[] here reads back
      // in pxr as an empty sublayer list)
      if (layer.meta().subLayers_set || !layer.meta().subLayers.empty()) {
        CrateField f = MakeStringVectorField("subLayers", layer.meta().subLayers);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(f);
        // subLayerOffsets must accompany subLayers (one offset per entry) —
        // pxr errors with "Invalid sublayer index" without it.
        // Layout: [u64 count][f64 offset, f64 scale] per entry.
        const size_t n = layer.meta().subLayers.size();
        std::vector<uint8_t> raw(8 + n * 16);
        uint64_t cnt = n;
        std::memcpy(raw.data(), &cnt, 8);
        for (size_t i = 0; i < n; ++i) {
          const bool have = i < layer.meta().subLayerOffsets.size();
          const double offset = have ? layer.meta().subLayerOffsets[i].first : 0.0;
          const double scale = have ? layer.meta().subLayerOffsets[i].second : 1.0;
          std::memcpy(raw.data() + 8 + i * 16, &offset, 8);
          std::memcpy(raw.data() + 8 + i * 16 + 8, &scale, 8);
        }
        uint64_t data_idx = InternBlock({TypeId::Invalid, std::move(raw)});
        CrateField fo;
        fo.token_index.value = InternToken("subLayerOffsets");
        fo.value_rep = ValueRep::Make(CrateTypeId::LayerOffsetVector, data_idx,
                                      false, false);
        fieldset.push_back(static_cast<uint32_t>(fields_.size()));
        fields_.push_back(fo);
      }

      {
        std::vector<std::string> root_children;
        if (!layer.root_indices().empty()) {
          root_children.reserve(layer.root_indices().size());
          for (uint32_t ri : layer.root_indices()) {
            if (const PrimSpec* rp = layer.prim(ri)) {
              root_children.push_back(rp->name());
            }
          }
        } else {
          for (const auto& prim : layer.prims()) {
            const std::string& path_str = prim.path().str();
            if (!path_str.empty() && path_str != "/" &&
                path_str.find('/', 1) == std::string::npos) {
              root_children.push_back(prim.name());
            }
          }
        }
        if (!root_children.empty()) {
          fieldset.push_back(static_cast<uint32_t>(fields_.size()));
          fields_.push_back(MakeTokenVectorField("primChildren", root_children));
        }
      }

      uint32_t fs_idx = static_cast<uint32_t>(fieldsets_.size());
      fieldsets_.push_back(std::move(fieldset));

      CrateSpec spec;
      spec.path_index.value = InternPath("/");
      spec.fieldset_index.value = fs_idx;
      spec.spec_type = SpecType::PseudoRoot;
      specs_.push_back(spec);
    }

    // Pre-scan variant holder prims ("/owning/{vset=var}") to recover, per
    // owning prim, each variant set's variant names — needed to emit
    // variantSetChildren (on the owning prim) and variantChildren (on each
    // VariantSet spec) so pxrUSD can enumerate the variants of an unflattened
    // layer (lightusd reconstructs them from the paths, but pxr needs these).
    std::unordered_map<std::string,
        std::map<std::string, std::vector<std::string>>> variant_names;
    for (const auto& p : layer.prims()) {
      const std::string& nm = p.name();
      size_t brace = nm.find('{');
      if (nm.size() < 3 || brace == std::string::npos || nm.back() != '}') continue;
      size_t eq = nm.find('=', brace + 1);
      if (eq == std::string::npos) continue;
      std::string var = nm.substr(eq + 1, nm.size() - eq - 2);  // between = and }
      if (var.empty()) continue;  // "{vset=}" is the VariantSet decl, not a variant
      const std::string& path = p.path().str();
      // The holder's own name is the LAST component; its owner is everything
      // before it. rfind so nested holders ("/P/{a=x}/{b=y}") attribute to
      // the inner owner ("/P/{a=x}"), not the outermost prim.
      size_t lb = path.rfind("/{");
      if (lb == std::string::npos) lb = path.rfind('/');
      if (lb == std::string::npos) continue;
      variant_names[path.substr(0, lb)][nm.substr(brace + 1, eq - brace - 1)].push_back(var);
    }

    std::unordered_map<std::string, std::vector<std::string>> parent_children;
    for (const auto& prim : layer.prims()) {
      const std::string& p = prim.path().str();
      if (p.empty() || p == "/") continue;
      const std::string& nm = prim.name();
      if (nm.size() >= 2 && nm.back() == '}' &&
          nm.find('{') != std::string::npos) continue;
      size_t slash = p.rfind('/');
      if (slash == std::string::npos) continue;
      std::string parent = (slash == 0) ? "/" : p.substr(0, slash);
      parent_children[parent].push_back(prim.name());
    }
    for (const auto& prim : layer.prims()) {
      if (prim.child_indices().empty()) continue;
      auto pit = parent_children.find(prim.path().str());
      if (pit == parent_children.end()) continue;
      std::vector<std::string> linked;
      linked.reserve(prim.child_indices().size());
      bool all_direct_children = true;
      for (uint32_t ci : prim.child_indices()) {
        const PrimSpec* c = layer.prim(ci);
        if (!c) {
          all_direct_children = false;
          continue;
        }
        const std::string& child_path = c->path().str();
        const size_t slash = child_path.rfind('/');
        const std::string parent = (slash == 0) ? "/" : child_path.substr(0, slash);
        if (parent != prim.path().str()) {
          all_direct_children = false;
          continue;
        }
        const std::string& cn = c->name();
        if (cn.size() >= 2 && cn.back() == '}' &&
            cn.find('{') != std::string::npos) continue;
        linked.push_back(cn);
      }
      if (all_direct_children && linked.size() == pit->second.size()) {
        pit->second = std::move(linked);
      }
    }

    // 2. Prim specs
    std::unordered_map<std::string, uint32_t> path_to_fs_idx;
    for (size_t i = 0; i < layer.prims().size(); ++i) {
      BuildOnePrim(layer, i, variant_names, parent_children, path_to_fs_idx);
      if (!result_.error.empty()) return;
    }

  }
