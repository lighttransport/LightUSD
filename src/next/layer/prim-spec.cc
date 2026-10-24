// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PrimSpec Implementation

#include "prim-spec.hh"
#include "../prim/identifier.hh"
#include "../../safe-arithmetic.hh"
#include <algorithm>
#include <cstring>
#include <list>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#endif

namespace lightusd {
namespace next {

struct PrimSpec::ColdData {

    std::unordered_map<std::string, ArcListOp> rel_edits;
    std::unordered_map<std::string, std::vector<RelationshipOpinion>>
        rel_opinion_stacks;
    std::unordered_map<std::string, uint16_t> rel_flags;
    std::unordered_map<uint32_t, ArcListOp> connection_edits;
    std::unordered_map<uint32_t, std::vector<RelationshipOpinion>>
        connection_opinion_stacks;
    // Raw USDA `.spline` values and unsupported defaults are keyed by property
    // id. Sparse array edits keep a structured twin of the raw default text.
    std::unordered_map<uint32_t, std::string> spline_sources;
    std::unordered_map<uint32_t, std::string> raw_default_sources;
    std::unordered_map<uint32_t, ArrayEditData> array_edits;
  std::unordered_map<std::string, std::vector<Path>> relationships;
  std::unordered_map<uint32_t, std::vector<Path>> connections;
  // Fallback for declared property type names. The common case lives on
  // PropSlot::type_name_id; this map only holds names recorded before the
  // property's slot existed (adopted onto the slot when it is added) or past
  // the TypeNameTable capacity.
  std::unordered_map<uint32_t, std::string> prop_type_names;
  std::unordered_map<uint32_t, std::unique_ptr<PropMeta>> prop_metas;
  ColdData() = default;
  ColdData(const ColdData& other) : rel_edits(other.rel_edits), rel_opinion_stacks(other.rel_opinion_stacks), rel_flags(other.rel_flags), connection_edits(other.connection_edits), connection_opinion_stacks(other.connection_opinion_stacks), spline_sources(other.spline_sources), raw_default_sources(other.raw_default_sources), array_edits(other.array_edits), relationships(other.relationships), connections(other.connections), prop_type_names(other.prop_type_names) {
    for (const auto& kv : other.prop_metas)
      if (kv.second) prop_metas[kv.first] = std::make_unique<PropMeta>(*kv.second);
  }
};

PrimSpec::PrimSpec() {}

PrimSpec::PrimSpec(const std::string& name) : name_(name) {}

PrimSpec::PrimSpec(const std::string& name, const std::string& type_name)
    : name_(name) {
  set_type_name(type_name);
}

PrimSpec::~PrimSpec() = default;

PrimSpec::PrimSpec(PrimSpec&& other) noexcept = default;
PrimSpec& PrimSpec::operator=(PrimSpec&& other) noexcept = default;

PrimSpec PrimSpec::Clone() const {
  PrimSpec c(name_);
  c.type_id_ = type_id_;
  c.specifier_ = specifier_;
  c.path_ = path_;

  // Deep copy properties (slots carry value-storage indices + flags).
  c.props_ = props_;

  // Deep copy the value storage. A slot's value_offset is a position in the
  // values_ vector, so copying the vector keeps every offset valid — no need to
  // re-add (the old code did both `c.props_ = props_` AND a re-add loop, which
  // duplicated every property). Lazy array Values copy as a shared_ptr bump
  // (no payload copy), so cloning a crate-backed prim stays cheap.
  if (values_) {
    c.values_ = std::make_unique<ValueBlockStorage>(*values_);
  }

  // Deep copy time samples
  if (time_samples_) {
    c.time_samples_ = std::make_unique<TimeSampleBlock>();
    // Copy time samples per property
    for (auto prop_id : time_sampled_properties()) {
      auto* samples = time_samples(prop_id);
      if (samples) {
        for (const auto& [t, offset] : *samples) {
          const Value* v = time_sample_value(offset);
          if (v) {
            c.add_time_sample(prop_id, t, *v);
          }
        }
      }
    }
  }

  if (cold_data_) c.cold_data_ = std::make_unique<ColdData>(*cold_data_);

  // Deep copy children
  c.child_indices_ = child_indices_;

  // Deep copy metadata (simple struct - copyable)
  c.meta_ = meta_;

  return c;
}

const std::string& PrimSpec::type_name() const {
  return GetTypeNameTable().get(type_id_);
}

void PrimSpec::set_type_name(const std::string& name) {
  type_id_ = GetTypeNameTable().intern(name);
}

const PropSlot* PrimSpec::property(PropNameId name_id) const {
  return props_.find(name_id);
}

const PropSlot* PrimSpec::property(const std::string& name) const {
  return props_.find(name);
}

const Value* PrimSpec::property_value(PropNameId name_id) const {
  const PropSlot* slot = property(name_id);
  if (!slot || !values_) return nullptr;
  return values_->get(slot->value_offset);
}

const Value* PrimSpec::property_value(const std::string& name) const {
  const PropSlot* slot = property(name);
  if (!slot || !values_) return nullptr;
  return values_->get(slot->value_offset);
}

void PrimSpec::add_property(const std::string& name, Value value, uint16_t flags) {
  // Authoring boundary: a property name must be a valid (possibly namespaced)
  // identifier. The parser only ever produces such names, so guarding here keeps
  // API-authored scenes round-trippable (a non-identifier name would be written
  // bare and be unparseable by this parser and by usdcat).
  if (!IsValidNamespacedIdentifier(name)) return;
  PropNameId name_id = GetPropNameTable().intern(name);
  add_property(name_id, std::move(value), flags);
}

void PrimSpec::add_property(PropNameId name_id, Value value, uint16_t flags) {
  // Store value
  if (!values_) values_ = std::make_unique<ValueBlockStorage>();
  uint32_t offset = values_->store(std::move(value));

  // Get type from stored value
  const Value* stored = values_->get(offset);
  uint16_t type = static_cast<uint16_t>(stored ? stored->type_id() : TypeId::Invalid);

  // Add slot
  PropSlot slot;
  slot.name_id = name_id;
  slot.value_offset = offset;
  slot.value_type = type;
  slot.flags = flags;

  if (stored && stored->is_array()) {
    slot.flags |= PropSlot::kFlagArray;
  }

  adopt_pending_type_name(slot);
  props_.add(slot);
}

void PrimSpec::add_property_slot(PropNameId name_id, TypeId type_id, uint16_t flags) {
  PropSlot slot;
  slot.name_id = name_id;
  slot.value_offset = UINT32_MAX;  // No default value
  slot.value_type = static_cast<uint16_t>(type_id);
  slot.flags = flags;
  adopt_pending_type_name(slot);
  props_.add(slot);
}

void PrimSpec::adopt_pending_type_name(PropSlot& slot) {
  if (!cold_data_ || cold_data_->prop_type_names.empty()) return;
  auto it = cold_data_->prop_type_names.find(slot.name_id.id);
  if (it == cold_data_->prop_type_names.end()) return;
  const TypeNameId tid = GetTypeNameTable().intern(it->second);
  if (!tid.is_valid()) return;  // table full: keep the string fallback
  slot.type_name_id = tid.id;
  cold_data_->prop_type_names.erase(it);
}

bool PrimSpec::fill_property_value_if_absent(PropNameId name_id, Value value) {
  PropSlot* slot = props_.find_mutable(name_id);
  if (!slot) return false;
  if (slot->value_offset != UINT32_MAX) return false;  // already has a value
  if (!values_) values_ = std::make_unique<ValueBlockStorage>();
  uint32_t offset = values_->store(std::move(value));
  const Value* stored = values_->get(offset);
  slot->value_offset = offset;
  // Bind the concrete stored type (and array flag); keep the declared slot type
  // if the incoming value is Invalid.
  if (stored && stored->type_id() != TypeId::Invalid) {
    slot->value_type = static_cast<uint16_t>(stored->type_id());
    if (stored->is_array()) slot->flags |= PropSlot::kFlagArray;
  }
  return true;
}

bool PrimSpec::set_property_value(PropNameId name_id, Value value) {
  PropSlot* slot = props_.find_mutable(name_id);
  if (!slot || slot->value_offset == UINT32_MAX) return false;
  if (!values_) return false;
  // Replace IN PLACE like upsert_property() does. Appending instead left the
  // previous Value -- potentially a multi-MB points/faceVertexIndices array --
  // stranded in values_ forever (nothing compacts ValueBlockStorage), so every
  // array-edit / PathExpression resolution permanently doubled that
  // attribute's memory.
  uint32_t offset = slot->value_offset;
  if (Value* dst = values_->get(offset)) {
    *dst = std::move(value);
  } else {
    offset = values_->store(std::move(value));
  }
  const Value* stored = values_->get(offset);
  slot->value_offset = offset;
  if (stored && stored->type_id() != TypeId::Invalid) {
    slot->value_type = static_cast<uint16_t>(stored->type_id());
    if (stored->is_array()) slot->flags |= PropSlot::kFlagArray;
    else slot->flags &= static_cast<uint16_t>(~PropSlot::kFlagArray);
  }
  return true;
}

void PrimSpec::upsert_property(PropNameId name_id, Value value, uint16_t flags) {
  PropSlot* slot = props_.find_mutable(name_id);
  if (!slot) {
    add_property(name_id, std::move(value), flags);
    return;
  }

  bool is_array = value.is_array();
  TypeId type = value.type_id();

  if (slot->value_offset == UINT32_MAX) {
    // Declared-only slot: attach a value.
    if (!values_) values_ = std::make_unique<ValueBlockStorage>();
    slot->value_offset = values_->store(std::move(value));
  } else if (Value* dst = values_ ? values_->get(slot->value_offset) : nullptr) {
    // Replace in place — no ValueBlockStorage growth on re-authoring.
    *dst = std::move(value);
  } else {
    if (!values_) values_ = std::make_unique<ValueBlockStorage>();
    slot->value_offset = values_->store(std::move(value));
  }

  if (type != TypeId::Invalid) {
    slot->value_type = static_cast<uint16_t>(type);
  }
  slot->flags |= flags;
  if (is_array) {
    slot->flags |= PropSlot::kFlagArray;
  } else {
    slot->flags &= static_cast<uint16_t>(~PropSlot::kFlagArray);
  }
}

void PrimSpec::upsert_property(const std::string& name, Value value,
                               uint16_t flags) {
  // Authoring boundary (see add_property): reject non-identifier names so API
  // scenes stay round-trippable.
  if (!IsValidNamespacedIdentifier(name)) return;
  upsert_property(GetPropNameTable().intern(name), std::move(value), flags);
}

bool PrimSpec::remove_property(PropNameId name_id) {
  if (!name_id.is_valid()) return false;
  bool removed = props_.remove(name_id);
  if (cold_data_) {
    if (cold_data_->connections.erase(name_id.id) > 0) removed = true;
    cold_data_->connection_edits.erase(name_id.id);
    cold_data_->connection_opinion_stacks.erase(name_id.id);
    cold_data_->prop_type_names.erase(name_id.id);
    cold_data_->prop_metas.erase(name_id.id);
  }
  if (time_samples_ && time_samples_->remove(name_id)) removed = true;
  return removed;
}

bool PrimSpec::remove_property(const std::string& name) {
  return remove_property(GetPropNameTable().find(name));
}

size_t PrimSpec::release_static_array_value(PropNameId name_id,
                                            size_t min_array_elements,
                                            size_t* released_elements) {
  PropSlot* slot = props_.find_mutable(name_id);
  if (!slot || slot->value_offset == UINT32_MAX || !values_ ||
      (time_samples_ && time_samples_->has(name_id))) {
    return 0;
  }
  Value* value = values_->get(slot->value_offset);
  if (!value || !value->is_array() || value->is_lazy() ||
      value->array_size() < min_array_elements) {
    return 0;
  }
  const size_t elements = value->array_size();
  const size_t element_bytes = GetTypeSize(value->type_id());
  const size_t released_bytes = elements * element_bytes;
  value->clear();
  // Keep the slot and its original value_type/kFlagArray so property-name and
  // declared-type inspection still work. Only the default becomes absent.
  slot->value_offset = UINT32_MAX;
  if (released_elements) *released_elements += elements;
  return released_bytes;
}

size_t PrimSpec::remap_asset_paths(
    const std::map<std::string, std::string>& remap) {
  if (remap.empty()) return 0;
  size_t count = 0;
  for (const PropSlot& slot : props_.slots()) {
    const Value* value = property_value(slot.name_id);
    if (!value) continue;
    auto path = value->as_asset_path();
    if (!path) continue;
    const std::string* mapped = FindAssetPathRemap(remap, *path);
    if (!mapped) continue;
    if (set_property_value(slot.name_id, Value::MakeAssetPath(*mapped))) {
      ++count;
    }
  }
  if (time_samples_) {
    count += time_samples_->remap_asset_paths(remap);
  }
  return count;
}

void PrimSpec::reserve_properties(size_t count) {
  props_.reserve(count);
}

void PrimSpec::finalize_properties() {
  props_.sort();
}

void PrimSpec::release_value_payloads() {
  if (values_) values_->release_payloads();
  if (time_samples_) time_samples_->release_payloads();
}

void PrimSpec::mark_property_time_sampled(PropNameId name_id) {
  if (PropSlot* slot = props_.find_mutable(name_id)) {
    slot->flags |= PropSlot::kFlagTimeSampled;
  }
}

void PrimSpec::add_time_sample(PropNameId name_id, double time, Value value,
                               bool dedup) {
  // Use deduplicated storage for array values (common case for animation).
  // `dedup=false`: deferred-fill value (see header), stored as-is like the
  // is_lazy() bypass in add_dedup.
  if (!time_samples_) time_samples_ = std::make_unique<TimeSampleBlock>();
  if (dedup) {
    time_samples_->add_dedup(name_id, time, std::move(value));
  } else {
    time_samples_->add(name_id, time, std::move(value));
  }
}

const std::vector<std::pair<double, uint32_t>>* PrimSpec::time_samples(PropNameId name_id) const {
  if (!time_samples_) return nullptr;
  return time_samples_->get(name_id);
}

void PrimSpec::remap_time_sample_times(double offset, double scale) {
  if (time_samples_) time_samples_->remap_times(offset, scale);
}

bool PrimSpec::has_time_samples(PropNameId name_id) const {
  if (!time_samples_) return false;
  return time_samples_->has(name_id);
}

const Value* PrimSpec::time_sample_value(uint32_t offset) const {
  if (!time_samples_) return nullptr;
  return time_samples_->value(offset);
}

std::vector<PropNameId> PrimSpec::time_sampled_properties() const {
  if (!time_samples_) return {};
  return time_samples_->properties();
}

bool PrimSpec::has_any_time_samples() const {
  if (!time_samples_) return false;
  return !time_samples_->empty();
}

TimeSampleBlock::Stats PrimSpec::time_sample_stats() const {
  if (!time_samples_) return {};
  return time_samples_->stats();
}

SampleResult PrimSpec::interpolate_time_sample(PropNameId name_id, double time,
                                               TimeInterpolation mode) const {
  if (!time_samples_) return SampleResult{};
  return time_samples_->interpolate(name_id, time, mode);
}

SampleResult PrimSpec::interpolate_time_sample(const std::string& name, double time,
                                               TimeInterpolation mode) const {
  PropNameId name_id = GetPropNameTable().find(name);
  if (!name_id.is_valid()) return SampleResult{};
  return interpolate_time_sample(name_id, time, mode);
}

void PrimSpec::set_spline_source(const std::string& prop_name,
                                 std::string source) {
  set_spline_source(GetPropNameTable().intern(prop_name), std::move(source));
}

void PrimSpec::set_spline_source(PropNameId name_id, std::string source) {
  ensure_cold_data().spline_sources[name_id.id] = std::move(source);
}

const std::string* PrimSpec::spline_source(PropNameId name_id) const {
  if (!name_id.is_valid() || !cold_data_) return nullptr;
  auto it = cold_data_->spline_sources.find(name_id.id);
  return it == cold_data_->spline_sources.end() ? nullptr : &it->second;
}

const std::string* PrimSpec::spline_source(
    const std::string& prop_name) const {
  return spline_source(GetPropNameTable().find(prop_name));
}

void PrimSpec::set_raw_default_source(const std::string& prop_name,
                                      std::string source) {
  set_raw_default_source(GetPropNameTable().intern(prop_name),
                         std::move(source));
}

void PrimSpec::set_raw_default_source(PropNameId name_id, std::string source) {
  ensure_cold_data().raw_default_sources[name_id.id] = std::move(source);
}

void PrimSpec::clear_raw_default_source(PropNameId name_id) {
  if (cold_data_) cold_data_->raw_default_sources.erase(name_id.id);
}

void PrimSpec::set_array_edit(const std::string& prop_name,
                              ArrayEditData edit) {
  set_array_edit(GetPropNameTable().intern(prop_name), std::move(edit));
}

void PrimSpec::set_array_edit(PropNameId name_id, ArrayEditData edit) {
  ensure_cold_data().array_edits[name_id.id] = std::move(edit);
}

const ArrayEditData* PrimSpec::array_edit(PropNameId name_id) const {
  if (!cold_data_) return nullptr;
  auto it = cold_data_->array_edits.find(name_id.id);
  return it == cold_data_->array_edits.end() ? nullptr : &it->second;
}

void PrimSpec::clear_array_edit(PropNameId name_id) {
  if (cold_data_) cold_data_->array_edits.erase(name_id.id);
}

bool PrimSpec::needs_property_composition() const {
  if (!cold_data_) return false;
  if (!cold_data_->array_edits.empty()) return true;
  for (const auto& entry : cold_data_->rel_edits) {
    if (!entry.second.is_explicit && entry.second.has_authored_opinion())
      return true;
  }
  for (const auto& entry : cold_data_->connection_edits) {
    if (!entry.second.is_explicit && entry.second.has_authored_opinion())
      return true;
  }
  return false;
}

const std::string* PrimSpec::raw_default_source(PropNameId name_id) const {
  if (!name_id.is_valid() || !cold_data_) return nullptr;
  auto it = cold_data_->raw_default_sources.find(name_id.id);
  return it == cold_data_->raw_default_sources.end() ? nullptr : &it->second;
}

void PrimSpec::add_relationship(const std::string& name, const Path& target) {
  // Authoring boundary (see add_property): keep relationship names valid
  // identifiers so API scenes stay round-trippable.
  if (!IsValidNamespacedIdentifier(name)) return;
  cold().relationships[name].push_back(target);
}

ArcListOp& PrimSpec::ensure_relationship_edit(const std::string& name) {
  return ensure_cold_data().rel_edits[name];
}

const std::vector<PrimSpec::RelationshipOpinion>*
PrimSpec::relationship_opinion_stack(const std::string& name) const {
  if (!cold_data_) return nullptr;
  auto it = cold_data_->rel_opinion_stacks.find(name);
  return it == cold_data_->rel_opinion_stacks.end() ? nullptr : &it->second;
}

void PrimSpec::set_relationship_opinion_stack(
    const std::string& name, std::vector<RelationshipOpinion> opinions) {
  ensure_cold_data().rel_opinion_stacks[name] = std::move(opinions);
}

uint16_t PrimSpec::relationship_flags(const std::string& name) const {
  if (!cold_data_) return 0;
  auto it = cold_data_->rel_flags.find(name);
  return it == cold_data_->rel_flags.end() ? 0 : it->second;
}

void PrimSpec::set_relationship_flags(const std::string& name, uint16_t flags) {
  if (flags) ensure_cold_data().rel_flags[name] = flags;
}

void PrimSpec::apply_relationship_list_op(const std::string& name,
                                          const std::vector<Path>& targets,
                                          RelationshipListOp op) {
  if (targets.empty()) return;
  std::vector<Path>& dst = cold().relationships[name];

  switch (op) {
    case RelationshipListOp::Prepend:
      dst.insert(dst.begin(), targets.begin(), targets.end());
      break;
    case RelationshipListOp::Add:
      for (const Path& t : targets) {
        if (std::find(dst.begin(), dst.end(), t) == dst.end()) {
          dst.push_back(t);
        }
      }
      break;
    case RelationshipListOp::Delete:
      dst.erase(std::remove_if(dst.begin(), dst.end(),
                               [&](const Path& p) {
                                 return std::find(targets.begin(), targets.end(),
                                                  p) != targets.end();
                               }),
                dst.end());
      if (dst.empty()) cold().relationships.erase(name);
      break;
    case RelationshipListOp::Reorder: {
      std::vector<Path> ordered;
      for (const Path& target : targets) {
        auto it = std::find(dst.begin(), dst.end(), target);
        if (it != dst.end()) ordered.push_back(*it);
      }
      for (const Path& target : dst) {
        if (std::find(ordered.begin(), ordered.end(), target) == ordered.end())
          ordered.push_back(target);
      }
      dst = std::move(ordered);
      break;
    }
    case RelationshipListOp::Append:
    default:
      dst.insert(dst.end(), targets.begin(), targets.end());
      break;
  }
}

void PrimSpec::set_relationship_targets(const std::string& name,
                                        std::vector<Path> targets) {
  cold().relationships[name] = std::move(targets);
}

bool PrimSpec::remove_relationship(const std::string& name) {
  bool removed = cold().relationships.erase(name) > 0;
  if (cold_data_) {
    cold_data_->rel_opinion_stacks.erase(name);
    cold_data_->rel_edits.erase(name);
  }
  PropNameId id = GetPropNameTable().find(name);
  if (id.is_valid()) {
    const PropSlot* slot = props_.find(id);
    if (slot && slot->is_relationship()) {
      props_.remove(id);
      removed = true;
    }
  }
  return removed;
}

const std::vector<Path>* PrimSpec::relationship(const std::string& name) const {
  auto it = cold().relationships.find(name);
  if (it == cold().relationships.end()) return nullptr;
  return &it->second;
}

bool PrimSpec::has_relationship_data() const {
  return cold_data_ && (!cold_data_->relationships.empty() ||
                        !cold_data_->rel_edits.empty() ||
                        !cold_data_->rel_opinion_stacks.empty());
}

std::vector<std::string> PrimSpec::relationship_names() const {
  std::vector<std::string> names;
  names.reserve(cold().relationships.size() + relationship_edits().size());
  for (const auto& [name, _] : cold().relationships) {
    names.push_back(name);
  }
  // A delete-only or otherwise inert list-op may have no effective target
  // entries in cold().relationships, but remains an authored relationship opinion.
  for (const auto& [name, _] : relationship_edits()) names.push_back(name);
  // Both maps are unordered; sort and deduplicate for stable enumeration.
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

void PrimSpec::add_connection(const std::string& prop_name, const Path& target) {
  // Authoring boundary (see add_property): keep connection property names
  // valid identifiers.
  if (!IsValidNamespacedIdentifier(prop_name)) return;
  const PropNameId id = GetPropNameTable().intern(prop_name);
  cold().connections[id.id].push_back(target);
  // Mark the slot as carrying a connection so serializers that gate on the
  // flag (the USDC writer) emit it even when the property ALSO has a default
  // value — otherwise `attr = v` + `attr.connect = <t>` loses the connection
  // through a crate round trip.
  if (PropSlot* s = property_mutable(id)) s->flags |= PropSlot::kFlagConnection;
}

void PrimSpec::set_connection_block(const std::string& prop_name) {
  const PropNameId id = GetPropNameTable().intern(prop_name);
  cold().connections[id.id];  // empty vector (authored `.connect = None`)
  if (PropSlot* s = property_mutable(id)) s->flags |= PropSlot::kFlagConnection;
}

const ArcListOp* PrimSpec::connection_edit(const std::string& prop_name) const {
  if (!cold_data_) return nullptr;
  return connection_edit(GetPropNameTable().find(prop_name));
}

const ArcListOp* PrimSpec::connection_edit(PropNameId id) const {
  if (!id.is_valid() || !cold_data_) return nullptr;
  auto it = cold_data_->connection_edits.find(id.id);
  return it == cold_data_->connection_edits.end() ? nullptr : &it->second;
}

ArcListOp& PrimSpec::ensure_connection_edit(const std::string& prop_name) {
  return ensure_connection_edit(GetPropNameTable().intern(prop_name));
}

ArcListOp& PrimSpec::ensure_connection_edit(PropNameId name_id) {
  return ensure_cold_data().connection_edits[name_id.id];
}

const std::vector<PrimSpec::RelationshipOpinion>*
PrimSpec::connection_opinion_stack(const std::string& prop_name) const {
  if (!cold_data_) return nullptr;
  return connection_opinion_stack(GetPropNameTable().find(prop_name));
}

const std::vector<PrimSpec::RelationshipOpinion>*
PrimSpec::connection_opinion_stack(PropNameId id) const {
  if (!id.is_valid() || !cold_data_) return nullptr;
  auto it = cold_data_->connection_opinion_stacks.find(id.id);
  return it == cold_data_->connection_opinion_stacks.end() ? nullptr
                                                           : &it->second;
}

void PrimSpec::set_connection_opinion_stack(
    const std::string& prop_name, std::vector<RelationshipOpinion> opinions) {
  set_connection_opinion_stack(GetPropNameTable().intern(prop_name),
                               std::move(opinions));
}

void PrimSpec::set_connection_opinion_stack(
    PropNameId name_id, std::vector<RelationshipOpinion> opinions) {
  ensure_cold_data().connection_opinion_stacks[name_id.id] =
      std::move(opinions);
}

void PrimSpec::set_connection_targets(const std::string& prop_name,
                                      std::vector<Path> targets) {
  set_connection_targets(GetPropNameTable().intern(prop_name),
                         std::move(targets));
}

void PrimSpec::set_connection_targets(PropNameId id,
                                      std::vector<Path> targets) {
  cold().connections[id.id] = std::move(targets);
  if (PropSlot* slot = property_mutable(id)) {
    slot->flags |= PropSlot::kFlagConnection;
  }
}

void PrimSpec::apply_connection_list_op(const std::string& prop_name,
                                        const std::vector<Path>& targets,
                                        RelationshipListOp op) {
  const PropNameId id = GetPropNameTable().intern(prop_name);
  std::vector<Path>& dst = cold().connections[id.id];
  switch (op) {
    case RelationshipListOp::Prepend:
      dst.insert(dst.begin(), targets.begin(), targets.end());
      break;
    case RelationshipListOp::Add:
      for (const Path& target : targets) {
        if (std::find(dst.begin(), dst.end(), target) == dst.end())
          dst.push_back(target);
      }
      break;
    case RelationshipListOp::Delete:
      dst.erase(std::remove_if(dst.begin(), dst.end(), [&](const Path& path) {
                  return std::find(targets.begin(), targets.end(), path) !=
                         targets.end();
                }), dst.end());
      break;
    case RelationshipListOp::Reorder: {
      std::vector<Path> ordered;
      for (const Path& target : targets) {
        auto it = std::find(dst.begin(), dst.end(), target);
        if (it != dst.end()) ordered.push_back(*it);
      }
      for (const Path& target : dst) {
        if (std::find(ordered.begin(), ordered.end(), target) == ordered.end())
          ordered.push_back(target);
      }
      dst = std::move(ordered);
      break;
    }
    case RelationshipListOp::Append:
      dst.insert(dst.end(), targets.begin(), targets.end());
      break;
  }
  if (PropSlot* slot = property_mutable(id)) {
    slot->flags |= PropSlot::kFlagConnection;
  }
}

const std::vector<Path>* PrimSpec::connection(const std::string& prop_name) const {
  if (!cold_data_) return nullptr;  // nothing authored: skip the name hash
  return connection(GetPropNameTable().find(prop_name));
}

const std::vector<Path>* PrimSpec::connection(PropNameId id) const {
  if (!id.is_valid() || !cold_data_) return nullptr;
  auto it = cold_data_->connections.find(id.id);
  if (it == cold_data_->connections.end()) return nullptr;
  return &it->second;
}

void PrimSpec::remap_target_prefix(const std::string& old_prefix,
                                   const std::string& new_prefix) {
  if (!cold_data_) return;
  auto remap = [&](std::vector<Path>& targets) {
    for (Path& t : targets) {
      const std::string s = t.str();
      if (s == old_prefix) {
        t = Path(new_prefix);
      } else if (s.size() > old_prefix.size() &&
                 s.compare(0, old_prefix.size(), old_prefix) == 0 &&
                 (s[old_prefix.size()] == '/' || s[old_prefix.size()] == '.')) {
        t = Path(new_prefix + s.substr(old_prefix.size()));
      }
    }
  };
  auto remap_strings = [&](std::vector<std::string>& targets) {
    for (std::string& value : targets) {
      if (value == old_prefix) {
        value = new_prefix;
      } else if (value.size() > old_prefix.size() &&
                 value.compare(0, old_prefix.size(), old_prefix) == 0 &&
                 (value[old_prefix.size()] == '/' ||
                  value[old_prefix.size()] == '.')) {
        value = new_prefix + value.substr(old_prefix.size());
      }
    }
  };
  auto remap_edit = [&](ArcListOp& edit) {
    remap_strings(edit.added);
    remap_strings(edit.prepended);
    remap_strings(edit.appended);
    remap_strings(edit.deleted);
    remap_strings(edit.ordered);
  };
  for (auto& kv : cold().relationships) remap(kv.second);
  for (auto& kv : cold().connections) remap(kv.second);
  if (cold_data_) {
    for (auto& kv : cold_data_->rel_edits) remap_edit(kv.second);
    for (auto& kv : cold_data_->connection_edits) remap_edit(kv.second);
    for (auto& kv : cold_data_->rel_opinion_stacks) {
      for (RelationshipOpinion& opinion : kv.second) {
        remap(opinion.items);
        remap_edit(opinion.edit);
      }
    }
    for (auto& kv : cold_data_->connection_opinion_stacks) {
      for (RelationshipOpinion& opinion : kv.second) {
        remap(opinion.items);
        remap_edit(opinion.edit);
      }
    }
  }
}

PrimSpec::ColdData& PrimSpec::ensure_cold_data() {
  if (!cold_data_) cold_data_ = std::make_unique<ColdData>();
  return *cold_data_;
}

PrimSpec::ColdData& PrimSpec::cold() { return ensure_cold_data(); }
const PrimSpec::ColdData& PrimSpec::cold() const {
  static const ColdData empty;
  return cold_data_ ? *cold_data_ : empty;
}

void PrimSpec::set_property_type_name(const std::string& prop_name,
                                      const std::string& type_name) {
  // Type names are few and highly shared: intern once, keep the id on the
  // slot. A name recorded before the slot exists (the USDA parser declares
  // the type ahead of the value) waits in the cold map until add_property /
  // add_property_slot adopts it.
  const PropNameId nid = GetPropNameTable().intern(prop_name);
  if (PropSlot* slot = props_.find_mutable(nid)) {
    const TypeNameId tid = GetTypeNameTable().intern(type_name);
    if (tid.is_valid()) {
      slot->type_name_id = tid.id;
      if (cold_data_) cold_data_->prop_type_names.erase(nid.id);
      return;
    }
    slot->type_name_id = PropSlot::kNoTypeName;
  }
  cold().prop_type_names[nid.id] = type_name;
}

void PrimSpec::set_property_type_name(PropNameId name_id,
                                      const std::string& type_name) {
  if (!name_id.is_valid()) return;
  const TypeNameId tid = GetTypeNameTable().intern(type_name);
  if (tid.is_valid()) {
    set_property_type_name(name_id, tid);
    return;
  }
  if (PropSlot* slot = props_.find_mutable(name_id)) {
    slot->type_name_id = PropSlot::kNoTypeName;
  }
  cold().prop_type_names[name_id.id] = type_name;
}

void PrimSpec::set_property_type_name(PropNameId name_id, TypeNameId type_id) {
  if (!name_id.is_valid()) return;
  if (PropSlot* slot = props_.find_mutable(name_id)) {
    slot->type_name_id = type_id.is_valid() ? type_id.id : PropSlot::kNoTypeName;
    if (cold_data_) cold_data_->prop_type_names.erase(name_id.id);
    return;
  }
  if (!type_id.is_valid()) {
    if (cold_data_) cold_data_->prop_type_names.erase(name_id.id);
    return;
  }
  cold().prop_type_names[name_id.id] = GetTypeNameTable().get(type_id);
}

const std::string* PrimSpec::property_type_name(PropNameId name_id) const {
  if (!name_id.is_valid()) return nullptr;
  if (const PropSlot* slot = props_.find(name_id)) {
    if (slot->type_name_id != PropSlot::kNoTypeName) {
      return &GetTypeNameTable().get(TypeNameId{slot->type_name_id});
    }
  }
  if (!cold_data_) return nullptr;
  auto it = cold_data_->prop_type_names.find(name_id.id);
  if (it == cold_data_->prop_type_names.end()) return nullptr;
  return &it->second;
}

const std::string* PrimSpec::property_type_name(const std::string& prop_name) const {
  return property_type_name(GetPropNameTable().find(prop_name));
}

const PropMeta* PrimSpec::property_meta(PropNameId name_id) const {
  if (!name_id.is_valid()) return nullptr;
  auto it = cold().prop_metas.find(name_id.id);
  if (it == cold().prop_metas.end()) return nullptr;
  return it->second.get();
}

const PropMeta* PrimSpec::property_meta(const std::string& prop_name) const {
  return property_meta(GetPropNameTable().find(prop_name));
}

PropMeta& PrimSpec::ensure_property_meta(PropNameId name_id) {
  std::unique_ptr<PropMeta>& slot = cold().prop_metas[name_id.id];
  if (!slot) slot = std::make_unique<PropMeta>();
  return *slot;
}

PropMeta& PrimSpec::ensure_property_meta(const std::string& prop_name) {
  return ensure_property_meta(GetPropNameTable().intern(prop_name));
}

void PrimSpec::add_child_index(uint32_t index) {
  child_indices_.push_back(index);
}

bool PrimSpec::remove_child_index(uint32_t index) {
  auto it = std::find(child_indices_.begin(), child_indices_.end(), index);
  if (it == child_indices_.end()) return false;
  child_indices_.erase(it);
  return true;
}

size_t PrimSpec::memory_usage() const {
  size_t size = sizeof(PrimSpec);
  auto add = [&size](size_t bytes) {
    size = safe::saturating_add(size, bytes);
  };
  add(name_.capacity());
  add(path_.str().capacity());
  const auto add_strings = [&add](const std::vector<std::string>& strings) {
    add(safe::saturating_mul(strings.capacity(), sizeof(std::string)));
    for (const std::string& text : strings) add(text.capacity());
  };
  const auto add_pairs = [&add](
      const std::vector<std::pair<std::string, std::string>>& pairs) {
    add(safe::saturating_mul(pairs.capacity(),
                             sizeof(std::pair<std::string, std::string>)));
    for (const auto& entry : pairs) {
      add(entry.first.capacity());
      add(entry.second.capacity());
    }
  };
  const auto add_value = [&add](const Value& value) {
    add(value.heap_memory_usage());
  };
  const auto add_extensions = [&add, &add_value](
      const std::vector<DecodedExtensionField>& fields) {
    add(safe::saturating_mul(fields.capacity(), sizeof(DecodedExtensionField)));
    for (const DecodedExtensionField& field : fields) {
      add(field.name.capacity());
      add(field.unregistered_source.capacity());
      add_value(field.value);
    }
  };
  const auto add_arc_edit = [&add_strings](const ArcListOp& edit) {
    add_strings(edit.added);
    add_strings(edit.prepended);
    add_strings(edit.appended);
    add_strings(edit.deleted);
    add_strings(edit.ordered);
  };
  const auto add_string_edit = [&add_strings](const StringListOps& edit) {
    add_strings(edit.explicit_items);
    add_strings(edit.added);
    add_strings(edit.prepended);
    add_strings(edit.appended);
    add_strings(edit.deleted);
    add_strings(edit.ordered);
  };
  const auto add_paths = [&add](const std::vector<Path>& paths) {
    add(safe::saturating_mul(paths.capacity(), sizeof(Path)));
    for (const Path& path : paths) add(path.str().capacity());
  };

  // Properties
  add(safe::saturating_mul(props_.slots().capacity(), sizeof(PropSlot)));
  if (values_) {
    add(values_->memory_usage());
  }

  // Time samples (use TimeSampleBlock's memory tracking)
  if (time_samples_) {
    add(time_samples_->memory_usage());
  }
  if (cold_data_) {
    add(sizeof(ColdData));
    for (const auto& entry : cold_data_->rel_edits) {
      add(entry.first.capacity() + sizeof(ArcListOp) + sizeof(void*) * 2);
      add_arc_edit(entry.second);
    }
    for (const auto& entry : cold_data_->connection_edits) {
      add(sizeof(entry) + sizeof(void*) * 2);
      add_arc_edit(entry.second);
    }
    for (const auto& entry : cold_data_->rel_opinion_stacks) {
      add(entry.first.capacity() + sizeof(entry) + sizeof(void*) * 2);
      add(safe::saturating_mul(entry.second.capacity(),
                               sizeof(RelationshipOpinion)));
      for (const RelationshipOpinion& opinion : entry.second) {
        add_paths(opinion.items);
        add_arc_edit(opinion.edit);
      }
    }
    for (const auto& entry : cold_data_->connection_opinion_stacks) {
      add(sizeof(entry) + sizeof(void*) * 2);
      add(safe::saturating_mul(entry.second.capacity(),
                               sizeof(RelationshipOpinion)));
      for (const RelationshipOpinion& opinion : entry.second) {
        add_paths(opinion.items);
        add_arc_edit(opinion.edit);
      }
    }
    for (const auto& entry : cold_data_->rel_flags)
      add(entry.first.capacity() + sizeof(entry) + sizeof(void*) * 2);
    for (const auto& entry : cold_data_->spline_sources)
      add(entry.second.capacity() + sizeof(entry) + sizeof(void*) * 2);
    for (const auto& entry : cold_data_->raw_default_sources)
      add(entry.second.capacity() + sizeof(entry) + sizeof(void*) * 2);
    for (const auto& entry : cold_data_->array_edits) {
      add(sizeof(entry) + sizeof(void*) * 2);
      add(safe::saturating_mul(entry.second.ops.capacity(),
                               sizeof(ArrayEditOpRec)));
      for (const ArrayEditOpRec& op : entry.second.ops) add(op.literal.capacity());
    }
  }

  // Relationships
  for (const auto& rel : cold().relationships) {
    add(rel.first.capacity() + sizeof(rel) + sizeof(void*) * 2);
    add_paths(rel.second);
  }
  for (const auto& connection : cold().connections) {
    add(sizeof(connection) + sizeof(void*) * 2);
    add_paths(connection.second);
  }
  for (const auto& entry : cold().prop_type_names)
    add(sizeof(entry) + sizeof(void*) * 2 + entry.second.capacity());
  for (const auto& entry : cold().prop_metas) {
    add(sizeof(entry) + sizeof(void*) * 2);
    if (!entry.second) continue;
    const PropMeta& meta = *entry.second;
    add(sizeof(PropMeta));
    for (const std::string* text : {&meta.interpolation, &meta.colorSpace,
         &meta.renderType, &meta.connectability, &meta.outputName,
         &meta.bindMaterialAs, &meta.kind, &meta.displayName,
         &meta.displayGroup, &meta.doc, &meta.comment, &meta.permission})
      add(text->capacity());
    add_strings(meta.allowedTokens);
    add_value(meta.customData);
    add_value(meta.assetInfo);
    add_value(meta.sdrMetadata);
    add_pairs(meta.unknownMeta);
    add_extensions(meta.unknownFields);
  }

  // Children
  add(safe::saturating_mul(child_indices_.capacity(), sizeof(uint32_t)));

  // Metadata (inline hot fields)
  add_strings(meta_.references);
  add_strings(meta_.payloads);
  add_strings(meta_.inherits);
  add_strings(meta_.specializes);
  add(meta_.variantSelection.capacity());
  // Cold fields only cost anything when the ext was allocated.
  if (const PrimMetadataCold* ext = meta_.ext()) {
    add(sizeof(PrimMetadataCold));
    add(ext->doc.capacity());
    add(ext->comment.capacity());
    add(ext->kind.capacity());
    add(ext->displayName.capacity());
    add(ext->permission.capacity());
    add(ext->instance_prototype.capacity());
    add(ext->apiSchemasQualifier.capacity());
    add_strings(ext->apiSchemas);
    add_strings(ext->displayGroupOrder);
    add_strings(ext->primOrder);
    add_strings(ext->propertyOrder);
    add_string_edit(ext->apiSchemaEdits);
    add_string_edit(ext->variantSetNameEdits);
    add_string_edit(ext->clipSetEdits);
    add_pairs(ext->variantSelections);
    add_pairs(ext->relocates);
    add_pairs(ext->unknownMeta);
    add_extensions(ext->unknownFields);
    add_value(ext->customData);
    add_value(ext->assetInfo);
    add_value(ext->sdrMetadata);
    add_value(ext->clips);
    add(safe::saturating_mul(ext->variantSets.capacity(),
                             sizeof(PrimVariantSetData)));
    std::vector<const PrimVariantSetData*> pending_sets;
    for (const PrimVariantSetData& set : ext->variantSets) pending_sets.push_back(&set);
    while (!pending_sets.empty()) {
      const PrimVariantSetData* set = pending_sets.back();
      pending_sets.pop_back();
      add(set->name.capacity());
      add(set->selected.capacity());
      add(safe::saturating_mul(set->variants.capacity(), sizeof(PrimVariantData)));
      for (const PrimVariantData& variant : set->variants) {
        for (const std::string* text : {&variant.name, &variant.doc,
             &variant.kind}) add(text->capacity());
        add(safe::saturating_mul(variant.properties.capacity(),
                                 sizeof(VariantProperty)));
        for (const VariantProperty& property : variant.properties) {
          add(property.name.capacity());
          add_value(property.value);
        }
        for (const auto& relationship : variant.relationships) {
          add(relationship.first.capacity() + sizeof(relationship) +
              sizeof(void*) * 2);
          add_paths(relationship.second);
        }
        for (const auto& flag : variant.relationshipFlags)
          add(flag.first.capacity() + sizeof(flag) + sizeof(void*) * 2);
        add_strings(variant.references);
        add_strings(variant.payloads);
        add_strings(variant.inherits);
        add_strings(variant.specializes);
        add_pairs(variant.variantSelections);
        add_pairs(variant.unknownMeta);
        add_extensions(variant.unknownFields);
        add(safe::saturating_mul(variant.variantSets.capacity(),
                                 sizeof(PrimVariantSetData)));
        for (const PrimVariantSetData& nested : variant.variantSets)
          pending_sets.push_back(&nested);
        // Shared variant content Layers can have multiple owners; they need
        // deduplicated layer-level accounting rather than per-option charging.
      }
    }
    add(safe::saturating_mul(ext->clipShadowedProps.capacity(),
                             sizeof(PropNameId)));
    if (ext->arc_edits) {
      add(sizeof(ArcListOps));
      add_arc_edit(ext->arc_edits->references);
      add_arc_edit(ext->arc_edits->payloads);
      add_arc_edit(ext->arc_edits->inherits);
      add_arc_edit(ext->arc_edits->specializes);
    }
  }

  return size;
}

// ---------------------------------------------------------------------------
// PrimMetadata accessors, moved out of prim-spec.hh so units that include it
// do not re-parse their bodies.
// ---------------------------------------------------------------------------
bool PrimMetadata::has_ext() const { return ext_ != nullptr; }
void PrimMetadata::ensure_ext() {
    if (!ext_) ext_.reset(new PrimMetadataCold());
  }
const PrimMetadataCold *PrimMetadata::ext() const { return ext_.get(); }
const ArcListOps *PrimMetadata::arc_edits() const {
    return ext_ ? ext_->arc_edits.get() : nullptr;
  }
void PrimMetadata::clear_arc_edits() {
    if (ext_) ext_->arc_edits.reset();
  }
ArcListOps &PrimMetadata::ensure_arc_edits() {
    ensure_ext();
    if (!ext_->arc_edits) ext_->arc_edits.reset(new ArcListOps());
    return *ext_->arc_edits;
  }
const std::string &PrimMetadata::doc() const {
    static const std::string kEmpty;
    return ext_ ? ext_->doc : kEmpty;
  }
std::string &PrimMetadata::doc() {
    ensure_ext();
    return ext_->doc;
  }
bool PrimMetadata::doc_authored() const { return ext_ && ext_->doc_authored; }
void PrimMetadata::set_doc_authored(bool authored ) {
    ensure_ext();
    ext_->doc_authored = authored;
  }
const std::string &PrimMetadata::comment() const {
    static const std::string kEmpty;
    return ext_ ? ext_->comment : kEmpty;
  }
std::string &PrimMetadata::comment() {
    ensure_ext();
    return ext_->comment;
  }
bool PrimMetadata::comment_authored() const { return ext_ && ext_->comment_authored; }
void PrimMetadata::set_comment_authored(bool authored ) {
    ensure_ext();
    ext_->comment_authored = authored;
  }
const std::string &PrimMetadata::kind() const {
    static const std::string kEmpty;
    return ext_ ? ext_->kind : kEmpty;
  }
std::string &PrimMetadata::kind() {
    ensure_ext();
    return ext_->kind;
  }
bool PrimMetadata::kindAuthored() const { return ext_ && ext_->kind_authored; }
void PrimMetadata::setKindAuthored(bool authored ) {
    ensure_ext();
    ext_->kind_authored = authored;
  }
const std::string &PrimMetadata::permission() const {
    static const std::string kEmpty;
    return ext_ ? ext_->permission : kEmpty;
  }
std::string &PrimMetadata::permission() {
    ensure_ext();
    return ext_->permission;
  }
const std::string &PrimMetadata::displayName() const {
    static const std::string kEmpty;
    return ext_ ? ext_->displayName : kEmpty;
  }
std::string &PrimMetadata::displayName() {
    ensure_ext();
    return ext_->displayName;
  }
bool PrimMetadata::displayNameAuthored() const {
    return ext_ && ext_->display_name_authored;
  }
void PrimMetadata::setDisplayNameAuthored(bool authored ) {
    ensure_ext();
    ext_->display_name_authored = authored;
  }
const std::string &PrimMetadata::instance_prototype() const {
    static const std::string kEmpty;
    return ext_ ? ext_->instance_prototype : kEmpty;
  }
std::string &PrimMetadata::instance_prototype() {
    ensure_ext();
    return ext_->instance_prototype;
  }
const std::vector<std::string> &PrimMetadata::apiSchemas() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->apiSchemas : kEmpty;
  }
std::vector<std::string> &PrimMetadata::apiSchemas() {
    ensure_ext();
    return ext_->apiSchemas;
  }
bool PrimMetadata::apiSchemasAuthored() const {
    return ext_ && ext_->apiSchemasAuthored;
  }
void PrimMetadata::setApiSchemasAuthored(bool authored ) {
    ensure_ext();
    ext_->apiSchemasAuthored = authored;
  }
const StringListOps &PrimMetadata::apiSchemaEdits() const {
    static const StringListOps kEmpty;
    return ext_ ? ext_->apiSchemaEdits : kEmpty;
  }
StringListOps &PrimMetadata::apiSchemaEdits() {
    ensure_ext();
    return ext_->apiSchemaEdits;
  }
const std::string &PrimMetadata::apiSchemasQualifier() const {
    static const std::string kEmpty;
    return ext_ ? ext_->apiSchemasQualifier : kEmpty;
  }
std::string &PrimMetadata::apiSchemasQualifier() {
    ensure_ext();
    return ext_->apiSchemasQualifier;
  }
const std::vector<PrimVariantSetData> &PrimMetadata::variantSets() const {
    static const std::vector<PrimVariantSetData> kEmpty;
    return ext_ ? ext_->variantSets : kEmpty;
  }
std::vector<PrimVariantSetData> &PrimMetadata::variantSets() {
    ensure_ext();
    return ext_->variantSets;
  }
const StringListOps &PrimMetadata::variantSetNameEdits() const {
    static const StringListOps kEmpty;
    return ext_ ? ext_->variantSetNameEdits : kEmpty;
  }
StringListOps &PrimMetadata::variantSetNameEdits() {
    ensure_ext();
    return ext_->variantSetNameEdits;
  }
const StringListOps &PrimMetadata::clipSetEdits() const {
    static const StringListOps kEmpty;
    return ext_ ? ext_->clipSetEdits : kEmpty;
  }
StringListOps &PrimMetadata::clipSetEdits() {
    ensure_ext();
    return ext_->clipSetEdits;
  }
bool PrimMetadata::primOrderAuthored() const {
    return ext_ && ext_->primOrderAuthored;
  }
void PrimMetadata::setPrimOrderAuthored(bool authored ) {
    ensure_ext();
    ext_->primOrderAuthored = authored;
  }
bool PrimMetadata::propertyOrderAuthored() const {
    return ext_ && ext_->propertyOrderAuthored;
  }
void PrimMetadata::setPropertyOrderAuthored(bool authored ) {
    ensure_ext();
    ext_->propertyOrderAuthored = authored;
  }
const std::vector<std::pair<std::string, std::string>> &PrimMetadata::variantSelections()
      const {
    static const std::vector<std::pair<std::string, std::string>> kEmpty;
    return ext_ ? ext_->variantSelections : kEmpty;
  }
std::vector<std::pair<std::string, std::string>> &PrimMetadata::variantSelections() {
    ensure_ext();
    return ext_->variantSelections;
  }
bool PrimMetadata::variantSelectionsAuthored() const {
    return ext_ && ext_->variantSelectionsAuthored;
  }
void PrimMetadata::setVariantSelectionsAuthored(bool authored ) {
    ensure_ext();
    ext_->variantSelectionsAuthored = authored;
  }
const std::vector<std::pair<std::string, std::string>> &PrimMetadata::relocates() const {
    static const std::vector<std::pair<std::string, std::string>> kEmpty;
    return ext_ ? ext_->relocates : kEmpty;
  }
std::vector<std::pair<std::string, std::string>> &PrimMetadata::relocates() {
    ensure_ext();
    return ext_->relocates;
  }
bool PrimMetadata::relocatesAuthored() const { return ext_ && ext_->relocatesAuthored; }
void PrimMetadata::setRelocatesAuthored(bool authored ) {
    ensure_ext();
    ext_->relocatesAuthored = authored;
  }
const Value &PrimMetadata::customData() const {
    static const Value kEmpty;
    return ext_ ? ext_->customData : kEmpty;
  }
Value &PrimMetadata::customData() {
    ensure_ext();
    return ext_->customData;
  }
bool PrimMetadata::customDataAuthored() const {
    return ext_ && ext_->customDataAuthored;
  }
void PrimMetadata::setCustomDataAuthored(bool authored ) {
    ensure_ext();
    ext_->customDataAuthored = authored;
  }
const Value &PrimMetadata::assetInfo() const {
    static const Value kEmpty;
    return ext_ ? ext_->assetInfo : kEmpty;
  }
Value &PrimMetadata::assetInfo() {
    ensure_ext();
    return ext_->assetInfo;
  }
bool PrimMetadata::assetInfoAuthored() const { return ext_ && ext_->assetInfoAuthored; }
void PrimMetadata::setAssetInfoAuthored(bool authored ) {
    ensure_ext();
    ext_->assetInfoAuthored = authored;
  }
const Value &PrimMetadata::sdrMetadata() const {
    static const Value kEmpty;
    return ext_ ? ext_->sdrMetadata : kEmpty;
  }
Value &PrimMetadata::sdrMetadata() {
    ensure_ext();
    return ext_->sdrMetadata;
  }
bool PrimMetadata::sdrMetadataAuthored() const {
    return ext_ && ext_->sdrMetadataAuthored;
  }
void PrimMetadata::setSdrMetadataAuthored(bool authored ) {
    ensure_ext();
    ext_->sdrMetadataAuthored = authored;
  }
const Value &PrimMetadata::clips() const {
    static const Value kEmpty;
    return ext_ ? ext_->clips : kEmpty;
  }
Value &PrimMetadata::clips() {
    ensure_ext();
    return ext_->clips;
  }
bool PrimMetadata::clipsAuthored() const { return ext_ && ext_->clipsAuthored; }
void PrimMetadata::setClipsAuthored(bool authored ) {
    ensure_ext();
    ext_->clipsAuthored = authored;
  }
const std::vector<PropNameId> &PrimMetadata::clipShadowedProps() const {
    static const std::vector<PropNameId> kEmpty;
    return ext_ ? ext_->clipShadowedProps : kEmpty;
  }
std::vector<PropNameId> &PrimMetadata::clipShadowedProps() {
    ensure_ext();
    return ext_->clipShadowedProps;
  }
const std::vector<std::string> &PrimMetadata::primOrder() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->primOrder : kEmpty;
  }
const std::vector<std::string> &PrimMetadata::displayGroupOrder() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->displayGroupOrder : kEmpty;
  }
std::vector<std::string> &PrimMetadata::editDisplayGroupOrder() {
    ensure_ext();
    return ext_->displayGroupOrder;
  }
bool PrimMetadata::displayGroupOrderAuthored() const {
    return ext_ && ext_->displayGroupOrderAuthored;
  }
void PrimMetadata::setDisplayGroupOrderAuthored(bool authored ) {
    ensure_ext();
    ext_->displayGroupOrderAuthored = authored;
  }
std::vector<std::string> &PrimMetadata::editPrimOrder() {
    ensure_ext();
    return ext_->primOrder;
  }
const std::vector<std::string> &PrimMetadata::propertyOrder() const {
    static const std::vector<std::string> kEmpty;
    return ext_ ? ext_->propertyOrder : kEmpty;
  }
std::vector<std::string> &PrimMetadata::editPropertyOrder() {
    ensure_ext();
    return ext_->propertyOrder;
  }
const std::vector<std::pair<std::string, std::string>> &PrimMetadata::unknownMeta() const {
    static const std::vector<std::pair<std::string, std::string>> kEmpty;
    return ext_ ? ext_->unknownMeta : kEmpty;
  }
const std::vector<DecodedExtensionField> &PrimMetadata::unknownFields() const {
    static const std::vector<DecodedExtensionField> kEmpty;
    return ext_ ? ext_->unknownFields : kEmpty;
  }
std::vector<DecodedExtensionField> &PrimMetadata::unknownFields() {
    ensure_ext();
    return ext_->unknownFields;
  }
std::vector<std::pair<std::string, std::string>> &PrimMetadata::unknownMeta() {
    ensure_ext();
    return ext_->unknownMeta;
  }

// ---------------------------------------------------------------------------
// Out-of-line accessor bodies moved out of prim-spec.hh.
// ---------------------------------------------------------------------------
const std::string& PrimSpec::name() const { return name_; }
void PrimSpec::set_name(const std::string& name) { name_ = name; }
TypeNameId PrimSpec::type_id() const { return type_id_; }
PrimSpecifier PrimSpec::specifier() const { return specifier_; }
void PrimSpec::set_specifier(PrimSpecifier spec) { specifier_ = spec; }
const Path& PrimSpec::path() const { return path_; }
void PrimSpec::set_path(const Path& path) { path_ = path; }
uint32_t PrimSpec::asset_anchor_id() const { return asset_anchor_id_; }
void PrimSpec::set_asset_anchor_id(uint32_t id) { asset_anchor_id_ = id; }
PropSlot* PrimSpec::property_mutable(PropNameId name_id) {
    return props_.find_mutable(name_id);
  }
const PropIndex& PrimSpec::properties() const { return props_; }
const std::unordered_map<uint32_t, ArrayEditData>& PrimSpec::array_edits() const {
    static const std::unordered_map<uint32_t, ArrayEditData> kEmpty;
    return cold_data_ ? cold_data_->array_edits : kEmpty;
  }
const std::unordered_map<std::string, ArcListOp>& PrimSpec::relationship_edits() const {
    static const std::unordered_map<std::string, ArcListOp> kEmpty;
    return cold_data_ ? cold_data_->rel_edits : kEmpty;
  }
const std::vector<uint32_t>& PrimSpec::child_indices() const { return child_indices_; }
std::vector<uint32_t>& PrimSpec::mutable_child_indices() { return child_indices_; }
void PrimSpec::clear_child_indices() { child_indices_.clear(); }
void PrimSpec::set_child_indices(std::vector<uint32_t>&& idx) {
    child_indices_ = std::move(idx);
  }
size_t PrimSpec::child_count() const { return child_indices_.size(); }
const PrimMetadata& PrimSpec::meta() const { return meta_; }
PrimMetadata& PrimSpec::meta() { return meta_; }
bool PropMeta::empty() const {
    return authored == 0 && unknownMeta.empty() && unknownFields.empty();
  }
bool TimeSampleBlock::empty() const { return samples_.empty(); }
size_t ValueBlockStorage::count() const { return values_.size(); }
bool TypeNameId::is_valid() const { return id != UINT16_MAX; }
bool ArcListOp::has_qualifiers() const {
    return authored && (!is_explicit || !added.empty() || !prepended.empty() ||
           !appended.empty() || !deleted.empty() || !ordered.empty());
  }
bool ArcListOp::has_authored_opinion() const {
    return authored;
  }
bool StringListOps::has_nonexplicit_items() const {
    return !added.empty() || !prepended.empty() || !appended.empty() ||
           !deleted.empty() || !ordered.empty();
  }

}  // namespace next
}  // namespace lightusd
