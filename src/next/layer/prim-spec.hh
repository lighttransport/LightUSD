// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PrimSpec
// Unified prim specification that can serve as both spec and runtime prim
// Avoids the PrimSpec→Prim tree duplication of the old design

#pragma once

#include "property-index.hh"
#include "intern-table.hh"
#include "../types/value.hh"
#include "../types/interpolation.hh"
#include "../prim/path.hh"
#include <algorithm>
#include <climits>
#include <cstdint>
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
  bool is_valid() const;
};

/// Type name interning table
/// Thread-safe under LIGHTUSD_ENABLE_THREAD (authoring interns new type names
/// at runtime, so concurrent readers need protection like PropNameTable).
class TypeNameTable : public detail::StringInternTable<TypeNameId, UINT16_MAX> {
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
};

TypeNameTable& GetTypeNameTable();

#include "prim-spec-variant-data.hh"

#include "prim-spec-metadata.hh"

#include "prim-spec-storage.hh"

class PrimSpec {
public:
  struct RelationshipOpinion {
    std::vector<Path> items;
    ArcListOp edit;
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
  const std::string& name() const;
  void set_name(const std::string& name);

  /// Get type name
  const std::string& type_name() const;
  TypeNameId type_id() const;
  void set_type_name(const std::string& name);

  /// Get specifier
  PrimSpecifier specifier() const;
  void set_specifier(PrimSpecifier spec);

  /// Get path (set during layer building)
  const Path& path() const;
  void set_path(const Path& path);

  /// Directory this prim's RELATIVE asset paths anchor to, as an id interned in
  /// asset-anchor.hh (0 = none -> the consumer falls back to the stage's base
  /// dir). Stamped from the resolved layer path when the layer is loaded, and
  /// carried through composition so a flattened prim still knows which layer
  /// authored it. See asset-anchor.hh.
  uint32_t asset_anchor_id() const;
  void set_asset_anchor_id(uint32_t id);

  // ============================================================
  // Properties (O(1) lookup for common names)
  // ============================================================

  /// Get property by name ID (fastest)
  const PropSlot* property(PropNameId name_id) const;

  /// Mutable slot lookup (flag merges during composition).
  PropSlot* property_mutable(PropNameId name_id);

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
  /// default IN PLACE (no ValueBlockStorage growth on re-authoring). `flags` are
  /// OR'd onto an existing slot (kFlagArray is recomputed from the value).
  void upsert_property(PropNameId name_id, Value value, uint16_t flags = 0);
  void upsert_property(const std::string& name, Value value, uint16_t flags = 0);

  /// Remove a property entirely: slot, connections, declared type name,
  /// per-property metadata, and time samples. Returns true if anything was
  /// removed. (The stored default Value stays in ValueBlockStorage as an
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
  const PropIndex& properties() const;

  /// Reserve property storage
  void reserve_properties(size_t count);

  /// Finalize properties (sort for binary search)
  void finalize_properties();

  /// Free the heap payloads of every property default value and time sample
  /// (AssetPath defaults are kept -- see ValueBlockStorage::release_payloads). Slot
  /// tables, names, types, meta, relationships and connections stay valid, so
  /// the PrimSpec remains structurally usable but value-stripped. Used by the
  /// crate writer's consume_values mode to cut peak RSS on write-and-discard
  /// flows.
  void release_value_payloads();

  // ============================================================
  // TimeSamples (stored separately for efficiency)
  // ============================================================

  /// Add a time sample for a property. `dedup=false` skips content-hash
  /// deduplication — required for deferred-fill values whose payload arrives
  /// later (batched USDA array parse): hashing them would read the payload
  /// while a parser worker is still writing it.
  void add_time_sample(PropNameId name_id, double time, Value value,
                       bool dedup = true);

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
  TimeSampleBlock::Stats time_sample_stats() const;

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
  void set_spline_source(PropNameId name_id, std::string source);
  const std::string* spline_source(PropNameId name_id) const;
  const std::string* spline_source(const std::string& prop_name) const;

  /// Lossless compatibility storage for a default whose declared extension or
  /// not-yet-supported foundational type has no Value decoder.
  void set_raw_default_source(const std::string& prop_name,
                              std::string source);
  void set_raw_default_source(PropNameId name_id, std::string source);
  const std::string* raw_default_source(PropNameId name_id) const;
  void clear_raw_default_source(PropNameId name_id);

  /// Sparse array edit authored as this property's default (`= edit [...]`).
  /// The canonical `edit [...]` text lives in raw_default_source (the usda
  /// writer re-emits it); this is the structured op list composition uses to
  /// stack and resolve edits (see layer/array-edit.hh).
  void set_array_edit(const std::string& prop_name, ArrayEditData edit);
  void set_array_edit(PropNameId name_id, ArrayEditData edit);
  const ArrayEditData* array_edit(PropNameId name_id) const;
  void clear_array_edit(PropNameId name_id);
  const std::unordered_map<uint32_t, ArrayEditData>& array_edits() const;

  /// Sparse arrays or qualified relationship/connection targets still need
  /// composition even when this prim has no composition arcs.
  bool needs_property_composition() const;

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
  /// An empty list registers the relationship; author an ArcListOp as well to
  /// distinguish `= []` or `= None` from a bare declaration.
  void set_relationship_targets(const std::string& name,
                                std::vector<Path> targets);

  /// Remove a relationship (both the target list and its property slot, if a
  /// kFlagRelationship slot was declared). Returns true if removed.
  bool remove_relationship(const std::string& name);

  /// Authored list-op edits for a relationship (prepend/append/delete
  /// sublists, like ArcListOp for composition arcs). Keyed by relationship
  /// name. Empty map = no relationship carries qualifiers.
  const std::unordered_map<std::string, ArcListOp>& relationship_edits() const;
  ArcListOp& ensure_relationship_edit(const std::string& name);

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
  /// False when this prim records no relationship targets, edits or opinion
  /// stacks at all (then relationship_names() is empty and every
  /// relationship(...) lookup misses) -- lets the compose merge skip the
  /// string-keyed relationship checks for the ordinary attribute-only prim.
  bool has_relationship_data() const;

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
  const ArcListOp* connection_edit(const std::string& prop_name) const;
  const ArcListOp* connection_edit(PropNameId name_id) const;
  ArcListOp& ensure_connection_edit(const std::string& prop_name);
  ArcListOp& ensure_connection_edit(PropNameId name_id);

  const std::vector<RelationshipOpinion>* connection_opinion_stack(
      const std::string& prop_name) const;
  const std::vector<RelationshipOpinion>* connection_opinion_stack(
      PropNameId name_id) const;
  void set_connection_opinion_stack(
      const std::string& prop_name, std::vector<RelationshipOpinion> opinions);
  void set_connection_opinion_stack(PropNameId name_id,
                                    std::vector<RelationshipOpinion> opinions);

  void set_connection_targets(const std::string& prop_name,
                              std::vector<Path> targets);
  void set_connection_targets(PropNameId name_id, std::vector<Path> targets);
  void apply_connection_list_op(const std::string& prop_name,
                                const std::vector<Path>& targets,
                                RelationshipListOp op);

  /// Get connection targets for an attribute (nullptr if none)
  const std::vector<Path>* connection(const std::string& prop_name) const;
  const std::vector<Path>* connection(PropNameId name_id) const;

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
  /// Same, by interned ids (no string hashing; the hot compose/reader path).
  /// An invalid `type_id` clears the recorded name.
  void set_property_type_name(PropNameId name_id, TypeNameId type_id);
  void set_property_type_name(PropNameId name_id, const std::string& type_name);

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
  const std::vector<uint32_t>& child_indices() const;

  /// Mutable child-index storage — used by Layer stitching (the parallel USDA
  /// subtree parse rebases/resolves placeholder indices in one pass).
  std::vector<uint32_t>& mutable_child_indices();

  /// Add child index
  void add_child_index(uint32_t index);

  /// Remove a single child index link (used by Layer::remove_prim_at_path).
  /// Returns true if the index was found and removed.
  bool remove_child_index(uint32_t index);

  /// Drop all child links (orphan the subtree). The child prims remain in the
  /// Layer but become unreachable from this prim — used by the extracted-
  /// prototype flatten to move an instance's inline subtree onto a
  /// `/Flattened_Prototype_N` root.
  void clear_child_indices();

  /// Replace the child index list wholesale (namespace reordering; e.g. the
  /// crate reader restoring authored order from primChildren).
  void set_child_indices(std::vector<uint32_t>&& idx);

  /// Get child count
  size_t child_count() const;

  // ============================================================
  // Metadata
  // ============================================================

  /// Get metadata
  const PrimMetadata& meta() const;
  PrimMetadata& meta();

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
  std::unique_ptr<ValueBlockStorage> values_;

  // TimeSamples storage with deduplication
  std::unique_ptr<TimeSampleBlock> time_samples_;

  // Connections, relationship opinions and authored property metadata are
  // sparse. Their STL containers are private to the compiled implementation.
  struct ColdData;
  // Move a type name recorded before `slot` existed onto the slot.
  void adopt_pending_type_name(PropSlot& slot);
  ColdData& ensure_cold_data();
  ColdData& cold();
  const ColdData& cold() const;
  std::unique_ptr<ColdData> cold_data_;

  // Children stored as indices into parent Layer's prim array
  std::vector<uint32_t> child_indices_;

  PrimMetadata meta_;
};

}  // namespace next
}  // namespace lightusd
