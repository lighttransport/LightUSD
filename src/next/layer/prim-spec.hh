// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PrimSpec
// Unified prim specification that can serve as both spec and runtime prim
// Avoids the PrimSpec→Prim tree duplication of the old design

#pragma once

#include "property-index.hh"
#include "../types/value.hh"
#include "../types/interpolation.hh"
#include "../prim/path.hh"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <list>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <map>
#include <unordered_map>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <shared_mutex>
#endif

namespace lightusd {
namespace next {

/// Prim specifier (def, over, class)
enum class PrimSpecifier : uint8_t {
  Def = 0,    // Concrete definition
  Over = 1,   // Override
  Class = 2   // Abstract class
};

/// Type name ID (interned like property names)
struct TypeNameId {
  uint16_t id = UINT16_MAX;
  bool is_valid() const { return id != UINT16_MAX; }
};

/// Type name interning table
/// Thread-safe under LIGHTUSD_ENABLE_THREAD (authoring interns new type names
/// at runtime, so concurrent readers need protection like PropNameTable).
class TypeNameTable {
public:
  TypeNameId intern(const std::string& name);
  const std::string& get(TypeNameId id) const;
  TypeNameId find(const std::string& name) const;

  // Pre-registered type IDs
  TypeNameId id_Xform;
  TypeNameId id_Scope;
  TypeNameId id_Mesh;
  TypeNameId id_Points;
  TypeNameId id_BasisCurves;
  TypeNameId id_NurbsCurves;
  TypeNameId id_Sphere;
  TypeNameId id_Cube;
  TypeNameId id_Cylinder;
  TypeNameId id_Cone;
  TypeNameId id_Capsule;
  TypeNameId id_Camera;
  TypeNameId id_Material;
  TypeNameId id_Shader;
  TypeNameId id_SkelRoot;
  TypeNameId id_Skeleton;
  TypeNameId id_SkelAnimation;
  TypeNameId id_BlendShape;
  TypeNameId id_PointInstancer;
  TypeNameId id_GeomSubset;

  void register_common_types();

private:
  // deque, not vector: get() returns a `const std::string&` that callers may
  // hold after the lock is released; deque never relocates on push_back.
  std::deque<std::string> names_;
  detail::StringIndex name_to_id_;
  TypeNameId find_live(const std::string& name) const;
#if defined(LIGHTUSD_ENABLE_THREAD)
  mutable std::shared_mutex mu_;
#endif
};

TypeNameTable& GetTypeNameTable();

// Forward declarations
struct VariantData;
struct VariantSetData;
class Layer;  // for VariantData::content (variant subtree)

/// Generic decodable extension field retained from USDC. Unregistered fields
/// use Crate's recursive UnregisteredValue wrapper and must be re-encoded with
/// that wrapper rather than silently changing their schema type.
struct TypedExtensionField {
  std::string name;
  Value value;
  bool unregistered = false;
  std::string unregistered_source;
};

/// One attribute authored inside a variant option. Carries the property flags
/// (custom / uniform / connection / array) so the variant graft preserves them
/// — a bare name->Value pair would silently drop `custom`/`uniform`.
struct VariantProperty {
  std::string name;
  Value value;
  uint16_t flags = 0;

  VariantProperty() = default;
  VariantProperty(std::string n, Value v, uint16_t f = 0)
      : name(std::move(n)), value(std::move(v)), flags(f) {}
};

/// Variant data - properties and prims inside a single variant option
struct VariantData {
  VariantData();
  VariantData(const VariantData&);
  VariantData(VariantData&&) noexcept;
  VariantData& operator=(const VariantData&);
  VariantData& operator=(VariantData&&) noexcept;
  ~VariantData();

  std::string name;
  bool active = true;
  bool hidden = false;
  // Authored-state bits: `active = true` / `hidden = false` authored on the
  // option are real opinions (they must compose over weaker sources and
  // round-trip) — distinguishable from the unauthored defaults only via
  // these flags.
  bool active_authored = false;
  bool hidden_authored = false;
  std::string doc;
  std::string kind;  // option-authored `kind` composes onto the host
  std::vector<VariantProperty> properties;
  std::unordered_map<std::string, std::vector<Path>> relationships;
  std::unordered_map<std::string, uint16_t> relationshipFlags;
  // Composition arcs authored in the variant OPTION's metadata block, e.g.
  // `"opt" ( prepend payload = @./geo.usd@</geo> ) {}`. Stored in the same
  // canonical "@asset@</prim>" / "</prim>" encoding as PrimSpecMeta's arc
  // vectors (Compositor::ParseReference/ParsePayload decodes them). Composed
  // when this variant is selected — the geometry-defining payload of XGen-style
  // assets lives here, not in the variant body. Empty on the common case.
  std::vector<std::string> references;
  std::vector<std::string> payloads;
  std::vector<std::string> inherits;
  std::vector<std::string> specializes;
  // Variant selections authored on this option. These may target a sibling
  // variant set on the host prim, not only a set nested inside this option.
  std::vector<std::pair<std::string, std::string>> variantSelections;
  // Nested variant sets authored inside this variant option (recursive).
  std::vector<VariantSetData> variantSets;
  // Unknown (unmodeled) variant-option metadata: raw authored source text
  // (USDA, re-emitted verbatim) and decoded typed extension fields (USDC via
  // the materialized holder prim). Variant specs previously had NO generic
  // field storage, silently dropping pipeline-specific opinions.
  std::vector<std::pair<std::string, std::string>> unknownMeta;
  std::vector<TypedExtensionField> unknownFields;
  // Optional subtree for variants that add prim-level opinions and/or child
  // prims: a Layer whose root prim "__self__" carries the host opinions and
  // whose descendants become the host prim's children when this variant is
  // selected. (Composed reference-style, so it supports child prims.)
  std::shared_ptr<Layer> content;
};

/// One op of a sparse array edit (VtArrayEdit, usda 1.2 / crate 0.14).
/// Literal element values are carried as canonical usda element TEXT --
/// evaluation reprints/reparses through the usda codec, which keeps the op
/// list element-type-agnostic (see layer/array-edit.hh).
struct ArrayEditOpRec {
  enum Kind : uint8_t {
    WriteLiteral,  // write <literal> to [a2]
    WriteRef,      // write [a1] to [a2]
    InsertLiteral, // insert <literal> at [a2] (append/prepend sugar included)
    InsertRef,     // insert [a1] at [a2]
    Erase,         // erase [a1]
    MinSize,       // minsize a1 (fill <literal> when has_fill)
    SetSize,       // resize a1 (fill <literal> when has_fill)
    MaxSize,       // maxsize a1
  };
  Kind kind = WriteLiteral;
  int64_t a1 = 0;         // source/ref index or size argument
  int64_t a2 = 0;         // destination index (write/insert)
  bool has_fill = false;  // minsize/resize carries a fill literal
  std::string literal;    // canonical element text (literal ops and fills)
};

/// Insert-at-end sentinel (`append`): mirrors Vt_ArrayEditOps::EndIndex.
constexpr int64_t kArrayEditEnd = INT64_MIN;

/// A property's authored sparse array edit: the ordered op list. Weaker-over-
/// stronger composition concatenates op lists (weaker first), like pxr's
/// VtArrayEdit::ComposeOver.
struct ArrayEditData {
  std::vector<ArrayEditOpRec> ops;
};

/// Variant set - a named set of variant options
struct VariantSetData {
  std::string name;
  std::string selected;  // selected variant name for this set ("" = none)
  std::vector<VariantData> variants;
};

/// One composition-arc field's list-op edits, exactly as authored (Phase 7 S5,
/// AOUSD §10.3.2). `is_explicit` marks a bare `references = [...]` (or an
/// explicit op), in which case the effective items are the inline arc vector on
/// PrimSpecMeta; otherwise the effective list is composed from the prepend /
/// append / delete / order lists. Kept so composition can merge a site's specs
/// across the layer stack and the writer can re-emit the original qualifier.
struct ArcEdit {
  bool authored = false;     // true when this arc field authored list-op edits.
  bool is_explicit = true;  // bare list (the common case)
  std::vector<std::string> added;
  std::vector<std::string> prepended;
  std::vector<std::string> appended;
  std::vector<std::string> deleted;
  std::vector<std::string> ordered;

  bool has_qualifiers() const {
    return authored && (!is_explicit || !added.empty() || !prepended.empty() ||
           !appended.empty() || !deleted.empty() || !ordered.empty());
  }
  bool has_authored_opinion() const {
    return authored;
  }
};

/// Per-arc-type list-op edits for one PrimSpec (lazily allocated; only prims
/// that author a non-bare arc qualifier pay for it).
struct ArcListOpEdits {
  ArcEdit references;
  ArcEdit payloads;
  ArcEdit inherits;
  ArcEdit specializes;
};

/// Exact authored SdfStringListOp sublists. `variantSetNames` needs this in
/// addition to the effective VariantSetData vector: delete/order operations
/// may be inert locally yet remain authored data, and explicit-empty must not
/// collapse to an unauthored field.
struct StringListOpEdits {
  bool authored = false;
  bool is_explicit = false;
  std::vector<std::string> explicit_items;
  std::vector<std::string> added;
  std::vector<std::string> prepended;
  std::vector<std::string> appended;
  std::vector<std::string> deleted;
  std::vector<std::string> ordered;

  bool has_nonexplicit_items() const {
    return !added.empty() || !prepended.empty() || !appended.empty() ||
           !deleted.empty() || !ordered.empty();
  }
};

/// Fill a stronger dictionary's missing keys from a weaker one
/// (AOUSD §6.6.2 / §12.2). A key where one side is a dictionary and the other
/// is not is a TYPE CONFLICT: the stronger opinion correctly wins, but the
/// weaker subtree is silently shadowed — when `conflicts` is provided, the
/// dotted key path of each such collision is recorded so callers can surface
/// a diagnostic.
inline void MergeWeakerDictionaryValue(
    Value* stronger, const Value& weaker,
    std::vector<std::string>* conflicts = nullptr,
    const std::string& key_prefix = std::string()) {
  if (!stronger || !weaker.is_dictionary()) return;
  if (!stronger->is_dictionary()) {
    *stronger = weaker;
    return;
  }
  // Explicit DFS avoids exhausting the C++ stack for deeply nested
  // dictionaries created through the API. A frame processes one dictionary at
  // a time; descending immediately also keeps Value pointers stable while a
  // parent Dict's entry vector may grow.
  struct Frame {
    Value* destination_value;
    const Value* source_value;
    size_t next_entry;
    std::string prefix;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{stronger, &weaker, 0, key_prefix});
  while (!stack.empty()) {
    Frame& frame = stack.back();
    Dict* destination = frame.destination_value->as_dictionary();
    const Dict* source = frame.source_value->as_dictionary();
    if (!destination || !source || frame.next_entry >= source->entries().size()) {
      stack.pop_back();
      continue;
    }

    const auto& entry = source->entries()[frame.next_entry++];
    Value* existing = destination->find(entry.first);
    if (!existing) {
      destination->set(entry.first, entry.second);
    } else if (existing->is_dictionary() && entry.second.is_dictionary()) {
      stack.push_back(
          Frame{existing, &entry.second, 0, frame.prefix + entry.first + "."});
    } else if (conflicts &&
               existing->is_dictionary() != entry.second.is_dictionary()) {
      conflicts->push_back(frame.prefix + entry.first);
    }
  }
}

inline void MergeWeakerExtensionFields(
    std::vector<TypedExtensionField>* stronger,
    const std::vector<TypedExtensionField>& weaker) {
  if (!stronger) return;
  for (const TypedExtensionField& field : weaker) {
    auto existing = std::find_if(
        stronger->begin(), stronger->end(),
        [&](const TypedExtensionField& own) { return own.name == field.name; });
    if (existing == stronger->end()) {
      stronger->push_back(field);
    } else if (existing->value.is_dictionary() && field.value.is_dictionary()) {
      MergeWeakerDictionaryValue(&existing->value, field.value);
      // The authored source represented only the stronger dictionary. Force
      // writers to regenerate the merged UnregisteredValue source.
      if (existing->unregistered) existing->unregistered_source.clear();
    }
  }
}

inline void MergeWeakerRawFields(
    std::vector<std::pair<std::string, std::string>>* stronger,
    const std::vector<std::pair<std::string, std::string>>& weaker) {
  if (!stronger) return;
  for (const auto& field : weaker) {
    const bool present = std::find_if(
        stronger->begin(), stronger->end(),
        [&](const auto& own) { return own.first == field.first; }) !=
        stronger->end();
    if (!present) stronger->push_back(field);
  }
}

/// Apply an authored string list-op to a weaker effective list. This preserves
/// the authored sublists above while providing one shared effective-order rule
/// for composition and consumers such as value clips.
/// Apply an `ordered` (reorder) sublist to `list` with pxr SdfListOp
/// semantics (_ReorderKeysHelper): each ordered item present drags along the
/// contiguous run of non-ordered items that follow it, sequences are emitted
/// in the authored ordered order, and items before any ordered item stay at
/// the front.
inline void ApplyStringListOrder(const std::vector<std::string> &order,
                                 std::vector<std::string> *list) {
  if (!list || list->empty() || order.empty()) return;
  std::vector<std::string> unique_order;
  for (const std::string &o : order) {
    if (std::find(unique_order.begin(), unique_order.end(), o) ==
        unique_order.end()) {
      unique_order.push_back(o);
    }
  }
  auto in_set = [&](const std::string &s) {
    return std::find(unique_order.begin(), unique_order.end(), s) !=
           unique_order.end();
  };
  std::list<std::string> scratch(list->begin(), list->end());
  std::list<std::string> result;
  for (const std::string &o : unique_order) {
    auto j = std::find(scratch.begin(), scratch.end(), o);
    if (j == scratch.end()) continue;
    auto e = std::next(j);
    while (e != scratch.end() && !in_set(*e)) ++e;
    result.splice(result.end(), scratch, j, e);
  }
  result.splice(result.begin(), scratch);
  list->assign(result.begin(), result.end());
}

inline std::vector<std::string> ApplyStringListOp(
    const StringListOpEdits &edits,
    const std::vector<std::string> &weaker) {
  std::vector<std::string> result;
  auto append_unique = [&](const std::string &item) {
    if (std::find(result.begin(), result.end(), item) == result.end()) {
      result.push_back(item);
    }
  };
  if (!edits.authored) {
    for (const std::string &item : weaker) append_unique(item);
    return result;
  }
  if (edits.is_explicit) {
    for (const std::string &item : edits.explicit_items) append_unique(item);
    return result;
  }

  for (const std::string &item : edits.prepended) append_unique(item);
  for (const std::string &item : weaker) {
    if (std::find(edits.deleted.begin(), edits.deleted.end(), item) ==
        edits.deleted.end()) {
      append_unique(item);
    }
  }
  for (const std::string &item : edits.added) append_unique(item);
  for (const std::string &item : edits.appended) {
    result.erase(std::remove(result.begin(), result.end(), item), result.end());
    result.push_back(item);
  }
  ApplyStringListOrder(edits.ordered, &result);
  return result;
}

/// Cold PrimSpec metadata: fields that are empty on the vast majority of prims
/// (no docs / variants / relocates / apiSchemas / instancing / list-op edits).
/// Held behind a lazily-allocated unique_ptr on PrimSpecMeta so an ordinary
/// prim pays only 8 pointer bytes instead of ~190 bytes of empty std::string /
/// std::vector heads (Phase 8.1 footprint split).
struct PrimSpecMetaExt {
  std::string doc;
  std::string comment;
  bool doc_authored = false;
  bool comment_authored = false;
  std::string kind;         // model kind (component/group/assembly/...)
  bool kind_authored = false;
  std::string displayName;  // UI display name
  bool display_name_authored = false;
  std::string permission;   // "public"/"private" (pxr SdfPermission)
  std::vector<std::string> displayGroupOrder;
  bool displayGroupOrderAuthored = false;
  // When non-empty (set by composition), this prim is an instance whose
  // children come from the prototype prim at this path (no duplicated subtree).
  std::string instance_prototype;
  std::vector<std::string> apiSchemas;
  bool apiSchemasAuthored = false;
  StringListOpEdits apiSchemaEdits;
  // Authored list-op qualifier for apiSchemas: "" (bare/explicit),
  // "prepend", "append" or "delete" — pxr's own convention is prepend.
  std::string apiSchemasQualifier;
  // Variant set definitions.
  std::vector<VariantSetData> variantSets;
  StringListOpEdits variantSetNameEdits;
  // Multiple variant selections (set -> selection); composed in addition to the
  // legacy single `variantSelection` field on PrimSpecMeta.
  std::vector<std::pair<std::string, std::string>> variantSelections;
  bool variantSelectionsAuthored = false;
  // Relocates: namespace renames (absolute source path -> absolute target path).
  std::vector<std::pair<std::string, std::string>> relocates;
  bool relocatesAuthored = false;
  // Dictionary-valued metadata (each a Dictionary Value; empty when unauthored).
  Value customData;
  Value assetInfo;
  Value sdrMetadata;
  Value clips;
  StringListOpEdits clipSetEdits;
  // Composed-output only (never parsed or serialized): properties whose
  // strongest authored opinion came from a composition source WEAKER than the
  // source that introduced the clips metadata. Value-clip resolution takes
  // precedence over these (LVRPS: local > clips > references/payloads).
  std::vector<PropNameId> clipShadowedProps;
  bool customDataAuthored = false;
  bool assetInfoAuthored = false;
  bool sdrMetadataAuthored = false;
  bool clipsAuthored = false;
  // Authored namespace ordering fields (`reorder nameChildren/properties`).
  std::vector<std::string> primOrder;
  std::vector<std::string> propertyOrder;
  bool primOrderAuthored = false;
  bool propertyOrderAuthored = false;
  // Unknown (unmodeled) prim metadata, preserved as (key, raw source text of
  // the value) in authored order so the USDA writer can re-emit it verbatim
  // (the legacy parser preserves unknown prim metadata; dropping it loses
  // pipeline-specific opinions like `sceneName`).
  std::vector<std::pair<std::string, std::string>> unknownMeta;
  std::vector<TypedExtensionField> unknownFields;
  // Arc list-op qualifiers (Phase 7 S5); null unless authored.
  std::unique_ptr<ArcListOpEdits> arc_edits;

  PrimSpecMetaExt() = default;
  PrimSpecMetaExt(const PrimSpecMetaExt &o)
      : doc(o.doc),
        comment(o.comment),
        doc_authored(o.doc_authored),
        comment_authored(o.comment_authored),
        kind(o.kind),
        kind_authored(o.kind_authored),
        displayName(o.displayName),
        display_name_authored(o.display_name_authored),
        permission(o.permission),
        displayGroupOrder(o.displayGroupOrder),
        displayGroupOrderAuthored(o.displayGroupOrderAuthored),
        instance_prototype(o.instance_prototype),
        apiSchemas(o.apiSchemas),
        apiSchemasAuthored(o.apiSchemasAuthored),
        apiSchemaEdits(o.apiSchemaEdits),
        apiSchemasQualifier(o.apiSchemasQualifier),
        variantSets(o.variantSets),
        variantSetNameEdits(o.variantSetNameEdits),
        variantSelections(o.variantSelections),
        variantSelectionsAuthored(o.variantSelectionsAuthored),
        relocates(o.relocates),
        relocatesAuthored(o.relocatesAuthored),
        customData(o.customData),
        assetInfo(o.assetInfo),
        sdrMetadata(o.sdrMetadata),
        clips(o.clips),
        clipSetEdits(o.clipSetEdits),
        clipShadowedProps(o.clipShadowedProps),
        customDataAuthored(o.customDataAuthored),
        assetInfoAuthored(o.assetInfoAuthored),
        sdrMetadataAuthored(o.sdrMetadataAuthored),
        clipsAuthored(o.clipsAuthored),
        primOrder(o.primOrder),
        propertyOrder(o.propertyOrder),
        primOrderAuthored(o.primOrderAuthored),
        propertyOrderAuthored(o.propertyOrderAuthored),
        unknownMeta(o.unknownMeta),
        unknownFields(o.unknownFields),
        arc_edits(o.arc_edits ? new ArcListOpEdits(*o.arc_edits) : nullptr) {}
  PrimSpecMetaExt &operator=(const PrimSpecMetaExt &) = delete;
};

/// PrimSpec metadata. Hot fields (flags, the four composition-arc lists, the
/// legacy single variant selection, and the layer offset) are inline; cold
/// fields live in a lazily-allocated PrimSpecMetaExt. The cold-field accessors
/// keep the old names, so call sites change only by gaining `()`. Const reads
/// never allocate (return a shared empty); mutable access allocates the ext on
/// first touch. Copyable with a deep copy of the ext.
struct PrimSpecMeta {
  bool active = true;
  bool hidden = false;
  // Authored-ness for the two bools above: plain bools cannot distinguish
  // "authored true/false" from "default", and fill-absent composition needs
  // to know (a weaker spec's DEFAULT active=true must not clobber a stronger
  // authored active=false).
  bool active_authored = false;
  bool hidden_authored = false;
  bool loaded = true;  // runtime PCP state; not authored/serialized metadata
  bool instanceable_authored = false;  // instanceable=false is a real opinion
  bool instanceable = false;  // when true (with arcs), the prim is an instance

  // Composition arcs (stored as paths for lazy resolution).
  std::vector<std::string> references;
  std::vector<std::string> payloads;
  std::vector<std::string> inherits;
  std::vector<std::string> specializes;

  // Legacy single "variantSet=selection" (kept inline; the plural list of
  // selections lives in the ext).
  std::string variantSelection;

  // Layer offset (applied at evaluation time). First = offset, second = scale.
  std::pair<double, double> layer_offset = {0.0, 1.0};

  PrimSpecMeta() = default;
  PrimSpecMeta(PrimSpecMeta &&) = default;
  PrimSpecMeta &operator=(PrimSpecMeta &&) = default;
  PrimSpecMeta(const PrimSpecMeta &o) { *this = o; }
  PrimSpecMeta &operator=(const PrimSpecMeta &o) {
    active = o.active;
    hidden = o.hidden;
    active_authored = o.active_authored;
    hidden_authored = o.hidden_authored;
    loaded = o.loaded;
    instanceable_authored = o.instanceable_authored;
    instanceable = o.instanceable;
    references = o.references;
    payloads = o.payloads;
    inherits = o.inherits;
    specializes = o.specializes;
    variantSelection = o.variantSelection;
    layer_offset = o.layer_offset;
    ext_ = o.ext_ ? std::unique_ptr<PrimSpecMetaExt>(
                        new PrimSpecMetaExt(*o.ext_))
                  : nullptr;
    return *this;
  }

  bool has_ext() const { return ext_ != nullptr; }
  void ensure_ext() {
    if (!ext_) ext_.reset(new PrimSpecMetaExt());
  }
  const PrimSpecMetaExt *ext() const { return ext_.get(); }

  // Arc list-op edits (Phase 7 S5). Const returns null when none authored (the
  // inline arc vectors are then implicit explicit lists); mutable allocates.
  const ArcListOpEdits *arc_edits() const {
    return ext_ ? ext_->arc_edits.get() : nullptr;
  }
  /// Drop all arc list-op edits (used when composition consumes the arcs).
  void clear_arc_edits() {
    if (ext_) ext_->arc_edits.reset();
  }
  ArcListOpEdits &ensure_arc_edits() {
    ensure_ext();
    if (!ext_->arc_edits) ext_->arc_edits.reset(new ArcListOpEdits());
    return *ext_->arc_edits;
  }

  const std::string &doc() const {
    static const std::string kEmpty;
    return ext_ ? ext_->doc : kEmpty;
  }
  std::string &doc() {
    ensure_ext();
    return ext_->doc;
  }
  bool doc_authored() const { return ext_ && ext_->doc_authored; }
  void set_doc_authored(bool authored = true) {
    ensure_ext();
    ext_->doc_authored = authored;
  }
  const std::string &comment() const {
    static const std::string kEmpty;
    return ext_ ? ext_->comment : kEmpty;
  }
  std::string &comment() {
    ensure_ext();
    return ext_->comment;
  }
  bool comment_authored() const { return ext_ && ext_->comment_authored; }
  void set_comment_authored(bool authored = true) {
    ensure_ext();
    ext_->comment_authored = authored;
  }
  const std::string &kind() const {
    static const std::string kEmpty;
    return ext_ ? ext_->kind : kEmpty;
  }
  std::string &kind() {
    ensure_ext();
    return ext_->kind;
  }
  bool kindAuthored() const { return ext_ && ext_->kind_authored; }
  void setKindAuthored(bool authored = true) {
    ensure_ext();
    ext_->kind_authored = authored;
  }
  const std::string &permission() const {
    static const std::string kEmpty;
    return ext_ ? ext_->permission : kEmpty;
  }
  std::string &permission() {
    ensure_ext();
    return ext_->permission;
  }
  const std::string &displayName() const {
    static const std::string kEmpty;
    return ext_ ? ext_->displayName : kEmpty;
  }
  std::string &displayName() {
    ensure_ext();
    return ext_->displayName;
  }
  bool displayNameAuthored() const {
    return ext_ && ext_->display_name_authored;
  }
  void setDisplayNameAuthored(bool authored = true) {
    ensure_ext();
    ext_->display_name_authored = authored;
  }
  const std::string &instance_prototype() const {
    static const std::string kEmpty;
    return ext_ ? ext_->instance_prototype : kEmpty;
  }
  std::string &instance_prototype() {
    ensure_ext();
    return ext_->instance_prototype;
  }
  const std::vector<std::string> &apiSchemas() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->apiSchemas : kEmpty;
  }
  std::vector<std::string> &apiSchemas() {
    ensure_ext();
    return ext_->apiSchemas;
  }
  bool apiSchemasAuthored() const {
    return ext_ && ext_->apiSchemasAuthored;
  }
  void setApiSchemasAuthored(bool authored = true) {
    ensure_ext();
    ext_->apiSchemasAuthored = authored;
  }
  const StringListOpEdits &apiSchemaEdits() const {
    static const StringListOpEdits kEmpty;
    return ext_ ? ext_->apiSchemaEdits : kEmpty;
  }
  StringListOpEdits &apiSchemaEdits() {
    ensure_ext();
    return ext_->apiSchemaEdits;
  }
  const std::string &apiSchemasQualifier() const {
    static const std::string kEmpty;
    return ext_ ? ext_->apiSchemasQualifier : kEmpty;
  }
  std::string &apiSchemasQualifier() {
    ensure_ext();
    return ext_->apiSchemasQualifier;
  }
  const std::vector<VariantSetData> &variantSets() const {
    static const std::vector<VariantSetData> kEmpty;
    return ext_ ? ext_->variantSets : kEmpty;
  }
  std::vector<VariantSetData> &variantSets() {
    ensure_ext();
    return ext_->variantSets;
  }
  const StringListOpEdits &variantSetNameEdits() const {
    static const StringListOpEdits kEmpty;
    return ext_ ? ext_->variantSetNameEdits : kEmpty;
  }
  StringListOpEdits &variantSetNameEdits() {
    ensure_ext();
    return ext_->variantSetNameEdits;
  }
  const StringListOpEdits &clipSetEdits() const {
    static const StringListOpEdits kEmpty;
    return ext_ ? ext_->clipSetEdits : kEmpty;
  }
  StringListOpEdits &clipSetEdits() {
    ensure_ext();
    return ext_->clipSetEdits;
  }
  bool primOrderAuthored() const {
    return ext_ && ext_->primOrderAuthored;
  }
  void setPrimOrderAuthored(bool authored = true) {
    ensure_ext();
    ext_->primOrderAuthored = authored;
  }
  bool propertyOrderAuthored() const {
    return ext_ && ext_->propertyOrderAuthored;
  }
  void setPropertyOrderAuthored(bool authored = true) {
    ensure_ext();
    ext_->propertyOrderAuthored = authored;
  }
  const std::vector<std::pair<std::string, std::string>> &variantSelections()
      const {
    static const std::vector<std::pair<std::string, std::string>> kEmpty;
    return ext_ ? ext_->variantSelections : kEmpty;
  }
  std::vector<std::pair<std::string, std::string>> &variantSelections() {
    ensure_ext();
    return ext_->variantSelections;
  }
  bool variantSelectionsAuthored() const {
    return ext_ && ext_->variantSelectionsAuthored;
  }
  void setVariantSelectionsAuthored(bool authored = true) {
    ensure_ext();
    ext_->variantSelectionsAuthored = authored;
  }
  const std::vector<std::pair<std::string, std::string>> &relocates() const {
    static const std::vector<std::pair<std::string, std::string>> kEmpty;
    return ext_ ? ext_->relocates : kEmpty;
  }
  std::vector<std::pair<std::string, std::string>> &relocates() {
    ensure_ext();
    return ext_->relocates;
  }
  bool relocatesAuthored() const { return ext_ && ext_->relocatesAuthored; }
  void setRelocatesAuthored(bool authored = true) {
    ensure_ext();
    ext_->relocatesAuthored = authored;
  }
  const Value &customData() const {
    static const Value kEmpty;
    return ext_ ? ext_->customData : kEmpty;
  }
  Value &customData() {
    ensure_ext();
    return ext_->customData;
  }
  bool customDataAuthored() const {
    return ext_ && ext_->customDataAuthored;
  }
  void setCustomDataAuthored(bool authored = true) {
    ensure_ext();
    ext_->customDataAuthored = authored;
  }
  const Value &assetInfo() const {
    static const Value kEmpty;
    return ext_ ? ext_->assetInfo : kEmpty;
  }
  Value &assetInfo() {
    ensure_ext();
    return ext_->assetInfo;
  }
  bool assetInfoAuthored() const { return ext_ && ext_->assetInfoAuthored; }
  void setAssetInfoAuthored(bool authored = true) {
    ensure_ext();
    ext_->assetInfoAuthored = authored;
  }
  const Value &sdrMetadata() const {
    static const Value kEmpty;
    return ext_ ? ext_->sdrMetadata : kEmpty;
  }
  Value &sdrMetadata() {
    ensure_ext();
    return ext_->sdrMetadata;
  }
  bool sdrMetadataAuthored() const {
    return ext_ && ext_->sdrMetadataAuthored;
  }
  void setSdrMetadataAuthored(bool authored = true) {
    ensure_ext();
    ext_->sdrMetadataAuthored = authored;
  }
  const Value &clips() const {
    static const Value kEmpty;
    return ext_ ? ext_->clips : kEmpty;
  }
  Value &clips() {
    ensure_ext();
    return ext_->clips;
  }
  bool clipsAuthored() const { return ext_ && ext_->clipsAuthored; }
  void setClipsAuthored(bool authored = true) {
    ensure_ext();
    ext_->clipsAuthored = authored;
  }
  const std::vector<PropNameId> &clipShadowedProps() const {
    static const std::vector<PropNameId> kEmpty;
    return ext_ ? ext_->clipShadowedProps : kEmpty;
  }
  std::vector<PropNameId> &clipShadowedProps() {
    ensure_ext();
    return ext_->clipShadowedProps;
  }
  const std::vector<std::string> &primOrder() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->primOrder : kEmpty;
  }
  const std::vector<std::string> &displayGroupOrder() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->displayGroupOrder : kEmpty;
  }
  std::vector<std::string> &editDisplayGroupOrder() {
    ensure_ext();
    return ext_->displayGroupOrder;
  }
  bool displayGroupOrderAuthored() const {
    return ext_ && ext_->displayGroupOrderAuthored;
  }
  void setDisplayGroupOrderAuthored(bool authored = true) {
    ensure_ext();
    ext_->displayGroupOrderAuthored = authored;
  }
  std::vector<std::string> &editPrimOrder() {
    ensure_ext();
    return ext_->primOrder;
  }
  const std::vector<std::string> &propertyOrder() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->propertyOrder : kEmpty;
  }
  std::vector<std::string> &editPropertyOrder() {
    ensure_ext();
    return ext_->propertyOrder;
  }
  const std::vector<std::pair<std::string, std::string>> &unknownMeta() const {
    static const std::vector<std::pair<std::string, std::string>> kEmpty;
    return ext_ ? ext_->unknownMeta : kEmpty;
  }
  const std::vector<TypedExtensionField> &unknownFields() const {
    static const std::vector<TypedExtensionField> kEmpty;
    return ext_ ? ext_->unknownFields : kEmpty;
  }
  std::vector<TypedExtensionField> &unknownFields() {
    ensure_ext();
    return ext_->unknownFields;
  }
  std::vector<std::pair<std::string, std::string>> &unknownMeta() {
    ensure_ext();
    return ext_->unknownMeta;
  }

 private:
  std::unique_ptr<PrimSpecMetaExt> ext_;
};

/// Per-property metadata (the `( ... )` block after an attribute/relationship).
/// Lazily allocated per property in PrimSpec::prop_metas_, so an ordinary
/// property pays nothing. `authored` records which fields were explicitly set so
/// the writer re-emits exactly the authored opinions (e.g. elementSize=1).
struct PropMeta {
  uint32_t authored = 0;
  enum : uint32_t {
    kInterpolation  = 1u << 0,  kElementSize   = 1u << 1,
    kColorSpace     = 1u << 2,  kDisplayName   = 1u << 3,
    kDisplayGroup   = 1u << 4,  kDoc           = 1u << 5,
    kHidden         = 1u << 6,  kRenderType    = 1u << 7,
    kConnectability = 1u << 8,  kOutputName    = 1u << 9,
    kBindMaterialAs = 1u << 10, kKind          = 1u << 11,
    kWeight         = 1u << 12, kUnauthoredIdx = 1u << 13,
    kAllowedTokens  = 1u << 14, kCustomData    = 1u << 15,
    kAssetInfo      = 1u << 16, kSdrMetadata   = 1u << 17,
    kUnknownMeta    = 1u << 18, kComment = 1u << 19,
    kPermission     = 1u << 20,
  };
  // token / string fields
  std::string interpolation;   // constant/uniform/varying/vertex/faceVarying
  std::string colorSpace;
  std::string renderType;
  std::string connectability;
  std::string outputName;
  std::string bindMaterialAs;
  std::string kind;
  std::string displayName;
  std::string displayGroup;
  std::string doc;
  std::string comment;
  std::string permission;  // "public"/"private" (pxr SdfPermission)
  // scalar fields
  int32_t elementSize = 1;
  int32_t unauthoredValuesIndex = -1;
  double  weight = 0.0;
  bool    hidden = false;
  // array / dict fields
  std::vector<std::string> allowedTokens;
  Value customData;
  Value assetInfo;
  Value sdrMetadata;
  // Unknown (unmodeled) property metadata preserved as raw source text,
  // authored order (key -> raw value text); the USDA writer re-emits it
  // verbatim so pipeline-specific opinions round-trip.
  std::vector<std::pair<std::string, std::string>> unknownMeta;
  std::vector<TypedExtensionField> unknownFields;

  bool empty() const { return authored == 0; }
};

/// Value storage block
/// Stores property values contiguously for cache efficiency
class ValueStorage {
public:
  ValueStorage();
  ~ValueStorage();

  /// Store a value and return its offset
  uint32_t store(const Value& value);
  uint32_t store(Value&& value);

  /// Get value at offset
  const Value* get(uint32_t offset) const;
  Value* get(uint32_t offset);

  /// Number of stored values
  size_t count() const { return values_.size(); }

  /// Approximate memory usage in bytes
  size_t memory_usage() const;

  /// Clear all storage
  void clear();

private:
  std::vector<Value> values_;
};

/// TimeSampleStorage - stores time samples with value deduplication
/// Design goals:
/// - Deduplicate identical array values across time samples
/// - Use hash + equality check for efficient lookup
/// - Store (time, value_offset) pairs per property
class TimeSampleStorage {
public:
  TimeSampleStorage();
  ~TimeSampleStorage();

  /// Add a time sample for a property
  /// Returns the value offset (may be shared if duplicate)
  uint32_t add(PropNameId name_id, double time, Value value);

  /// Add a time sample with explicit deduplication check
  /// If an identical value exists, reuses its offset
  uint32_t add_dedup(PropNameId name_id, double time, Value value);

private:
  /// Keep each property's (time, offset) vector sorted by time (consumers —
  /// interpolation's FindBracket, the crate writer's times block — assume it),
  /// with last-wins upsert for a re-authored time.
  void insert_sample(PropNameId name_id, double time, uint32_t offset);

public:

  /// Get time samples for a property
  /// Returns vector of (time, value_offset) pairs, sorted by time
  const std::vector<std::pair<double, uint32_t>>* get(PropNameId name_id) const;

  /// Get value at offset
  const Value* value(uint32_t offset) const;

  /// Rewrite scalar asset path sample values.
  size_t remap_asset_paths(const std::map<std::string, std::string>& remap);

  /// Check if property has time samples
  bool has(PropNameId name_id) const;

  /// Remove all time samples for a property. Returns true if any existed.
  /// (Stored values remain in the value pool; they may be shared.)
  bool remove(PropNameId name_id);

  /// Remap every sample time t -> offset + scale * t (layer-offset baking).
  void remap_times(double offset, double scale);

  /// Get interpolated value at a given time
  /// @param name_id Property name ID
  /// @param time Time to sample at
  /// @param mode Interpolation mode (default: Linear)
  /// @return SampleResult with interpolated value
  SampleResult interpolate(PropNameId name_id, double time,
                           TimeInterpolation mode = TimeInterpolation::Linear) const;

  /// Get all property IDs with time samples
  std::vector<PropNameId> properties() const;

  /// Check if empty
  bool empty() const { return samples_.empty(); }

  /// Memory usage in bytes
  size_t memory_usage() const;

  /// Clear all storage
  void clear();

  /// Statistics
  struct Stats {
    size_t property_count;     // Properties with time samples
    size_t total_samples;      // Total (time, offset) pairs
    size_t unique_values;      // Unique values stored
    size_t dedup_count;        // Values deduplicated (savings)
    size_t memory_bytes;
  };
  Stats stats() const;

private:
  // Property -> vector of (time, value_offset)
  std::unordered_map<uint32_t, std::vector<std::pair<double, uint32_t>>> samples_;

  // Value storage
  std::vector<Value> values_;

  // Deduplication: hash -> list of (offset, hash) for collision handling
  std::unordered_map<uint64_t, std::vector<uint32_t>> hash_to_offsets_;

  // Stats
  size_t dedup_count_ = 0;

  // Find existing value or store new one
  uint32_t find_or_store(Value value);
};

/// PrimSpec - unified spec/prim representation
/// Design goals:
/// - No separate Prim type needed (PrimSpec IS the prim)
/// - Properties accessed via O(1) indexed lookup
/// - Values stored in contiguous memory
/// - Children stored as indices, not pointers
class PrimSpec {
public:
  struct RelationshipOpinion {
    std::vector<Path> items;
    ArcEdit edit;
    bool qualified = false;
  };
  PrimSpec();
  explicit PrimSpec(const std::string& name);
  PrimSpec(const std::string& name, const std::string& type_name);
  ~PrimSpec();

  // Move only (for efficiency)
  PrimSpec(PrimSpec&& other) noexcept;
  PrimSpec& operator=(PrimSpec&& other) noexcept;
  PrimSpec(const PrimSpec&) = delete;
  PrimSpec& operator=(const PrimSpec&) = delete;

  /// Clone this PrimSpec (deep copy)
  PrimSpec Clone() const;

  // ============================================================
  // Identity
  // ============================================================

  /// Get prim name
  const std::string& name() const { return name_; }
  void set_name(const std::string& name) { name_ = name; }

  /// Get type name
  const std::string& type_name() const;
  TypeNameId type_id() const { return type_id_; }
  void set_type_name(const std::string& name);

  /// Get specifier
  PrimSpecifier specifier() const { return specifier_; }
  void set_specifier(PrimSpecifier spec) { specifier_ = spec; }

  /// Get path (set during layer building)
  const Path& path() const { return path_; }
  void set_path(const Path& path) { path_ = path; }

  /// Directory this prim's RELATIVE asset paths anchor to, as an id interned in
  /// asset-anchor.hh (0 = none -> the consumer falls back to the stage's base
  /// dir). Stamped from the resolved layer path when the layer is loaded, and
  /// carried through composition so a flattened prim still knows which layer
  /// authored it. See asset-anchor.hh.
  uint32_t asset_anchor_id() const { return asset_anchor_id_; }
  void set_asset_anchor_id(uint32_t id) { asset_anchor_id_ = id; }

  // ============================================================
  // Properties (O(1) lookup for common names)
  // ============================================================

  /// Get property by name ID (fastest)
  const PropSlot* property(PropNameId name_id) const;

  /// Mutable slot lookup (flag merges during composition).
  PropSlot* property_mutable(PropNameId name_id) {
    return props_.find_mutable(name_id);
  }

  /// Get property by name string
  const PropSlot* property(const std::string& name) const;

  /// Get property value
  const Value* property_value(PropNameId name_id) const;
  const Value* property_value(const std::string& name) const;

  /// Add a property
  void add_property(const std::string& name, Value value, uint16_t flags = 0);
  void add_property(PropNameId name_id, Value value, uint16_t flags = 0);

  /// Add a property slot without value (for time-sampled properties)
  void add_property_slot(PropNameId name_id, TypeId type_id, uint16_t flags);

  /// Mark an existing property slot as time-sampled. Needed when an attribute is
  /// declared first (e.g. `<type> <name> ( meta )`) and its `.timeSamples` arrive
  /// on a later statement: the slot already exists, so the time-sampled flag must
  /// be OR'd onto it (else the writer's is_time_sampled() check drops the
  /// samples). No-op if the slot is absent.
  void mark_property_time_sampled(PropNameId name_id);

  /// Field-level fill-absent: if a slot for `name_id` exists but carries no
  /// authored default value (a connection-only / declared-only attribute), set
  /// its default to `value`. No-op if the slot is absent or already has a value.
  /// pxr composes an attribute's default value and its connections as
  /// independent fields, so a weaker source's default fills a stronger
  /// connection-only opinion. Returns true if a value was filled.
  bool fill_property_value_if_absent(PropNameId name_id, Value value);

  /// Replace an existing property's authored default value. Returns false when
  /// the slot is absent or has no authored default value.
  bool set_property_value(PropNameId name_id, Value value);

  /// Author a property value regardless of prior state: creates the slot if
  /// absent, fills a value-less (declared-only) slot, or replaces the existing
  /// default IN PLACE (no ValueStorage growth on re-authoring). `flags` are
  /// OR'd onto an existing slot (kFlagArray is recomputed from the value).
  void upsert_property(PropNameId name_id, Value value, uint16_t flags = 0);
  void upsert_property(const std::string& name, Value value, uint16_t flags = 0);

  /// Remove a property entirely: slot, connections, declared type name,
  /// per-property metadata, and time samples. Returns true if anything was
  /// removed. (The stored default Value stays in ValueStorage as an
  /// unreferenced entry; storage is append-only.)
  bool remove_property(PropNameId name_id);
  bool remove_property(const std::string& name);

  /// Release a static array default while retaining its property slot, declared
  /// type and metadata. Time-sampled properties are never changed. This is a
  /// deliberately lossy residency operation for reconstructable composed
  /// stages; a subsequent composition rebuild restores the authored value.
  /// Returns the approximate payload bytes released (zero when ineligible).
  size_t release_static_array_value(PropNameId name_id,
                                    size_t min_array_elements,
                                    size_t* released_elements = nullptr);

  /// Get property index (for iteration)
  const PropIndex& properties() const { return props_; }

  /// Reserve property storage
  void reserve_properties(size_t count);

  /// Finalize properties (sort for binary search)
  void finalize_properties();

  // ============================================================
  // TimeSamples (stored separately for efficiency)
  // ============================================================

  /// Add a time sample for a property
  void add_time_sample(PropNameId name_id, double time, Value value);

  /// Get time samples for a property (returns vector of (time, value_offset))
  const std::vector<std::pair<double, uint32_t>>* time_samples(PropNameId name_id) const;

  /// Get value at a specific time sample offset
  const Value* time_sample_value(uint32_t offset) const;

  /// Rewrite scalar asset paths stored in defaults and time samples.
  size_t remap_asset_paths(const std::map<std::string, std::string>& remap);

  /// Remap every time sample's time by a layer offset (t -> offset+scale*t).
  void remap_time_sample_times(double offset, double scale);

  /// Get all property names that have time samples
  std::vector<PropNameId> time_sampled_properties() const;

  /// Check if property has time samples
  bool has_time_samples(PropNameId name_id) const;

  /// Check if any property has time samples
  bool has_any_time_samples() const;

  /// Get time sample statistics (for debugging/profiling)
  TimeSampleStorage::Stats time_sample_stats() const;

  /// Get interpolated value at a given time
  /// @param name_id Property name ID
  /// @param time Time to sample at
  /// @param mode Interpolation mode (default: Linear)
  /// @return SampleResult with interpolated value
  SampleResult interpolate_time_sample(PropNameId name_id, double time,
                                       TimeInterpolation mode = TimeInterpolation::Linear) const;

  /// Get interpolated value at a given time (by property name)
  SampleResult interpolate_time_sample(const std::string& name, double time,
                                       TimeInterpolation mode = TimeInterpolation::Linear) const;

  // ============================================================
  // Spline authored field preservation
  // ============================================================

  /// Preserve the normative `.spline` field text until the typed TsSpline
  /// evaluator/Crate codec is available. This prevents successful rewrites
  /// from silently deleting spline data. Strict AOUSD parsing rejects the
  /// unsupported field instead of returning a partially interpreted stage.
  void set_spline_source(const std::string& prop_name, std::string source);
  const std::string* spline_source(PropNameId name_id) const;
  const std::string* spline_source(const std::string& prop_name) const;

  /// Lossless compatibility storage for a default whose declared extension or
  /// not-yet-supported foundational type has no Value decoder.
  void set_raw_default_source(const std::string& prop_name,
                              std::string source);
  const std::string* raw_default_source(PropNameId name_id) const;
  void clear_raw_default_source(PropNameId name_id);

  /// Sparse array edit authored as this property's default (`= edit [...]`).
  /// The canonical `edit [...]` text lives in raw_default_source (the usda
  /// writer re-emits it); this is the structured op list composition uses to
  /// stack and resolve edits (see layer/array-edit.hh).
  void set_array_edit(const std::string& prop_name, ArrayEditData edit);
  const ArrayEditData* array_edit(PropNameId name_id) const;
  void clear_array_edit(PropNameId name_id);
  const std::unordered_map<uint32_t, ArrayEditData>& array_edits() const {
    static const std::unordered_map<uint32_t, ArrayEditData> kEmpty;
    return cold_data_ ? cold_data_->array_edits : kEmpty;
  }

  // ============================================================
  // Relationships
  // ============================================================

  enum class RelationshipListOp {
    Append,
    Prepend,
    Add,
    Delete,
    Reorder,
  };

  /// Add a relationship target
  void add_relationship(const std::string& name, const Path& target);

  /// Replace a relationship's targets wholesale (creates it if absent).
  /// An empty target list authors an explicit empty relationship.
  void set_relationship_targets(const std::string& name,
                                std::vector<Path> targets);

  /// Remove a relationship (both the target list and its property slot, if a
  /// kFlagRelationship slot was declared). Returns true if removed.
  bool remove_relationship(const std::string& name);

  /// Authored list-op edits for a relationship (prepend/append/delete
  /// sublists, like ArcEdit for composition arcs). Keyed by relationship
  /// name. Empty map = no relationship carries qualifiers.
  const std::unordered_map<std::string, ArcEdit>& relationship_edits() const {
    static const std::unordered_map<std::string, ArcEdit> kEmpty;
    return cold_data_ ? cold_data_->rel_edits : kEmpty;
  }
  ArcEdit& ensure_relationship_edit(const std::string& name);

  const std::vector<RelationshipOpinion>* relationship_opinion_stack(
      const std::string& name) const;
  void set_relationship_opinion_stack(
      const std::string& name, std::vector<RelationshipOpinion> opinions);

  /// Per-relationship qualifier flags (PropSlot::kFlagCustom /
  /// kFlagUniform). 0 when unauthored.
  uint16_t relationship_flags(const std::string& name) const;
  void set_relationship_flags(const std::string& name, uint16_t flags);

  /// Apply same-spec relationship list-op targets. Used by USDA body syntax
  /// such as `prepend rel prototypes = [...]`.
  void apply_relationship_list_op(const std::string& name,
                                  const std::vector<Path>& targets,
                                  RelationshipListOp op);

  /// Get relationship targets
  const std::vector<Path>* relationship(const std::string& name) const;

  /// Get all relationship names
  std::vector<std::string> relationship_names() const;

  // ============================================================
  // Attribute connections / declared type names (USDC fidelity)
  // ============================================================

  /// Add an attribute connection target (e.g. inputs:x.connect = </path>).
  /// The property itself should also exist as a slot (kFlagConnection).
  void add_connection(const std::string& prop_name, const Path& target);

  /// Author a connection BLOCK (`attr.connect = None`): an empty entry in the
  /// connection map (distinct from "no connection authored").
  void set_connection_block(const std::string& prop_name);

  /// Exact authored connectionPaths list-op for this attribute.
  const ArcEdit* connection_edit(const std::string& prop_name) const;
  ArcEdit& ensure_connection_edit(const std::string& prop_name);

  const std::vector<RelationshipOpinion>* connection_opinion_stack(
      const std::string& prop_name) const;
  void set_connection_opinion_stack(
      const std::string& prop_name, std::vector<RelationshipOpinion> opinions);

  void set_connection_targets(const std::string& prop_name,
                              std::vector<Path> targets);
  void apply_connection_list_op(const std::string& prop_name,
                                const std::vector<Path>& targets,
                                RelationshipListOp op);

  /// Get connection targets for an attribute (nullptr if none)
  const std::vector<Path>* connection(const std::string& prop_name) const;

  /// Rewrite relationship + connection target paths that lie WITHIN `old_prefix`
  /// (== it, or a descendant `old_prefix/...`) to the corresponding path under
  /// `new_prefix`. Used by the extracted-prototype flatten to retarget internal
  /// material:binding / connections when a prototype subtree moves to a
  /// `/Flattened_Prototype_N` root. Targets outside `old_prefix` are untouched.
  void remap_target_prefix(const std::string& old_prefix,
                           const std::string& new_prefix);

  /// Record the declared USD type name of a property (e.g. "color3f",
  /// "token", "float[]"). Needed to faithfully re-emit attributes that have
  /// no authored default value (connection-only / declared-only) and to
  /// round-trip the exact role type for valued attributes.
  void set_property_type_name(const std::string& prop_name,
                              const std::string& type_name);

  /// Get the declared type name of a property (nullptr if not recorded)
  const std::string* property_type_name(const std::string& prop_name) const;
  const std::string* property_type_name(PropNameId name_id) const;

  // ============================================================
  // Per-property metadata (interpolation / customData / ...)
  // ============================================================

  /// Get a property's metadata, or nullptr if none authored (never allocates).
  const PropMeta* property_meta(PropNameId name_id) const;
  const PropMeta* property_meta(const std::string& prop_name) const;

  /// Get (lazily allocating) a property's mutable metadata for population.
  PropMeta& ensure_property_meta(const std::string& prop_name);
  PropMeta& ensure_property_meta(PropNameId name_id);

  // ============================================================
  // Children (stored as indices into Layer's prim array)
  // ============================================================

  /// Get child indices
  const std::vector<uint32_t>& child_indices() const { return child_indices_; }

  /// Add child index
  void add_child_index(uint32_t index);

  /// Remove a single child index link (used by Layer::remove_prim_at_path).
  /// Returns true if the index was found and removed.
  bool remove_child_index(uint32_t index);

  /// Drop all child links (orphan the subtree). The child prims remain in the
  /// Layer but become unreachable from this prim — used by the extracted-
  /// prototype flatten to move an instance's inline subtree onto a
  /// `/Flattened_Prototype_N` root.
  void clear_child_indices() { child_indices_.clear(); }

  /// Replace the child index list wholesale (namespace reordering; e.g. the
  /// crate reader restoring authored order from primChildren).
  void set_child_indices(std::vector<uint32_t>&& idx) {
    child_indices_ = std::move(idx);
  }

  /// Get child count
  size_t child_count() const { return child_indices_.size(); }

  // ============================================================
  // Metadata
  // ============================================================

  /// Get metadata
  const PrimSpecMeta& meta() const { return meta_; }
  PrimSpecMeta& meta() { return meta_; }

  // ============================================================
  // Memory
  // ============================================================

  /// Get approximate memory usage
  size_t memory_usage() const;

private:
  std::string name_;
  TypeNameId type_id_;
  PrimSpecifier specifier_ = PrimSpecifier::Def;
  // Interned dir this prim's relative asset paths anchor to (0 = none).
  // Sits in the padding after `specifier_`, so it costs nothing per prim.
  uint32_t asset_anchor_id_ = 0;
  Path path_;

  PropIndex props_;
  std::unique_ptr<ValueStorage> values_;

  // TimeSamples storage with deduplication
  std::unique_ptr<TimeSampleStorage> time_samples_;

  // Relationships: name -> targets
  std::unordered_map<std::string, std::vector<Path>> relationships_;

  // Attribute connections: interned property-name id -> connection targets.
  // (Keyed by PropNameId.id rather than a string to avoid a key string per
  // connected property on shader-heavy scenes.)
  std::unordered_map<uint32_t, std::vector<Path>> connections_;

  // Rare authored fields share one lazy block. Keeping these eight hash-map
  // objects inline cost hundreds of bytes on every prim even though ordinary
  // geometry/xform prims use none of them.
  struct ColdData {
    std::unordered_map<std::string, ArcEdit> rel_edits;
    std::unordered_map<std::string, std::vector<RelationshipOpinion>>
        rel_opinion_stacks;
    std::unordered_map<std::string, uint16_t> rel_flags;
    std::unordered_map<uint32_t, ArcEdit> connection_edits;
    std::unordered_map<uint32_t, std::vector<RelationshipOpinion>>
        connection_opinion_stacks;
    // Raw USDA `.spline` values and unsupported defaults are keyed by property
    // id. Sparse array edits keep a structured twin of the raw default text.
    std::unordered_map<uint32_t, std::string> spline_sources;
    std::unordered_map<uint32_t, std::string> raw_default_sources;
    std::unordered_map<uint32_t, ArrayEditData> array_edits;
  };
  ColdData& ensure_cold_data();
  std::unique_ptr<ColdData> cold_data_;

  // Declared USD type names: interned property-name id -> interned typeName id
  // (both interned in the global PropNameTable). Lets the writer re-emit the
  // exact `typeName` (incl. role types and value-less / connection-only attrs)
  // while storing only two uint32s per property instead of two strings — the
  // "string pooling" of the original low-memory plan (typeNames are highly
  // repeated, so interning collapses ~150k strings to a few dozen).
  // Declared type spellings are cold metadata. Keep their ordinary strings
  // here instead of pinning them in the hot global property-name pool.
  std::unordered_map<uint32_t, std::string> prop_type_names_;

  // Per-property metadata, keyed by interned PropNameId.id (lazily allocated, so
  // ordinary properties cost nothing). Same side-table pattern as
  // prop_type_names_ / connections_ — sort-stable across finalize_properties().
  std::unordered_map<uint32_t, std::unique_ptr<PropMeta>> prop_metas_;

  // Children stored as indices into parent Layer's prim array
  std::vector<uint32_t> child_indices_;

  PrimSpecMeta meta_;
};

}  // namespace next
}  // namespace lightusd
