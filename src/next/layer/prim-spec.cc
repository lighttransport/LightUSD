// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PrimSpec Implementation

#include "prim-spec.hh"
#include "../prim/identifier.hh"
#include <algorithm>
#include <cstring>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#endif

namespace lightusd {
namespace next {

namespace {

std::string NormalizeAssetPathForRemap(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  while (path.rfind("./", 0) == 0) path = path.substr(2);
  while (!path.empty() && path[0] == '/') path = path.substr(1);
  return path;
}

const std::string* FindAssetPathRemap(
    const std::map<std::string, std::string>& remap,
    const std::string& path) {
  auto it = remap.find(path);
  if (it != remap.end()) return &it->second;
  const std::string norm = NormalizeAssetPathForRemap(path);
  it = remap.find(norm);
  if (it != remap.end()) return &it->second;
  return nullptr;
}

void CopyVariantDataShallow(const VariantData& src, VariantData* dst) {
  dst->name = src.name;
  dst->active = src.active;
  dst->hidden = src.hidden;
  dst->active_authored = src.active_authored;
  dst->hidden_authored = src.hidden_authored;
  dst->doc = src.doc;
  dst->kind = src.kind;
  dst->properties = src.properties;
  dst->relationships = src.relationships;
  dst->relationshipFlags = src.relationshipFlags;
  dst->references = src.references;
  dst->payloads = src.payloads;
  dst->inherits = src.inherits;
  dst->specializes = src.specializes;
  dst->variantSelections = src.variantSelections;
  dst->unknownMeta = src.unknownMeta;
  dst->unknownFields = src.unknownFields;
  dst->content = src.content;
}

void CopyVariantSetsIterative(const std::vector<VariantSetData>& src,
                              std::vector<VariantSetData>* dst) {
  struct CopyFrame {
    const std::vector<VariantSetData>* src;
    std::vector<VariantSetData>* dst;
  };

  dst->resize(src.size());
  std::vector<CopyFrame> stack;
  stack.push_back(CopyFrame{&src, dst});
  while (!stack.empty()) {
    const CopyFrame frame = stack.back();
    stack.pop_back();
    for (size_t si = 0; si < frame.src->size(); ++si) {
      const VariantSetData& src_set = (*frame.src)[si];
      VariantSetData& dst_set = (*frame.dst)[si];
      dst_set.name = src_set.name;
      dst_set.selected = src_set.selected;
      dst_set.variants.resize(src_set.variants.size());
      for (size_t vi = 0; vi < src_set.variants.size(); ++vi) {
        const VariantData& src_option = src_set.variants[vi];
        VariantData& dst_option = dst_set.variants[vi];
        CopyVariantDataShallow(src_option, &dst_option);
        if (!src_option.variantSets.empty()) {
          dst_option.variantSets.resize(src_option.variantSets.size());
          stack.push_back(
              CopyFrame{&src_option.variantSets, &dst_option.variantSets});
        }
      }
    }
  }
}

}  // namespace

VariantData::VariantData() = default;
VariantData::VariantData(const VariantData& other) {
  CopyVariantDataShallow(other, this);
  CopyVariantSetsIterative(other.variantSets, &variantSets);
}
VariantData::VariantData(VariantData&&) noexcept = default;
VariantData& VariantData::operator=(const VariantData& other) {
  if (this != &other) {
    VariantData copy(other);
    *this = std::move(copy);
  }
  return *this;
}
VariantData& VariantData::operator=(VariantData&&) noexcept = default;

VariantData::~VariantData() {
  // Variant sets recursively own more VariantData values. A default vector
  // teardown consumes one C++ stack frame per nested option and can overflow
  // for deeply authored API data even though writer traversal is iterative.
  // Move descendants into an explicit worklist and detach each level before
  // its ordinary destructor runs.
  std::vector<VariantSetData> pending;
  pending.swap(variantSets);
  while (!pending.empty()) {
    VariantSetData set = std::move(pending.back());
    pending.pop_back();
    for (VariantData& option : set.variants) {
      for (VariantSetData& child : option.variantSets) {
        pending.push_back(std::move(child));
      }
      option.variantSets.clear();
    }
  }
}

// ============================================================
// TypeNameTable
// ============================================================

namespace {
const char* TypeNameKey(const void* context, size_t index, size_t* length) {
  const auto& names = *static_cast<const std::deque<std::string>*>(context);
  if (index >= names.size()) return nullptr;
  *length = names[index].size();
  return names[index].data();
}
}  // namespace

TypeNameId TypeNameTable::find_live(const std::string& name) const {
  const size_t id = name_to_id_.find(name.data(), name.size(), TypeNameKey, &names_);
  if (id < UINT16_MAX) return TypeNameId{static_cast<uint16_t>(id)};
  if (name_to_id_.size() != names_.size()) {
    for (size_t i = 0; i < names_.size(); ++i) {
      if (names_[i] == name) return TypeNameId{static_cast<uint16_t>(i)};
    }
  }
  return TypeNameId{};
}

TypeNameId TypeNameTable::intern(const std::string& name) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  {
    std::shared_lock<std::shared_mutex> rlk(mu_);
    const TypeNameId hit = find_live(name);
    if (hit.is_valid()) return hit;
  }
  std::unique_lock<std::shared_mutex> wlk(mu_);
#endif
  const TypeNameId hit = find_live(name);
  if (hit.is_valid()) return hit;

  if (names_.size() >= UINT16_MAX) {
    return TypeNameId{};  // Table full
  }

  uint16_t id = static_cast<uint16_t>(names_.size());
  names_.push_back(name);
  if (name_to_id_.size() + 1 == names_.size())
    name_to_id_.insert(id, TypeNameKey, &names_);
  else
    name_to_id_.rebuild(names_.size(), TypeNameKey, &names_);
  return TypeNameId{id};
}

const std::string& TypeNameTable::get(TypeNameId id) const {
  static const std::string empty;
#if defined(LIGHTUSD_ENABLE_THREAD)
  // names_ is a deque: elements never relocate on push_back, so the returned
  // reference stays valid after the lock is released.
  std::shared_lock<std::shared_mutex> rlk(mu_);
#endif
  if (!id.is_valid() || id.id >= names_.size()) return empty;
  return names_[id.id];
}

TypeNameId TypeNameTable::find(const std::string& name) const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  std::shared_lock<std::shared_mutex> rlk(mu_);
#endif
  const TypeNameId hit = find_live(name);
  if (hit.is_valid()) return hit;
  return TypeNameId{};
}

void TypeNameTable::register_common_types() {
  // Empty string for typeless prims
  intern("");

  // Core types
  id_Xform = intern("Xform");
  id_Scope = intern("Scope");

  // Geometry
  id_Mesh = intern("Mesh");
  id_Points = intern("Points");
  id_BasisCurves = intern("BasisCurves");
  id_NurbsCurves = intern("NurbsCurves");
  id_Sphere = intern("Sphere");
  id_Cube = intern("Cube");
  id_Cylinder = intern("Cylinder");
  id_Cone = intern("Cone");
  id_Capsule = intern("Capsule");
  id_Camera = intern("Camera");
  id_PointInstancer = intern("PointInstancer");
  id_GeomSubset = intern("GeomSubset");

  // Materials
  id_Material = intern("Material");
  id_Shader = intern("Shader");

  // Skeleton
  id_SkelRoot = intern("SkelRoot");
  id_Skeleton = intern("Skeleton");
  id_SkelAnimation = intern("SkelAnimation");
  id_BlendShape = intern("BlendShape");

  // Lights
  intern("SphereLight");
  intern("DomeLight");
  intern("DiskLight");
  intern("RectLight");
  intern("CylinderLight");
  intern("DistantLight");
  intern("PortalLight");
  intern("PluginLight");

  // Volume
  intern("Volume");
  intern("OpenVDBAsset");

  // Render
  intern("RenderSettings");
  intern("RenderProduct");
  intern("RenderVar");

  // Other
  intern("Model");
  intern("Group");
  intern("Assembly");
  intern("Component");
}

// Registration happens inside the (thread-safe) static local initialization,
// so concurrent first callers cannot observe a half-registered table.
TypeNameTable& GetTypeNameTable() {
  static TypeNameTable& table = []() -> TypeNameTable& {
    static TypeNameTable t;
    t.register_common_types();
    return t;
  }();
  return table;
}

// ============================================================
// ValueStorage
// ============================================================

ValueStorage::ValueStorage() {
  // Grow on demand: eager per-prim reservations cost KBs per prim regardless
  // of property count, which is catastrophic on high-prim-count scenes.
}

ValueStorage::~ValueStorage() = default;

uint32_t ValueStorage::store(const Value& value) {
  uint32_t idx = static_cast<uint32_t>(values_.size());
  values_.push_back(value);
  return idx;
}

uint32_t ValueStorage::store(Value&& value) {
  uint32_t idx = static_cast<uint32_t>(values_.size());
  values_.push_back(std::move(value));
  return idx;
}

const Value* ValueStorage::get(uint32_t offset) const {
  if (offset >= values_.size()) return nullptr;
  return &values_[offset];
}

Value* ValueStorage::get(uint32_t offset) {
  if (offset >= values_.size()) return nullptr;
  return &values_[offset];
}

size_t ValueStorage::memory_usage() const {
  size_t size = values_.capacity() * sizeof(Value);
  for (const auto& v : values_) {
    if (v.is_array()) {
      size += v.array_size() * 4;  // Approximate (element payload)
    }
  }
  return size;
}

void ValueStorage::clear() {
  values_.clear();
}

// ============================================================
// TimeSampleStorage
// ============================================================

TimeSampleStorage::TimeSampleStorage() {
  // Grow on demand (was reserve(64) ~= 8.7 KB per prim with time samples).
}

TimeSampleStorage::~TimeSampleStorage() = default;

uint32_t TimeSampleStorage::find_or_store(Value value) {
  // Compute hash
  uint64_t h = value.hash();

  // Check for existing value with same hash
  auto it = hash_to_offsets_.find(h);
  if (it != hash_to_offsets_.end()) {
    // Check each offset for equality (handle hash collisions)
    for (uint32_t offset : it->second) {
      if (values_[offset] == value) {
        // Found duplicate - reuse existing offset
        ++dedup_count_;
        return offset;
      }
    }
  }

  // Store new value
  uint32_t offset = static_cast<uint32_t>(values_.size());
  values_.push_back(std::move(value));

  // Add to hash table
  hash_to_offsets_[h].push_back(offset);

  return offset;
}

void TimeSampleStorage::insert_sample(PropNameId name_id, double time,
                                      uint32_t offset) {
  auto& vec = samples_[name_id.id];
  if (vec.empty() || time > vec.back().first) {  // common: authored in order
    vec.emplace_back(time, offset);
    return;
  }
  auto it = std::lower_bound(
      vec.begin(), vec.end(), time,
      [](const std::pair<double, uint32_t>& a, double t) { return a.first < t; });
  if (it != vec.end() && it->first == time) {
    it->second = offset;  // re-authored time: last opinion wins
    return;
  }
  vec.insert(it, {time, offset});
}

uint32_t TimeSampleStorage::add(PropNameId name_id, double time, Value value) {
  // Store without deduplication (faster for unique values)
  uint32_t offset = static_cast<uint32_t>(values_.size());
  values_.push_back(std::move(value));
  insert_sample(name_id, time, offset);
  return offset;
}

uint32_t TimeSampleStorage::add_dedup(PropNameId name_id, double time, Value value) {
  // Lazy (crate-backed) arrays: skip content dedup. find_or_store() hashes the
  // value, and Value::hash() forces an in-place materialize — decoding every
  // array-valued time sample into the heap at parse time, which is the dominant
  // peak-RSS cost on animation-heavy scenes (e.g. Caldera). Store the lazy ref
  // as-is via the no-dedup path so it stays a cheap byte-range reference until
  // the writer decodes it transiently. (Scalar samples are never lazy, so they
  // keep deduping through the path below.)
  if (value.is_lazy()) {
    return add(name_id, time, std::move(value));
  }
  // Store with deduplication (good for repeated array values)
  uint32_t offset = find_or_store(std::move(value));
  insert_sample(name_id, time, offset);
  return offset;
}

const std::vector<std::pair<double, uint32_t>>* TimeSampleStorage::get(PropNameId name_id) const {
  auto it = samples_.find(name_id.id);
  if (it == samples_.end()) return nullptr;
  return &it->second;
}

const Value* TimeSampleStorage::value(uint32_t offset) const {
  if (offset >= values_.size()) return nullptr;
  return &values_[offset];
}

size_t TimeSampleStorage::remap_asset_paths(
    const std::map<std::string, std::string>& remap) {
  if (remap.empty()) return 0;
  size_t count = 0;
  for (Value& value : values_) {
    const std::string* path = value.as_asset_path();
    if (!path) continue;
    const std::string* mapped = FindAssetPathRemap(remap, *path);
    if (!mapped) continue;
    value = Value::MakeAssetPath(*mapped);
    ++count;
  }
  return count;
}

bool TimeSampleStorage::has(PropNameId name_id) const {
  return samples_.find(name_id.id) != samples_.end();
}

bool TimeSampleStorage::remove(PropNameId name_id) {
  return samples_.erase(name_id.id) > 0;
}

void TimeSampleStorage::remap_times(double offset, double scale) {
  if (offset == 0.0 && scale == 1.0) return;
  for (auto& kv : samples_) {
    // Degenerate offset (scale == 0): every time collapses onto `offset`.
    // Rewriting in place would leave N samples sharing one key -- a malformed,
    // no-longer-sorted-by-distinct-time vector that interpolate() reads as its
    // LAST entry. pxr freezes such a layer at its FIRST sample instead (see
    // CopyLocalOpinions), so keep only that one. Samples are stored ascending.
    if (scale == 0.0) {
      if (!kv.second.empty()) {
        kv.second.resize(1);
        kv.second[0].first = offset;
      }
      continue;
    }
    for (auto& tv : kv.second) {
      tv.first = offset + scale * tv.first;
    }
  }
}

SampleResult TimeSampleStorage::interpolate(PropNameId name_id, double time,
                                            TimeInterpolation mode) const {
  const auto* samples = get(name_id);
  if (!samples || samples->empty()) {
    return SampleResult{};
  }

  // Use the template method with a lambda to get values
  return TimeInterpolator::InterpolateWithOffsets(
      *samples,
      [this](uint32_t offset) -> const Value* { return value(offset); },
      time,
      mode);
}

std::vector<PropNameId> TimeSampleStorage::properties() const {
  std::vector<PropNameId> result;
  result.reserve(samples_.size());
  for (const auto& kv : samples_) {
    result.push_back(PropNameId{kv.first});
  }
  return result;
}

size_t TimeSampleStorage::memory_usage() const {
  size_t size = sizeof(*this);

  // Values
  size += values_.capacity() * sizeof(Value);
  for (const auto& v : values_) {
    // Add array storage size if applicable
    if (v.is_array()) {
      size += v.array_size() * 4;  // Approximate
    }
  }

  // Samples map
  for (const auto& kv : samples_) {
    size += kv.second.capacity() * sizeof(std::pair<double, uint32_t>);
  }

  // Hash table
  for (const auto& kv : hash_to_offsets_) {
    size += kv.second.capacity() * sizeof(uint32_t);
  }

  return size;
}

void TimeSampleStorage::clear() {
  samples_.clear();
  values_.clear();
  hash_to_offsets_.clear();
  dedup_count_ = 0;
}

TimeSampleStorage::Stats TimeSampleStorage::stats() const {
  Stats s = {};
  s.property_count = samples_.size();
  for (const auto& kv : samples_) {
    s.total_samples += kv.second.size();
  }
  s.unique_values = values_.size();
  s.dedup_count = dedup_count_;
  s.memory_bytes = memory_usage();
  return s;
}

// ============================================================
// PrimSpec
// ============================================================

// values_ / time_samples_ are allocated lazily on first use: most prims in
// composed scenes carry no local values or samples, and two eager heap blocks
// per prim dominated the per-prim fixed cost on high-prim-count layers.
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
    c.values_ = std::make_unique<ValueStorage>(*values_);
  }

  // Deep copy time samples
  if (time_samples_) {
    c.time_samples_ = std::make_unique<TimeSampleStorage>();
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

  // Deep copy relationships and the rare authored-field block.
  c.relationships_ = relationships_;
  if (cold_data_) c.cold_data_ = std::make_unique<ColdData>(*cold_data_);

  // Deep copy attribute connections + declared type names
  c.connections_ = connections_;
  c.prop_type_names_ = prop_type_names_;

  // Deep copy per-property metadata (unique_ptr side table).
  for (const auto& kv : prop_metas_) {
    if (kv.second) c.prop_metas_[kv.first] = std::make_unique<PropMeta>(*kv.second);
  }

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
  if (!values_) values_ = std::make_unique<ValueStorage>();
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

  props_.add(slot);
}

void PrimSpec::add_property_slot(PropNameId name_id, TypeId type_id, uint16_t flags) {
  PropSlot slot;
  slot.name_id = name_id;
  slot.value_offset = UINT32_MAX;  // No default value
  slot.value_type = static_cast<uint16_t>(type_id);
  slot.flags = flags;
  props_.add(slot);
}

bool PrimSpec::fill_property_value_if_absent(PropNameId name_id, Value value) {
  PropSlot* slot = props_.find_mutable(name_id);
  if (!slot) return false;
  if (slot->value_offset != UINT32_MAX) return false;  // already has a value
  if (!values_) values_ = std::make_unique<ValueStorage>();
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
  // stranded in values_ forever (nothing compacts ValueStorage), so every
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
    if (!values_) values_ = std::make_unique<ValueStorage>();
    slot->value_offset = values_->store(std::move(value));
  } else if (Value* dst = values_ ? values_->get(slot->value_offset) : nullptr) {
    // Replace in place — no ValueStorage growth on re-authoring.
    *dst = std::move(value);
  } else {
    if (!values_) values_ = std::make_unique<ValueStorage>();
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
  if (connections_.erase(name_id.id) > 0) removed = true;
  if (cold_data_) {
    cold_data_->connection_edits.erase(name_id.id);
    cold_data_->connection_opinion_stacks.erase(name_id.id);
  }
  prop_type_names_.erase(name_id.id);
  prop_metas_.erase(name_id.id);
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
    const std::string* path = value->as_asset_path();
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

void PrimSpec::mark_property_time_sampled(PropNameId name_id) {
  if (PropSlot* slot = props_.find_mutable(name_id)) {
    slot->flags |= PropSlot::kFlagTimeSampled;
  }
}

void PrimSpec::add_time_sample(PropNameId name_id, double time, Value value) {
  // Use deduplicated storage for array values (common case for animation)
  if (!time_samples_) time_samples_ = std::make_unique<TimeSampleStorage>();
  time_samples_->add_dedup(name_id, time, std::move(value));
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

TimeSampleStorage::Stats PrimSpec::time_sample_stats() const {
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
  ensure_cold_data().spline_sources[GetPropNameTable().intern(prop_name).id] =
      std::move(source);
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
  ensure_cold_data().raw_default_sources[
      GetPropNameTable().intern(prop_name).id] =
      std::move(source);
}

void PrimSpec::clear_raw_default_source(PropNameId name_id) {
  if (cold_data_) cold_data_->raw_default_sources.erase(name_id.id);
}

void PrimSpec::set_array_edit(const std::string& prop_name,
                              ArrayEditData edit) {
  ensure_cold_data().array_edits[GetPropNameTable().intern(prop_name).id] =
      std::move(edit);
}

const ArrayEditData* PrimSpec::array_edit(PropNameId name_id) const {
  if (!cold_data_) return nullptr;
  auto it = cold_data_->array_edits.find(name_id.id);
  return it == cold_data_->array_edits.end() ? nullptr : &it->second;
}

void PrimSpec::clear_array_edit(PropNameId name_id) {
  if (cold_data_) cold_data_->array_edits.erase(name_id.id);
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
  relationships_[name].push_back(target);
}

ArcEdit& PrimSpec::ensure_relationship_edit(const std::string& name) {
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
  std::vector<Path>& dst = relationships_[name];

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
      if (dst.empty()) relationships_.erase(name);
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
  relationships_[name] = std::move(targets);
}

bool PrimSpec::remove_relationship(const std::string& name) {
  bool removed = relationships_.erase(name) > 0;
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
  auto it = relationships_.find(name);
  if (it == relationships_.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> PrimSpec::relationship_names() const {
  std::vector<std::string> names;
  names.reserve(relationships_.size());
  for (const auto& [name, _] : relationships_) {
    names.push_back(name);
  }
  // relationships_ is an unordered_map; sort for a deterministic, stable order
  // across runs/platforms (callers and tests rely on consistent ordering).
  std::sort(names.begin(), names.end());
  return names;
}

void PrimSpec::add_connection(const std::string& prop_name, const Path& target) {
  // Authoring boundary (see add_property): keep connection property names
  // valid identifiers.
  if (!IsValidNamespacedIdentifier(prop_name)) return;
  const PropNameId id = GetPropNameTable().intern(prop_name);
  connections_[id.id].push_back(target);
  // Mark the slot as carrying a connection so serializers that gate on the
  // flag (the USDC writer) emit it even when the property ALSO has a default
  // value — otherwise `attr = v` + `attr.connect = <t>` loses the connection
  // through a crate round trip.
  if (PropSlot* s = property_mutable(id)) s->flags |= PropSlot::kFlagConnection;
}

void PrimSpec::set_connection_block(const std::string& prop_name) {
  const PropNameId id = GetPropNameTable().intern(prop_name);
  connections_[id.id];  // empty vector (authored `.connect = None`)
  if (PropSlot* s = property_mutable(id)) s->flags |= PropSlot::kFlagConnection;
}

const ArcEdit* PrimSpec::connection_edit(const std::string& prop_name) const {
  if (!cold_data_) return nullptr;
  const PropNameId id = GetPropNameTable().find(prop_name);
  if (!id.is_valid()) return nullptr;
  auto it = cold_data_->connection_edits.find(id.id);
  return it == cold_data_->connection_edits.end() ? nullptr : &it->second;
}

ArcEdit& PrimSpec::ensure_connection_edit(const std::string& prop_name) {
  return ensure_cold_data()
      .connection_edits[GetPropNameTable().intern(prop_name).id];
}

const std::vector<PrimSpec::RelationshipOpinion>*
PrimSpec::connection_opinion_stack(const std::string& prop_name) const {
  const PropNameId id = GetPropNameTable().find(prop_name);
  if (!id.is_valid() || !cold_data_) return nullptr;
  auto it = cold_data_->connection_opinion_stacks.find(id.id);
  return it == cold_data_->connection_opinion_stacks.end() ? nullptr
                                                           : &it->second;
}

void PrimSpec::set_connection_opinion_stack(
    const std::string& prop_name, std::vector<RelationshipOpinion> opinions) {
  ensure_cold_data().connection_opinion_stacks[
      GetPropNameTable().intern(prop_name).id] = std::move(opinions);
}

void PrimSpec::set_connection_targets(const std::string& prop_name,
                                      std::vector<Path> targets) {
  const PropNameId id = GetPropNameTable().intern(prop_name);
  connections_[id.id] = std::move(targets);
  if (PropSlot* slot = property_mutable(id)) {
    slot->flags |= PropSlot::kFlagConnection;
  }
}

void PrimSpec::apply_connection_list_op(const std::string& prop_name,
                                        const std::vector<Path>& targets,
                                        RelationshipListOp op) {
  const PropNameId id = GetPropNameTable().intern(prop_name);
  std::vector<Path>& dst = connections_[id.id];
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
  PropNameId id = GetPropNameTable().find(prop_name);
  if (!id.is_valid()) return nullptr;
  auto it = connections_.find(id.id);
  if (it == connections_.end()) return nullptr;
  return &it->second;
}

void PrimSpec::remap_target_prefix(const std::string& old_prefix,
                                   const std::string& new_prefix) {
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
  auto remap_edit = [&](ArcEdit& edit) {
    remap_strings(edit.added);
    remap_strings(edit.prepended);
    remap_strings(edit.appended);
    remap_strings(edit.deleted);
    remap_strings(edit.ordered);
  };
  for (auto& kv : relationships_) remap(kv.second);
  for (auto& kv : connections_) remap(kv.second);
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

void PrimSpec::set_property_type_name(const std::string& prop_name,
                                      const std::string& type_name) {
  // Intern both name and typeName; typeNames are few and highly shared.
  prop_type_names_[GetPropNameTable().intern(prop_name).id] = type_name;
}

const std::string* PrimSpec::property_type_name(PropNameId name_id) const {
  if (!name_id.is_valid()) return nullptr;
  auto it = prop_type_names_.find(name_id.id);
  if (it == prop_type_names_.end()) return nullptr;
  return &it->second;
}

const std::string* PrimSpec::property_type_name(const std::string& prop_name) const {
  return property_type_name(GetPropNameTable().find(prop_name));
}

const PropMeta* PrimSpec::property_meta(PropNameId name_id) const {
  if (!name_id.is_valid()) return nullptr;
  auto it = prop_metas_.find(name_id.id);
  if (it == prop_metas_.end()) return nullptr;
  return it->second.get();
}

const PropMeta* PrimSpec::property_meta(const std::string& prop_name) const {
  return property_meta(GetPropNameTable().find(prop_name));
}

PropMeta& PrimSpec::ensure_property_meta(PropNameId name_id) {
  std::unique_ptr<PropMeta>& slot = prop_metas_[name_id.id];
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
  size += name_.capacity();
  size += path_.str().capacity();

  // Properties
  size += props_.slots().capacity() * sizeof(PropSlot);
  if (values_) {
    size += values_->memory_usage();
  }

  // Time samples (use TimeSampleStorage's memory tracking)
  if (time_samples_) {
    size += time_samples_->memory_usage();
  }
  if (cold_data_) size += sizeof(ColdData);

  // Relationships
  for (const auto& rel : relationships_) {
    size += rel.first.capacity();
    size += rel.second.capacity() * sizeof(Path);
  }

  // Children
  size += child_indices_.capacity() * sizeof(uint32_t);

  // Metadata (inline hot fields)
  for (const auto& s : meta_.references) size += s.capacity();
  for (const auto& s : meta_.payloads) size += s.capacity();
  for (const auto& s : meta_.inherits) size += s.capacity();
  for (const auto& s : meta_.specializes) size += s.capacity();
  size += meta_.variantSelection.capacity();
  // Cold fields only cost anything when the ext was allocated.
  if (const PrimSpecMetaExt* ext = meta_.ext()) {
    size += sizeof(PrimSpecMetaExt);
    size += ext->doc.capacity();
    size += ext->comment.capacity();
    size += ext->instance_prototype.capacity();
    for (const auto& s : ext->apiSchemas) size += s.capacity();
  }

  return size;
}

}  // namespace next
}  // namespace lightusd
