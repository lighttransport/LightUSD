// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "crate-reader-internal.hh"
#include "../layer/array-edit.hh"
#include "../types/type-info.hh"
#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lightusd {
namespace next {
using namespace crate_stage_detail;

void CrateReader::Impl::EmitPrimBody(
    PrimBuildState& state, size_t entry_index,
    std::vector<std::pair<std::string_view, Value>>& prim_value_field_scratch,
    [[maybe_unused]] const ReportFn& report) {
  auto& layer = state.layer;
  auto& prim_entries = state.prim_entries;
  auto& entry_prim = state.entry_prim;
  auto& children_order = state.children_order;
  auto& variant_sel = state.variant_sel;
  auto& variant_opts = state.variant_opts;
  auto& attr_refs = state.attr_refs;
  auto& rel_refs = state.rel_refs;
  auto& attr_off = state.attr_off;
  auto& rel_off = state.rel_off;
    auto& entry = prim_entries[entry_index];
    if (entry_prim[entry_index] == kSkipEntry) {
      AddWarning("Ignoring malformed non-absolute prim path: " +
                 std::string(entry.full_path));
      return;
    }
    PrimSpec* ps = layer.prim(entry_prim[entry_index]);

    // Extracts a token-list metadata field (written as a token array, with
    // single-token / string fallbacks for older encodings). Warns rather than
    // silently dropping a known arc field that fails to decode.
    auto append_token_list = [&](const Value& v, std::vector<std::string>& dst,
                                 const char* field_name) {
      if (auto arr = v.as_token_array()) {
        // Marker-aware: keep prepend/append sublist items, skip
        // deleted/ordered sublists, strip the markers themselves.
        bool keep = true;
        for (const auto& s : *arr) {
          if (!s.empty() && s[0] == '\x01') {
            keep = !(s == "\x01" "D" || s == "\x01" "O");
            continue;
          }
          if (keep) dst.push_back(s);
        }
      } else if (auto s = v.as_token()) {
        dst.push_back(*s);
      } else if (auto s = v.as_string()) {
        dst.push_back(*s);
      } else {
        AddWarning(std::string("Composition arc field '") + field_name +
                   "' has unexpected encoding; dropped");
      }
    };

    // Phase 7 S5: decode a `<arc>_listOp` companion token[] (marker-delimited
    // prepend/append/delete/order sublists) into an ArcListOp.
    auto decode_arc_listop = [&](const Value& v, ArcListOp& e) {
      auto arr = v.as_token_array();
      if (!arr) return;
      // "\x01E" alone = authored explicit-clear (`references = None` /
      // pxr's explicit-empty listop): authored with is_explicit kept true
      // and no items, which the USDA writer re-emits as `= None`.
      if (arr->size() == 1 && ((*arr)[0] == "\x01" "E" || (*arr)[0] == "\x01E")) {
        e = ArcListOp();
        e.authored = true;
        return;
      }
      e.authored = true;
      e.is_explicit = false;
      std::vector<std::string>* cur = nullptr;
      for (const std::string& s : *arr) {
        // Both marker spellings are accepted: the correct two-byte form
        // ("\x01" "A") and the legacy single-byte form (in C++, "\x01A"
        // parses as the one char 0x1A because 'A' is a hex digit) written by
        // older next crates.
        if (s == "\x01" "G") cur = &e.added;
        else if (s == "\x01" "P" || s == "\x01P") cur = &e.prepended;
        else if (s == "\x01" "A" || s == "\x1A") cur = &e.appended;
        else if (s == "\x01" "D" || s == "\x1D") cur = &e.deleted;
        else if (s == "\x01" "O" || s == "\x01O") cur = &e.ordered;
        else if (s == "\x01" "N" || s == "\x01N") cur = nullptr;
        else if (cur) cur->push_back(s);
      }
    };

    ResolveFieldset(entry.fieldset_index, prim_value_field_scratch);

    // Add properties and extract composition arcs / metadata into PrimMetadata
    for (auto& field : prim_value_field_scratch) {
      if (field.first == "typeName" || field.first == "specifier") continue;

      // Reserved prim metadata stored inline in the prim spec (pxrUSD keeps
      // these in the prim's fieldset, not as separate property specs). Route
      // them to PrimMetadata so they do not leak in as phantom properties.
      if (ps) {
        if (field.first == "active") {
          if (const bool* b = field.second.as_bool()) {
            ps->meta().active = *b;
            ps->meta().active_authored = true;
          }
          continue;
        }
        if (field.first == "hidden") {
          if (const bool* b = field.second.as_bool()) {
            ps->meta().hidden = *b;
            ps->meta().hidden_authored = true;
          }
          continue;
        }
        if (field.first == "documentation" || field.first == "doc") {
          if (auto s = field.second.as_string()) {
            ps->meta().doc() = *s;
            ps->meta().set_doc_authored();
          } else if (auto s = field.second.as_token()) {
            ps->meta().doc() = *s;
            ps->meta().set_doc_authored();
          }
          continue;
        }
      }

      // Composition arc + metadata fields: store in PrimMetadata, not as
      // regular properties. (Guarded by ps; if current() were null we simply
      // skip them rather than crash.)
      if (ps) {
        if (field.first == "primOrder") {
          ps->meta().setPrimOrderAuthored();
          if (auto names =
                  field.second.as_token_array()) {
            ps->meta().editPrimOrder().assign(names->begin(), names->end());
          }
          continue;
        }
        if (field.first == "propertyOrder") {
          ps->meta().setPropertyOrderAuthored();
          if (auto names =
                  field.second.as_token_array()) {
            ps->meta().editPropertyOrder().assign(names->begin(), names->end());
          }
          continue;
        }
        if (field.first == "displayGroupOrder") {
          if (auto names =
                  field.second.as_token_array()) {
            ps->meta().editDisplayGroupOrder().assign(names->begin(), names->end());
            ps->meta().setDisplayGroupOrderAuthored();
          }
          continue;
        }
        if (field.first == "apiSchemas") {
          ps->meta().setApiSchemasAuthored();
          StringListOps& edits = ps->meta().apiSchemaEdits();
          edits = StringListOps();
          edits.authored = true;
          if (auto arr =
                  field.second.as_token_array()) {
            if (arr->size() == 1 && (*arr)[0] == "\x01" "E") {
              edits.is_explicit = true;
            } else if (!arr->empty() && !(*arr)[0].empty() &&
                       (*arr)[0][0] == '\x01') {
              edits.is_explicit = false;
              std::vector<std::string>* current = nullptr;
              for (const std::string& item : *arr) {
                if (item == "\x01" "G") current = &edits.added;
                else if (item == "\x01" "P") current = &edits.prepended;
                else if (item == "\x01" "A") current = &edits.appended;
                else if (item == "\x01" "D") current = &edits.deleted;
                else if (item == "\x01" "O") current = &edits.ordered;
                else if (!item.empty() && item[0] == '\x01') current = nullptr;
                else if (current) current->push_back(item);
              }
            } else {
              edits.is_explicit = true;
              edits.explicit_items.assign(arr->begin(), arr->end());
            }
          } else {
            edits.is_explicit = true;
            append_token_list(field.second, edits.explicit_items, "apiSchemas");
          }

          std::vector<std::string> applied = edits.is_explicit
              ? edits.explicit_items
              : edits.added;
          if (!edits.is_explicit) {
            applied.insert(applied.begin(), edits.prepended.begin(),
                           edits.prepended.end());
            applied.insert(applied.end(), edits.appended.begin(),
                           edits.appended.end());
            std::vector<std::string> ordered;
            for (const std::string& schema : edits.ordered) {
              auto it = std::find(applied.begin(), applied.end(), schema);
              if (it != applied.end()) {
                ordered.push_back(*it);
                applied.erase(it);
              }
            }
            ordered.insert(ordered.end(), applied.begin(), applied.end());
            applied = std::move(ordered);
          }
          ps->meta().apiSchemas() = std::move(applied);
          if (!edits.prepended.empty()) {
            ps->meta().apiSchemasQualifier() = "prepend";
          } else if (!edits.appended.empty() || !edits.added.empty()) {
            ps->meta().apiSchemasQualifier() = "append";
          } else if (!edits.deleted.empty()) {
            ps->meta().apiSchemasQualifier() = "delete";
          }
          continue;
        }
        // Composition arc lists. A marker-delimited token array (first entry
        // "\x01?") is a non-explicit list-op decoded from the native crate
        // encoding: reconstruct the authored edits and derive the effective
        // (within-spec) list; otherwise it is a plain explicit list.
        auto is_marker_list = [](const Value& v) {
          auto arr = v.as_token_array();
          return arr && !arr->empty() && !(*arr)[0].empty() &&
                 (*arr)[0][0] == '\x01';
        };
        auto apply_arc_field = [&](const Value& v,
                                   std::vector<std::string>& target,
                                   ArcListOp& (*sel)(ArcListOps&),
                                   const char* what) {
          if (is_marker_list(v)) {
            ArcListOp& e = sel(ps->meta().ensure_arc_edits());
            decode_arc_listop(v, e);
            // Effective list within a single spec: prepended then appended
            // (delete/reorder operate on weaker opinions, not this list).
            target = e.prepended;
            for (const std::string& item : e.added) {
              if (std::find(target.begin(), target.end(), item) == target.end())
                target.push_back(item);
            }
            target.insert(target.end(), e.appended.begin(), e.appended.end());
          } else {
            append_token_list(v, target, what);
          }
        };
        if (field.first == "references") {
          apply_arc_field(field.second, ps->meta().references,
                          [](ArcListOps& a) -> ArcListOp& { return a.references; },
                          "references");
          continue;
        }
        if (field.first == "payload") {
          apply_arc_field(field.second, ps->meta().payloads,
                          [](ArcListOps& a) -> ArcListOp& { return a.payloads; },
                          "payload");
          continue;
        }
        if (field.first == "inherits" || field.first == "inheritPaths") {
          // pxr's crate field name is "inheritPaths"; next's own writer used
          // "inherits". Accept both.
          apply_arc_field(field.second, ps->meta().inherits,
                          [](ArcListOps& a) -> ArcListOp& { return a.inherits; },
                          "inherits");
          continue;
        }
        if (field.first == "specializes") {
          apply_arc_field(field.second, ps->meta().specializes,
                          [](ArcListOps& a) -> ArcListOp& { return a.specializes; },
                          "specializes");
          continue;
        }
        if (field.first == "references_listOp") {
          decode_arc_listop(field.second, ps->meta().ensure_arc_edits().references);
          continue;
        }
        if (field.first == "payload_listOp") {
          decode_arc_listop(field.second, ps->meta().ensure_arc_edits().payloads);
          continue;
        }
        if (field.first == "inherits_listOp") {
          decode_arc_listop(field.second, ps->meta().ensure_arc_edits().inherits);
          continue;
        }
        if (field.first == "specializes_listOp") {
          decode_arc_listop(field.second,
                            ps->meta().ensure_arc_edits().specializes);
          continue;
        }
        if (field.first == "variantSelection") {
          if (auto s = field.second.as_token())
            ps->meta().variantSelection = *s;
          else if (auto s = field.second.as_string())
            ps->meta().variantSelection = *s;
          continue;
        }
        if (field.first == "comment") {
          if (auto s = field.second.as_token())
            ps->meta().comment() = *s;
          else if (auto s = field.second.as_string())
            ps->meta().comment() = *s;
          ps->meta().set_comment_authored();
          continue;
        }
        if (field.first == "kind") {
          if (auto s = field.second.as_token())
            ps->meta().kind() = *s;
          else if (auto s = field.second.as_string())
            ps->meta().kind() = *s;
          ps->meta().setKindAuthored();
          continue;
        }
        if (field.first == "permission") {
          if (auto s = field.second.as_token())
            ps->meta().permission() = *s;
          else if (auto s = field.second.as_string())
            ps->meta().permission() = *s;
          continue;
        }
        if (field.first == "displayName") {
          if (auto s = field.second.as_string())
            ps->meta().displayName() = *s;
          else if (auto s = field.second.as_token())
            ps->meta().displayName() = *s;
          ps->meta().setDisplayNameAuthored();
          continue;
        }
        if (field.first == "relocates") {
          ps->meta().setRelocatesAuthored();
          if (auto pairs =
                  field.second.as_token_array()) {
            for (size_t i = 0; i + 1 < pairs->size(); i += 2) {
              ps->meta().relocates().emplace_back((*pairs)[i],
                                                  (*pairs)[i + 1]);
            }
          }
          continue;
        }
        if (field.first == "instanceable") {
          if (const bool* b = field.second.as_bool()) {
            ps->meta().instanceable = *b;
            ps->meta().instanceable_authored = true;
          }
          continue;
        }
        if (field.first == "clipSets") {
          StringListOps& edits = ps->meta().clipSetEdits();
          edits = StringListOps();
          edits.authored = true;
          if (auto arr =
                  field.second.as_token_array()) {
            if (arr->size() == 1 && (*arr)[0] == "\x01" "E") {
              edits.is_explicit = true;
            } else if (!arr->empty() && !(*arr)[0].empty() &&
                       (*arr)[0][0] == '\x01') {
              std::vector<std::string>* current = nullptr;
              for (const std::string& item : *arr) {
                if (item == "\x01" "G") current = &edits.added;
                else if (item == "\x01" "P") current = &edits.prepended;
                else if (item == "\x01" "A") current = &edits.appended;
                else if (item == "\x01" "D") current = &edits.deleted;
                else if (item == "\x01" "O") current = &edits.ordered;
                else if (!item.empty() && item[0] == '\x01') current = nullptr;
                else if (current) current->push_back(item);
              }
            } else {
              edits.is_explicit = true;
              edits.explicit_items.assign(arr->begin(), arr->end());
            }
          } else {
            edits.is_explicit = true;
            append_token_list(field.second, edits.explicit_items, "clipSets");
          }
          continue;
        }
        if (field.first == "customData" || field.first == "assetInfo" ||
            field.first == "sdrMetadata" || field.first == "clips") {
          // pxr-authored: a binary VtDictionary (already decoded to a Dictionary
          // Value). next-authored: USDA dict text in a String field.
          Value d;
          if (field.second.is_dictionary()) {
            d = std::move(field.second);
          } else if (auto s = field.second.as_string()) {
            d = ParseDictText(*s);
          } else if (auto s = field.second.as_token()) {
            d = ParseDictText(*s);
          }
          if (d.is_dictionary()) {
            if (field.first == "customData") {
              ps->meta().customData() = std::move(d);
              ps->meta().setCustomDataAuthored();
            } else if (field.first == "assetInfo") {
              ps->meta().assetInfo() = std::move(d);
              ps->meta().setAssetInfoAuthored();
            } else if (field.first == "sdrMetadata") {
              ps->meta().sdrMetadata() = std::move(d);
              ps->meta().setSdrMetadataAuthored();
            } else {
              ps->meta().clips() = std::move(d);
              ps->meta().setClipsAuthored();
            }
          }
          continue;
        }
        if (field.first == "variantSets" ||
            field.first == "variantSetNames") {
          // The AUTHORED `variantSets` declaration (`variantSetNames` is pxr's
          // StringListOp form). Reconstruct name-only entries; the selection /
          // holder content is grafted on later. Strip any list-op markers a
          // StringListOp carries so a bare set name survives. Skip sets already
          // present (e.g. from holder specs) to avoid duplicates.
          StringListOps& edits = ps->meta().variantSetNameEdits();
          edits = StringListOps();
          edits.authored = true;
          if (auto arr =
                  field.second.as_token_array()) {
            if (arr->size() == 1 && (*arr)[0] == "\x01" "E") {
              edits.is_explicit = true;
            } else if (!arr->empty() && !(*arr)[0].empty() &&
                       (*arr)[0][0] == '\x01') {
              edits.is_explicit = false;
              std::vector<std::string>* current = nullptr;
              for (const std::string& item : *arr) {
                if (item == "\x01" "G") current = &edits.added;
                else if (item == "\x01" "P") current = &edits.prepended;
                else if (item == "\x01" "A") current = &edits.appended;
                else if (item == "\x01" "D") current = &edits.deleted;
                else if (item == "\x01" "O") current = &edits.ordered;
                else if (!item.empty() && item[0] == '\x01') current = nullptr;
                else if (current) current->push_back(item);
              }
            } else {
              edits.is_explicit = true;
              edits.explicit_items.assign(arr->begin(), arr->end());
            }
          } else {
            edits.is_explicit = true;
            append_token_list(field.second, edits.explicit_items,
                              "variantSets");
          }

          std::vector<std::string> names = edits.is_explicit
              ? edits.explicit_items
              : edits.added;
          if (!edits.is_explicit) {
            names.insert(names.begin(), edits.prepended.begin(),
                         edits.prepended.end());
            names.insert(names.end(), edits.appended.begin(),
                         edits.appended.end());
            std::vector<std::string> ordered_names;
            for (const std::string& ordered : edits.ordered) {
              auto it = std::find(names.begin(), names.end(), ordered);
              if (it != names.end()) {
                ordered_names.push_back(*it);
                names.erase(it);
              }
            }
            ordered_names.insert(ordered_names.end(), names.begin(), names.end());
            names = std::move(ordered_names);
          }
          for (const std::string& n : names) {
            bool have = false;
            for (const PrimVariantSetData& evs : ps->meta().variantSets()) {
              if (evs.name == n) { have = true; break; }
            }
            if (have) continue;
            PrimVariantSetData vsd;
            vsd.name = n;
            ps->meta().variantSets().push_back(std::move(vsd));
          }
          continue;
        }
        if (field.first == "__lightusd_unknownMeta") {
          // lightusd-private field: length-prefixed (key, raw-value) pairs of
          // unmodeled prim metadata the parser preserved (see the writer).
          auto blob = field.second.as_string();
          if (!blob) blob = field.second.as_token();
          if (blob) {
            size_t p = 0;
            auto read_chunk = [&](std::string& out) -> bool {
              size_t colon = blob->find(':', p);
              if (colon == std::string::npos) return false;
              size_t len = 0;
              for (size_t i = p; i < colon; ++i) {
                if ((*blob)[i] < '0' || (*blob)[i] > '9') return false;
                len = len * 10 + static_cast<size_t>((*blob)[i] - '0');
                // Cap: a chunk cannot exceed the blob. Prevents the multiply and
                // the bounds check from overflowing on a hostile length (which
                // would wrap past the check and re-parse forever -> DoS).
                if (len > blob->size()) return false;
              }
              if (len > blob->size() - (colon + 1)) return false;  // cannot wrap
              out = blob->substr(colon + 1, len);
              p = colon + 1 + len;  // colon >= p, so p strictly increases
              return true;
            };
            std::string key, val;
            while (p < blob->size() && read_chunk(key) && read_chunk(val)) {
              ps->meta().unknownMeta().emplace_back(std::move(key),
                                                    std::move(val));
            }
          }
          continue;
        }
      }
      // A loose sibling "variability" field cannot be re-associated with a
      // property here; consume it so it does not surface as a stray property.
      // Per-property variability is decoded above through Attribute/Relationship
      // specs and preserved as kFlagUniform.
      if (field.first == "variability") continue;

      // Reserved Sdf children-key ordering fields. pxrUSD stores prim/property
      // order in these (SdfChildrenKeys); USDA flatten output does NOT emit them
      // as body attributes -- order is implicit in the authored child/property
      // sequence. Composition derives child order from child_indices() directly
      // (see cache.cc ComposeInto), so consume these so they do not leak as
      // phantom `token[] primChildren`/`properties` attributes.
      if (field.first == "primChildren") {
        // Authored sibling order: the hierarchy below is built in path-sorted
        // order, so capture the order here and restore the child links after
        // finalize (see the prim_children_order pass). Do NOT also set
        // primOrder()/rootPrimOrder: pxr writes `primChildren` on every prim as
        // the natural order and preserves it purely by emission order, never
        // by re-emitting a `reorder nameChildren`/`reorder rootPrims`
        // statement. Restoring child_indices already reproduces that emission
        // order; setting primOrder() here made every USDC->USDA round trip gain
        // a spurious reorder statement pxr never emits.
        if (auto names =
                field.second.as_token_array()) {
          if (!names->empty()) {
            children_order[entry_index].assign(names->begin(), names->end());
          }
        }
        continue;
      }
      if (field.first == "properties" ||
          field.first == "propertyChildren") {
        // pxr writes the `properties` field (natural spec order) for EVERY
        // prim; it is not an authored `reorder properties`. Synthesizing a
        // propertyOrder from it made every USDC->USDA round trip gain a
        // spurious `reorder properties` statement that pxr's own round trip
        // never emits (pxr drops the order and sorts). So consume the field
        // without pinning an order. Authored USDA `reorder properties` still
        // round-trips through the USDA path (parser -> writer); like pxr, it
        // is not preserved through a crate round trip.
        continue;
      }
      if (field.first == "variantChildren" ||
          field.first == "variantSetChildren") {
        continue;
      }

      // `variantSetNames` (the declared list of variant-set names) likewise must
      // not leak as a phantom `string[] variantSetNames` body property. The
      // compositor drives variants from the bracketed-holder specs + the
      // variantSelection (not from this list), and flatten output drops variant
      // metadata entirely (see cache.cc ComposeInto), so consume it.
      if (field.first == "variantSetNames") {
        continue;
      }

      // Unknown Prim fields are extension metadata, not body properties.
      // Preserve the decoded typed value and re-emit it as the same field.
      if (ps) {
        ps->meta().unknownFields().push_back(
            MakeTypedExtensionField(std::string(field.first),
                                    std::move(field.second)));
      }
    }

    // Attach separate Attribute / Relationship specs that belong to this prim.
    // The dedup guard keeps any inline field authoritative.
    if (ps) {
      for (uint32_t k = attr_off[entry_index]; k < attr_off[entry_index + 1];
           ++k) {
        AttrInfo& ai = *attr_refs[k];
        const PropNameId nid = GetPropNameTable().intern(ai.name);
        if (ps->property(nid)) continue;  // inline opinion wins
        uint16_t flags = 0;
        if (ai.uniform) flags |= PropSlot::kFlagUniform;
        if (ai.custom) flags |= PropSlot::kFlagCustom;
        if (ai.is_connection) flags |= PropSlot::kFlagConnection;
        // The writer gates timeSamples emission on the slot flag, so mark a
        // time-sampled attribute (the value lives in add_time_sample below).
        if (!ai.time_samples.empty()) flags |= PropSlot::kFlagTimeSampled;
        const bool is_array =
            ai.type_name.size() >= 2 &&
            ai.type_name.compare(ai.type_name.size() - 2, 2, "[]") == 0;
        if (ai.has_default) {
          ps->add_property(nid, std::move(ai.default_value), flags);
        } else if (ai.has_array_edit) {
          // Sparse array-edit default: a typed slot with the structured edit
          // and its canonical `edit [...]` text (usda writer re-emits it;
          // composition stacks/resolves it).
          uint16_t edit_flags = flags;
          if (is_array) edit_flags |= PropSlot::kFlagArray;
          std::string base_tn = is_array
              ? ai.type_name.substr(0, ai.type_name.size() - 2)
              : ai.type_name;
          ps->add_property_slot(nid,
                                GetTypeIdFromName(base_tn.c_str()),
                                edit_flags);
          ps->set_raw_default_source(ai.name,
                                     BuildArrayEditText(ai.cold->array_edit));
          ps->set_array_edit(ai.name, std::move(ai.cold->array_edit));
        } else {
          // Connection-only / declared-only / timeSamples-only attribute:
          // register a typed slot with no authored default so it round-trips.
          if (is_array) flags |= PropSlot::kFlagArray;
          std::string base = is_array
              ? ai.type_name.substr(0, ai.type_name.size() - 2)
              : ai.type_name;
          TypeId tid = GetTypeIdFromName(base.c_str());
          ps->add_property_slot(nid, tid, flags);
        }
        // Time samples (an attribute may have timeSamples with or without a
        // default).
        if (!ai.time_samples.empty()) {
          for (auto& ts : ai.time_samples) {
            ps->add_time_sample(nid, ts.first, std::move(ts.second));
          }
        }
        if (!ai.type_name.empty()) {
          ps->set_property_type_name(nid, GetTypeNameTable().intern(ai.type_name));
        }
        if (ai.is_connection && ai.cold->connection_targets.empty()) {
          ps->set_connection_block(ai.name);  // authored `.connect = None`
        }
        if (ai.cold && !ai.cold->spline_text.empty()) {
          ps->set_spline_source(ai.name, std::move(ai.cold->spline_text));
        }
        if (ai.cold) {
          for (const auto& t : ai.cold->connection_targets) {
            ps->add_connection(ai.name, Path(t));
          }
        }
        if (ai.is_connection) {
          ps->ensure_connection_edit(ai.name) =
              std::move(ai.cold->connection_edit);
        }
        // Per-property metadata (full PropMeta round-trip).
        if (ai.meta && ai.meta->authored != 0) {
          ps->ensure_property_meta(ai.name) = std::move(*ai.meta);
        }
        ai = AttrInfo();  // release the record's leftovers on this task
      }
    }
    if (ps) {
      for (uint32_t k = rel_off[entry_index]; k < rel_off[entry_index + 1];
           ++k) {
        RelInfo& ri = *rel_refs[k];
        // A target-less relationship stays a declared (empty) relationship;
        // relationships with targets push one Path per target.
        if (ri.targets.empty()) {
          if (!ps->relationship(ri.name)) {
            ps->set_relationship_targets(ri.name, {});
          }
        } else {
          for (const auto& t : ri.targets) ps->add_relationship(ri.name, Path(t));
        }
        if (ri.edits.authored) {
          ps->ensure_relationship_edit(ri.name) = std::move(ri.edits);
        }
        if (ri.custom) {
          ps->set_relationship_flags(
              ri.name, static_cast<uint16_t>(ps->relationship_flags(ri.name) |
                                             PropSlot::kFlagCustom));
        }
        if (ri.variability_authored) {
          uint16_t flags = PropSlot::kFlagVariabilityAuthored;
          if (ri.uniform) flags |= PropSlot::kFlagUniform;
          if (ri.varying) flags |= PropSlot::kFlagVarying;
          ps->set_relationship_flags(
              ri.name, static_cast<uint16_t>(ps->relationship_flags(ri.name) |
                                             flags));
        }
        if (ri.meta && ri.meta->authored != 0) {
          ps->ensure_property_meta(ri.name) = std::move(*ri.meta);
        }
        ri = RelInfo();  // release the record's leftovers on this task
      }
    }

    // Variant sets: attach a PrimVariantSetData per set, carrying the selection
    // and the option NAMES (as empty PrimVariantData entries — the compositor
    // ignores option entries without content/properties and grafts the
    // bracketed holder prims instead, which hold the variant CONTENT).
    // Most layers have no variant maps. Avoid allocating a lookup string for
    // every prim in those layers while retaining the owning variant records.
    const std::string variant_path = variant_sel.empty() && variant_opts.empty()
                                         ? std::string()
                                         : std::string(entry.full_path);
    auto sel = variant_sel.find(variant_path);
    auto opt = variant_opts.find(variant_path);
    if (ps && (sel != variant_sel.end() || opt != variant_opts.end())) {
      // Union of set names from selections and holder specs.
      std::map<std::string, PrimVariantSetData> sets;
      if (opt != variant_opts.end()) {
        for (const auto& kv : opt->second) {
          PrimVariantSetData& vsd = sets[kv.first];
          vsd.name = kv.first;
          for (const std::string& var : kv.second) {
            PrimVariantData vd;
            vd.name = var;
            vsd.variants.push_back(std::move(vd));
          }
        }
      }
      if (sel != variant_sel.end()) {
        for (const auto& kv : sel->second) {
          // Only stamp the selection onto a variant set that is actually
          // DEFINED here (present in variant_opts / an existing set). A
          // dangling selection — `variants = {set=sel}` with no local
          // variantSet definition (the variant set lives in a referenced
          // layer) — must NOT synthesize a variantSets declaration or an empty
          // `variantSet` block; it is recorded only in variantSelections()
          // below. pxr emits neither for such a prim.
          auto it = sets.find(kv.first);
          if (it != sets.end()) it->second.selected = kv.second;
        }
      }
      for (auto& kv : sets) {
        // Merge into a same-named entry when one already exists (older
        // next-authored crates carry a legacy `variantSets` token field that
        // pre-pushed name-only entries; pushing a second full entry would
        // duplicate the set — grafted twice and emitted twice as USDA).
        PrimVariantSetData* existing = nullptr;
        for (PrimVariantSetData& evs : ps->meta().variantSets()) {
          if (evs.name == kv.first) {
            existing = &evs;
            break;
          }
        }
        if (existing) {
          if (existing->selected.empty()) {
            existing->selected = kv.second.selected;
          }
          for (PrimVariantData& vd : kv.second.variants) {
            bool have = false;
            for (const PrimVariantData& evd : existing->variants) {
              if (evd.name == vd.name) {
                have = true;
                break;
              }
            }
            if (!have) existing->variants.push_back(std::move(vd));
          }
        } else {
          ps->meta().variantSets().push_back(std::move(kv.second));
        }
      }
      // Record ALL selections in the plural list (the single legacy string
      // can carry only one set; consumers that read it alone would lose
      // every selection after the first on a multi-set prim).
      if (sel != variant_sel.end()) {
        // Authored bit even for an explicit-empty selection dict.
        ps->meta().setVariantSelectionsAuthored();
        if (!sel->second.empty()) {
          for (const auto& kv : sel->second) {
            ps->meta().variantSelections().emplace_back(kv.first, kv.second);
          }
          const auto& first = *sel->second.begin();
          ps->meta().variantSelection = first.first + "=" + first.second;
        }
      }
    }
    // Pre-sort the property index here (per prim, on the body task) so the
    // serial Layer::finalize() sweep finds it already sorted. Idempotent: a
    // later body on the same prim (irregular entries) just re-sorts.
    ps->finalize_properties();
}

}  // namespace next
}  // namespace lightusd
