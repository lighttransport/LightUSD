// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Layer / PrimSpec diff (see layer-diff.hh).

#include "layer-diff.hh"
#include "layer-diff-internal.hh"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

#include "../layer/prim-spec.hh"
#include "../layer/property-index.hh"
#include "../types/type-id.hh"
#include "../types/value.hh"
#include "../writer/value-printer.hh"

// This translation unit performs exact bit-level value comparisons for diffing
// (the `a == b` fast path in ScalarAlmostEqual deliberately treats inf==inf as
// equal and +0/-0 as equal; layer-offset compares are exact). These intentional
// float equality checks trip -Wfloat-equal.
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace lightusd {
namespace next {

namespace diff_detail {
std::string JoinPrimPath(const std::string &parent, const std::string &child) {
  if (parent.empty() || parent == "/") {
    return "/" + child;
  }
  return parent + "/" + child;
}

}  // namespace diff_detail

namespace {

using namespace diff_detail;

std::string TruncateForDiff(const std::string &s) {
  // Retain enough of each value that the differing region survives for the
  // diff-aware centering done at render time (CenterValuePairForDiff).
  constexpr size_t kMaxDiffValueChars = 4096;
  if (s.size() <= kMaxDiffValueChars) {
    return s;
  }
  return s.substr(0, kMaxDiffValueChars) + "...";
}


std::string JoinPathList(const std::vector<Path> &paths) {
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i > 0) {
      ss << ", ";
    }
    ss << "<" << paths[i].str() << ">";
  }
  ss << "]";
  return ss.str();
}

// ---------------------------------------------------------------------------
// Property view: unified enumeration of attributes and relationships.
// ---------------------------------------------------------------------------

struct PropView {
  const PropSlot *slot = nullptr;               // attribute slot (may be null)
  const std::vector<Path> *rel_targets = nullptr;  // set for relationships
  bool is_rel = false;
};

// Name-sorted so diff output ordering is deterministic.
std::map<std::string, PropView> CollectProps(const PrimSpec &ps) {
  std::map<std::string, PropView> out;
  const PropNameTable &names = GetPropNameTable();
  for (const PropSlot &slot : ps.properties().slots()) {
    if (!slot.name_id.is_valid()) continue;
    PropView v;
    v.slot = &slot;
    v.is_rel = slot.is_relationship();
    out[std::string(names.get(slot.name_id))] = v;
  }
  for (const std::string &rel : ps.relationship_names()) {
    PropView &v = out[rel];
    v.is_rel = true;
    v.rel_targets = ps.relationship(rel);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Canonical strings / formatting.
// ---------------------------------------------------------------------------

std::string DblStr(double v) {
  std::stringstream ss;
  ss << v;
  return ss.str();
}

std::string StrListToStr(const std::vector<std::string> &v) {
  std::stringstream ss;
  ss << "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) ss << ", ";
    ss << v[i];
  }
  ss << "]";
  return ss.str();
}

std::string ArcEditToStr(const ArcListOp &e) {
  if (!e.authored) return "<none>";
  std::stringstream ss;
  ss << (e.is_explicit ? "explicit" : "listop");
  if (e.is_value_block) ss << " valueBlock";
  ss << " add" << StrListToStr(e.added);
  ss << " prepend" << StrListToStr(e.prepended);
  ss << " append" << StrListToStr(e.appended);
  ss << " delete" << StrListToStr(e.deleted);
  ss << " order" << StrListToStr(e.ordered);
  return ss.str();
}

std::string VariantSelectionsToStr(
    const std::string &legacy_single,
    const std::vector<std::pair<std::string, std::string>> &sels) {
  // Canonical, order-insensitive set of "set=sel" entries.
  std::set<std::string> all;
  if (!legacy_single.empty()) all.insert(legacy_single);
  for (const auto &kv : sels) all.insert(kv.first + "=" + kv.second);
  std::stringstream ss;
  for (const auto &s : all) ss << s << ",";
  return ss.str();
}

std::string PairListToStr(
    const std::vector<std::pair<std::string, std::string>> &v) {
  std::stringstream ss;
  for (const auto &kv : v) ss << kv.first << "->" << kv.second << ",";
  return ss.str();
}

std::string ValueToDiffString(const Value &v) {
  if (v.is_block()) return "None";
  if (v.is_empty()) return "<none>";
  PrintOptions popts;
  popts.compact = true;
  // PrintValue materializes lazy arrays internally (via a temporary copy).
  return TruncateForDiff(PrintValue(v, popts));
}

// Declared type name of a property, preferring the recorded USD type string
// and falling back to the slot's runtime type.
std::string DeclaredTypeName(const PrimSpec &ps, const std::string &name,
                             const PropView &view) {
  if (const std::string *tn = ps.property_type_name(name)) {
    return *tn;
  }
  if (view.slot) {
    return PrintTypeName(static_cast<TypeId>(view.slot->value_type),
                         view.slot->is_array());
  }
  return "";
}

// Role-type equivalence for declared type names: point3f / normal3f / color3f
// vs float3 (etc.) share storage and count as the same underlying type.
bool TypeNamesEquivalent(const std::string &lhs, const std::string &rhs) {
  if (lhs == rhs) return true;
  if (lhs.empty() || rhs.empty()) return false;

  auto split = [](const std::string &s, bool *is_array) -> std::string {
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "[]") == 0) {
      *is_array = true;
      return s.substr(0, s.size() - 2);
    }
    *is_array = false;
    return s;
  };
  bool larr = false, rarr = false;
  const std::string lbase = split(lhs, &larr);
  const std::string rbase = split(rhs, &rarr);
  if (larr != rarr) return false;

  const TypeId lt = GetTypeIdFromName(lbase.c_str());
  const TypeId rt = GetTypeIdFromName(rbase.c_str());
  if (lt == TypeId::Invalid || rt == TypeId::Invalid) return false;
  if (lt == rt) return true;

  // Quaternions stay distinct from plain vec4s (matching legacy semantics).
  auto is_quat = [](TypeId t) {
    return t == TypeId::Quath || t == TypeId::Quatf || t == TypeId::Quatd;
  };
  if (is_quat(lt) != is_quat(rt)) return false;

  const TypeId lcomp = GetComponentType(lt);
  const TypeId rcomp = GetComponentType(rt);
  if (lcomp == TypeId::Invalid || rcomp == TypeId::Invalid) return false;
  return lcomp == rcomp && GetComponentCount(lt) == GetComponentCount(rt);
}

std::string FormatPropertyForDiff(const PrimSpec &ps, const std::string &name,
                                  const PropView &view) {
  std::stringstream ss;
  const uint16_t flags =
      view.slot ? view.slot->flags
                : (view.is_rel ? ps.relationship_flags(name) : uint16_t(0));
  if (flags & PropSlot::kFlagCustom) ss << "custom ";
  if (flags & PropSlot::kFlagUniform) ss << "uniform ";

  if (view.is_rel) {
    ss << "rel";
    const std::vector<Path> *targets =
        view.rel_targets ? view.rel_targets : ps.relationship(name);
    if (targets) {
      if (targets->size() == 1) {
        ss << " target=<" << (*targets)[0].str() << ">";
      } else {
        ss << " targets=" << JoinPathList(*targets);
      }
    }
    const auto &edits = ps.relationship_edits();
    auto it = edits.find(name);
    if (it != edits.end() && it->second.has_qualifiers()) {
      ss << " listOp=" << ArcEditToStr(it->second);
    }
    return ss.str();
  }

  ss << "attr";
  const std::string tn = DeclaredTypeName(ps, name, view);
  if (!tn.empty()) ss << " type=" << tn;

  if (view.slot) {
    const std::vector<Path> *conns = ps.connection(name);
    if (conns) {
      ss << " connections=" << JoinPathList(*conns);
    }
    const Value *v = ps.property_value(view.slot->name_id);
    if (v && v->is_block()) {
      ss << " value=None";
    } else if (v && !v->is_empty()) {
      ss << " value=" << ValueToDiffString(*v);
    }
    const auto *samples = ps.time_samples(view.slot->name_id);
    if (samples && !samples->empty()) {
      ss << " timeSamples=" << samples->size()
         << " firstTime=" << samples->front().first
         << " lastTime=" << samples->back().first;
    }
  }
  return ss.str();
}

// ---------------------------------------------------------------------------
// Per-property metadata (PropMeta) comparison.
// ---------------------------------------------------------------------------

bool ExtensionFieldsEqual(const std::vector<DecodedExtensionField>& lhs,
                          const std::vector<DecodedExtensionField>& rhs,
                          const DiffOptions& opts) {
  if (lhs.size() != rhs.size()) return false;
  for (const DecodedExtensionField& field : lhs) {
    const auto it = std::find_if(
        rhs.begin(), rhs.end(), [&](const DecodedExtensionField& candidate) {
          return candidate.name == field.name;
        });
    if (it == rhs.end() || it->unregistered != field.unregistered ||
        it->unregistered_source != field.unregistered_source ||
        !ValuesEquivalentForDiff(field.value, it->value, opts)) {
      return false;
    }
  }
  return true;
}

bool ComparePropMetas(const PropMeta *lhs, const PropMeta *rhs,
                      const DiffOptions &opts,
                      std::vector<std::string> *reasons) {
  static const PropMeta kEmpty;
  const PropMeta &l = lhs ? *lhs : kEmpty;
  const PropMeta &r = rhs ? *rhs : kEmpty;
  bool equal = true;
  auto note = [&](const char *field) {
    equal = false;
    if (reasons) reasons->push_back(std::string("meta:") + field);
  };
  auto both = [&](uint32_t bit) {
    return (l.authored & bit) || (r.authored & bit);
  };
  auto cmp_str = [&](uint32_t bit, const std::string &a, const std::string &b,
                     const char *field) {
    if (((l.authored & bit) != (r.authored & bit)) || (both(bit) && a != b)) {
      note(field);
    }
  };

  cmp_str(PropMeta::kInterpolation, l.interpolation, r.interpolation,
          "interpolation");
  cmp_str(PropMeta::kColorSpace, l.colorSpace, r.colorSpace, "colorSpace");
  cmp_str(PropMeta::kDisplayName, l.displayName, r.displayName, "displayName");
  cmp_str(PropMeta::kDisplayGroup, l.displayGroup, r.displayGroup,
          "displayGroup");
  cmp_str(PropMeta::kDoc, l.doc, r.doc, "doc");
  cmp_str(PropMeta::kRenderType, l.renderType, r.renderType, "renderType");
  cmp_str(PropMeta::kConnectability, l.connectability, r.connectability,
          "connectability");
  cmp_str(PropMeta::kOutputName, l.outputName, r.outputName, "outputName");
  cmp_str(PropMeta::kBindMaterialAs, l.bindMaterialAs, r.bindMaterialAs,
          "bindMaterialAs");
  cmp_str(PropMeta::kKind, l.kind, r.kind, "kind");

  if (((l.authored & PropMeta::kElementSize) !=
       (r.authored & PropMeta::kElementSize)) ||
      (both(PropMeta::kElementSize) && l.elementSize != r.elementSize)) {
    note("elementSize");
  }
  if (((l.authored & PropMeta::kUnauthoredIdx) !=
       (r.authored & PropMeta::kUnauthoredIdx)) ||
      (both(PropMeta::kUnauthoredIdx) &&
       l.unauthoredValuesIndex != r.unauthoredValuesIndex)) {
    note("unauthoredValuesIndex");
  }
  if (((l.authored & PropMeta::kWeight) != (r.authored & PropMeta::kWeight)) ||
      (both(PropMeta::kWeight) && !ScalarAlmostEqual(l.weight, r.weight, opts))) {
    note("weight");
  }
  if (((l.authored & PropMeta::kHidden) != (r.authored & PropMeta::kHidden)) ||
      (both(PropMeta::kHidden) && l.hidden != r.hidden)) {
    note("hidden");
  }
  if (((l.authored & PropMeta::kAllowedTokens) !=
       (r.authored & PropMeta::kAllowedTokens)) ||
      (both(PropMeta::kAllowedTokens) && l.allowedTokens != r.allowedTokens)) {
    note("allowedTokens");
  }
  if (!CompareDictValues(l.customData, r.customData, opts, nullptr)) {
    note(DictsHaveTypeConflict(l.customData, r.customData) ? "customData(type-conflict)"
                                      : "customData");
  }
  if (!CompareDictValues(l.assetInfo, r.assetInfo, opts, nullptr)) {
    note(DictsHaveTypeConflict(l.assetInfo, r.assetInfo) ? "assetInfo(type-conflict)"
                                      : "assetInfo");
  }
  if (!CompareDictValues(l.sdrMetadata, r.sdrMetadata, opts, nullptr)) {
    note(DictsHaveTypeConflict(l.sdrMetadata, r.sdrMetadata) ? "sdrMetadata(type-conflict)"
                                      : "sdrMetadata");
  }
  if (!ExtensionFieldsEqual(l.unknownFields, r.unknownFields, opts)) {
    note("extensionFields");
  }
  return equal;
}

// ---------------------------------------------------------------------------
// Property comparison (attribute or relationship).
// ---------------------------------------------------------------------------

bool ComparePropertyDetailed(const PrimSpec &lp, const std::string &name,
                             const PropView &lv, const PrimSpec &rp,
                             const PropView &rv, const DiffOptions &opts,
                             std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lv.is_rel != rv.is_rel) {
    note("kind");
    return false;
  }

  const uint16_t lflags =
      lv.slot ? lv.slot->flags
              : (lv.is_rel ? lp.relationship_flags(name) : uint16_t(0));
  const uint16_t rflags =
      rv.slot ? rv.slot->flags
              : (rv.is_rel ? rp.relationship_flags(name) : uint16_t(0));
  if ((lflags & PropSlot::kFlagCustom) != (rflags & PropSlot::kFlagCustom)) {
    note("custom");
  }
  if ((lflags & PropSlot::kFlagUniform) != (rflags & PropSlot::kFlagUniform)) {
    note("variability");
  }

  if (lv.is_rel) {
    // Relationship: target list (exact path compare).
    static const std::vector<Path> kNoPaths;
    const std::vector<Path> *lt = lv.rel_targets ? lv.rel_targets : &kNoPaths;
    const std::vector<Path> *rt = rv.rel_targets ? rv.rel_targets : &kNoPaths;
    bool tEqual = (lt->size() == rt->size());
    for (size_t i = 0; tEqual && i < lt->size(); ++i) {
      if ((*lt)[i].str() != (*rt)[i].str()) tEqual = false;
    }
    if (!tEqual) note("targets");

    // Authored list-op edits.
    static const ArcListOp kNoEdit;
    auto edit_of = [&](const PrimSpec &ps) -> const ArcListOp & {
      const auto &edits = ps.relationship_edits();
      auto it = edits.find(name);
      return (it == edits.end()) ? kNoEdit : it->second;
    };
    if (ArcEditToStr(edit_of(lp)) != ArcEditToStr(edit_of(rp))) {
      note("listOp");
    }
    return equal;
  }

  // Attribute: declared type.
  const std::string ltn = DeclaredTypeName(lp, name, lv);
  const std::string rtn = DeclaredTypeName(rp, name, rv);
  if (!TypeNamesEquivalent(ltn, rtn)) {
    note("type");
  }

  // Connections (exact path compare; nullptr == not authored).
  const std::vector<Path> *lc = lp.connection(name);
  const std::vector<Path> *rc = rp.connection(name);
  if ((lc == nullptr) != (rc == nullptr)) {
    note("connections");
  } else if (lc && rc) {
    bool cEqual = (lc->size() == rc->size());
    for (size_t i = 0; cEqual && i < lc->size(); ++i) {
      if ((*lc)[i].str() != (*rc)[i].str()) cEqual = false;
    }
    if (!cEqual) note("connections");
  }
  static const ArcListOp kNoConnectionEdit;
  const ArcListOp* lce = lp.connection_edit(name);
  const ArcListOp* rce = rp.connection_edit(name);
  if (ArcEditToStr(lce ? *lce : kNoConnectionEdit) !=
      ArcEditToStr(rce ? *rce : kNoConnectionEdit)) {
    note("connectionListOp");
  }

  // Default value. A value BLOCK (`= None`) transition gets its own
  // differential: "blocked" distinguishes deleting/blocking an opinion from
  // an ordinary value edit.
  const Value *lval = lv.slot ? lp.property_value(lv.slot->name_id) : nullptr;
  const Value *rval = rv.slot ? rp.property_value(rv.slot->name_id) : nullptr;
  const bool lhas = lval && !lval->is_empty();
  const bool rhas = rval && !rval->is_empty();
  const bool lblock = lval && lval->is_block();
  const bool rblock = rval && rval->is_block();
  if (lblock != rblock) {
    note("blocked");
  } else if (lhas != rhas) {
    note("value");
  } else if (lhas && !lblock) {
    if (!ValuesEquivalentForDiff(*lval, *rval, opts)) {
      note("value");
    }
  }

  // Time samples (times compared with timeUlps; values with the normal opts).
  const std::vector<std::pair<double, uint32_t>> *ls =
      lv.slot ? lp.time_samples(lv.slot->name_id) : nullptr;
  const std::vector<std::pair<double, uint32_t>> *rs =
      rv.slot ? rp.time_samples(rv.slot->name_id) : nullptr;
  const bool lts = ls && !ls->empty();
  const bool rts = rs && !rs->empty();
  if (lts != rts) {
    note("timeSamples");
  } else if (lts) {
    DiffOptions timeOpts = opts;
    timeOpts.doubleUlps = opts.timeUlps;
    bool tsEqual = (ls->size() == rs->size());
    for (size_t i = 0; tsEqual && i < ls->size(); ++i) {
      if (!ScalarAlmostEqual((*ls)[i].first, (*rs)[i].first, timeOpts)) {
        tsEqual = false;
        break;
      }
      const Value *lsv = lp.time_sample_value((*ls)[i].second);
      const Value *rsv = rp.time_sample_value((*rs)[i].second);
      if ((lsv == nullptr) != (rsv == nullptr)) {
        tsEqual = false;
      } else if (lsv && !ValuesEquivalentForDiff(*lsv, *rsv, opts)) {
        tsEqual = false;
      }
    }
    if (!tsEqual) note("timeSamples");
  }

  // Per-property metadata.
  if (opts.compareMetadata) {
    const PropMeta *lm = lp.property_meta(name);
    const PropMeta *rm = rp.property_meta(name);
    if (!ComparePropMetas(lm, rm, opts, reasons)) {
      equal = false;
    }
  }

  return equal;
}

// ---------------------------------------------------------------------------
// Deep per-field variant-set comparison. Replaces the former stringified
// name/selection/option-name comparison, which produced no differential for
// variant-INNER edits (properties, arcs, relationships, unknown metadata,
// nested sets). On the first difference, `reason` is set to a granular
// "variantSets:<set>/<option>:<field>" tag.
// ---------------------------------------------------------------------------

bool VariantOptionEqual(const PrimVariantData &l, const PrimVariantData &r,
                        const DiffOptions &opts, std::string *reason,
                        int depth);

bool VariantSetsEqual(const std::vector<PrimVariantSetData> &l,
                      const std::vector<PrimVariantSetData> &r,
                      const DiffOptions &opts, std::string *reason,
                      int depth = 0) {
  auto fail = [&](const std::string &r_) {
    if (reason && reason->empty()) *reason = r_;
    return false;
  };
  if (depth > 64) return true;  // matches the parser's nesting cap
  if (l.size() != r.size()) return fail("variantSets:count");
  for (const PrimVariantSetData &ls : l) {
    const PrimVariantSetData *rs = nullptr;
    for (const PrimVariantSetData &cand : r) {
      if (cand.name == ls.name) {
        rs = &cand;
        break;
      }
    }
    if (!rs) return fail("variantSets:" + ls.name);
    if (ls.selected != rs->selected) {
      return fail("variantSets:" + ls.name + ":selected");
    }
    if (ls.variants.size() != rs->variants.size()) {
      return fail("variantSets:" + ls.name + ":options");
    }
    for (const PrimVariantData &lv : ls.variants) {
      const PrimVariantData *rv = nullptr;
      for (const PrimVariantData &cand : rs->variants) {
        if (cand.name == lv.name) {
          rv = &cand;
          break;
        }
      }
      const std::string opt = "variantSets:" + ls.name + "/" + lv.name;
      if (!rv) return fail(opt);
      std::string inner;
      if (!VariantOptionEqual(lv, *rv, opts, &inner, depth)) {
        return fail(opt + ":" + inner);
      }
    }
  }
  return true;
}

bool VariantOptionEqual(const PrimVariantData &l, const PrimVariantData &r,
                        const DiffOptions &opts, std::string *reason,
                        int depth) {
  auto fail = [&](const char *r_) {
    if (reason && reason->empty()) *reason = r_;
    return false;
  };
  if (l.active != r.active) return fail("active");
  if (l.hidden != r.hidden) return fail("hidden");
  if (l.doc != r.doc) return fail("doc");
  if (l.references != r.references) return fail("references");
  if (l.payloads != r.payloads) return fail("payload");
  if (l.inherits != r.inherits) return fail("inherits");
  if (l.specializes != r.specializes) return fail("specializes");
  if (l.variantSelections != r.variantSelections) return fail("variants");
  if (l.unknownMeta != r.unknownMeta) return fail("unknownMeta");
  if (!ExtensionFieldsEqual(l.unknownFields, r.unknownFields, opts)) {
    return fail("extensionFields");
  }
  if (l.properties.size() != r.properties.size()) return fail("properties");
  for (const VariantProperty &lp : l.properties) {
    const VariantProperty *rp = nullptr;
    for (const VariantProperty &cand : r.properties) {
      if (cand.name == lp.name) {
        rp = &cand;
        break;
      }
    }
    if (!rp || lp.flags != rp->flags || !(lp.value == rp->value)) {
      return fail("properties");
    }
  }
  if (l.relationships.size() != r.relationships.size()) {
    return fail("relationships");
  }
  for (const auto &lr : l.relationships) {
    auto it = r.relationships.find(lr.first);
    if (it == r.relationships.end() || it->second != lr.second) {
      return fail("relationships");
    }
  }
  if (l.relationshipFlags != r.relationshipFlags) {
    return fail("relationships");
  }
  // Content subtrees compare structurally (prim count + paths): a full
  // recursive layer diff here would recurse into this same machinery.
  const size_t lc = l.content ? l.content->prim_count() : 0;
  const size_t rc = r.content ? r.content->prim_count() : 0;
  if (lc != rc) return fail("content");
  if (l.content && r.content) {
    for (size_t i = 0; i < lc; ++i) {
      const PrimSpec *lp = l.content->prim(static_cast<uint32_t>(i));
      const PrimSpec *rp = r.content->prim(static_cast<uint32_t>(i));
      if (!lp || !rp || lp->path().str() != rp->path().str()) {
        return fail("content");
      }
    }
  }
  std::string nested;
  if (!VariantSetsEqual(l.variantSets, r.variantSets, opts, &nested,
                        depth + 1)) {
    return fail("nested");
  }
  return true;
}

// ---------------------------------------------------------------------------
// PrimSpec own-field comparison (specifier / typeName / metadata).
// ---------------------------------------------------------------------------

bool ComparePrimMeta(const PrimMetadata &l, const PrimMetadata &r,
                     const DiffOptions &opts,
                     std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *field) {
    equal = false;
    if (reasons) reasons->push_back(field);
  };

  if (l.active != r.active || l.active_authored != r.active_authored) {
    note("meta:active");
  }
  if (l.hidden != r.hidden || l.hidden_authored != r.hidden_authored) {
    note("meta:hidden");
  }
  if (l.instanceable != r.instanceable) {
    note("meta:instanceable");
  }

  // Composition arcs: inline vectors + authored list-op qualifiers.
  static const ArcListOps kNoEdits;
  const ArcListOps &le = l.arc_edits() ? *l.arc_edits() : kNoEdits;
  const ArcListOps &re = r.arc_edits() ? *r.arc_edits() : kNoEdits;
  auto cmp_arc = [&](const std::vector<std::string> &a,
                     const std::vector<std::string> &b, const ArcListOp &ea,
                     const ArcListOp &eb, const char *field) {
    if (a != b || ArcEditToStr(ea) != ArcEditToStr(eb)) {
      note(field);
    }
  };
  cmp_arc(l.references, r.references, le.references, re.references,
          "meta:references");
  cmp_arc(l.payloads, r.payloads, le.payloads, re.payloads, "meta:payload");
  cmp_arc(l.inherits, r.inherits, le.inherits, re.inherits, "meta:inherits");
  cmp_arc(l.specializes, r.specializes, le.specializes, re.specializes,
          "meta:specializes");

  if (VariantSelectionsToStr(l.variantSelection, l.variantSelections()) !=
      VariantSelectionsToStr(r.variantSelection, r.variantSelections())) {
    note("meta:variants");
  }
  {
    std::string vreason;
    if (!VariantSetsEqual(l.variantSets(), r.variantSets(), opts,
                          &vreason)) {
      equal = false;
      if (reasons) reasons->push_back("meta:" + vreason);
    }
  }
  const StringListOps& lv = l.variantSetNameEdits();
  const StringListOps& rv = r.variantSetNameEdits();
  if (lv.authored != rv.authored || lv.is_explicit != rv.is_explicit ||
      lv.explicit_items != rv.explicit_items || lv.added != rv.added ||
      lv.prepended != rv.prepended || lv.appended != rv.appended ||
      lv.deleted != rv.deleted || lv.ordered != rv.ordered) {
    note("meta:variantSetNames");
  }
  if (l.primOrder() != r.primOrder() ||
      l.primOrderAuthored() != r.primOrderAuthored()) {
    note("meta:primOrder");
  }
  if (l.propertyOrder() != r.propertyOrder() ||
      l.propertyOrderAuthored() != r.propertyOrderAuthored()) {
    note("meta:propertyOrder");
  }
  if (l.layer_offset != r.layer_offset) {
    note("meta:layerOffset");
  }

  if (l.kind() != r.kind() || l.kindAuthored() != r.kindAuthored())
    note("meta:kind");
  if (l.doc() != r.doc()) note("meta:doc");
  if (l.comment() != r.comment()) note("meta:comment");
  if (l.displayName() != r.displayName() ||
      l.displayNameAuthored() != r.displayNameAuthored())
    note("meta:displayName");
  if (l.instance_prototype() != r.instance_prototype()) {
    note("meta:instancePrototype");
  }
  if (l.apiSchemas() != r.apiSchemas() ||
      l.apiSchemasQualifier() != r.apiSchemasQualifier()) {
    note("meta:apiSchemas");
  }
  const StringListOps& la = l.apiSchemaEdits();
  const StringListOps& ra = r.apiSchemaEdits();
  if (la.authored != ra.authored || la.is_explicit != ra.is_explicit ||
      la.explicit_items != ra.explicit_items || la.added != ra.added ||
      la.prepended != ra.prepended || la.appended != ra.appended ||
      la.deleted != ra.deleted || la.ordered != ra.ordered) {
    note("meta:apiSchemasListOp");
  }
  if (PairListToStr(l.relocates()) != PairListToStr(r.relocates())) {
    note("meta:relocates");
  }
  if (!CompareDictValues(l.customData(), r.customData(), opts, nullptr)) {
    note(DictsHaveTypeConflict(l.customData(), r.customData()) ? "meta:customData(type-conflict)"
                                      : "meta:customData");
  }
  if (!CompareDictValues(l.assetInfo(), r.assetInfo(), opts, nullptr)) {
    note(DictsHaveTypeConflict(l.assetInfo(), r.assetInfo()) ? "meta:assetInfo(type-conflict)"
                                      : "meta:assetInfo");
  }
  if (!CompareDictValues(l.sdrMetadata(), r.sdrMetadata(), opts, nullptr)) {
    note(DictsHaveTypeConflict(l.sdrMetadata(), r.sdrMetadata()) ? "meta:sdrMetadata(type-conflict)"
                                      : "meta:sdrMetadata");
  }
  if (!CompareDictValues(l.clips(), r.clips(), opts, nullptr)) {
    note("meta:clips");
  }
  if (!ExtensionFieldsEqual(l.unknownFields(), r.unknownFields(), opts)) {
    note("meta:extensionFields");
  }
  const StringListOps& lc = l.clipSetEdits();
  const StringListOps& rc = r.clipSetEdits();
  if (lc.authored != rc.authored || lc.is_explicit != rc.is_explicit ||
      lc.explicit_items != rc.explicit_items || lc.added != rc.added ||
      lc.prepended != rc.prepended || lc.appended != rc.appended ||
      lc.deleted != rc.deleted || lc.ordered != rc.ordered) {
    note("meta:clipSets");
  }
  return equal;
}

bool ComparePrimSpecs(const PrimSpec &lhs, const PrimSpec &rhs,
                      const DiffOptions &opts,
                      std::vector<std::string> *reasons) {
  bool equal = true;
  auto note = [&](const char *r) {
    equal = false;
    if (reasons) reasons->push_back(r);
  };

  if (lhs.specifier() != rhs.specifier()) note("specifier");
  if (lhs.type_name() != rhs.type_name()) note("typeName");

  if (opts.compareMetadata) {
    if (!ComparePrimMeta(lhs.meta(), rhs.meta(), opts, reasons)) {
      equal = false;
    }
  }
  return equal;
}

// ---------------------------------------------------------------------------
// Property diff at one prim path.
// ---------------------------------------------------------------------------

void ComputePropDiff(const std::string &path, const PrimSpec &lhs,
                     const PrimSpec &rhs, const DiffOptions &opts,
                     std::unordered_map<std::string, PropDiff> &propDiffs) {
  const std::map<std::string, PropView> lprops = CollectProps(lhs);
  const std::map<std::string, PropView> rprops = CollectProps(rhs);

  PropDiff diff;

  for (const auto &kv : rprops) {
    if (lprops.find(kv.first) == lprops.end()) {
      diff.addedProps.push_back(kv.first);
    }
  }
  for (const auto &kv : lprops) {
    if (rprops.find(kv.first) == rprops.end()) {
      diff.deletedProps.push_back(kv.first);
    }
  }
  for (const auto &kv : lprops) {
    auto it = rprops.find(kv.first);
    if (it == rprops.end()) continue;
    std::vector<std::string> reasons;
    if (!ComparePropertyDetailed(lhs, kv.first, kv.second, rhs, it->second,
                                 opts, &reasons)) {
      diff.modifiedProps.push_back(kv.first);
      PropDiff::ModifiedProp mp;
      mp.name = kv.first;
      mp.lhs = FormatPropertyForDiff(lhs, kv.first, kv.second);
      mp.rhs = FormatPropertyForDiff(rhs, kv.first, it->second);
      mp.reasons = std::move(reasons);
      diff.modifiedPropDetails.push_back(std::move(mp));
    }
  }

  if (!diff.addedProps.empty() || !diff.deletedProps.empty() ||
      !diff.modifiedProps.empty()) {
    propDiffs[path] = std::move(diff);
  }
}

// ---------------------------------------------------------------------------
// Recursive prim-tree diff.
// ---------------------------------------------------------------------------

bool ComputeDiffImpl(uint32_t depth, const std::string &path,
                     const Layer &lhsLayer, const PrimSpec &lhs,
                     const Layer &rhsLayer, const PrimSpec &rhs,
                     std::unordered_map<std::string, PrimSpecDiff> &psDiffs,
                     std::unordered_map<std::string, PropDiff> &propDiffs,
                     const DiffOptions &opts,
                     std::vector<std::string> *selfReasons) {
  if (depth > kMaxTraversalDepth) {
    return false;
  }

  // This prim's own structural / metadata differences.
  std::vector<std::string> reasons;
  ComparePrimSpecs(lhs, rhs, opts, &reasons);
  bool hasDiff = !reasons.empty();

  // Property differences (reported separately under this path).
  ComputePropDiff(path, lhs, rhs, opts, propDiffs);

  // Children (matched by name; order changes are not reported as diffs).
  //
  // Sorted vectors of (name, spec) rather than two std::map<std::string, ...>
  // built and destroyed for EVERY visited prim: the maps cost two red-black
  // nodes plus a string copy per child, then three more lookup passes over
  // them. These hold pointers into the specs' own names (no copies) and are
  // merged in a single pass. Name order -- which the reported add/delete/
  // modify lists depend on -- is preserved.
  using ChildRef = std::pair<const std::string *, const PrimSpec *>;
  auto by_name = [](const ChildRef &a, const ChildRef &b) {
    return *a.first < *b.first;
  };
  auto collect = [&](const PrimSpec &parent, const Layer &layer,
                     std::vector<ChildRef> *out) {
    out->reserve(parent.child_indices().size());
    for (uint32_t ci : parent.child_indices()) {
      if (const PrimSpec *c = layer.prim(ci)) out->push_back({&c->name(), c});
    }
    std::stable_sort(out->begin(), out->end(), by_name);
    // Duplicate names: keep the LAST, matching the previous `map[name] = c`.
    if (!out->empty()) {
      size_t w = 0;
      for (size_t r = 1; r < out->size(); ++r) {
        if (*(*out)[r].first == *(*out)[w].first) {
          (*out)[w] = (*out)[r];
        } else {
          (*out)[++w] = (*out)[r];
        }
      }
      out->resize(w + 1);
    }
  };

  std::vector<ChildRef> lhs_children, rhs_children;
  collect(lhs, lhsLayer, &lhs_children);
  collect(rhs, rhsLayer, &rhs_children);

  PrimSpecDiff psDiff;

  // Added first, then deleted, then modified -- the emission order the three
  // separate map passes produced.
  for (const ChildRef &r : rhs_children) {
    if (!std::binary_search(lhs_children.begin(), lhs_children.end(), r,
                            by_name)) {
      psDiff.addedPS.push_back(*r.first);
      hasDiff = true;
    }
  }
  for (const ChildRef &l : lhs_children) {
    if (!std::binary_search(rhs_children.begin(), rhs_children.end(), l,
                            by_name)) {
      psDiff.deletedPS.push_back(*l.first);
      hasDiff = true;
    }
  }
  for (const ChildRef &l : lhs_children) {
    const auto it =
        std::lower_bound(rhs_children.begin(), rhs_children.end(), l, by_name);
    if (it == rhs_children.end() || *it->first != *l.first) continue;
    const std::string child_path = JoinPrimPath(path, *l.first);
    std::vector<std::string> childReasons;
    if (ComputeDiffImpl(depth + 1, child_path, lhsLayer, *l.second, rhsLayer,
                        *it->second, psDiffs, propDiffs, opts,
                        &childReasons)) {
      psDiff.modifiedPS.push_back(*l.first);
      ModifiedPrimSpec mps;
      mps.name = *l.first;
      mps.reasons = std::move(childReasons);
      psDiff.modifiedDetails.push_back(std::move(mps));
      hasDiff = true;
    }
  }

  if (!psDiff.addedPS.empty() || !psDiff.deletedPS.empty() ||
      !psDiff.modifiedPS.empty()) {
    psDiffs[path] = std::move(psDiff);
  }

  if (selfReasons) *selfReasons = std::move(reasons);
  return hasDiff;
}

// ---------------------------------------------------------------------------
// Layer (stage) metadata comparison.
// ---------------------------------------------------------------------------

bool CompareLayerMetas(const LayerMeta &lhs, const LayerMeta &rhs,
                       const DiffOptions &opts, LayerMetaDiff &out) {
  auto noteField = [&](const std::string &field, const std::string &ls,
                       const std::string &rs) {
    out.changedFields.push_back("~" + field);
    PropDiff::ModifiedProp mp;
    mp.name = field;
    mp.lhs = ls;
    mp.rhs = rs;
    out.details.push_back(std::move(mp));
  };

  auto cmpDouble = [&](const char *field, double a, double b) {
    if (!ScalarAlmostEqual(a, b, opts)) {
      noteField(field, DblStr(a), DblStr(b));
    }
  };

  cmpDouble("metersPerUnit", lhs.metersPerUnit, rhs.metersPerUnit);
  cmpDouble("timeCodesPerSecond", lhs.timeCodesPerSecond,
            rhs.timeCodesPerSecond);
  cmpDouble("framesPerSecond", lhs.framesPerSecond, rhs.framesPerSecond);
  cmpDouble("startTimeCode", lhs.startTimeCode, rhs.startTimeCode);
  cmpDouble("endTimeCode", lhs.endTimeCode, rhs.endTimeCode);
  cmpDouble("kilogramsPerUnit", lhs.kilogramsPerUnit, rhs.kilogramsPerUnit);

  if (lhs.upAxis != rhs.upAxis) {
    noteField("upAxis", lhs.upAxis, rhs.upAxis);
  }
  if (lhs.defaultPrim != rhs.defaultPrim ||
      lhs.defaultPrim_set != rhs.defaultPrim_set) {
    noteField("defaultPrim", lhs.defaultPrim, rhs.defaultPrim);
  }
  if (lhs.rootPrimOrder != rhs.rootPrimOrder ||
      lhs.rootPrimOrder_set != rhs.rootPrimOrder_set) {
    noteField("primOrder", std::to_string(lhs.rootPrimOrder.size()),
              std::to_string(rhs.rootPrimOrder.size()));
  }
  if (lhs.comment != rhs.comment) {
    noteField("comment", lhs.comment, rhs.comment);
  }
  if (lhs.doc != rhs.doc) {
    noteField("documentation", lhs.doc, rhs.doc);
  }
  if (lhs.owner != rhs.owner || lhs.owner_set != rhs.owner_set) {
    noteField("owner", lhs.owner, rhs.owner);
  }
  if (lhs.colorConfiguration != rhs.colorConfiguration) {
    noteField("colorConfiguration", lhs.colorConfiguration,
              rhs.colorConfiguration);
  }
  if (lhs.colorManagementSystem != rhs.colorManagementSystem) {
    noteField("colorManagementSystem", lhs.colorManagementSystem,
              rhs.colorManagementSystem);
  }
  if (lhs.renderSettingsPrimPath != rhs.renderSettingsPrimPath ||
      lhs.renderSettingsPrimPath_set != rhs.renderSettingsPrimPath_set) {
    noteField("renderSettingsPrimPath", lhs.renderSettingsPrimPath,
              rhs.renderSettingsPrimPath);
  }

  // subLayers (count + per-layer asset path / offset).
  bool subEqual = (lhs.subLayers.size() == rhs.subLayers.size());
  auto offset_of = [](const LayerMeta &m,
                      size_t i) -> std::pair<double, double> {
    if (i < m.subLayerOffsets.size()) return m.subLayerOffsets[i];
    return {0.0, 1.0};
  };
  for (size_t i = 0; subEqual && i < lhs.subLayers.size(); ++i) {
    const bool pathOk =
        opts.fuzzyAssetPaths
            ? AssetPathStringsEquivalentForDiff(lhs.subLayers[i],
                                                rhs.subLayers[i])
            : (lhs.subLayers[i] == rhs.subLayers[i]);
    if (!pathOk || offset_of(lhs, i) != offset_of(rhs, i)) {
      subEqual = false;
    }
  }
  if (!subEqual) {
    noteField("subLayers", std::to_string(lhs.subLayers.size()),
              std::to_string(rhs.subLayers.size()));
  }

  // customLayerData / expressionVariables dictionaries.
  {
    std::vector<std::string> keys;
    if (!CompareDictValues(lhs.customLayerData, rhs.customLayerData, opts,
                           &keys)) {
      for (const auto &k : keys) {
        out.changedFields.push_back("customLayerData:" + k);
      }
    }
  }
  {
    std::vector<std::string> keys;
    if (!CompareDictValues(lhs.expressionVariables, rhs.expressionVariables,
                           opts, &keys)) {
      for (const auto &k : keys) {
        out.changedFields.push_back("expressionVariables:" + k);
      }
    }
  }
  if (!ExtensionFieldsEqual(lhs.unknownFields, rhs.unknownFields, opts)) {
    noteField("extensionFields", std::to_string(lhs.unknownFields.size()),
              std::to_string(rhs.unknownFields.size()));
  }

  return out.changed();
}


}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------


void Diff(const Layer &lhs, const Layer &rhs,
          std::unordered_map<std::string, PrimSpecDiff> &psDiffs,
          std::unordered_map<std::string, PropDiff> &propDiffs,
          const DiffOptions &opts, LayerMetaDiff *layerMetaDiff) {
  // Root prims by name.
  std::map<std::string, const PrimSpec *> lhs_roots;
  std::map<std::string, const PrimSpec *> rhs_roots;
  for (uint32_t ri : lhs.root_indices()) {
    if (const PrimSpec *p = lhs.prim(ri)) lhs_roots[p->name()] = p;
  }
  for (uint32_t ri : rhs.root_indices()) {
    if (const PrimSpec *p = rhs.prim(ri)) rhs_roots[p->name()] = p;
  }

  PrimSpecDiff rootDiff;

  for (const auto &kv : rhs_roots) {
    if (lhs_roots.find(kv.first) == lhs_roots.end()) {
      rootDiff.addedPS.push_back(kv.first);
    }
  }
  for (const auto &kv : lhs_roots) {
    if (rhs_roots.find(kv.first) == rhs_roots.end()) {
      rootDiff.deletedPS.push_back(kv.first);
    }
  }
  for (const auto &kv : lhs_roots) {
    auto it = rhs_roots.find(kv.first);
    if (it == rhs_roots.end()) continue;
    const std::string prim_path = "/" + kv.first;
    std::vector<std::string> selfReasons;
    if (ComputeDiffImpl(0, prim_path, lhs, *kv.second, rhs, *it->second,
                        psDiffs, propDiffs, opts, &selfReasons)) {
      rootDiff.modifiedPS.push_back(kv.first);
      ModifiedPrimSpec mps;
      mps.name = kv.first;
      mps.reasons = std::move(selfReasons);
      rootDiff.modifiedDetails.push_back(std::move(mps));
    }
  }

  // Stage / layer metadata.
  LayerMetaDiff localLayerDiff;
  if (opts.compareMetadata) {
    CompareLayerMetas(lhs.meta(), rhs.meta(), opts, localLayerDiff);
  }
  if (layerMetaDiff) {
    *layerMetaDiff = std::move(localLayerDiff);
  }

  if (!rootDiff.addedPS.empty() || !rootDiff.deletedPS.empty() ||
      !rootDiff.modifiedPS.empty()) {
    psDiffs["/"] = std::move(rootDiff);
  }
}


}  // namespace next
}  // namespace lightusd
