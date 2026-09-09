// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - registry of PrimSpec string list-op metadata fields.
//
// The AOUSD elective prim fields composed with SdfStringListOp semantics
// (apiSchemas, variantSetNames, clipSets) share one merge rule: a STRONGER
// spec's authored op resolves over the WEAKER effective name list and the
// flattened result is stored as an explicit op (so downstream consumers
// cannot re-apply the local edit). This table gives composition (and any
// future writer/diff enumeration) a single registry instead of bespoke
// per-field blocks.

#pragma once

#include "prim-spec.hh"

#include <algorithm>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

struct StringListOpFieldDef {
  const char* name;

  /// The field's authored list-op, NORMALIZED: fields that can carry an
  /// authored opinion outside StringListOpEdits (apiSchemas' legacy
  /// qualifier + applied vector) are converted to an equivalent edit.
  /// `authored == false` means the spec has no mergeable op for this field.
  StringListOpEdits (*normalized_edits)(const PrimSpecMeta& meta);

  /// Names implied by the field's backing storage on this spec (the applied
  /// apiSchemas vector, clips dictionary keys, variantSets declarations).
  /// These join the weaker base list before a stronger op resolves over it.
  std::vector<std::string> (*local_names)(const PrimSpecMeta& meta);

  StringListOpEdits& (*edits_mut)(PrimSpecMeta& meta);

  /// Field-specific post steps (null when unneeded). `post_resolve` runs
  /// after a stronger op resolved over the weaker list (apiSchemas: publish
  /// the resolved vector + clear the legacy qualifier); `post_fill` runs
  /// after fill-absent copied a weaker authored op (apiSchemas: copy the
  /// applied vector + qualifier).
  void (*post_resolve)(PrimSpecMeta& target,
                       const std::vector<std::string>& resolved);
  void (*post_fill)(PrimSpecMeta& target, const PrimSpecMeta& source);
};

namespace listop_field_detail {

inline StringListOpEdits ApiSchemasNormalizedEdits(const PrimSpecMeta& meta) {
  if (meta.apiSchemaEdits().authored) return meta.apiSchemaEdits();
  StringListOpEdits edits;
  const std::string& qualifier = meta.apiSchemasQualifier();
  // A BARE authored list (empty qualifier) is an EXPLICIT op (pxr round-trips
  // bare authoring as explicit): it replaces weaker opinions outright.
  if (qualifier.empty()) {
    if (meta.apiSchemasAuthored() || !meta.apiSchemas().empty()) {
      edits.authored = true;
      edits.is_explicit = true;
      edits.explicit_items = meta.apiSchemas();
    }
  } else if (qualifier == "prepend") {
    edits.authored = true;
    edits.is_explicit = false;
    edits.prepended = meta.apiSchemas();
  } else if (qualifier == "append") {
    edits.authored = true;
    edits.is_explicit = false;
    edits.appended = meta.apiSchemas();
  } else if (qualifier == "delete") {
    edits.authored = true;
    edits.is_explicit = false;
    edits.deleted = meta.apiSchemas();
  }
  return edits;
}

inline std::vector<std::string> ApiSchemasLocalNames(const PrimSpecMeta& meta) {
  return meta.apiSchemas();
}

inline StringListOpEdits& ApiSchemasEditsMut(PrimSpecMeta& meta) {
  return meta.apiSchemaEdits();
}

inline void ApiSchemasPostResolve(PrimSpecMeta& target,
                                  const std::vector<std::string>& resolved) {
  target.apiSchemas() = resolved;
  target.setApiSchemasAuthored();
  target.apiSchemasQualifier().clear();
}

inline void ApiSchemasPostFill(PrimSpecMeta& target,
                               const PrimSpecMeta& source) {
  if (target.apiSchemas().empty()) {
    target.apiSchemas() = source.apiSchemas();
    target.setApiSchemasAuthored();
    if (target.apiSchemasQualifier().empty()) {
      target.apiSchemasQualifier() = source.apiSchemasQualifier();
    }
  }
}

inline StringListOpEdits VariantSetNamesNormalizedEdits(
    const PrimSpecMeta& meta) {
  return meta.variantSetNameEdits().authored ? meta.variantSetNameEdits()
                                             : StringListOpEdits();
}

inline std::vector<std::string> VariantSetNamesLocalNames(
    const PrimSpecMeta& meta) {
  std::vector<std::string> names;
  for (const VariantSetData& vs : meta.variantSets()) {
    if (std::find(names.begin(), names.end(), vs.name) == names.end()) {
      names.push_back(vs.name);
    }
  }
  return names;
}

inline StringListOpEdits& VariantSetNamesEditsMut(PrimSpecMeta& meta) {
  return meta.variantSetNameEdits();
}

inline StringListOpEdits ClipSetsNormalizedEdits(const PrimSpecMeta& meta) {
  return meta.clipSetEdits().authored ? meta.clipSetEdits()
                                      : StringListOpEdits();
}

inline std::vector<std::string> ClipSetsLocalNames(const PrimSpecMeta& meta) {
  std::vector<std::string> names;
  if (const Dict* d = meta.clips().as_dictionary()) {
    for (const auto& entry : d->entries()) names.push_back(entry.first);
  }
  // Dictionary storage order is not strength order; name order is the
  // deterministic weaker baseline (see ParseValueClipSets).
  std::sort(names.begin(), names.end());
  return names;
}

inline StringListOpEdits& ClipSetsEditsMut(PrimSpecMeta& meta) {
  return meta.clipSetEdits();
}

}  // namespace listop_field_detail

inline const StringListOpFieldDef* GetStringListOpFieldTable(size_t* count) {
  using namespace listop_field_detail;
  static const StringListOpFieldDef kFields[] = {
      {"apiSchemas", ApiSchemasNormalizedEdits, ApiSchemasLocalNames,
       ApiSchemasEditsMut, ApiSchemasPostResolve, ApiSchemasPostFill},
      {"variantSetNames", VariantSetNamesNormalizedEdits,
       VariantSetNamesLocalNames, VariantSetNamesEditsMut, nullptr, nullptr},
      {"clipSets", ClipSetsNormalizedEdits, ClipSetsLocalNames,
       ClipSetsEditsMut, nullptr, nullptr},
  };
  if (count) *count = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

/// Merge one registered string list-op field from a WEAKER `source` spec into
/// the STRONGER `target` (flatten direction, strong-first iteration):
/// - target has an authored op: resolve it over the weaker effective list
///   (source op applied to source storage names, plus target storage names)
///   and store the result as an explicit op.
/// - target has no opinion: fill-absent — copy the weaker authored op.
inline void MergeWeakerStringListOpField(const StringListOpFieldDef& def,
                                         const PrimSpecMeta& source,
                                         PrimSpecMeta& target) {
  const StringListOpEdits source_edits = def.normalized_edits(source);
  // Nothing weaker to merge: keep the target's authored representation
  // untouched (avoids converting a lone opinion to explicit form).
  if (!source_edits.authored && def.local_names(source).empty()) return;
  const StringListOpEdits target_edits = def.normalized_edits(target);

  if (target_edits.authored) {
    // Weaker effective list: the source's op applied over its own storage
    // names (an unauthored source op passes the names through).
    std::vector<std::string> base =
        ApplyStringListOp(source_edits, def.local_names(source));
    // The target's own storage names are weaker than its op but stronger
    // than the source; append the ones the source didn't provide.
    for (const std::string& name : def.local_names(target)) {
      if (std::find(base.begin(), base.end(), name) == base.end()) {
        base.push_back(name);
      }
    }
    std::vector<std::string> resolved = ApplyStringListOp(target_edits, base);
    StringListOpEdits& dst = def.edits_mut(target);
    dst = StringListOpEdits();
    dst.authored = true;
    dst.is_explicit = true;
    dst.explicit_items = resolved;
    if (def.post_resolve) def.post_resolve(target, resolved);
  } else if (source_edits.authored) {
    def.edits_mut(target) = source_edits;
    if (def.post_fill) def.post_fill(target, source);
  }
}

}  // namespace next
}  // namespace lightusd
