// SPDX-License-Identifier: Apache-2.0
// Composition metadata, apiSchemas, clips, attribute/relationship metadata
// and ColorSpace/CollectionAPI schema validation rules, extracted from
// usd-validation.cc for compile-time parallelism. Part of the
// validation_detail namespace shared through usd-validation-internal.hh.

#include "usd-validation-internal.hh"

#include "../composition/composition.hh"
#include "../layer/listop-field-table.hh"
#include "../prim/identifier.hh"
#include "../prim/relocate-path.hh"
#include "../schema/schema-registry.hh"
#include "validation-context.hh"

#include "../../external/fast_float/include/fast_float/fast_float.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lightusd {
namespace next {
namespace validation_detail {

// ---------------------------------------------------------------------------
// Composition metadata (arcs, variants, relocates)
// ---------------------------------------------------------------------------

// True if `ps` authors any composition arc (used to gate locality-dependent
// rules; a composed prim may pull opinions from another layer).
bool HasCompositionArc(const PrimSpec &ps) {
  const PrimMetadata &m = ps.meta();
  if (!m.references.empty() || !m.payloads.empty() || !m.inherits.empty() ||
      !m.specializes.empty()) {
    return true;
  }
  if (!m.variantSelection.empty() || !m.variantSelections().empty() ||
      !m.variantSets().empty()) {
    return true;
  }
  const ArcListOps *edits = m.arc_edits();
  if (edits && (edits->references.has_authored_opinion() ||
                edits->payloads.has_authored_opinion() ||
                edits->inherits.has_authored_opinion() ||
                edits->specializes.has_authored_opinion())) {
    return true;
  }
  return false;
}

bool IsOverride(const PrimSpec &ps) {
  return ps.specifier() == PrimSpecifier::Over;
}

// Gather every authored arc string of one arc field (inline explicit list +
// any list-op edit buckets).
std::vector<std::string> GatherArcStrings(const std::vector<std::string> &inline_items,
                                          const ArcListOp *edit) {
  std::vector<std::string> items = inline_items;
  if (edit) {
    items.insert(items.end(), edit->prepended.begin(), edit->prepended.end());
    items.insert(items.end(), edit->appended.begin(), edit->appended.end());
    items.insert(items.end(), edit->deleted.begin(), edit->deleted.end());
    items.insert(items.end(), edit->ordered.begin(), edit->ordered.end());
  }
  return items;
}

bool ParseFiniteLayerOffsetNumber(const char *begin, const char *end,
                                  double *out) {
  if (!out || begin == end) return false;
  auto parsed = fast_float::from_chars(begin, end, *out);
  if (!(parsed.ec == std::errc{} && parsed.ptr == end) && begin < end &&
      *begin == '+') {
    parsed = fast_float::from_chars(begin + 1, end, *out);
  }
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool ParseRawLayerOffset(const std::string &text, double *offset,
                         double *scale) {
  if (!offset || !scale) return false;
  *offset = 0.0;
  *scale = 1.0;
  const char *begin = text.data();
  const char *end = begin + text.size();
  const size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return ParseFiniteLayerOffsetNumber(begin, end, offset);
  }
  return ParseFiniteLayerOffsetNumber(begin, begin + colon, offset) &&
         ParseFiniteLayerOffsetNumber(begin + colon + 1, end, scale);
}

void ValidateLayerOffset(double offset, double scale,
                         const std::string &rule_id,
                         const std::string &location,
                         USDValidationResult *result) {
  if (!std::isfinite(offset)) {
    AddError(result, rule_id, location,
             "composition layerOffset offset must be finite");
  }
  if (!std::isfinite(scale) || !(scale > 0.0)) {
    AddError(result, rule_id, location,
             "composition layerOffset scale must be finite and greater than 0");
  }
}

void ValidateRefArcString(const std::string &arc_str, bool is_payload,
                          const std::string &rule_id,
                          const std::string &prim_location,
                          USDValidationResult *result) {
  CompositionArc arc = is_payload ? Compositor::ParsePayload(arc_str)
                                  : Compositor::ParseReference(arc_str);
  // Programmatically-built layers may store a bare prim path (the USDA parser
  // stores the canonical "</path>" form). Accept both, like the compositor.
  if (arc.prim_path.empty() && arc.asset_path.empty() && !arc_str.empty() &&
      arc_str[0] == '/') {
    arc.prim_path = arc_str;
  }
  const char *what = is_payload ? "payload" : "reference";
  if (arc.asset_path.empty() && arc.prim_path.empty()) {
    AddError(result, rule_id, prim_location,
             std::string(what) + " must author an asset path or prim path");
  }
  if (!arc.prim_path.empty()) {
    std::string err;
    if (!IsValidAbsolutePrimTargetPathString(arc.prim_path, &err)) {
      AddError(result, rule_id, prim_location,
               std::string(what) + " primPath `" + arc.prim_path +
                   "` is invalid: " + err);
    }
  }
  if (!arc.layer_offset.empty()) {
    double offset = 0.0;
    double scale = 1.0;
    if (!ParseRawLayerOffset(arc.layer_offset, &offset, &scale)) {
      AddError(result, rule_id, prim_location,
               "composition layerOffset must contain numeric offset/scale values");
    } else {
      ValidateLayerOffset(offset, scale, rule_id, prim_location, result);
    }
  }
}

void ValidatePathArcString(const std::string &arc_str,
                           const std::string &rule_id,
                           const std::string &prim_location,
                           const PrimSpecifierByPath &prim_specifiers,
                           USDValidationResult *result,
                           bool expect_class_target) {
  CompositionArc arc = Compositor::ParseReference(arc_str);
  if (arc.prim_path.empty() && arc.asset_path.empty() && !arc_str.empty() &&
      arc_str[0] == '/') {
    arc.prim_path = arc_str;
  }
  const std::string &target = arc.prim_path;
  std::string err;
  if (!IsValidAbsolutePrimTargetPathString(target, &err)) {
    AddError(result, rule_id, prim_location,
             "target `" + (target.empty() ? arc_str : target) +
                 "` is invalid: " + err);
    return;
  }
  if (target == prim_location) {
    AddError(result, rule_id, prim_location,
             "composition arc must not target the prim itself");
  }
  const PrimSpecifier *target_spec = FindPrimSpecifier(prim_specifiers, target);
  if (expect_class_target && target_spec &&
      *target_spec != PrimSpecifier::Class) {
    AddWarning(result, rule_id, prim_location,
               "local target `" + target + "` is not a class prim");
  }
}

void ValidateVariantSetsAndSelections(const PrimSpec &ps,
                                      const std::string &prim_location,
                                      USDValidationResult *result) {
  // NOTE (structural approximation): variant OPTION bodies (properties/child
  // prims inside a variant) are stored as PrimVariantData / graft layers in
  // next-core rather than PrimSpecs, and are not deep-validated here. Only
  // set / variant / selection names are checked (legacy additionally walks
  // the variant PrimSpec bodies).
  std::set<std::string> local_sets;
  std::map<std::string, std::set<std::string>> local_variants;
  for (const PrimVariantSetData &vs : ps.meta().variantSets()) {
    local_sets.insert(vs.name);
    if (vs.name.empty() || !IsValidIdentifier(vs.name)) {
      AddError(result, "core.composition.variantSet", prim_location,
               "variant set statement name `" + vs.name +
                   "` is not a valid identifier");
    }
    if (vs.variants.empty()) {
      AddWarning(result, "core.composition.variantSet", prim_location,
                 "variant set `" + vs.name + "` has no variants");
    }
    for (const PrimVariantData &variant : vs.variants) {
      local_variants[vs.name].insert(variant.name);
      if (variant.name.empty() || !IsValidIdentifier(variant.name)) {
        AddError(result, "core.composition.variantSet", prim_location,
                 "variant name `" + variant.name + "` in set `" + vs.name +
                     "` is not a valid identifier");
      }
    }
  }

  // Collect selections: the plural list plus the legacy inline single
  // selection (the USDA parser mirrors the first selection into both fields,
  // so deduplicate).
  std::vector<std::pair<std::string, std::string>> selections =
      ps.meta().variantSelections();
  if (!ps.meta().variantSelection.empty()) {
    const VariantSelection sel =
        Compositor::ParseVariantSelection(ps.meta().variantSelection);
    if (!sel.variant_set.empty() || !sel.variant_name.empty()) {
      const std::pair<std::string, std::string> entry(sel.variant_set,
                                                      sel.variant_name);
      if (std::find(selections.begin(), selections.end(), entry) ==
          selections.end()) {
        selections.push_back(entry);
      }
    }
  }

  for (const auto &selection : selections) {
    const std::string &set_name = selection.first;
    const std::string &variant_name = selection.second;
    if (set_name.empty() || !IsValidIdentifier(set_name)) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection set name `" + set_name +
                   "` is not a valid identifier");
    }
    if (variant_name.empty() || !IsValidIdentifier(variant_name)) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection `" + variant_name + "` for set `" +
                   set_name + "` is not a valid identifier");
      continue;
    }
    if (local_sets.count(set_name) &&
        local_variants[set_name].count(variant_name) == 0) {
      AddError(result, "core.composition.variantSelection", prim_location,
               "variant selection `" + variant_name + "` is not authored in "
                   "local variant set `" +
                   set_name + "`");
    }
  }
}

// Core v1.1.0 L2494-2510, L2861-2897: names may be speculative, but
// should be valid prim/property names. No local child-membership requirement.
void ValidateNameOrder(const std::vector<std::string> &names, bool properties,
                       const std::string &rule, const std::string &location,
                       USDValidationResult *result) {
  for (size_t i = 0; i < names.size(); ++i) {
    const bool valid = properties ? IsValidNamespacedIdentifier(names[i])
                                  : IsValidIdentifier(names[i]);
    if (!valid) {
      AddWarning(result, rule, location,
                 "order entry " + std::to_string(i) + " `" + names[i] +
                     "` should be a valid " + (properties ? "property" : "prim") +
                     " name");
    }
  }
}

// Both layer relocates and legacy per-prim relocates retain authored entries.
void ValidateRelocates(const std::vector<std::pair<std::string, std::string>> &entries,
                       const std::string &prim_location, const std::string &anchor,
                       USDValidationResult *result) {
  const auto anchored = [&](const std::string& path) {
    std::string result = path;
    AnchorRelocatePath(path, anchor, &result);
    return result;
  };
  // These are local authoring constraints, not selection across sublayers.
  std::set<std::string> relocated_sources, valid_sources;
  std::map<std::string, std::string> first_targets, first_sources;
  std::set<std::string> reported_sources, reported_targets;
  const auto below = [](const std::string& path, const std::string& parent) {
    return path.size() > parent.size() && path.compare(0, parent.size(), parent) == 0 &&
           path[parent.size()] == '/';
  };
  for (const auto &entry : entries) {
    const std::string source = anchored(entry.first);
    const std::string target = anchored(entry.second);
    std::string source_err, target_err;
    const bool source_valid = IsValidAbsolutePrimTargetPathString(source, &source_err);
    const bool target_valid = target.empty() ||
        IsValidAbsolutePrimTargetPathString(target, &target_err);
    if (!source_valid) {
      AddError(result, "core.layer.layerRelocates.source", prim_location,
               "layerRelocates source `" + source + "` must be a valid absolute prim path");
    } else {
      valid_sources.insert(source);
    }
    if (!target_valid) {
      AddError(result, "core.layer.layerRelocates.target", prim_location,
               "layerRelocates target `" + target +
                   "` must be empty or a valid absolute prim path");
    }
    if (source == "/" || target == "/" ||
        (source_valid && source.find('/', 1) == std::string::npos)) {
      AddError(result, "core.layer.layerRelocates.root", prim_location,
               "relocate sources must be below a root prim and targets must not be the pseudo-root; "
               "root-prim targets are allowed by OpenUSD compatibility (LEAN-515)");
    }
    if (!source.empty() && !target.empty() && source == target) {
      AddError(result, "core.layer.layerRelocates.identity", prim_location,
               "layerRelocates source and target must not be identical");
    }
    if (!source.empty() && !relocated_sources.insert(source).second) {
      AddError(result, "core.layer.layerRelocates.source", prim_location,
               "layerRelocates contains duplicate source `" + source + "`");
    }
    if (!source_valid || !target_valid) continue;
    const auto source_opinion = first_targets.emplace(source, target);
    if (!source_opinion.second && source_opinion.first->second != target &&
        reported_sources.insert(source).second) {
      AddError(result, "core.layer.layerRelocates.duplicateSource", prim_location,
               "relocate source `" + source + "` has different targets in this authored field");
    }
    if (target.empty()) continue;
    if (below(source, target)) {
      AddError(result, "core.layer.layerRelocates.ancestor", prim_location,
               "relocate target `" + target + "` is an ancestor of source `" + source + "`");
    }
    if (below(target, source)) {
      AddError(result, "core.layer.layerRelocates.targetUnderSource", prim_location,
               "relocate target `" + target + "` is below its source `" + source + "`");
    }
    const auto target_opinion = first_sources.emplace(target, source);
    if (!target_opinion.second && target_opinion.first->second != source &&
        reported_targets.insert(target).second) {
      AddError(result, "core.layer.layerRelocates.duplicateTarget", prim_location,
               "relocate target `" + target + "` has different sources in this authored field");
    }
  }
  std::set<std::string> reported_nested;
  for (const auto& entry : entries) {
    const std::string source = anchored(entry.first);
    const std::string target = anchored(entry.second);
    if (!valid_sources.count(source)) continue;
    // Parent lookup respects component boundaries: /P/A is not above /P/AB.
    std::string parent = source;
    while (parent.rfind('/') != 0 && parent.rfind('/') != std::string::npos) {
      parent.resize(parent.rfind('/'));
      if (valid_sources.count(parent)) {
        if (reported_nested.insert(source).second)
          AddError(result, "core.layer.layerRelocates.nestedSource", prim_location,
                   "relocate source `" + source + "` uses the unrelocated namespace below `" + parent + "`");
        break;
      }
    }
    std::string target_err;
    if (target.empty() || !IsValidAbsolutePrimTargetPathString(target, &target_err)) continue;
    if (target != source && valid_sources.count(target)) {
      AddError(result, "core.layer.layerRelocates.targetIsSource", prim_location,
               "relocate target `" + target + "` is another source (OpenUSD compatibility, LEAN-514)");
    }
    parent = target;
    while (parent.rfind('/') != 0 && parent.rfind('/') != std::string::npos) {
      parent.resize(parent.rfind('/'));
      if (parent != source && valid_sources.count(parent)) {
        AddError(result, "core.layer.layerRelocates.targetUnderOtherSource", prim_location,
                 "relocate target `" + target + "` is below another source `" + parent +
                     "` (OpenUSD compatibility)");
        break;
      }
    }
  }
}

void ValidateRelocates(const PrimSpec &ps, const std::string &prim_location,
                       USDValidationResult *result) {
  ValidateRelocates(ps.meta().relocates(), prim_location, ps.path().str(), result);
}

// ---------------------------------------------------------------------------
// List-op field integrity (AOUSD 6.6.3, L1308-1337 unique sequences;
// L1699-1708 deprecated add/reorder). Lean: UsdSpec.Composable (Nodup) and
// UsdSpec.ListOp.Legacy.applyLegacy.
// ---------------------------------------------------------------------------

namespace {

// Report every repeated item of one list-op sequence under
// core.listop.duplicateItems. `field` names the metadata field and `sequence`
// the sub-list (explicit/prepended/...); duplicates are reported once each.
void CheckSequenceUnique(const std::vector<std::string> &items,
                         const std::string &field, const char *sequence,
                         const std::string &location,
                         USDValidationResult *result) {
  if (items.size() < 2) return;
  std::set<std::string> seen, reported;
  for (const std::string &item : items) {
    if (seen.insert(item).second || !reported.insert(item).second) continue;
    AddError(result, "core.listop.duplicateItems", location,
             field + " " + sequence + " sequence repeats `" + item + "`");
  }
}

void CheckDeprecatedOps(const std::vector<std::string> &added,
                        const std::vector<std::string> &ordered,
                        const std::string &field, const std::string &location,
                        USDValidationResult *result) {
  if (!added.empty()) {
    AddWarning(result, "core.listop.deprecatedAdd", location,
               field + " uses the deprecated `add` list operation; author "
                       "`append` instead");
  }
  if (!ordered.empty()) {
    AddWarning(result, "core.listop.deprecatedReorder", location,
               field + " uses the deprecated `reorder` list operation");
  }
}

// `explicit_items` is the field's bare/explicit list (the inline arc vector,
// relationship targets, ...). It is only a list-op sequence when no
// composable edit is authored for the field.
void CheckArcListOp(const std::vector<std::string> &explicit_items,
                    const ArcListOp *edit, const std::string &field,
                    const std::string &location, USDValidationResult *result) {
  if (!edit || !edit->authored || edit->is_explicit) {
    CheckSequenceUnique(explicit_items, field, "explicit", location, result);
    return;
  }
  CheckSequenceUnique(edit->prepended, field, "prepended", location, result);
  CheckSequenceUnique(edit->appended, field, "appended", location, result);
  CheckSequenceUnique(edit->deleted, field, "deleted", location, result);
  CheckSequenceUnique(edit->added, field, "added", location, result);
  CheckSequenceUnique(edit->ordered, field, "ordered", location, result);
  CheckDeprecatedOps(edit->added, edit->ordered, field, location, result);
}

void CheckStringListOp(const StringListOps &edits, const std::string &field,
                       const std::string &location,
                       USDValidationResult *result) {
  if (!edits.authored) return;
  if (edits.is_explicit) {
    CheckSequenceUnique(edits.explicit_items, field, "explicit", location,
                        result);
    return;
  }
  CheckSequenceUnique(edits.prepended, field, "prepended", location, result);
  CheckSequenceUnique(edits.appended, field, "appended", location, result);
  CheckSequenceUnique(edits.deleted, field, "deleted", location, result);
  CheckSequenceUnique(edits.added, field, "added", location, result);
  CheckSequenceUnique(edits.ordered, field, "ordered", location, result);
  CheckDeprecatedOps(edits.added, edits.ordered, field, location, result);
}

std::vector<std::string> PathStrings(const std::vector<Path> *paths) {
  std::vector<std::string> out;
  if (!paths) return out;
  out.reserve(paths->size());
  for (const Path &p : *paths) out.push_back(p.str());
  return out;
}

// AOUSD 1.1 schema-name grammar (L8252-8270): ASCII identifier
// `[A-Za-z_][A-Za-z0-9_]*` split into Family ('_' Version)?. Prim identifiers
// may be Unicode; schema names may not. Lean: UsdSpec.Schema.parseFamilyVersion.
bool MatchesSchemaNameGrammar(const std::string &name) {
  if (name.empty()) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       c == '_';
    const bool digit = c >= '0' && c <= '9';
    if (!(alpha || (i > 0 && digit))) return false;
  }
  std::string family;
  uint32_t version = 0;
  bool has_version = false;
  return ParseSchemaFamilyAndVersion(name, &family, &version, &has_version) &&
         !family.empty();
}

}  // namespace

void ValidateSchemaNameGrammar(const std::string &name, const char *what,
                               const std::string &location,
                               USDValidationResult *result) {
  if (name.empty() || MatchesSchemaNameGrammar(name)) return;
  AddError(result, "core.schema.familyVersion", location,
           std::string(what) + " `" + name +
               "` does not match the schema family/version grammar "
               "(`Family(_Version)?`, ASCII identifier)");
}

void ValidateListOpFields(const PrimSpec &ps, const std::string &prim_location,
                          USDValidationResult *result) {
  const PrimMetadata &m = ps.meta();
  const ArcListOps *edits = m.arc_edits();
  CheckArcListOp(m.references, edits ? &edits->references : nullptr,
                 "references", prim_location, result);
  CheckArcListOp(m.payloads, edits ? &edits->payloads : nullptr, "payload",
                 prim_location, result);
  CheckArcListOp(m.inherits, edits ? &edits->inherits : nullptr, "inherits",
                 prim_location, result);
  CheckArcListOp(m.specializes, edits ? &edits->specializes : nullptr,
                 "specializes", prim_location, result);

  // apiSchemas: a bare list or legacy qualifier is normalized to an op.
  CheckStringListOp(listop_field_detail::ApiSchemasNormalizedEdits(m),
                    "apiSchemas", prim_location, result);
  CheckStringListOp(m.variantSetNameEdits(), "variantSets", prim_location,
                    result);
  CheckStringListOp(m.clipSetEdits(), "clipSets", prim_location, result);

  // Relationship targets (SdfPathListOp).
  const auto &rel_edits = ps.relationship_edits();
  for (const std::string &name : ps.relationship_names()) {
    const auto it = rel_edits.find(name);
    CheckArcListOp(PathStrings(ps.relationship(name)),
                   it == rel_edits.end() ? nullptr : &it->second,
                   "relationship " + name,
                   MakePropertyLocation(prim_location, name), result);
  }
  // Attribute connections (connectionPaths SdfPathListOp).
  for (const auto &slot : ps.properties().slots()) {
    if (!slot.is_connection()) continue;
    const std::string name(GetPropNameTable().get(slot.name_id));
    CheckArcListOp(PathStrings(ps.connection(slot.name_id)),
                   ps.connection_edit(slot.name_id), "connection " + name,
                   MakePropertyLocation(prim_location, name), result);
  }
}

void ValidateCompositionMetadata(const PrimSpec &ps,
                                 const std::string &prim_location,
                                 const PrimSpecifierByPath &prim_specifiers,
                                 USDValidationResult *result) {
  const PrimMetadata &m = ps.meta();
  const ArcListOps *edits = m.arc_edits();

  ValidateListOpFields(ps, prim_location, result);

  // NOTE: invalid list-edit qualifiers are unrepresentable in next-core
  // (ArcListOp qualifiers are typed), so the legacy "invalid list-edit
  // qualifier" errors have no equivalent here.
  for (const std::string &ref : GatherArcStrings(
           m.references, edits ? &edits->references : nullptr)) {
    ValidateRefArcString(ref, /* is_payload */ false,
                         "core.composition.reference", prim_location, result);
  }
  for (const std::string &pl :
       GatherArcStrings(m.payloads, edits ? &edits->payloads : nullptr)) {
    ValidateRefArcString(pl, /* is_payload */ true,
                         "core.composition.payload", prim_location, result);
  }
  for (const std::string &inh :
       GatherArcStrings(m.inherits, edits ? &edits->inherits : nullptr)) {
    ValidatePathArcString(inh, "core.composition.inherits", prim_location,
                          prim_specifiers, result,
                          /* expect_class_target */ true);
  }
  for (const std::string &sp : GatherArcStrings(
           m.specializes, edits ? &edits->specializes : nullptr)) {
    ValidatePathArcString(sp, "core.composition.specializes", prim_location,
                          prim_specifiers, result,
                          /* expect_class_target */ true);
  }

  // Programmatic and USDC inputs can bypass USDA's strict parser, so validate
  // both components here as well as on the authored arc strings above.
  ValidateLayerOffset(m.layer_offset.first, m.layer_offset.second,
                      "core.composition.reference", prim_location, result);

  ValidateVariantSetsAndSelections(ps, prim_location, result);
  ValidateRelocates(ps, prim_location, result);

  if (ps.specifier() == PrimSpecifier::Over && ps.type_name().empty() &&
      !HasCompositionArc(ps) && ps.properties().size() == 0 &&
      ps.relationship_names().empty() && ps.child_indices().empty()) {
    AddWarning(result, "core.composition.over", prim_location,
               "empty typeless over has no local opinions or composition "
               "arcs");
  }
}

// ---------------------------------------------------------------------------
// apiSchemas / prim metadata
// ---------------------------------------------------------------------------

std::vector<AppliedSchema> CollectAppliedSchemas(const PrimSpec &ps) {
  std::vector<AppliedSchema> schemas;
  for (const std::string &entry : ps.meta().apiSchemas()) {
    std::string schema_name = entry;
    std::string instance_name;
    const size_t colon_pos = entry.find(':');
    if (colon_pos != std::string::npos) {
      schema_name = entry.substr(0, colon_pos);
      instance_name = entry.substr(colon_pos + 1);
    }
    schemas.push_back({schema_name, instance_name});
  }
  return schemas;
}

std::set<std::string> CollectAppliedCollectionInstances(
    const std::vector<AppliedSchema> &schemas) {
  std::set<std::string> instances;
  for (const auto &schema : schemas) {
    if (schema.name == "CollectionAPI" && !schema.instance_name.empty()) {
      instances.insert(schema.instance_name);
    }
  }
  return instances;
}

void ValidateAPISchemasMetadata(const std::vector<AppliedSchema> &schemas,
                                const std::string &prim_location,
                                USDValidationResult *result,
                                const ValidationRegistry& registry) {
  std::set<std::string> seen;
  for (const AppliedSchema &schema : schemas) {
    if (schema.name.empty() || !IsValidIdentifier(schema.name)) {
      AddError(result, "core.apiSchema.name", prim_location,
               "apiSchemas entry `" + schema.name +
                   "` is not a valid schema identifier");
    }
    ValidateSchemaNameGrammar(schema.name, "apiSchemas entry", prim_location,
                              result);
    if (!schema.instance_name.empty() &&
        !IsValidNamespacedIdentifier(schema.instance_name)) {
      AddError(result, "core.apiSchema.instance", prim_location,
               "apiSchemas instance `" + schema.instance_name +
                   "` is not a valid namespaced identifier");
    }
    const auto definition = registry.FindSchema(schema.name);
    // The exported source manifest records classes without an explicit
    // apiSchemaType as abstract. APISchemaBase descendants default to
    // singleApply in OpenUSD; explicit nonApplied definitions stay non-applied.
    const bool implicit_single = definition && definition->kind == "abstract" &&
        schema.name != "APISchemaBase" && registry.InheritsFrom(schema.name, "APISchemaBase");
    if (definition && definition->kind != "multipleApply" && definition->kind != "singleApply" && !implicit_single)
      AddError(result, "core.apiSchema.kind", prim_location,
               schema.name + " is not an applied API schema");
    const bool multiple = definition ? definition->kind == "multipleApply" : IsMultipleApplySchemaName(schema.name);
    const bool known = definition || IsKnownAPISchemaName(schema.name);
    if (multiple && schema.instance_name.empty()) {
      AddError(result, "core.apiSchema.instance", prim_location,
               schema.name + " is a multiple-apply API schema and requires "
                             "an instance name");
    }
    if (known && !multiple &&
        !schema.instance_name.empty()) {
      AddError(result, "core.apiSchema.instance", prim_location,
               schema.name + " is a single-apply API schema and must not "
                             "have an instance name");
    }
    if (!schema.name.empty() && !known) {
      AddWarning(result, "core.apiSchema.unknown", prim_location,
                 "API schema `" + schema.name +
                     "` is not present in LightUSD's built-in schema registry");
    }
    const std::string key = schema.name + ":" + schema.instance_name;
    if (!seen.insert(key).second) {
      AddWarning(result, "core.apiSchema.duplicate", prim_location,
                 "duplicate apiSchemas entry `" + key + "`");
    }
  }
}

void ValidatePrimMetadata(const PrimSpec &ps, const std::string &prim_location,
                          const std::vector<AppliedSchema> &applied_schemas,
                          USDValidationResult *result, const ValidationRegistry& registry) {
  const PrimMetadata &m = ps.meta();

  ValidateNameOrder(m.primOrder(), false, "core.prim.primOrder", prim_location, result);
  ValidateNameOrder(m.propertyOrder(), true, "core.prim.propertyOrder", prim_location, result);

  ValidateSchemaNameGrammar(ps.type_name(), "prim typeName", prim_location,
                            result);

  if (!m.kind().empty() && !IsValidNamespacedIdentifier(m.kind())) {
    AddError(result, "core.prim.kind", prim_location,
             "kind `" + m.kind() + "` is not a valid token");
  }

  const ArcListOps *edits = m.arc_edits();
  const bool has_ref_or_payload =
      !m.references.empty() || !m.payloads.empty() ||
      (edits && (edits->references.has_authored_opinion() ||
                 edits->payloads.has_authored_opinion()));
  if (m.instanceable && !has_ref_or_payload && ps.child_indices().empty()) {
    AddWarning(result, "core.prim.instanceable", prim_location,
               "instanceable prim should usually compose a prototype via "
               "references or payloads, or contain authored children");
  }

  ValidateDictValue(m.customData(), "core.prim.customData",
                    prim_location + ".customData", result);

  if (auto asset_info = m.assetInfo().as_dictionary()) {
    ValidateDictionaryKeys(*asset_info, "core.prim.assetInfo",
                           prim_location + ".assetInfo", result);

    std::string identifier;
    if (GetMetaAssetPath(*asset_info, "identifier", &identifier) &&
        identifier.empty()) {
      AddError(result, "core.prim.assetInfo",
               prim_location + ".assetInfo.identifier",
               "assetInfo identifier must not be empty");
    }
    std::string name;
    if (GetMetaString(*asset_info, "name", &name) && name.empty()) {
      AddWarning(result, "core.prim.assetInfo",
                 prim_location + ".assetInfo.name",
                 "assetInfo name should not be empty");
    }
    std::vector<std::string> deps;
    if (GetMetaStringArray(*asset_info, "payloadAssetDependencies", &deps)) {
      for (size_t i = 0; i < deps.size(); i++) {
        if (deps[i].empty()) {
          AddError(result, "core.prim.assetInfo",
                   prim_location + ".assetInfo.payloadAssetDependencies",
                   "payloadAssetDependencies[" + std::to_string(i) +
                       "] must not be empty");
        }
      }
    }
  }

  ValidateAPISchemasMetadata(applied_schemas, prim_location, result, registry);
}

void ValidateMaterialXReferenceConventions(const PrimSpec &ps,
                                           const std::string &prim_location,
                                           USDValidationResult *result) {
  const ArcListOps *edits = ps.meta().arc_edits();
  for (const std::string &ref : GatherArcStrings(
           ps.meta().references, edits ? &edits->references : nullptr)) {
    const CompositionArc arc = Compositor::ParseReference(ref);
    if (!IsMaterialXAssetPath(arc.asset_path)) {
      continue;
    }
    if (arc.prim_path != "/MaterialX") {
      AddWarning(result, "shade.materialX.referencePrimPath", prim_location,
                 "MaterialX references should target </MaterialX>");
    }
  }
}

// ---------------------------------------------------------------------------
// clips metadata
// ---------------------------------------------------------------------------

void ValidateClipPairArray(const std::vector<std::array<double, 2>> &pairs,
                           const std::string &rule_id,
                           const std::string &location,
                           const std::string &field_name,
                           USDValidationResult *result) {
  bool have_prev_stage_time = false;
  double prev_stage_time = 0.0;
  for (size_t i = 0; i < pairs.size(); i++) {
    const double stage_time = pairs[i][0];
    const double mapped_value = pairs[i][1];
    if (!std::isfinite(stage_time) || !std::isfinite(mapped_value)) {
      AddError(result, rule_id, location,
               field_name + "[" + std::to_string(i) +
                   "] must contain finite values");
    }
    if (have_prev_stage_time && stage_time <= prev_stage_time) {
      AddError(result, rule_id, location,
               field_name + " stage times must be strictly increasing");
    }
    have_prev_stage_time = true;
    prev_stage_time = stage_time;
  }
}

void ValidateClipSetMetadata(const std::string &clip_set_name,
                             DictView clip_set,
                             const std::string &prim_location,
                             USDValidationResult *result) {
  const std::string location = prim_location + ".clips[" + clip_set_name + "]";
  std::vector<std::string> asset_paths;
  const bool have_asset_paths =
      GetMetaStringArray(clip_set, "assetPaths", &asset_paths);

  std::string template_asset_path;
  const bool have_template_asset =
      GetMetaAssetPath(clip_set, "templateAssetPath", &template_asset_path);

  if (!have_asset_paths && !have_template_asset) {
    AddError(result, "core.clips.assets", location,
             "clip set must author assetPaths or templateAssetPath");
  }

  if (have_asset_paths) {
    if (asset_paths.empty()) {
      AddError(result, "core.clips.assets", location + ".assetPaths",
               "assetPaths must not be empty");
    }
    for (size_t i = 0; i < asset_paths.size(); i++) {
      if (asset_paths[i].empty()) {
        AddError(result, "core.clips.assets", location + ".assetPaths",
                 "assetPaths[" + std::to_string(i) + "] must not be empty");
      }
    }
  }

  if (have_template_asset) {
    if (template_asset_path.empty()) {
      AddError(result, "core.clips.template", location + ".templateAssetPath",
               "templateAssetPath must not be empty");
    }
    double start_time = 0.0;
    double end_time = 0.0;
    double stride = 1.0;
    double active_offset = 0.0;
    const bool have_start =
        GetMetaDouble(clip_set, "templateStartTime", &start_time);
    const bool have_end =
        GetMetaDouble(clip_set, "templateEndTime", &end_time);
    const bool have_stride =
        GetMetaDouble(clip_set, "templateStride", &stride);
    const bool have_active_offset =
        GetMetaDouble(clip_set, "templateActiveOffset", &active_offset);
    if (!have_start || !have_end) {
      AddError(result, "core.clips.template", location,
               "template clip sets must author templateStartTime and "
               "templateEndTime");
    } else {
      if (!std::isfinite(start_time) || !std::isfinite(end_time)) {
        AddError(result, "core.clips.template", location,
                 "templateStartTime and templateEndTime must be finite");
      }
      if (end_time < start_time) {
        AddError(result, "core.clips.template", location,
                 "templateEndTime must be greater than or equal to "
                 "templateStartTime");
      }
    }
    if (have_stride && (!std::isfinite(stride) || stride <= 0.0)) {
      AddError(result, "core.clips.template", location + ".templateStride",
               "templateStride must be positive and finite");
    }
    if (have_active_offset &&
        (!std::isfinite(active_offset) || !std::isfinite(stride) ||
         stride <= 0.0 || std::fabs(active_offset) > stride)) {
      AddError(result, "core.clips.template",
               location + ".templateActiveOffset",
               "templateActiveOffset must be finite and no greater than "
               "templateStride");
    }
  }

  std::vector<std::array<double, 2>> active;
  if (GetMetaDouble2Array(clip_set, "active", &active)) {
    if (active.empty()) {
      AddError(result, "core.clips.active", location + ".active",
               "active must not be empty");
    }
    ValidateClipPairArray(active, "core.clips.active", location + ".active",
                          "active", result);
    if (have_asset_paths) {
      for (size_t i = 0; i < active.size(); i++) {
        const double asset_index = active[i][1];
        const double rounded = std::floor(asset_index);
        if (std::fabs(asset_index - rounded) > 1.0e-9 || asset_index < 0.0 ||
            static_cast<size_t>(asset_index) >= asset_paths.size()) {
          AddError(result, "core.clips.active", location + ".active",
                   "active[" + std::to_string(i) +
                       "] references asset index " +
                       std::to_string(asset_index) + ", but assetPaths has " +
                       std::to_string(asset_paths.size()) + " entries");
        }
      }
    }
  } else if (have_asset_paths) {
    AddError(result, "core.clips.active", location,
             "explicit clip sets with assetPaths must author active");
  }

  std::vector<std::array<double, 2>> times;
  if (GetMetaDouble2Array(clip_set, "times", &times)) {
    ValidateClipPairArray(times, "core.clips.times", location + ".times",
                          "times", result);
  }

  std::string prim_path;
  if (GetMetaString(clip_set, "primPath", &prim_path)) {
    std::string err;
    if (!IsValidAbsolutePrimPathString(prim_path, &err) || prim_path == "/") {
      AddError(result, "core.clips.primPath", location + ".primPath",
               "primPath must be an absolute prim path, but is `" + prim_path +
                   "`");
    }
  }

  std::string manifest_asset_path;
  if (GetMetaAssetPath(clip_set, "manifestAssetPath", &manifest_asset_path) &&
      manifest_asset_path.empty()) {
    AddError(result, "core.clips.assets", location + ".manifestAssetPath",
             "manifestAssetPath must not be empty");
  }
}

void ValidateClipsMetadata(const PrimSpec &ps,
                           const std::string &prim_location,
                           USDValidationResult *result) {
  auto clips = ps.meta().clips().as_dictionary();
  if (!clips) {
    return;
  }
  if (clips->empty()) {
    AddError(result, "core.clips", prim_location + ".clips",
             "clips dictionary must contain at least one clip set");
    return;
  }

  for (const auto &clip_entry : clips->entries()) {
    auto clip_set = clip_entry.second.as_dictionary();
    if (!clip_set) {
      AddError(result, "core.clips", prim_location + ".clips",
               "clip set `" + clip_entry.first + "` must be a dictionary");
      continue;
    }
    ValidateClipSetMetadata(clip_entry.first, *clip_set, prim_location,
                            result);
  }
}

// ---------------------------------------------------------------------------
// Attribute / relationship metadata (PropMeta), attribute type checks
// ---------------------------------------------------------------------------

void ValidateTokenAttributeType(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result) {
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }
  const std::string type_name = AttrTypeNameOf(ps, prop_name);
  if (type_name != "token") {
    AddError(result, rule_id, location,
             "expected attribute type `token`, but found `" + type_name +
                 "`");
  }
}

void ValidateExactAttributeType(const PrimSpec &ps,
                                const std::string &prop_name,
                                const std::string &expected,
                                const std::string &rule_id,
                                const std::string &location,
                                USDValidationResult *result) {
  if (IsRelationshipProp(ps, prop_name)) {
    AddError(result, rule_id, location,
             "expected an attribute, but found a relationship");
    return;
  }
  const std::string type_name = AttrTypeNameOf(ps, prop_name);
  if (type_name != expected) {
    AddError(result, rule_id, location,
             "expected attribute type `" + expected + "`, but found `" +
                 type_name + "`");
  }
}

void ValidateUniformAttribute(const PrimSpec &ps, const std::string &prop_name,
                              const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result) {
  const PropSlot *slot = GetSlot(ps, prop_name);
  if (!slot || slot->is_relationship()) {
    return;
  }
  if (!slot->is_uniform()) {
    AddError(result, rule_id, location, "expected `uniform` variability");
  }
}

void ValidateVaryingAttribute(const PrimSpec &ps, const std::string &prop_name,
                              const std::string &rule_id,
                              const std::string &location,
                              USDValidationResult *result) {
  const PropSlot *slot = GetSlot(ps, prop_name);
  if (!slot || slot->is_relationship()) {
    return;
  }
  if (slot->is_uniform()) {
    AddError(result, rule_id, location, "expected `varying` variability");
  }
}

void ValidateAttributeMetadata(const PrimSpec &ps,
                               const std::string &prop_name,
                               const std::string &location,
                               USDValidationResult *result) {
  const PropMeta *metas = ps.property_meta(prop_name);
  if (!metas) {
    return;
  }

  if (metas->authored & PropMeta::kCustomData) {
    ValidateDictValue(metas->customData, "core.attr.customData",
                      location + ".customData", result);
  }
  if (metas->authored & PropMeta::kSdrMetadata) {
    ValidateDictValue(metas->sdrMetadata, "core.attr.sdrMetadata",
                      location + ".sdrMetadata", result);
  }

  if ((metas->authored & PropMeta::kInterpolation) &&
      !IsValidPrimvarInterpolation(metas->interpolation)) {
    AddError(result, "core.attr.interpolation", location,
             "invalid interpolation token `" + metas->interpolation + "`");
  }
  if ((metas->authored & PropMeta::kElementSize) && metas->elementSize <= 0) {
    AddError(result, "core.attr.elementSize", location,
             "elementSize must be greater than 0");
  }
  if ((metas->authored & PropMeta::kUnauthoredIdx) &&
      metas->unauthoredValuesIndex < -1) {
    AddError(result, "core.attr.unauthoredValuesIndex", location,
             "unauthoredValuesIndex must be -1 or greater");
  }

  if (metas->authored & PropMeta::kAllowedTokens) {
    const std::vector<std::string> &allowed = metas->allowedTokens;
    if (allowed.empty()) {
      AddWarning(result, "core.attr.allowedTokens", location,
                 "allowedTokens should not be empty when authored");
    }

    std::set<std::string> allowed_set;
    for (size_t i = 0; i < allowed.size(); i++) {
      const std::string &token = allowed[i];
      if (token.empty()) {
        AddError(result, "core.attr.allowedTokens", location,
                 "allowedTokens[" + std::to_string(i) +
                     "] must not be empty");
      } else if (!allowed_set.insert(token).second) {
        AddWarning(result, "core.attr.allowedTokens", location,
                   "duplicate allowedTokens entry `" + token + "`");
      }
    }

    const Value *v = GetAttrValue(ps, prop_name);
    if (v && !allowed_set.empty()) {
      if (auto token_value = v->as_token()) {
        if (allowed_set.count(*token_value) == 0) {
          AddError(result, "core.attr.allowedTokens", location,
                   prop_name + " value `" + *token_value +
                       "` is not listed in allowedTokens");
        }
      }
      if (v->is_array() && v->type_id() == TypeId::Token) {
        auto token_array = v->as_token_array();
        if (token_array) {
          for (size_t i = 0; i < token_array->size(); i++) {
            const std::string &token = (*token_array)[i];
            if (allowed_set.count(token) == 0) {
              AddError(result, "core.attr.allowedTokens", location,
                       prop_name + "[" + std::to_string(i) + "] value `" +
                           token + "` is not listed in allowedTokens");
            }
          }
        }
      }
    }
  }

  if (metas->authored & PropMeta::kConnectability) {
    if (metas->connectability != "full" &&
        metas->connectability != "interfaceOnly") {
      AddError(result, "core.attr.connectability", location,
               "connectability must be `full` or `interfaceOnly`, but is `" +
                   metas->connectability + "`");
    }
  }
  if ((metas->authored & PropMeta::kRenderType) &&
      metas->renderType.empty()) {
    AddWarning(result, "core.attr.renderType", location,
               "renderType should not be empty when authored");
  }
  if ((metas->authored & PropMeta::kOutputName) &&
      metas->outputName.empty()) {
    AddWarning(result, "core.attr.outputName", location,
               "outputName should not be empty when authored");
  }
}

void ValidateRelationshipMetadata(const PrimSpec &ps,
                                  const std::string &rel_name,
                                  const std::string &location,
                                  USDValidationResult *result) {
  const uint16_t flags = ps.relationship_flags(rel_name);
  if ((flags & PropSlot::kFlagVariabilityAuthored) &&
      (flags & PropSlot::kFlagVarying)) {
    AddWarning(result, "core.relationship.variability", location,
               "relationships have uniform variability; authored `varying` "
               "is ignored by USD value resolution");
  }
  const PropMeta *metas = ps.property_meta(rel_name);
  if (!metas) {
    return;
  }
  if (metas->authored & PropMeta::kCustomData) {
    ValidateDictValue(metas->customData, "core.relationship.customData",
                      location + ".customData", result);
  }
  if (metas->authored & PropMeta::kBindMaterialAs) {
    const std::string &value = metas->bindMaterialAs;
    if (value != "strongerThanDescendants" &&
        value != "weakerThanDescendants") {
      AddError(result, "core.relationship.bindMaterialAs", location,
               "bindMaterialAs must be `strongerThanDescendants` or "
               "`weakerThanDescendants`, but is `" +
                   value + "`");
    }
  }
}

}  // namespace validation_detail
}  // namespace next
}  // namespace lightusd
