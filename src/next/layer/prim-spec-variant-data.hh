// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../types/value.hh"
#include "../prim/path.hh"

#include <memory>
#include <unordered_map>
#include <vector>

// Forward declarations
struct PrimVariantData;
struct PrimVariantSetData;
class Layer;  // for PrimVariantData::content (variant subtree)
struct Int64ListOpStack;

/// Generic decodable extension field retained from USDC. Unregistered fields
/// use Crate's recursive UnregisteredValue wrapper and must be re-encoded with
/// that wrapper rather than silently changing their schema type.
struct DecodedExtensionField {
  std::string name;
  Value value;
  bool unregistered = false;
  std::string unregistered_source;
  // Optional structured metadata; ordinary fields do not allocate a list-op.
  std::shared_ptr<const Int64ListOpStack> int64_list_op = nullptr;
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
struct PrimVariantData {
  PrimVariantData();
  PrimVariantData(const PrimVariantData&);
  PrimVariantData(PrimVariantData&&) noexcept;
  PrimVariantData& operator=(const PrimVariantData&);
  PrimVariantData& operator=(PrimVariantData&&) noexcept;
  ~PrimVariantData();

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
  // canonical "@asset@</prim>" / "</prim>" encoding as PrimMetadata's arc
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
  std::vector<PrimVariantSetData> variantSets;
  // Unknown (unmodeled) variant-option metadata: raw authored source text
  // (USDA, re-emitted verbatim) and decoded typed extension fields (USDC via
  // the materialized holder prim). Variant specs previously had NO generic
  // field storage, silently dropping pipeline-specific opinions.
  std::vector<std::pair<std::string, std::string>> unknownMeta;
  std::vector<DecodedExtensionField> unknownFields;
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
struct PrimVariantSetData {
  std::string name;
  std::string selected;  // selected variant name for this set ("" = none)
  std::vector<PrimVariantData> variants;
};

/// One composition-arc field's list-op edits, exactly as authored (Phase 7 S5,
/// AOUSD §10.3.2). `is_explicit` marks a bare `references = [...]` (or an
/// explicit op), in which case the effective items are the inline arc vector on
/// PrimMetadata; otherwise the effective list is composed from the prepend /
/// append / delete / order lists. Kept so composition can merge a site's specs
/// across the layer stack and the writer can re-emit the original qualifier.
struct ArcListOp {
  bool authored = false;     // true when this arc field authored list-op edits.
  bool is_explicit = true;  // bare list (the common case)
  bool is_value_block = false;  // relationship `= None`, distinct from `= []`
  std::vector<std::string> added;
  std::vector<std::string> prepended;
  std::vector<std::string> appended;
  std::vector<std::string> deleted;
  std::vector<std::string> ordered;

  bool has_qualifiers() const;
  bool has_authored_opinion() const;
};

/// Per-arc-type list-op edits for one PrimSpec (lazily allocated; only prims
/// that author a non-bare arc qualifier pay for it).
struct ArcListOps {
  ArcListOp references;
  ArcListOp payloads;
  ArcListOp inherits;
  ArcListOp specializes;
};

/// Exact authored SdfStringListOp sublists. `variantSetNames` needs this in
/// addition to the effective PrimVariantSetData vector: delete/order operations
/// may be inert locally yet remain authored data, and explicit-empty must not
/// collapse to an unauthored field.
struct StringListOps {
  bool authored = false;
  bool is_explicit = false;
  std::vector<std::string> explicit_items;
  std::vector<std::string> added;
  std::vector<std::string> prepended;
  std::vector<std::string> appended;
  std::vector<std::string> deleted;
  std::vector<std::string> ordered;

  bool has_nonexplicit_items() const;
};


