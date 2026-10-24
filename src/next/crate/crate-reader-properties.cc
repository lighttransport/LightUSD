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

// Set authored bits for recognized metadata, including malformed known fields.
// Return false only when the field name is not property metadata.
bool CrateReader::Impl::DecodePropMetaField(std::string_view name,
                                            ValueRep rep, PropMeta& pm) {
  auto tok_or_str = [&](std::string& dst, uint32_t bit) -> bool {
    Value v;
    if (!UnpackValue(rep, v)) return true;  // known field, bad value: eat it
    if (auto s = v.as_token()) { dst = *s; pm.authored |= bit; }
    else if (auto s = v.as_string()) { dst = *s; pm.authored |= bit; }
    return true;
  };
  auto dict_field = [&](Value& dst, uint32_t bit) -> bool {
    Value v;
    if (!UnpackValue(rep, v)) return true;
    if (v.is_dictionary()) { dst = std::move(v); pm.authored |= bit; }
    else if (auto s = v.as_string()) {
      Value d = ParseDictText(*s);
      if (d.is_dictionary()) { dst = std::move(d); pm.authored |= bit; }
    } else if (auto s = v.as_token()) {
      Value d = ParseDictText(*s);
      if (d.is_dictionary()) { dst = std::move(d); pm.authored |= bit; }
    }
    return true;
  };

  if (name == "interpolation") return tok_or_str(pm.interpolation, PropMeta::kInterpolation);
  if (name == "colorSpace") return tok_or_str(pm.colorSpace, PropMeta::kColorSpace);
  if (name == "renderType") return tok_or_str(pm.renderType, PropMeta::kRenderType);
  if (name == "connectability") return tok_or_str(pm.connectability, PropMeta::kConnectability);
  if (name == "outputName") return tok_or_str(pm.outputName, PropMeta::kOutputName);
  if (name == "bindMaterialAs") return tok_or_str(pm.bindMaterialAs, PropMeta::kBindMaterialAs);
  if (name == "kind") return tok_or_str(pm.kind, PropMeta::kKind);
  if (name == "permission") {
    return tok_or_str(pm.permission, PropMeta::kPermission);
  }
  if (name == "displayName") return tok_or_str(pm.displayName, PropMeta::kDisplayName);
  if (name == "displayGroup") return tok_or_str(pm.displayGroup, PropMeta::kDisplayGroup);
  if (name == "comment") return tok_or_str(pm.comment, PropMeta::kComment);
  if (name == "documentation" || name == "doc") return tok_or_str(pm.doc, PropMeta::kDoc);
  if (name == "elementSize") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const int32_t* i = v.as_int()) {
        pm.elementSize = *i;
        pm.authored |= PropMeta::kElementSize;
      }
    }
    return true;
  }
  if (name == "unauthoredValuesIndex") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const int32_t* i = v.as_int()) {
        pm.unauthoredValuesIndex = *i;
        pm.authored |= PropMeta::kUnauthoredIdx;
      }
    }
    return true;
  }
  if (name == "weight") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const double* d = v.as_double()) {
        pm.weight = *d;
        pm.authored |= PropMeta::kWeight;
      } else if (const float* f = v.as_float()) {
        pm.weight = *f;
        pm.authored |= PropMeta::kWeight;
      }
    }
    return true;
  }
  if (name == "hidden") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (const bool* b = v.as_bool()) {
        pm.hidden = *b;
        pm.authored |= PropMeta::kHidden;
      }
    }
    return true;
  }
  if (name == "allowedTokens") {
    Value v;
    if (UnpackValue(rep, v)) {
      if (auto toks = v.as_token_array()) {
        pm.allowedTokens.assign(toks->begin(), toks->end());
        pm.authored |= PropMeta::kAllowedTokens;
      }
    }
    return true;
  }
  if (name == "customData") return dict_field(pm.customData, PropMeta::kCustomData);
  if (name == "assetInfo") return dict_field(pm.assetInfo, PropMeta::kAssetInfo);
  if (name == "sdrMetadata") return dict_field(pm.sdrMetadata, PropMeta::kSdrMetadata);
  return false;
}

bool CrateReader::Impl::CollectPropertyRecords(
    const std::vector<PrimEntry>& prim_entries,
    const std::unordered_map<std::string_view, uint32_t>& entry_by_path,
    const std::vector<uint32_t>& slot_to_entry, const ReportFn& report_stage,
    size_t spec_begin, size_t spec_end, AttrRecords& attr_out,
    RelRecords& rel_out,
    std::vector<std::pair<std::string_view, ValueRep>>& property_raw_field_scratch,
    bool report) {
  auto split_prop_path = [](const std::string_view raw, std::string& prim_path,
                            std::string& prop_name) -> bool {
    if (raw.empty()) return false;
    size_t begin = (raw[0] == '.') ? 1 : 0;
    size_t slash = raw.rfind('/');
    if (slash == std::string_view::npos || slash < begin) return false;
    prim_path.assign(raw.substr(begin, slash - begin));
    prop_name.assign(raw.substr(slash + 1));
    return !prim_path.empty() && !prop_name.empty();
  };
  auto owning_entry = [&](const std::string& prim_path,
                          uint32_t prop_slot) -> uint32_t {
    if (prop_slot < path_parent_.size()) {
      const uint32_t parent_slot = path_parent_[prop_slot];
      if (parent_slot < slot_to_entry.size()) {
        const uint32_t e = slot_to_entry[parent_slot];
        if (e != UINT32_MAX && prim_entries[e].full_path == prim_path) return e;
      }
    }
    auto it = entry_by_path.find(std::string_view(prim_path));
    return it == entry_by_path.end() ? UINT32_MAX : it->second;
  };

    {
      // Exact-reserve the record sinks (no growth moves).
      size_t n_attr = 0;
      size_t n_rel = 0;
      for (size_t i = spec_begin; i < spec_end; ++i) {
        n_attr += specs_[i].spec_type == SpecType::Attribute;
        n_rel += specs_[i].spec_type == SpecType::Relationship;
      }
      attr_out.reserve(attr_out.size() + n_attr);
      rel_out.reserve(rel_out.size() + n_rel);
    }
    for (size_t spec_index = spec_begin; spec_index < spec_end; ++spec_index) {
      if (report && spec_index > 0 &&
          !report_stage("stage.properties", spec_index, specs_.size())) {
        return false;
      }
      const auto& spec = specs_[spec_index];
      const bool is_attr = spec.spec_type == SpecType::Attribute;
      const bool is_rel = spec.spec_type == SpecType::Relationship;
      if (!is_attr && !is_rel) continue;
      if (spec.path_index.value >= paths_.size()) continue;
      const std::string_view raw_path = paths_.view(spec.path_index.value);

      // Variant attributes (bracketed paths) attach to their bracketed holder /
      // child prim like any other attribute (split_prop_path handles them): e.g.
      // ".Prim/{vset=sel}/prop" -> prim_path "/Prim/{vset=sel}" (the holder),
      // ".Prim/{vset=sel}/Geo/attr" -> "/Prim/{vset=sel}/Geo" (a variant child).
      // The compositor reads the selected holder; the writer re-emits it.
      std::string prim_path, prop_name;
      if (!split_prop_path(raw_path, prim_path, prop_name)) {
        continue;
      }

      if (!ResolveFieldsetRaw(spec.fieldset_index.value, property_raw_field_scratch)) continue;

      if (is_rel) {
        RelInfo ri;
        ri.name = std::move(prop_name);
        GetPropNameTable().intern(ri.name);  // see the attribute branch
        for (auto& f : property_raw_field_scratch) {
          if (f.first == "targetPaths") {
            std::vector<std::string> raw;
            if (!DecodePathTargets(f.second, raw, /*with_markers=*/true)) {
              AddWarning("Failed to decode relationship targets on " +
                         prim_path + "." + ri.name);
              continue;
            }
            if (raw.size() == 1 && (raw[0] == "\x01" "E" || raw[0] == "\x01E")) {
              // Authored explicit-clear (`rel r = None`): a declared,
              // target-less relationship with an authored (explicit) edit.
              ri.edits = ArcListOp();
              ri.edits.authored = true;
              ri.edits.is_value_block = true;
              raw.clear();
            } else if (!raw.empty() && !raw[0].empty() && raw[0][0] == '\x01') {
              // Non-explicit list op: sublists are marker-delimited.
              ri.edits.authored = true;
              ri.edits.is_explicit = false;
              std::vector<std::string>* cur = nullptr;
              for (std::string& t : raw) {
                if (t == "\x01" "P") cur = &ri.edits.prepended;
                else if (t == "\x01" "G") cur = &ri.edits.added;
                else if (t == "\x01" "A") cur = &ri.edits.appended;
                else if (t == "\x01" "D") cur = &ri.edits.deleted;
                else if (t == "\x01" "O") cur = &ri.edits.ordered;
                else if (cur) cur->push_back(std::move(t));
              }
              // Within-spec effective list: prepended then appended.
              ri.targets = ri.edits.prepended;
              for (const std::string& item : ri.edits.added) {
                if (std::find(ri.targets.begin(), ri.targets.end(), item) ==
                    ri.targets.end()) ri.targets.push_back(item);
              }
              ri.targets.insert(ri.targets.end(), ri.edits.appended.begin(),
                                ri.edits.appended.end());
            } else {
              ri.targets = std::move(raw);
            }
          } else if (f.first == "custom") {
            Value v;
            if (UnpackValue(f.second, v)) {
              if (const bool* b = v.as_bool()) ri.custom = *b;
            } else {
              AddWarning("Failed to decode relationship custom flag on " +
                         prim_path + "." + ri.name);
            }
          } else if (f.first == "variability") {
            Value v;
            if (UnpackValue(f.second, v)) {
              if (auto s = v.as_token()) {
                ri.variability_authored = true;
                ri.uniform = (*s == "uniform");
                ri.varying = (*s == "varying");
              }
            } else {
              AddWarning("Failed to decode relationship variability on " +
                         prim_path + "." + ri.name);
            }
          } else if (!DecodePropMetaField(f.first, f.second,
                                          ri.ensure_meta())) {
            Value value;
            if (UnpackValue(f.second, value)) {
              ri.meta->unknownFields.push_back(
                  MakeTypedExtensionField(std::string(f.first),
                                          std::move(value)));
              ri.meta->authored |= PropMeta::kUnknownMeta;
            } else {
              AddWarning("Unknown relationship field '" +
                         std::string(f.first) + "' on " +
                         prim_path + "." + ri.name + " is not decodable");
            }
          }
        }
        const uint32_t owner = owning_entry(prim_path, spec.path_index.value);
      if (owner != UINT32_MAX) rel_out.emplace_back(owner, std::move(ri));
        continue;
      }

      // Attribute spec.
      AttrInfo ai;
      ai.name = std::move(prop_name);
      // Intern now (in the parallel collect) so the body pass only hits the
      // name table's lock-free snapshot.
      GetPropNameTable().intern(ai.name);
      for (auto& f : property_raw_field_scratch) {
        if (f.first == "typeName") {
          Value v;
          if (UnpackValue(f.second, v)) {
            if (auto s = v.as_token()) ai.type_name = *s;
          } else {
            AddWarning("Failed to decode typeName on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "default") {
          if (f.second.is_array_edit()) {
            // Sparse VtArrayEdit default: structured edit, not a Value.
            if (UnpackArrayEditData(f.second, &ai.ensure_cold().array_edit)) {
              ai.has_array_edit = true;
            }
          } else {
            Value v;
            if (UnpackValue(f.second, v)) {
              ai.default_value = std::move(v);
              ai.has_default = true;
            } else {
              AddWarning("Failed to decode default value on " + prim_path +
                         "." + ai.name);
            }
          }
        } else if (f.first == "timeSamples") {
          if (!DecodeTimeSamples(f.second, &ai.time_samples)) {
            AddWarning("Failed to decode timeSamples on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "spline") {
          // Crate type-59 spline: decode to USDA text (PrimSpec's storage form).
          if (!DecodeSplineToText(f.second, &ai.ensure_cold().spline_text)) {
            AddWarning("Failed to decode spline on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "connectionPaths") {
          std::vector<std::string> raw;
          if (DecodePathTargets(f.second, raw, /*with_markers=*/true)) {
            // Present-but-empty = authored connection block (`.connect = None`).
            ai.is_connection = true;
            AttrCold& cold = ai.ensure_cold();
            cold.connection_edit.authored = true;
            if (raw.size() == 1 && raw[0] == "\x01" "E") {
              cold.connection_edit.is_explicit = true;
            } else if (!raw.empty() && !raw[0].empty() && raw[0][0] == '\x01') {
              cold.connection_edit.is_explicit = false;
              std::vector<std::string>* current = nullptr;
              for (std::string& item : raw) {
                if (item == "\x01" "G") current = &cold.connection_edit.added;
                else if (item == "\x01" "P") current = &cold.connection_edit.prepended;
                else if (item == "\x01" "A") current = &cold.connection_edit.appended;
                else if (item == "\x01" "D") current = &cold.connection_edit.deleted;
                else if (item == "\x01" "O") current = &cold.connection_edit.ordered;
                else if (current) current->push_back(std::move(item));
              }
              cold.connection_targets = cold.connection_edit.prepended;
              for (const std::string& item : cold.connection_edit.added) {
                if (std::find(cold.connection_targets.begin(),
                              cold.connection_targets.end(), item) ==
                    cold.connection_targets.end()) {
                  cold.connection_targets.push_back(item);
                }
              }
              cold.connection_targets.insert(cold.connection_targets.end(),
                                           cold.connection_edit.appended.begin(),
                                           cold.connection_edit.appended.end());
            } else {
              cold.connection_edit.is_explicit = true;
              cold.connection_targets = std::move(raw);
            }
          } else {
            AddWarning("Failed to decode connection paths on " + prim_path +
                       "." + ai.name);
          }
        } else if (f.first == "variability") {
          Value v;
          if (UnpackValue(f.second, v)) {
            if (auto s = v.as_token()) ai.uniform = (*s == "uniform");
          } else {
            AddWarning("Failed to decode variability on " + prim_path + "." +
                       ai.name);
          }
        } else if (f.first == "custom") {
          // The legacy `custom` qualifier (pxr stores a bool field). Preserved on
          // read; the USDA writer only re-emits it under emit_custom/--openusd-compat.
          Value v;
          if (UnpackValue(f.second, v)) {
            if (const bool* b = v.as_bool()) ai.custom = *b;
          } else {
            AddWarning("Failed to decode custom flag on " + prim_path + "." +
                       ai.name);
          }
        } else if (!DecodePropMetaField(f.first, f.second,
                                        ai.ensure_meta())) {
          Value value;
          if (UnpackValue(f.second, value)) {
            ai.meta->unknownFields.push_back(
                MakeTypedExtensionField(std::string(f.first),
                                        std::move(value)));
            ai.meta->authored |= PropMeta::kUnknownMeta;
          } else {
            AddWarning("Unknown property field '" + std::string(f.first) +
                       "' on " +
                       prim_path + "." + ai.name + " is not decodable");
          }
        }
      }
      // Role types (texCoord2f, point3f, color3h, ...) exist in the crate only
      // via the declared typeName — the value itself is stored as its base type
      // (Vec2f, Vec3f, ...). Re-tag so the in-memory Value matches what the
      // usda parser produces for the same authoring.
      if (ai.has_default && !ai.type_name.empty()) {
        std::string base = ai.type_name;
        if (base.size() >= 2 && base.compare(base.size() - 2, 2, "[]") == 0) {
          base.resize(base.size() - 2);
        }
        TypeId declared = GetTypeIdFromName(base.c_str());
        if (declared != TypeId::Invalid) ai.default_value.retag_role(declared);
      }
      const uint32_t owner = owning_entry(prim_path, spec.path_index.value);
      if (owner != UINT32_MAX) attr_out.emplace_back(owner, std::move(ai));
    }
    return true;

}

}  // namespace next
}  // namespace lightusd
