// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - CrateWriter::Impl fieldset/spec construction.
// Included inside CrateWriter::Impl private section.

  // One prim spec + its property specs. Operates on `this` Impl's tables, so a
  // worker Impl can build a disjoint prim range in parallel. `variant_names` is
  // a read-only prescan shared across workers.
  void BuildOnePrim(
      const Layer& layer, size_t i,
      const std::unordered_map<std::string,
          std::map<std::string, std::vector<std::string>>>& variant_names,
      const std::unordered_map<std::string, std::vector<std::string>>&
          parent_children) {
      // Batched write: an ancestor an earlier batch already wrote.
      if (skip_prims_ && (*skip_prims_)[i]) return;
      const PrimSpec& prim = layer.prims()[i];
      // The next prim's path probe (InternPathStable below) is a random load
      // into the path table; start it now.
      if (i + 1 < layer.prims().size()) {
        const PathRec nk = SplitPath(layer.prims()[i + 1].path().str());
        PrefetchPathSlot(HashPathKey(HashStem(nk.stem()), nk.name()));
      }
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
        f.token_index.value = specifier_token_;
        uint64_t spec_val = static_cast<uint64_t>(prim.specifier());
        f.value_rep = ValueRep::Make(CrateTypeId::Specifier, spec_val, false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // typeName
      if (!is_variant_holder && !prim.type_name().empty()) {
        CrateField f;
        f.token_index.value = type_name_token_;
        f.value_rep = ValueRep::Make(CrateTypeId::Token,
                                     InternToken(prim.type_name()),
                                     false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
            fieldset.push_back(WFieldCount());
            WPushField(MakeTokenVectorField("variantChildren", vit->second));
          }
        }
      }

      // active (authored true also round-trips via active_authored)
      if (!prim.meta().active || prim.meta().active_authored) {
        CrateField f;
        f.token_index.value = FieldToken(kTokActive);
        f.value_rep = ValueRep::Make(CrateTypeId::Bool,
                                     prim.meta().active ? 1 : 0, false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // hidden (authored false also round-trips via hidden_authored)
      if (prim.meta().hidden || prim.meta().hidden_authored) {
        CrateField f;
        f.token_index.value = FieldToken(kTokHidden);
        f.value_rep = ValueRep::Make(CrateTypeId::Bool,
                                     prim.meta().hidden ? 1 : 0, false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // doc
      if (prim.meta().doc_authored() || !prim.meta().doc().empty()) {
        CrateField f;
        f.token_index.value = FieldToken(kTokDocumentation);
        uint32_t str_idx = InternString(prim.meta().doc());
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
          fieldset.push_back(WFieldCount());
          WPushField(
              MakeTokenListOpField("apiSchemas", prim.meta().apiSchemaEdits()));
        } else {
        size_t count = prim.meta().apiSchemas().size();
        size_t items_bytes = 0;
        size_t api_data_size = 0;
        if (!safe::mul(count, size_t(4), &items_bytes) ||
            !safe::add(size_t(9), items_bytes, &api_data_size)) {
          WSetError("apiSchemas payload size overflow");
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
        uint64_t data_idx = InternBlock({TypeId::Token, std::move(api_data)}, kBlobTokenListOp);

        CrateField f;
        f.token_index.value = FieldToken(kTokApiSchemas);
        f.value_rep = ValueRep::Make(CrateTypeId::TokenListOp, data_idx, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        uint64_t idx = InternBlock({TypeId::Invalid, std::move(blob)}, kBlobRelocates);
        CrateField f;
        f.token_index.value = InternToken("relocates");
        f.value_rep =
            ValueRep::Make(CrateTypeId::Relocates, idx, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
        WRequireMinor(11);
      }

      // Population-order opinions are distinct from the persistent natural
      // child/property lists below. Preserve the sparse authored fields so a
      // Crate round trip retains traversal semantics.
      if (prim.meta().primOrderAuthored() ||
          !prim.meta().primOrder().empty()) {
        fieldset.push_back(WFieldCount());
        WPushField(
            MakeTokenVectorField("primOrder", prim.meta().primOrder()));
      }
      if (prim.meta().propertyOrderAuthored() ||
          !prim.meta().propertyOrder().empty()) {
        fieldset.push_back(WFieldCount());
        WPushField(MakeTokenVectorField(
            "propertyOrder", prim.meta().propertyOrder()));
      }
      if (prim.meta().displayGroupOrderAuthored() ||
          !prim.meta().displayGroupOrder().empty()) {
        fieldset.push_back(WFieldCount());
        WPushField(MakeStringVectorField(
            "displayGroupOrder", prim.meta().displayGroupOrder()));
      }

      auto batch_children = stream_child_tokens_.empty()
                                ? stream_child_tokens_.end()
                                : stream_child_tokens_.find(static_cast<uint32_t>(i));
      if (batch_children != stream_child_tokens_.end()) {
        // Batched write: a prim whose children span batches (tokens of its
        // full child list, resolved before the batch).
        if (!batch_children->second.empty()) {
          fieldset.push_back(WFieldCount());
          WPushField(MakeTokenVectorFieldFromTokens(
              FieldToken(kTokPrimChildren), batch_children->second));
        }
      } else if (children_from_links_) {
        // Verified fully linked layer: the children are exactly the linked
        // prims, in link order (the order the map path substitutes anyway).
        const std::vector<uint32_t>& ci = prim.child_indices();
        if (!ci.empty()) {
          std::vector<uint32_t> toks;
          toks.reserve(ci.size());
          for (uint32_t c : ci) {
            const std::string& cn = layer.prims()[c].name();
            if (cn.size() >= 2 && cn.back() == '}' &&
                cn.find('{') != std::string::npos) continue;
            toks.push_back(prim_name_tokens_[c]);
          }
          if (!toks.empty()) {
            fieldset.push_back(WFieldCount());
            WPushField(MakeTokenVectorFieldFromTokens(
                FieldToken(kTokPrimChildren), toks));
          }
        }
      } else {
        auto cit = parent_children.find(prim.path().str());
        if (cit != parent_children.end() && !cit->second.empty()) {
          fieldset.push_back(WFieldCount());
          WPushField(MakeTokenVectorField("primChildren", cit->second));
        }
      }

      // Properties, relationships, connections and time samples are emitted as
      // separate pxrUSD-style Attribute/Relationship specs (below). Collect the
      // child property names here for the prim's "properties" field.
      EmittedProps emitted = EmitPrimProperties(prim);
      std::vector<std::string_view>& prop_names = emitted.names;
      if (!prim.meta().propertyOrder().empty()) {
        // Views of the emitted names (not of propertyOrder) keep the list
        // pointing at PropNameTable storage.
        EmittedProps ordered;
        ordered.names.reserve(prop_names.size());
        ordered.tokens.reserve(prop_names.size());
        auto take = [&](size_t idx) {
          if (std::find(ordered.names.begin(), ordered.names.end(),
                        prop_names[idx]) == ordered.names.end()) {
            ordered.push(prop_names[idx], emitted.tokens[idx]);
          }
        };
        for (const std::string& wanted : prim.meta().propertyOrder()) {
          auto it = std::find(prop_names.begin(), prop_names.end(),
                              std::string_view(wanted));
          if (it != prop_names.end()) {
            take(static_cast<size_t>(it - prop_names.begin()));
          }
        }
        for (size_t idx = 0; idx < prop_names.size(); ++idx) take(idx);
        emitted = std::move(ordered);
      }

      // Composition arcs in pxr-native encodings (pxr rejects the whole layer
      // when a known field like `references` holds a token[] instead of its
      // schema type). The effective (composed) list is written explicitly;
      // authored list-op sublists additionally round-trip via the next-private
      // `<arc>_listOp` companion fields below.
      const ArcListOps* arc_edits = prim.meta().arc_edits();
      auto add_ref_listop_field = [&](const char* name,
                                      const std::vector<std::string>& arcs,
                                      bool is_payload, const ArcListOp* e) {
        if (arcs.empty() && !(e && e->authored)) return;
        uint64_t blk = StoreReferenceListOp(arcs, is_payload, e);
        CrateField f;
        f.token_index.value = InternToken(name);
        f.value_rep = ValueRep::Make(is_payload ? CrateTypeId::PayloadListOp
                                                : CrateTypeId::ReferenceListOp,
                                     blk, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      };
      auto add_path_listop_field = [&](const char* name,
                                       const std::vector<std::string>& arcs,
                                       const ArcListOp* e) {
        if (arcs.empty() && !(e && e->authored)) return;
        uint64_t blk = StorePathArcListOp(arcs, e);
        CrateField f;
        f.token_index.value = InternToken(name);
        f.value_rep =
            ValueRep::Make(CrateTypeId::PathListOp, blk, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        f.token_index.value = FieldToken(kTokComment);
        uint32_t str_idx = InternString(prim.meta().comment());
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // kind (token), displayName (string), instanceable (bool).
      if (prim.meta().kindAuthored() || !prim.meta().kind().empty()) {
        CrateField f;
        f.token_index.value = FieldToken(kTokKind);
        f.value_rep = ValueRep::Make(CrateTypeId::Token,
                                     InternToken(prim.meta().kind()), false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      if (!prim.meta().permission().empty()) {
        CrateField f;
        f.token_index.value = FieldToken(kTokPermission);
        f.value_rep = ValueRep::Make(
            CrateTypeId::Token, InternToken(prim.meta().permission()), false,
            true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      // displayName is string-typed in pxr's schema (Token would be dropped).
      if (prim.meta().displayNameAuthored() ||
          !prim.meta().displayName().empty()) {
        CrateField f;
        f.token_index.value = FieldToken(kTokDisplayName);
        uint32_t str_idx = InternString(prim.meta().displayName());
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      // instanceable=false is a real (blocking) opinion when authored.
      if (prim.meta().instanceable || prim.meta().instanceable_authored) {
        CrateField f;
        f.token_index.value = FieldToken(kTokInstanceable);
        f.value_rep = ValueRep::Make(CrateTypeId::Bool,
                                     prim.meta().instanceable ? 1 : 0, false,
                                     true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      for (const auto& extension : prim.meta().unknownFields()) {
        CrateField f;
        f.token_index.value = InternToken(extension.name);
        f.value_rep = EncodeExtensionField(extension);
        if (f.value_rep.type_id() == CrateTypeId::Invalid) {
          if (options_.strict_aousd_conformance && result_.error.empty()) {
            WSetError("Cannot encode extension prim field '" +
                            extension.name + "'");
          }
          continue;
        }
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      // variantSelection: a VariantSelectionMap of each set's selected variant.
      // The reader rebuilds the PrimVariantSetData from this plus the bracketed
      // holder prims, so no separate variantSets token field is emitted. (The
      // bracketed holder/child prims carry the variant CONTENT and are emitted
      // above for unflattened layers.)
      // NOTE: not gated on variantSets(): a prim may only SELECT a variant
      // defined by a referenced asset ("variants = {...}" next to a
      // reference), in which case it has selections but no local sets.
      {
        // Selections can live in three places depending on how the layer was
        // produced: PrimVariantSetData::selected (crate reader / authoring API,
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
          vf.token_index.value = FieldToken(kTokVariantSelection);
          vf.value_rep =
              ValueRep::Make(CrateTypeId::VariantSelectionMap, blk, false, false);
          fieldset.push_back(WFieldCount());
          WPushField(vf);
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
          fieldset.push_back(WFieldCount());
          WPushField(
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
        const StringListOps& authored_variant_sets =
            prim.meta().variantSetNameEdits();
        if (authored_variant_sets.authored) {
          StringListOps edits = authored_variant_sets;
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
          fieldset.push_back(WFieldCount());
          WPushField(
              MakeStringListOpField("variantSetNames", edits));
        } else if (!decl_sets.empty()) {
          fieldset.push_back(WFieldCount());
          // Variant declarations imply a composable prepend, matching the
          // USDA writer. Explicit authoring would erase weaker set names.
          StringListOps inferred;
          inferred.authored = true;
          inferred.is_explicit = false;
          inferred.prepended = decl_sets;
          WPushField(MakeStringListOpField("variantSetNames", inferred));
        }
      }

      // "properties" field: TokenVector listing this prim's child property and
      // relationship names (pxrUSD enumerates properties from it). The next
      // reader rebuilds properties from the separate specs, so it ignores this.
      if (!emitted.names.empty()) {
        // Same interning point (and order) as the per-name loop this
        // replaced: only names BuildTables did not record are looked up.
        for (size_t k = 0; k < emitted.tokens.size(); ++k) {
          if (emitted.tokens[k] == UINT32_MAX)
            emitted.tokens[k] = InternToken(emitted.names[k]);
        }
        fieldset.push_back(WFieldCount());
        WPushField(MakeTokenVectorFieldFromTokens(
            FieldToken(kTokProperties), emitted.tokens));
      }

      // Record fieldset
      const uint32_t fs_idx = WAddFieldset(fieldset);
      // consume_values: every field referencing this prim's values is now
      // built (their blocks interned), and no later writer pass reads property
      // values -- release them so the layer shrinks as value blocks accumulate
      // instead of coexisting in full. Cross-prim reads (variant-owner check,
      // child lists) touch meta and names only.
      if (options_.consume_values) {
        const_cast<PrimSpec&>(prim).release_value_payloads();
      }

      // Create spec. A prim whose LAST path element is a variant selector
      // ("{vset=sel}") is a Variant spec; "{vset=}" is the VariantSet
      // declaration. (Variant CHILD prims like "/Prim/{vset=sel}/Geo" keep a
      // normal name -> Prim.) Using the right spec type keeps the unflattened
      // output readable by pxrUSD (which rejects a Prim named "{...}").
      CrateSpec spec;
      spec.path_index.value = InternPathStable(prim.path().str());
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
      WPushSpec(spec);
      if (consume_prims_ && !reap_prims_)
        const_cast<PrimSpec&>(prim).release_to_skeleton();
  }

  static bool IsVariantHolderName(const std::string& nm) {
    return nm.size() >= 2 && nm.back() == '}' &&
           nm.find('{') != std::string::npos;
  }

  // True when child_indices reproduce the path-derived parent->children
  // relation exactly: every child link points at a prim whose path is
  // "<parent path>/<name>", and every non-root, non-holder prim with a
  // regular path is linked exactly once.
  bool ChildrenFullyLinked(const Layer& layer) const {
    const auto& prims = layer.prims();
    const size_t n = prims.size();
    if (n == 0) return false;
    // Both passes are read-only per prim apart from the per-child link count
    // (atomic), so slices run concurrently; the answer is the AND of all
    // per-prim checks, independent of the order they are evaluated in.
    std::unique_ptr<std::atomic<uint8_t>[]> linked(new std::atomic<uint8_t>[n]);
    for (size_t i = 0; i < n; ++i) linked[i].store(0, std::memory_order_relaxed);
    std::atomic<bool> ok{true};
    ParallelRanges(n, 100000, [&](size_t, size_t lo, size_t hi) {
      for (size_t i = lo; i < hi; ++i) {
        const std::string& p = prims[i].path().str();
        for (uint32_t c : prims[i].child_indices()) {
          if (c >= n) { ok.store(false); return; }
          const std::string& cp = prims[c].path().str();
          // Direct child of this prim: "<p>/<name>" with no further '/'.
          if (p.empty() || p == "/") { ok.store(false); return; }
          if (cp.size() <= p.size() + 1 || cp.compare(0, p.size(), p) != 0 ||
              cp[p.size()] != '/' ||
              cp.find('/', p.size() + 1) != std::string::npos) {
            ok.store(false);
            return;
          }
          // linked twice
          if (linked[c].fetch_add(1, std::memory_order_relaxed)) {
            ok.store(false);
            return;
          }
        }
        if (!ok.load(std::memory_order_relaxed)) return;
      }
    });
    if (!ok.load()) return false;
    ParallelRanges(n, 100000, [&](size_t, size_t lo, size_t hi) {
      for (size_t i = lo; i < hi; ++i) {
        const std::string& p = prims[i].path().str();
        if (p.empty() || p == "/") continue;
        if (IsVariantHolderName(prims[i].name())) continue;
        const size_t slash = p.rfind('/');
        if (slash == std::string::npos) continue;
        const uint8_t l = linked[i].load(std::memory_order_relaxed);
        if (slash == 0) {
          if (l) { ok.store(false); return; }  // a root prim linked under a prim
          continue;
        }
        if (l != 1) { ok.store(false); return; }
      }
    });
    return ok.load();
  }

  void BuildFieldsAndSpecs(const Layer& layer) {
    // 1. PseudoRoot spec (index 0)
    EmitPseudoRootSpec(layer, nullptr);
    // 2. Prim specs
    BuildPrimSpecs(layer);
  }

  // The pseudo-root spec from the layer metadata. Its primChildren are the
  // layer's root prims, or `root_names` when given (a batched write whose
  // batch holds only some of them).
  void EmitPseudoRootSpec(const Layer& layer,
                          const std::vector<std::string>* root_names) {
    {
      std::vector<uint32_t> fieldset;

      // defaultPrim
      if (layer.meta().defaultPrim_set || !layer.meta().defaultPrim.empty()) {
        CrateField f;
        f.token_index.value = InternToken("defaultPrim");
        f.value_rep = ValueRep::Make(CrateTypeId::Token,
                                     InternToken(layer.meta().defaultPrim),
                                     false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // metersPerUnit
      if (layer.meta().metersPerUnit_set || layer.meta().metersPerUnit != 0.01) {
        CrateField f;
        f.token_index.value = InternToken("metersPerUnit");
        double v = layer.meta().metersPerUnit;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // timeCodesPerSecond
      if (layer.meta().timeCodesPerSecond_set || layer.meta().timeCodesPerSecond != 24.0) {
        CrateField f;
        f.token_index.value = InternToken("timeCodesPerSecond");
        double v = layer.meta().timeCodesPerSecond;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // framesPerSecond / kilogramsPerUnit (only when authored).
      if (layer.meta().framesPerSecond_set) {
        CrateField f;
        f.token_index.value = InternToken("framesPerSecond");
        double v = layer.meta().framesPerSecond;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      if (layer.meta().kilogramsPerUnit_set) {
        CrateField f;
        f.token_index.value = InternToken("kilogramsPerUnit");
        double v = layer.meta().kilogramsPerUnit;
        uint64_t data_idx = StoreValueData(&v, sizeof(v), TypeId::Double);
        f.value_rep = ValueRep::Make(CrateTypeId::Double, data_idx, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // comment
      if (layer.meta().comment_set || !layer.meta().comment.empty()) {
        CrateField f;
        f.token_index.value = InternToken("comment");
        uint32_t str_idx = InternString(layer.meta().comment);
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      if (layer.meta().owner_set || !layer.meta().owner.empty()) {
        CrateField f;
        f.token_index.value = InternToken("owner");
        uint32_t str_idx = InternString(layer.meta().owner);
        f.value_rep = ValueRep::Make(CrateTypeId::String, str_idx, false,
                                     true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // USDZ playback metadata is a typed boolean field on the pseudo-root.
      if (layer.meta().autoPlay_set) {
        CrateField f;
        f.token_index.value = InternToken("autoPlay");
        f.value_rep = EncodeValue(Value(layer.meta().autoPlay));
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      if (layer.meta().playbackMode_set) {
        CrateField f;
        f.token_index.value = InternToken("playbackMode");
        f.value_rep = ValueRep::Make(
            CrateTypeId::Token, InternToken(layer.meta().playbackMode), false,
            true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      for (const auto& extension : layer.meta().unknownFields) {
        CrateField f;
        f.token_index.value = InternToken(extension.name);
        f.value_rep = EncodeExtensionField(extension);
        if (f.value_rep.type_id() == CrateTypeId::Invalid) {
          if (options_.strict_aousd_conformance && result_.error.empty()) {
            WSetError("Cannot encode extension layer field '" +
                            extension.name + "'");
          }
          continue;
        }
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        uint64_t bidx = InternBlock({TypeId::Invalid, std::move(blob)}, kBlobRelocates);
        CrateField f;
        f.token_index.value = InternToken("layerRelocates");
        f.value_rep =
            ValueRep::Make(CrateTypeId::Relocates, bidx, false, false);
        fieldset.push_back(WFieldCount());
        WPushField(f);
        WRequireMinor(11);
      }

      if (layer.meta().rootPrimOrder_set ||
          !layer.meta().rootPrimOrder.empty()) {
        fieldset.push_back(WFieldCount());
        WPushField(
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
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      if (layer.meta().colorManagementSystem_set ||
          !layer.meta().colorManagementSystem.empty()) {
        CrateField f;
        f.token_index.value = InternToken("colorManagementSystem");
        f.value_rep = ValueRep::Make(
            CrateTypeId::Token,
            InternToken(layer.meta().colorManagementSystem), false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }
      if (layer.meta().renderSettingsPrimPath_set ||
          !layer.meta().renderSettingsPrimPath.empty()) {
        CrateField f;
        f.token_index.value = InternToken("renderSettingsPrimPath");
        f.value_rep = ValueRep::Make(
            CrateTypeId::String,
            InternString(layer.meta().renderSettingsPrimPath), false, true);
        fieldset.push_back(WFieldCount());
        WPushField(f);
      }

      // subLayers (StringVector — pxr's schema type; a token[] here reads back
      // in pxr as an empty sublayer list)
      if (layer.meta().subLayers_set || !layer.meta().subLayers.empty()) {
        CrateField f = MakeStringVectorField("subLayers", layer.meta().subLayers);
        fieldset.push_back(WFieldCount());
        WPushField(f);
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
        fieldset.push_back(WFieldCount());
        WPushField(fo);
      }

      {
        std::vector<std::string> root_children;
        if (root_names) {
          root_children = *root_names;
        } else if (!layer.root_indices().empty()) {
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
          fieldset.push_back(WFieldCount());
          WPushField(MakeTokenVectorField("primChildren", root_children));
        }
      }

      const uint32_t fs_idx = WAddFieldset(fieldset);

      CrateSpec spec;
      spec.path_index.value = InternPath("/");
      spec.fieldset_index.value = fs_idx;
      spec.spec_type = SpecType::PseudoRoot;
      WPushSpec(spec);
    }
  }

  // Every prim's specs, in layer order (prims flagged in skip_prims_ are left
  // out).
  void BuildPrimSpecs(const Layer& layer) {
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

    // primChildren source. When every non-root, non-variant-holder prim is
    // linked exactly once from the prim whose path is its parent, the links
    // ARE the parent->children relation in the order the map path below would
    // substitute, so skip building 10M-entry string maps and emit from them
    // (children_from_links_). Any irregularity falls back to the map.
    children_from_links_ = ChildrenFullyLinked(layer);
    std::unordered_map<std::string, std::vector<std::string>> parent_children;
    for (const auto& prim : layer.prims()) {
      if (children_from_links_) break;
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
      if (children_from_links_) break;
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

    const size_t nprims = layer.prims().size();
    // A prim path with a variant component: its encoding reads the owner
    // prim's metadata (and workers then probe prims by path).
    std::atomic<bool> any_variant_path{false};
    ParallelRanges(nprims, 65536, [&](size_t, size_t lo, size_t hi) {
      for (size_t i = lo; i < hi && !any_variant_path.load(std::memory_order_relaxed); ++i)
        if (layer.prims()[i].path().str().find('{') != std::string::npos)
          any_variant_path.store(true, std::memory_order_relaxed);
    });
    // Release prims as they are written only when no prim is read by another
    // prim's encoding (variant owners are).
    consume_prims_ = options_.consume_values && options_.consume_prims &&
                     !any_variant_path.load();
#if defined(LIGHTUSD_ENABLE_THREAD)
    const bool want_parallel = options_.parallel_fields &&
                               nprims >= kMinPrimsForParallelFields &&
                               ResolveThreads() > 1;
#else
    const bool want_parallel = false;
#endif
    if (want_parallel) {
      // Workers may probe prims by path (variant-owner lookups); the call
      // builds the deferred Layer path index once BEFORE threads run (the
      // index would otherwise be built under a process mutex by whichever
      // worker probes first while others read a half-built unordered_map).
      // Relationship names not covered by property slots get PropNameTable
      // ids at first emit; intern them up front so worker threads never
      // mutate the shared table (and every PropNameId has a token slot).
      PropNameTable& name_table = GetPropNameTable();
      for (const auto& prim : layer.prims()) {
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
        // Deferred fill: no relationships yet; workers intern new names in
        // the (thread-safe) table as they encode -- ids may differ, content
        // does not.
        if (!deferred_filler_)
#endif
        // (relationship_names() copies and sorts; most prims have none.)
        if (prim.has_relationship_data()) {
          for (const std::string& rel_name : prim.relationship_names()) {
            (void)name_table.intern(rel_name);
          }
        }
      }
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
      if (deferred_filler_ && any_variant_path) {
        // Variant-owner checks read another prim's metadata, which may sit in
        // a chunk not filled yet: fill everything up front instead.
        for (size_t i = 0; i < layer.prims().size(); ++i) FillForWrite(layer, i);
        deferred_filler_ = nullptr;
      }
#endif
      // Workers probe prims by path only for variant-owner lookups (a prim
      // path with a variant component). When there are such paths, build the
      // deferred Layer path index once BEFORE threads run (it would otherwise
      // be built under a process mutex by whichever worker probes first
      // while others read a half-built unordered_map).
      if (any_variant_path) (void)layer.prim_at_path("/");
      if (prop_name_tokens_.size() < name_table.size()) {
        prop_name_tokens_.resize(name_table.size(), UINT32_MAX);
      }
      // Workers only intern() names that already exist (pre-warmed above);
      // publish the lock-free snapshot so those calls never even take the
      // table's rwlock.
      name_table.publish_snapshot();
      // Provisional id domains start here (pseudo-root already committed).
      w_base_tok = static_cast<uint32_t>(tokens_.size());
      w_base_str = static_cast<uint32_t>(string_indices_.size());
      w_base_path = static_cast<uint32_t>(paths_.size());
#if defined(LIGHTUSD_ENABLE_THREAD)
      if (BuildFieldsAndSpecsParallel(layer, variant_names, parent_children)) {
        return;  // done (or failed with result_.error set)
      }
#endif
    }
    for (size_t i = 0; i < nprims; ++i) {
      FillForWrite(layer, i);
      BuildOnePrim(layer, i, variant_names, parent_children);
      ReleaseAfterWrite(layer, i);
      if (!result_.error.empty()) return;
    }

  }

  // ============================================================
  // Parallel fields_specs: ordered commit + orchestration
  // ============================================================

  static constexpr size_t kMinPrimsForParallelFields = 8192;
  static constexpr size_t kFieldsChunkPrims = 1024;
  static constexpr size_t kChunksForWideFields = 256;  // ~262 K prims

  // Final id tables for one chunk's provisional id domains: fin_tok[i] maps
  // token provisional id (w_base_tok + i) to its final global index, likewise
  // strings/paths; fin_block[i] maps block provisional ref (op i) to the final
  // value_data_ index (staged) or absolute VALUE-section offset (streaming).
  struct ChunkFin {
    const std::vector<uint32_t>* tok;
    const std::vector<uint32_t>* str;
    const std::vector<uint32_t>* path;
    const std::vector<uint64_t>* block;
  };

  // Remap one ValueRep from provisional worker ids to final ones. Inlined
  // scalar payloads carry token/string-domain ids only for Token / AssetPath
  // (token domain) and String (string domain); every other inline scalar keeps
  // its raw bits. A non-inlined payload is either the reserved empty-array
  // marker (0, untouched) or a provisional block ref (op index + 1).
  ValueRep RemapValueRep(ValueRep rep, const ChunkFin& fin) const {
    const uint64_t p = rep.payload();
    if (rep.is_inlined()) {
      switch (rep.type_id()) {
        case CrateTypeId::Token:
        case CrateTypeId::AssetPath: {
          if (p < w_base_tok) return rep;
          return ValueRep::Make(rep.type_id(), (*fin.tok)[p - w_base_tok],
                                rep.is_array(), true, rep.is_compressed(),
                                rep.is_array_edit());
        }
        case CrateTypeId::String: {
          if (p < w_base_str) return rep;
          return ValueRep::Make(rep.type_id(), (*fin.str)[p - w_base_str],
                                rep.is_array(), true, rep.is_compressed(),
                                rep.is_array_edit());
        }
        default:
          return rep;
      }
    }
    if (p == 0) return rep;  // reserved empty-array marker
    return ValueRep::Make(rep.type_id(), (*fin.block)[p - 1], rep.is_array(),
                          false, rep.is_compressed(), rep.is_array_edit());
  }

  // Fix one provisionally-embedded ValueRep (8 bytes) inside a block.
  // Returns true when the bytes changed (the content digest must be redone).
  bool FixEmbeddedRep64(void* p8, const ChunkFin& fin) {
    uint64_t raw;
    std::memcpy(&raw, p8, 8);
    const ValueRep fixed = RemapValueRep(ValueRep(raw), fin);
    if (fixed.raw() == raw) return false;
    const uint64_t out = fixed.raw();
    std::memcpy(p8, &out, 8);
    return true;
  }

  // Fix blob-internal provisional ids of a worker-logged block to FINAL
  // values. Runs before the serial dedup probe so identical-block matching
  // compares final bytes, exactly like the serial writer. The layout per tag
  // mirrors the encoder that produced the blob (see the kBlob* tags); the
  // fix-up is applied exactly once per rep, so it is not idempotent by
  // design (all payloads here are provisional op refs).
  // Fix-up passes: token/string/path ids (need only the chunk's interned
  // ids) and embedded ValueReps (need fin.block: earlier blocks' final
  // offsets). The walk reads only counts/lengths, which neither pass rewrites,
  // so running the passes separately (streaming stage B: kFixIds in parallel,
  // kFixReps in op order) gives the same bytes as one combined pass.
  enum : unsigned { kFixIds = 1, kFixReps = 2 };
  // True when the block may carry embedded ValueReps (fixed by kFixReps).
  static bool OpHasEmbeddedReps(const WorkerCtx::BlockLogOp& op) {
    switch (op.tag) {
      case kBlobDict:
      case kBlobTimeSamples:
      case kBlobSpline:
      case kBlobRefItems:
      case kBlobNestedRep:
        return true;
      case kBlobTokenVec:
      case kBlobStringVec:
      case kBlobTokenListOp:
      case kBlobStringListOp:
      case kBlobPathListOp:
      case kBlobVarSel:
      case kBlobRelocates:
      case kBlobPayloadItems:
      case kBlobString4:
        return false;
      default:
        return !op.block.embedded_value_rep_offsets.empty();
    }
  }
  bool FixOpBlobIds(WorkerCtx::BlockLogOp& op, const ChunkFin& fin,
                    unsigned mode = kFixIds | kFixReps) {
    bool changed = false;
    auto fix_rep = [&](void* p8) {
      return (mode & kFixReps) ? FixEmbeddedRep64(p8, fin) : false;
    };
    auto fix_ids = [&](uint8_t* first, size_t count, uint32_t base,
                       const std::vector<uint32_t>& ids) {
      if (!(mode & kFixIds)) return;
      uint32_t* v = reinterpret_cast<uint32_t*>(first);
      for (size_t i = 0; i < count; ++i) {
        if (v[i] >= base) {
          v[i] = ids[v[i] - base];
          changed = true;
        }
      }
    };
    auto fix_tok_run = [&](uint8_t* p, size_t n) {  // [u64 count][u32*]
      if (n < 8) return;
      uint64_t c;
      std::memcpy(&c, p, 8);
      if (c > (n - 8) / 4) c = (n - 8) / 4;
      fix_ids(p + 8, static_cast<size_t>(c), w_base_tok, *fin.tok);
    };
    auto fix_str_run = [&](uint8_t* p, size_t n) {
      if (n < 8) return;
      uint64_t c;
      std::memcpy(&c, p, 8);
      if (c > (n - 8) / 4) c = (n - 8) / 4;
      fix_ids(p + 8, static_cast<size_t>(c), w_base_str, *fin.str);
    };
    // [u8 header][run]* where explicit == one run and non-explicit runs in the
    // encoders' order: added(0x04), prepended(0x20), appended(0x40),
    // deleted(0x08), ordered(0x10).
    auto fix_generic_listop = [&](uint8_t* b, size_t n, bool path_dom) {
      if (n == 0) return;
      const uint8_t bits = b[0];
      size_t at = 1;
      const auto runs = [&](bool one_run) {
        if (!one_run || at + 8 > n) return;
        uint64_t c;
        std::memcpy(&c, b + at, 8);
        at += 8;
        if (at + c * 4 > n) c = (n - at) / 4;
        if (path_dom)
          fix_ids(b + at, static_cast<size_t>(c), w_base_path, *fin.path);
        else if (op.tag == kBlobStringListOp)
          fix_ids(b + at, static_cast<size_t>(c), w_base_str, *fin.str);
        else
          fix_ids(b + at, static_cast<size_t>(c), w_base_tok, *fin.tok);
        at += c * 4;
      };
      if (bits & 0x01) {
        // Explicit: a run exists only when HasExplicitItems (0x02) is set.
        runs((bits & 0x02) != 0);
      } else {
        const bool order[] = {(bits & 0x04) != 0, (bits & 0x20) != 0,
                              (bits & 0x40) != 0, (bits & 0x08) != 0,
                              (bits & 0x10) != 0};
        for (bool pr : order) runs(pr);
      }
    };
    auto fix_item = [&](uint8_t* it, size_t n) -> uint8_t* {
      // [u32 assetStr][u32 pathIdx][f64 offset][f64 scale] (+ customData)
      if (n < 24) return it + n;
      fix_ids(it, 1, w_base_str, *fin.str);
      fix_ids(it + 4, 1, w_base_path, *fin.path);
      uint8_t* p = it + 24;
      if (op.tag != kBlobRefItems || n < 32) return p;
      uint64_t cc;
      std::memcpy(&cc, p, 8);
      p += 8;
      for (uint64_t k = 0; k < cc; ++k) {
        if (p + 20 > it + n) return p;
        fix_ids(p, 1, w_base_str, *fin.str);
        changed |= fix_rep(p + 12);
        p += 20;
      }
      return p;
    };
    switch (op.tag) {
      case kBlobTokenVec: {
        uint8_t* b = op.block.data.data();
        fix_tok_run(b, op.block.data.size());
        break;
      }
      case kBlobStringVec: {
        uint8_t* b = op.block.data.data();
        fix_str_run(b, op.block.data.size());
        break;
      }
      case kBlobTokenListOp:
      case kBlobStringListOp:
      case kBlobPathListOp: {
        fix_generic_listop(op.block.data.data(), op.block.data.size(),
                           op.tag == kBlobPathListOp);
        break;
      }
      case kBlobVarSel: {
        // [u64 count][ (u32 string key, u32 string value) ]*
        const size_t n = op.block.data.size();
        if (n < 8) break;
        uint64_t c;
        std::memcpy(&c, op.block.data.data(), 8);
        if (c > (n - 8) / 8) c = (n - 8) / 8;
        fix_ids(op.block.data.data() + 8, static_cast<size_t>(c) * 2,
                w_base_str, *fin.str);
        break;
      }
      case kBlobRelocates: {
        // [u64 count][ (u32 srcPath, u32 dstPath) ]*
        const size_t n = op.block.data.size();
        if (n < 8) break;
        uint64_t c;
        std::memcpy(&c, op.block.data.data(), 8);
        if (c > (n - 8) / 8) c = (n - 8) / 8;
        fix_ids(op.block.data.data() + 8, static_cast<size_t>(c) * 2,
                w_base_path, *fin.path);
        break;
      }
      case kBlobDict: {
        // [u64 count] + entries [u32 key][i64][u64 rep]
        const size_t n = op.block.data.size();
        if (n < 8) break;
        uint64_t c;
        std::memcpy(&c, op.block.data.data(), 8);
        size_t at = 8;
        for (uint64_t k = 0; k < c && at + 20 <= n; ++k) {
          fix_ids(op.block.data.data() + at, 1, w_base_str, *fin.str);
          changed |= fix_rep(op.block.data.data() + at + 12);
          at += 20;
        }
        break;
      }
      case kBlobTimeSamples: {
        // [i64 fwd=8][u64 times rep][i64 fwd=8][u64 n][rep per sample]
        const size_t n = op.block.data.size();
        if (n < 32) break;
        uint64_t ns;
        std::memcpy(&ns, op.block.data.data() + 24, 8);
        changed |= fix_rep(op.block.data.data() + 8);
        for (uint64_t k = 0; k < ns && 32 + (k + 1) * 8 <= n; ++k)
          changed |= fix_rep(
              op.block.data.data() + 32 + static_cast<size_t>(k) * 8);
        break;
      }
      case kBlobSpline: {
        // [u64 blobSz][blob][u64 customCount] then per item
        // [f64 time][u64 entries] + entries [u32 key][i64 8][u64 rep]
        const size_t n = op.block.data.size();
        if (n < 8) break;
        uint64_t sz;
        std::memcpy(&sz, op.block.data.data(), 8);
        size_t at = 8 + static_cast<size_t>(sz);
        if (at + 8 > n) break;
        uint64_t cc;
        std::memcpy(&cc, op.block.data.data() + at, 8);
        at += 8;
        for (uint64_t g = 0; g < cc && at + 16 <= n; ++g) {
          at += 8;  // time
          uint64_t ec;
          std::memcpy(&ec, op.block.data.data() + at, 8);
          at += 8;
          for (uint64_t k = 0; k < ec && at + 20 <= n; ++k) {
            fix_ids(op.block.data.data() + at, 1, w_base_str, *fin.str);
            changed |= fix_rep(op.block.data.data() + at + 12);
            at += 20;
          }
        }
        break;
      }
      case kBlobRefItems:
      case kBlobPayloadItems: {
        // [u8 header][run]*, run = [u64 n][item]* (see fix_generic_listop for
        // the run order), item = [u32 asset][u32 path][f64][f64]; only
        // references additionally carry a per-item customData dict. The
        // encoder's embedded-rep list covers the same reps at the same
        // offsets, so text and reps are fixed coherently here.
        uint8_t* b = op.block.data.data();
        const size_t n = op.block.data.size();
        if (n == 0) break;
        const uint8_t bits = b[0];
        size_t at = 1;
        static const int kRunBits[] = {0x04, 0x20, 0x40, 0x08, 0x10};
        for (int bit : kRunBits) {
          const bool present =
              (bits & 0x01) ? (bit == 0x04 && (bits & 0x02))
                            : (bits & bit) != 0;
          if (!present || at + 8 > n) continue;
          uint64_t c;
          std::memcpy(&c, b + at, 8);
          at += 8;
          for (uint64_t k = 0; k < c && at < n; ++k) {
            const uint8_t* end = fix_item(b + at, n - at);
            at = static_cast<size_t>(end - b);
          }
        }
        break;
      }
      case kBlobString4: {
        // single [u32 string] (scalar pathExpression)
        if (op.block.data.size() >= 4)
          fix_ids(op.block.data.data(), 1, w_base_str, *fin.str);
        break;
      }
      case kBlobNestedRep: {
        // [i64 size=8][u64 rep]
        if (op.block.data.size() >= 16)
          changed |= fix_rep(op.block.data.data() + 8);
        break;
      }
      case kBlobNone:
      default:
        // Blocks without a tag fix their ValueReps from the encoder-recorded
        // list (array-edit et al).
        for (const size_t off : op.block.embedded_value_rep_offsets) {
          if (off + 8 <= op.block.data.size())
            changed |= fix_rep(op.block.data.data() + off);
        }
        break;
    }
    return changed;
  }

  // Replay ONE worker chunk into the shared tables, in stages that touch
  // disjoint writer state, so consecutive chunks pipeline across four commit
  // threads: A1(k) after A1(k-1); A2(k) after A2(k-1); B(k) after A1(k),
  // A2(k) and B(k-1); C(k) after B(k) and C(k-1). Each stage alone keeps the
  // serial writer's first-use order, so the output equals a one-thread replay.
  //   A1: intern events (token/string tables)
  //   A2: path events (path table)
  //   B:  block ops, fixed ids then exact intern/push semantics (VALUE side)
  //   C:  fields -> fieldsets -> specs, then the counter merges
  void CommitStageA1(WorkerCtx& w) {
    // 1. intern events (tokens and strings, exactly in event order)
    w.fin_tok.assign(w.ntok, 0);
    w.fin_str.assign(w.nstr, 0);
    for (const InternEvent& ev : w.events) {
      const uint32_t fin =
          ev.kind == 0 ? InternToken(ev.text) : InternString(ev.text);
      if (ev.kind == 0) w.fin_tok[ev.local - w_base_tok] = fin;
      else w.fin_str[ev.local - w_base_str] = fin;
    }
  }

  // Group a finished chunk's path events by path shard (worker side).
  static void BucketPathEvents(WorkerCtx& w) {
    uint32_t* off = w.pe_shard_off;
    for (size_t s = 0; s <= kPathShards; ++s) off[s] = 0;
    for (const PathEvent& pe : w.pevents) ++off[PathShardOf(pe.hash) + 1];
    for (size_t s = 0; s < kPathShards; ++s) off[s + 1] += off[s];
    w.pe_by_shard.resize(w.pevents.size());
    uint32_t cursor[kPathShards];
    for (size_t s = 0; s < kPathShards; ++s) cursor[s] = off[s];
    for (uint32_t e = 0; e < static_cast<uint32_t>(w.pevents.size()); ++e)
      w.pe_by_shard[cursor[PathShardOf(w.pevents[e].hash)]++] = e;
  }

  // A fixed team of threads that runs one job over shard slices; Run() calls
  // job(t) for t = 0 .. size-1 (t = 0 on the caller) and returns when all are
  // done. Used by stage A2 twice per chunk.
  struct ShardTeam {
    size_t size = 1;
#if defined(LIGHTUSD_ENABLE_THREAD)
    // Runs come back to back (several per committed chunk, ~10 K chunks on a
    // large write), so both sides spin briefly before sleeping: a condvar
    // wake per run otherwise adds its latency to every commit stage.
    static constexpr int kSpin = 4096;
    std::mutex m;
    std::condition_variable go, done;
    std::atomic<uint64_t> gen{0};
    std::atomic<size_t> pending{0};
    std::atomic<int> sleepers{0};  // helpers blocked on `go` (counted under m)
    std::atomic<bool> quit{false};
    std::atomic<const std::function<void(size_t)>*> job{nullptr};
    std::vector<std::thread> threads;

    static void Relax() {
#if defined(__x86_64__) || defined(__i386__)
      __builtin_ia32_pause();
#endif
    }

    void Start(size_t n) {
      size = n < 1 ? 1 : n;
      for (size_t t = 1; t < size; ++t) {
        threads.emplace_back([this, t] {
          uint64_t seen = 0;
          for (;;) {
            bool ready = false;
            for (int i = 0; i < kSpin && !ready; ++i) {
              if (quit.load(std::memory_order_acquire)) return;
              ready = gen.load(std::memory_order_acquire) != seen;
              if (!ready) Relax();
            }
            if (!ready) {
              std::unique_lock<std::mutex> lk(m);
              sleepers.fetch_add(1, std::memory_order_relaxed);
              go.wait(lk, [&] {
                return quit.load(std::memory_order_acquire) ||
                       gen.load(std::memory_order_acquire) != seen;
              });
              sleepers.fetch_sub(1, std::memory_order_relaxed);
              if (quit.load(std::memory_order_acquire)) return;
            }
            seen = gen.load(std::memory_order_acquire);
            (*job.load(std::memory_order_acquire))(t);
            if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
              std::lock_guard<std::mutex> lk(m);
              done.notify_one();
            }
          }
        });
      }
    }
    void Run(const std::function<void(size_t)>& fn) {
      if (size == 1) {
        fn(0);
        return;
      }
      job.store(&fn, std::memory_order_release);
      pending.store(size - 1, std::memory_order_release);
      {
        // Under m: a helper about to sleep has counted itself and re-checks
        // `gen` while holding m, so this bump is never missed.
        std::lock_guard<std::mutex> lk(m);
        gen.fetch_add(1, std::memory_order_acq_rel);
      }
      if (sleepers.load(std::memory_order_acquire) > 0) go.notify_all();
      fn(0);
      for (int i = 0; i < kSpin; ++i) {
        if (pending.load(std::memory_order_acquire) == 0) return;
        Relax();
      }
      std::unique_lock<std::mutex> lk(m);
      done.wait(lk, [&] { return pending.load(std::memory_order_acquire) == 0; });
    }
    void Stop() {
      {
        std::lock_guard<std::mutex> lk(m);
        quit = true;
      }
      go.notify_all();
      for (std::thread& th : threads) th.join();
      threads.clear();
    }
#else
    void Run(const std::function<void(size_t)>& fn) { fn(0); }
#endif
  };

  void CommitStageA2(WorkerCtx& w, ShardTeam& team) {
    // 2. path events. Serial semantics: probe + insert in event order. The
    // events of one chunk are distinct keys (the worker's table dedups them),
    // so a chunk's probes see only earlier chunks' inserts. Hence: probe all
    // events (shards in parallel), append the misses as new records in event
    // order (serial: record ids = first-use order), then index the new
    // records (shards in parallel). Same ids as the one-by-one replay.
    const size_t npe = w.pevents.size();
    w.fin_path.assign(npe, UINT32_MAX);
    const size_t T = team.size;
    const std::function<void(size_t)> probe = [&](size_t t) {
      constexpr size_t kAhead = 8;
      for (size_t s = t; s < kPathShards; s += T) {
        const uint32_t lo = w.pe_shard_off[s], hi = w.pe_shard_off[s + 1];
        const std::vector<uint64_t>& slots = path_shards_[s].slots;
        for (uint32_t k = lo; k < hi; ++k) {
#if defined(__GNUC__) || defined(__clang__)
          if (k + kAhead < hi && !slots.empty()) {
            const uint64_t ha = w.pevents[w.pe_by_shard[k + kAhead]].hash;
            __builtin_prefetch(&slots[static_cast<size_t>(ha) & (slots.size() - 1)]);
          }
#endif
          const uint32_t e = w.pe_by_shard[k];
          const PathEvent& pe = w.pevents[e];
          w.fin_path[e] = FindPathKey(pe.stem, pe.name, pe.hash);
        }
      }
    };
    team.Run(probe);

    const uint32_t first_new = static_cast<uint32_t>(paths_.size());
    for (size_t e = 0; e < npe; ++e) {
      if (w.fin_path[e] != UINT32_MAX) continue;
      const PathEvent& pe = w.pevents[e];
      PathRec rec{pe.stem, pe.name};
      if (pe.store != kStableText) {
        // Transient caller text: the views point into this ctx's path_store,
        // which the next chunk resets. Adopt that string (a move keeps a heap
        // buffer in place; re-split for short strings).
        path_owned_.push_back(std::move(w.path_store[pe.store]));
        rec = SplitPath(path_owned_.back());
      }
      w.fin_path[e] = static_cast<uint32_t>(paths_.size());
      paths_.push_back(rec);
      path_fingerprints_.push_back(static_cast<uint16_t>(pe.hash));
    }
    if (paths_.size() == first_new) return;

    const std::function<void(size_t)> insert = [&](size_t t) {
      for (size_t s = t; s < kPathShards; s += T) {
        const uint32_t lo = w.pe_shard_off[s], hi = w.pe_shard_off[s + 1];
        for (uint32_t k = lo; k < hi; ++k) {
          const uint32_t e = w.pe_by_shard[k];
          const uint32_t id = w.fin_path[e];
          if (id >= first_new) InsertPathSlot(w.pevents[e].hash, id);
        }
      }
    };
    team.Run(insert);
  }

  void CommitStageB(WorkerCtx& w) {
    // 3. block ops: fix provisional ids to final, then replay the exact
    // serial InternBlock / PushValueBlock semantics (identical counters).
    w.fin_block.assign(w.ops.size(), 0);
    const ChunkFin fin{&w.fin_tok, &w.fin_str, &w.fin_path, &w.fin_block};
    // Streaming dedup probes are random loads into multi-GB tables: prefetch
    // the bucket head kAheadBucket ops ahead and its first entry kAheadEntry
    // ahead (the worker digest of a block the fix-up later changes points at
    // the wrong bucket -- harmless, it is only a hint).
    constexpr size_t kAheadBucket = 16, kAheadEntry = 8;
    const size_t nops = w.ops.size();
    auto prefetch_op = [&](size_t j, bool entry) {
      const WorkerCtx::BlockLogOp& o = w.ops[j];
      if (o.kind != kOpIntern) return;
      if (entry) {
        stream_dedup_.prefetch_entry(o.digest);
      } else {
        stream_dedup_.prefetch_bucket(o.digest);
        if (o.block.is_ref())
          stream_source_ranges_.prefetch(o.block.src->identity(),
                                         o.block.src_offset, o.block.src_len);
      }
    };
    for (size_t i = 0; i < nops; ++i) {
      if (stream_values_) {
        if (i + kAheadBucket < nops) prefetch_op(i + kAheadBucket, false);
        if (i + kAheadEntry < nops) prefetch_op(i + kAheadEntry, true);
      }
      WorkerCtx::BlockLogOp& op = w.ops[i];
      const bool changed = FixOpBlobIds(op, fin);
      if (op.kind == kOpPush) {
        if (stream_values_) {
          const uint64_t off = StreamRawBlock(op.block.bytes(), op.block.size());
          if (op.block.is_ref()) DropStreamedSourceRange(op.block);
          w.fin_block[i] = off;
        } else {
          const uint64_t idx = value_data_.size();
          value_data_.push_back(std::move(op.block));
          w.fin_block[i] = idx;
        }
        continue;
      }
      // The worker's digest covers the PROVISIONAL bytes; when the id fix-up
      // changed them it no longer describes the final block, so redo it here
      // (the streaming dedup table trusts digests without comparing bytes).
      uint64_t h = op.digest, h2 = op.digest2;
      if (changed) {
        if (op.block.size() == 0) {
          h = 0;
          h2 = 0;
        } else if (stream_values_) {
          const XXH128_hash_t digest =
              StreamDigest(op.block.bytes(), op.block.size());
          h = digest.low64;
          h2 = digest.high64;
        } else {
          h = HashBlockBytes(op.block.bytes(), op.block.size());
        }
      }
      if (stream_values_) {
        w.fin_block[i] = StreamInternBlockWithDigest(op.block, h, h2);
      } else {
        w.fin_block[i] = InternBlockWithHash(std::move(op.block), h);
      }
    }
  }

  // Content digest of a block as the streaming / staged dedup tables key it.
  void DigestBlock(const DataBlock& b, uint64_t* h, uint64_t* h2) const {
    *h = 0;
    *h2 = 0;
    if (b.size() == 0) return;
    if (stream_values_) {
      const XXH128_hash_t d = StreamDigest(b.bytes(), b.size());
      *h = d.low64;
      *h2 = d.high64;
    } else {
      *h = HashBlockBytes(b.bytes(), b.size());
    }
  }

  // Entries one chunk added to the dedup / source-range tables, for the
  // in-chunk lookups of streaming stage B (P2). Generation-stamped, so a new
  // chunk starts empty without clearing; owned by the stage-B thread.
  struct ChunkDedupTables {
    struct DD { uint64_t h, h2, n, off; uint32_t gen; };
    struct RG { uint64_t id, o, len, out; uint32_t gen; };
    std::vector<DD> dd;
    std::vector<RG> rg;
    uint32_t gen = 0;
    void Begin(size_t max_inserts) {
      size_t cap = 64;
      while (cap < max_inserts * 2) cap *= 2;
      if (cap > dd.size()) {
        dd.assign(cap, DD{0, 0, 0, 0, 0});
        rg.assign(cap, RG{0, 0, 0, 0, 0});
        gen = 0;
      }
      ++gen;
    }
    const uint64_t* find_dd(uint64_t h, uint64_t h2, uint64_t n) const {
      const size_t mask = dd.size() - 1;
      for (size_t i = static_cast<size_t>(h) & mask; dd[i].gen == gen;
           i = (i + 1) & mask) {
        if (dd[i].h == h && dd[i].h2 == h2 && dd[i].n == n) return &dd[i].off;
      }
      return nullptr;
    }
    void add_dd(uint64_t h, uint64_t h2, uint64_t n, uint64_t off) {
      const size_t mask = dd.size() - 1;
      size_t i = static_cast<size_t>(h) & mask;
      while (dd[i].gen == gen) i = (i + 1) & mask;
      dd[i] = DD{h, h2, n, off, gen};
    }
    const uint64_t* find_rg(uint64_t id, uint64_t o, uint64_t len) const {
      if (len == 0) return nullptr;
      const size_t mask = rg.size() - 1;
      for (size_t i = StreamSourceRangeTable::Hash(id, o, len) & mask;
           rg[i].gen == gen; i = (i + 1) & mask) {
        if (rg[i].id == id && rg[i].o == o && rg[i].len == len) return &rg[i].out;
      }
      return nullptr;
    }
    void add_rg(uint64_t id, uint64_t o, uint64_t len, uint64_t out) {
      const size_t mask = rg.size() - 1;
      size_t i = StreamSourceRangeTable::Hash(id, o, len) & mask;
      while (rg[i].gen == gen) i = (i + 1) & mask;
      rg[i] = RG{id, o, len, out, gen};
    }
  } chunk_dedup_;

  // Streaming-mode stage B. Serial semantics per op (CommitStageB): fix ids
  // and embedded reps, redo the digest if bytes changed, then push (write) or
  // intern (source-range hit -> content hit -> write + index). Split:
  //   P1 (parallel slices): id fix-up of every block; for blocks without
  //      embedded reps also the digest redo and the probes of the global
  //      tables, which hold exactly the earlier chunks' entries.
  //   P2 (serial, op order): embedded-rep fix-up (needs earlier blocks'
  //      offsets), probes that must also see this chunk's earlier entries
  //      (chunk-local tables), output offsets planned exactly as
  //      StreamRawBlock would place them, dedup counters.
  //   P3 (parallel shards): insert the chunk's new entries.
  // The writes themselves happen in stage W (CommitStageW) in op order.
  void CommitStageBStream(WorkerCtx& w, ShardTeam& team) {
    const size_t nops = w.ops.size();
    w.fin_block.assign(nops, 0);
    w.pre_kind.assign(nops, 0);
    w.pre_off.resize(nops);
    w.wjob_op.clear();
    w.wjob_off.clear();
    w.pend_dd.clear();
    w.pend_rg.clear();
    const size_t T = team.size;

    const ChunkFin fin_ids{&w.fin_tok, &w.fin_str, &w.fin_path, nullptr};
    const std::function<void(size_t)> p1 = [&](size_t t) {
      const size_t lo = nops * t / T, hi = nops * (t + 1) / T;
      for (size_t i = lo; i < hi; ++i) {
        WorkerCtx::BlockLogOp& op = w.ops[i];
        const bool changed = FixOpBlobIds(op, fin_ids, kFixIds);
        if (OpHasEmbeddedReps(op)) {
          op.ids_changed = changed;
          continue;
        }
        if (op.kind != kOpIntern) continue;
        if (changed) DigestBlock(op.block, &op.digest, &op.digest2);
        if (op.block.is_ref()) {
          if (const uint64_t* c = stream_source_ranges_.find(
                  op.block.src->identity(), op.block.src_offset,
                  op.block.src_len)) {
            w.pre_kind[i] = 1;
            w.pre_off[i] = *c;
            continue;
          }
        }
        if (const uint64_t* o =
                stream_dedup_.find(op.digest, op.digest2, op.block.size())) {
          w.pre_kind[i] = 2;
          w.pre_off[i] = *o;
        } else {
          w.pre_kind[i] = 3;
        }
      }
    };
    team.Run(p1);

    // P2
    chunk_dedup_.Begin(nops);
    const ChunkFin fin{&w.fin_tok, &w.fin_str, &w.fin_path, &w.fin_block};
    auto plan = [&](size_t n) {
      const uint64_t pad = (8 - (stream_plan_pos_ % 8)) % 8;
      const uint64_t off = stream_plan_pos_ + pad;
      stream_plan_pos_ = off + n;
      return off;
    };
    auto write = [&](size_t i, uint64_t off) {
      w.wjob_op.push_back(static_cast<uint32_t>(i));
      w.wjob_off.push_back(off);
    };
    for (size_t i = 0; i < nops; ++i) {
      WorkerCtx::BlockLogOp& op = w.ops[i];
      const size_t n = op.block.size();
      const bool reps = OpHasEmbeddedReps(op);
      bool rep_changed = false;
      if (reps) rep_changed = FixOpBlobIds(op, fin, kFixReps);
      if (op.kind == kOpPush) {
        const uint64_t off = plan(n);
        write(i, off);
        w.fin_block[i] = off;
        continue;
      }
      uint64_t h = op.digest, h2 = op.digest2;
      uint8_t kind = w.pre_kind[i];  // 0 for blocks with embedded reps
      if (reps && (rep_changed || op.ids_changed)) DigestBlock(op.block, &h, &h2);
      const bool ref = op.block.is_ref();
      const uint64_t key_id = ref ? op.block.src->identity() : 0;
      if (ref) {
        const uint64_t* c = nullptr;
        if (kind == 1) {
          c = &w.pre_off[i];
        } else {
          c = chunk_dedup_.find_rg(key_id, op.block.src_offset, op.block.src_len);
          if (!c && kind == 0)
            c = stream_source_ranges_.find(key_id, op.block.src_offset,
                                           op.block.src_len);
        }
        if (c) {
          ++blocks_deduped_;
          w.fin_block[i] = *c;
          continue;
        }
      }
      // The source range is new to both tables here, so the emplace below
      // always inserts (as the serial emplace would).
      auto add_range = [&](uint64_t out) {
        if (!ref || op.block.src_len == 0) return;
        chunk_dedup_.add_rg(key_id, op.block.src_offset, op.block.src_len, out);
        w.pend_rg.push_back(WorkerCtx::PendRange{key_id, op.block.src_offset,
                                                 op.block.src_len, out});
      };
      const uint64_t* o = nullptr;
      if (kind == 2) {
        o = &w.pre_off[i];
      } else {
        o = chunk_dedup_.find_dd(h, h2, n);
        if (!o && kind == 0) o = stream_dedup_.find(h, h2, n);
      }
      if (o) {
        ++blocks_deduped_;
        const uint64_t off = *o;
        add_range(off);
        w.fin_block[i] = off;
        continue;
      }
      const uint64_t off = plan(n);
      write(i, off);
      // Empty blocks are never shared (a staged empty block is never deduped).
      if (n) {
        chunk_dedup_.add_dd(h, h2, n, off);
        w.pend_dd.push_back(WorkerCtx::PendDedup{h, h2, n, off});
      }
      add_range(off);
      w.fin_block[i] = off;
    }

    // P3
    if (w.pend_dd.empty() && w.pend_rg.empty()) return;
    const std::function<void(size_t)> p3 = [&](size_t t) {
      for (const WorkerCtx::PendDedup& d : w.pend_dd) {
        const size_t s = ShardedStreamDedup::ShardOf(d.h);
        if (s % T == t) stream_dedup_.shards[s].insert(d.h, d.h2, d.n, d.off);
      }
      for (const WorkerCtx::PendRange& r : w.pend_rg) {
        const size_t s = ShardedSourceRanges::ShardOf(r.id, r.off, r.len);
        if (s % T == t)
          stream_source_ranges_.shards[s].emplace(r.id, r.off, r.len, r.out);
      }
    };
    team.Run(p3);
  }

  // Streaming-mode stage W: write the blocks stage B planned, in op order
  // (chunk order across chunks), at exactly the planned offsets.
  void CommitStageW(WorkerCtx& w) {
    for (size_t j = 0; j < w.wjob_op.size(); ++j) {
      const DataBlock& b = w.ops[w.wjob_op[j]].block;
      const uint64_t off = StreamRawBlock(b.bytes(), b.size());
      if (off != w.wjob_off[j]) stream_plan_mismatch_ = true;
      if (b.is_ref()) DropStreamedSourceRange(b);
    }
  }

  void CommitStageC(WorkerCtx& w) {
    const ChunkFin fin{&w.fin_tok, &w.fin_str, &w.fin_path, &w.fin_block};
    // 4. fields (token domain + rep remap)
    const size_t fields_before = fields_.tokens.size();
    {
      std::vector<uint32_t>& toks = w.fields.tokens;
      for (uint32_t& t : toks) {
        if (t >= w_base_tok) t = w.fin_tok[t - w_base_tok];
      }
      std::vector<ValueRep>& reps = w.fields.reps;
      for (ValueRep& r : reps) r = RemapValueRep(r, fin);
      fields_.tokens.insert(fields_.tokens.end(), toks.begin(), toks.end());
      fields_.reps.insert(fields_.reps.end(), reps.begin(), reps.end());
    }

    // 5. fieldsets (rebase local flat ids by the fields appended so far)
    const size_t fs_before = fieldset_offsets_.size();
    {
      const size_t flat_before = flat_fieldsets_.size();
      for (uint32_t& v : w.flat) {
        if (v != UINT32_MAX) v += static_cast<uint32_t>(fields_before);
      }
      // w.fs_offsets entries are uint32_t slot offsets into w.flat.
      for (uint32_t& v : w.fs_offsets)
        v += static_cast<uint32_t>(flat_before);
      fieldset_offsets_.insert(fieldset_offsets_.end(), w.fs_offsets.begin(),
                               w.fs_offsets.end());
      flat_fieldsets_.insert(flat_fieldsets_.end(), w.flat.begin(), w.flat.end());
    }

    // 6. specs (path domain remap, fieldset rebase)
    // NOTE: no per-chunk reserve() here -- libstdc++'s reserve() allocates the
    // exact request, so reserving size+delta every chunk would re-copy the
    // whole specs_ vector per chunk (quadratic). push_back grows geometrically.
    for (const CrateSpec& s : w.specs) {
      CrateSpec c = s;
      const uint32_t lp = c.path_index.value;
      if (lp >= w_base_path) c.path_index.value = w.fin_path[lp - w_base_path];
      c.fieldset_index.value += static_cast<uint32_t>(fs_before);
      specs_.push_back(c);
    }

    // 7. counters
    if (w.req_minor > required_version_minor_)
      required_version_minor_ = w.req_minor;
    arrays_passed_through_ += w.passed;
    arrays_reencoded_ += w.reencoded;
  }

  // Free the per-thread ctx pointers before the ctx storage goes away.
  static void ReleaseWorkerTLS() { WorkerTLS() = nullptr; }

  void BuildFieldsChunk(
      const Layer& layer, size_t chunk,
      const std::unordered_map<std::string,
          std::map<std::string, std::vector<std::string>>>& variant_names,
      const std::unordered_map<std::string, std::vector<std::string>>&
          parent_children) {
    const size_t nprims = layer.prims().size();
    const size_t lo = chunk * kFieldsChunkPrims;
    const size_t hi = std::min(nprims, lo + kFieldsChunkPrims);
    for (size_t i = lo; i < hi; ++i) {
      FillForWrite(layer, i);
      BuildOnePrim(layer, i, variant_names, parent_children);
      ReleaseAfterWrite(layer, i);
      if (WHaveError()) return;
    }
  }

  // Parallel fields_specs build: W workers encode contiguous prim chunks into
  // private WorkerCtx buffers; the main thread commits chunks strictly in
  // index order as they finish. Returns true when the build was completed (or
  // failed fatally after >= 1 committed chunk: result_.error is set); returns
  // false to let the caller fall back to the serial loop (no chunk had been
  // committed yet, so the global tables are untouched).
  bool BuildFieldsAndSpecsParallel(
      const Layer& layer,
      const std::unordered_map<std::string,
          std::map<std::string, std::vector<std::string>>>& variant_names,
      const std::unordered_map<std::string, std::vector<std::string>>&
          parent_children) {
    const size_t nprims = layer.prims().size();
    const size_t nchunks =
        (nprims + kFieldsChunkPrims - 1) / kFieldsChunkPrims;
    if (nchunks == 0) return false;
    struct ReapScope {
      bool& flag;
      ReapScope(bool& f, bool on) : flag(f) { flag = on; }
      ~ReapScope() { flag = false; }
    } reap_scope(reap_prims_, consume_prims_);

#if !defined(LIGHTUSD_ENABLE_THREAD)
    (void)variant_names;  // only the worker path emits per-chunk specs
    (void)parent_children;
    return false;
#else
    // Each worker holds up to one encoded chunk until its commit finishes, a
    // roughly fixed per-worker cost: 16 workers pay off only on large layers
    // (district: fields_specs 3.1 -> 2.5 s from 8 to 16), while on a small one
    // (merged ALab, ~24 chunks) they just add ~50 MB of peak RSS. A batched
    // (low-memory) write stays narrow: Island's in-flight chunks hold 0.6 GB
    // when wide, and full45's batches are written under the next compose.
    int want = ResolveThreads();
    const bool wide = nchunks >= kChunksForWideFields && !serial_sections_;
    const int max_workers = wide ? 16 : 8;
    if (want > max_workers) want = max_workers;
    if (want > static_cast<int>(nchunks)) want = static_cast<int>(nchunks);
    if (want <= 1) return false;
    const size_t W = static_cast<size_t>(want);

    // ctx pool (stable addresses). A worker takes any free ctx for its next
    // chunk; commit stage C returns it. With the commit pipelined over
    // several stages a chunk stays in flight a while after it is built, so
    // wide (large-layer) runs get 2 ctx per worker -- with one, workers spent
    // more time parked than encoding. Small layers keep one per worker (each
    // ctx holds an encoded chunk: memory).
    const size_t P = wide ? 2 * W : W;
    std::unique_ptr<WorkerCtx[]> ctx(new WorkerCtx[P]);
    std::vector<size_t> free_ctx;
    free_ctx.reserve(P);
    for (size_t c = P; c-- > 0;) free_ctx.push_back(c);
    // Stage A2's shard team (the commit thread + helpers).
    ShardTeam path_team;
    path_team.Start(std::min<size_t>(4, static_cast<size_t>(ResolveThreads())));
    // Streaming stage B's team; stage W then writes the planned blocks.
    ShardTeam block_team;
    if (stream_values_)
      block_team.Start(std::min<size_t>(4, static_cast<size_t>(ResolveThreads())));
    stream_plan_pos_ = value_stream_pos_;
    stream_plan_mismatch_ = false;
    for (size_t c = 0; c < P; ++c) ctx[c].Reset();


    std::vector<uint8_t> done(nchunks, 0);  // 0 building, 1 ready, 3 error
    std::vector<int> owner_slot(nchunks, -1);
    std::atomic<size_t> next_take{0};
    std::mutex mut;
    std::condition_variable cv;
    bool fatal = false;         // set by the commit loop; releases all waiters
    bool fallback = false;      // fatal with zero chunks committed
    // Chunks fully through commit stage A2 / A1 / B / C (per-stage order is
    // strict).
    size_t a_done = 0, a1_done = 0, b_done = 0, w_done = 0, c_done = 0;

    {
      std::vector<std::thread> threads;
      threads.reserve(W + 4);
      for (size_t t = 0; t < W; ++t) {
        threads.emplace_back([this, &layer, &variant_names, &parent_children,
                              &ctx, &free_ctx, &done, &owner_slot, &next_take,
                              &mut, &cv, &fatal, nchunks] {
          for (;;) {
            if (next_take.load() >= nchunks) break;  // no chunk left to take
            size_t c;
            {
              // Bound transient memory: wait for a ctx the commit released.
              std::unique_lock<std::mutex> lk(mut);
              cv.wait(lk, [&] { return fatal || !free_ctx.empty(); });
              if (fatal) break;
              c = free_ctx.back();
              free_ctx.pop_back();
            }
            const size_t i = next_take.fetch_add(1);
            if (i >= nchunks) {
              std::lock_guard<std::mutex> lk(mut);
              free_ctx.push_back(c);
              break;
            }
            WorkerTLS() = &ctx[c];
            ctx[c].Reset();
            BuildFieldsChunk(layer, i, variant_names, parent_children);
            BucketPathEvents(ctx[c]);
            {
              std::lock_guard<std::mutex> lk(mut);
              done[i] = ctx[c].error.empty() ? 1 : 3;
              owner_slot[i] = static_cast<int>(c);
            }
            cv.notify_all();
          }
          ReleaseWorkerTLS();
        });
      }

      // consume_prims: committed chunks are released to their skeletons on
      // reaper threads, off the encoders' path.
      constexpr size_t kReapers = 2;
      if (reap_prims_) {
        for (size_t r = 0; r < kReapers; ++r) {
          threads.emplace_back([&, r] {
            for (size_t k = r; k < nchunks; k += kReapers) {
              {
                std::unique_lock<std::mutex> lk(mut);
                // Committed: a fallback to the serial build (which
                // re-encodes every prim) happens only before any commit.
                cv.wait(lk, [&] { return fatal || c_done > k; });
                if (fatal) return;
              }
              const size_t lo = k * kFieldsChunkPrims;
              const size_t hi = std::min(nprims, lo + kFieldsChunkPrims);
              for (size_t i = lo; i < hi; ++i)
                const_cast<PrimSpec&>(layer.prims()[i]).release_to_skeleton();
            }
          });
        }
      }

      // Commit stages A1, B and C trail chunk readiness / the stage before
      // them chunk by chunk on their own threads; stage A2 runs below on this
      // thread and owns the error handling. Each stage owns its slice of the
      // writer state (see CommitStageA1).
      auto follow = [&](auto ready, auto run, size_t* my_done) {
        for (size_t k = 0; k < nchunks; ++k) {
          int slot;
          {
            std::unique_lock<std::mutex> lk(mut);
            cv.wait(lk, [&] { return fatal || ready(k); });
            if (fatal || done[k] == 3) return;
            slot = owner_slot[k];
          }
          run(ctx[slot]);
          {
            std::lock_guard<std::mutex> lk(mut);
            ++*my_done;
            // The last stage is done with this ctx: back to the pool.
            if (my_done == &c_done) free_ctx.push_back(static_cast<size_t>(slot));
          }
          cv.notify_all();
        }
      };
      threads.emplace_back([&] {
        follow([&](size_t k) { return done[k] != 0; },
               [&](WorkerCtx& w) { CommitStageA1(w); }, &a1_done);
      });
      threads.emplace_back([&] {
        follow([&](size_t k) { return a1_done > k && a_done > k; },
               [&](WorkerCtx& w) {
                 if (stream_values_) CommitStageBStream(w, block_team);
                 else CommitStageB(w);
               },
               &b_done);
      });
      // W follows B; C follows W so a worker's ctx (whose blocks W writes) is
      // reused only after both.
      threads.emplace_back([&] {
        follow([&](size_t k) { return b_done > k; },
               [&](WorkerCtx& w) {
                 if (stream_values_) CommitStageW(w);
               },
               &w_done);
      });
      threads.emplace_back([&] {
        follow([&](size_t k) { return w_done > k; },
               [&](WorkerCtx& w) { CommitStageC(w); }, &c_done);
      });

      {
        std::unique_lock<std::mutex> lk(mut);
        while (a_done < nchunks) {
          const size_t k = a_done;
          cv.wait(lk, [&] { return fatal || done[k] != 0; });
          if (fatal) break;
          if (done[k] == 3) {
            if (a_done > 0) {
              if (result_.error.empty()) {
                result_.error =
                    ctx[owner_slot[k]].error.empty()
                        ? "Crate parallel fields build failed"
                        : ctx[owner_slot[k]].error;
              }
            } else {
              fallback = true;  // global tables still pristine
            }
            fatal = true;
            cv.notify_all();
            break;
          }
          // Leave the lock during the stage: the owner worker stays parked
          // until stage C is done with this chunk, A1 only reads the chunk's
          // intern events, and B/C reach it only after a_done passes it.
          const int slot = owner_slot[k];
          lk.unlock();
          CommitStageA2(ctx[slot], path_team);
          lk.lock();
          ++a_done;
          cv.notify_all();
        }
      }
      cv.notify_all();
      for (std::thread& th : threads) th.join();
      path_team.Stop();
      block_team.Stop();
    }
    ReleaseWorkerTLS();

    if (stream_values_ && !fallback && result_.error.empty() &&
        (stream_plan_mismatch_ || stream_plan_pos_ != value_stream_pos_)) {
      result_.error = "Crate parallel VALUE stream plan mismatch";
    }
    return !fallback;
#endif
  }

