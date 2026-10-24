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

PrimMetadataCold::PrimMetadataCold(const PrimMetadataCold &o)
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
        arc_edits(o.arc_edits ? new ArcListOps(*o.arc_edits) : nullptr) {}

PrimMetadataCold::PrimMetadataCold() = default;

PrimMetadataCold::~PrimMetadataCold() = default;

PrimMetadata::PrimMetadata() = default;

PrimMetadata::~PrimMetadata() = default;

PrimMetadata::PrimMetadata(PrimMetadata&&) noexcept = default;

PrimMetadata& PrimMetadata::operator=(PrimMetadata&&) noexcept = default;

PrimMetadata::PrimMetadata(const PrimMetadata& other) { *this = other; }

PrimMetadata &PrimMetadata::operator=(const PrimMetadata &o) {
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
    ext_ = o.ext_ ? std::unique_ptr<PrimMetadataCold>(
                        new PrimMetadataCold(*o.ext_))
                  : nullptr;
    return *this;
  }

void MergeWeakerDictionaryValue(
    Value* stronger, const Value& weaker,
    std::vector<std::string>* conflicts,
    const std::string& key_prefix) {
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
    lu_value* destination_value;
    ValueView source_value;
    size_t next_entry;
    std::string prefix;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{stronger->mutable_core(), weaker.view(), 0, key_prefix});
  while (!stack.empty()) {
    Frame& frame = stack.back();
    auto destination = MutableDictView(frame.destination_value);
    auto source = frame.source_value->as_dictionary();
    if (!destination || !source || frame.next_entry >= source->entries().size()) {
      stack.pop_back();
      continue;
    }

    const auto& entry = source->entries()[frame.next_entry++];
    auto existing = destination->find(entry.first);
    if (!existing) {
      destination->set(entry.first, entry.second);
    } else if (existing->is_dictionary() && entry.second.is_dictionary()) {
      lu_value* mutable_child = lu_value_dict_mutable_value(
          frame.destination_value, entry.first.data(), entry.first.size());
      if (mutable_child) stack.push_back(
          Frame{mutable_child, entry.second, 0, frame.prefix + entry.first + "."});
    } else if (conflicts &&
               existing->is_dictionary() != entry.second.is_dictionary()) {
      conflicts->push_back(frame.prefix + entry.first);
    }
  }
}

void MergeWeakerExtensionFields(
    std::vector<DecodedExtensionField>* stronger,
    const std::vector<DecodedExtensionField>& weaker) {
  if (!stronger) return;
  for (const DecodedExtensionField& field : weaker) {
    auto existing = std::find_if(
        stronger->begin(), stronger->end(),
        [&](const DecodedExtensionField& own) { return own.name == field.name; });
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

void MergeWeakerRawFields(
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

void ApplyStringListOrder(const std::vector<std::string> &order,
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

std::vector<std::string> ApplyStringListOp(
    const StringListOps &edits,
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

void CopyVariantDataShallow(const PrimVariantData& src, PrimVariantData* dst) {
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

void CopyVariantSetsIterative(const std::vector<PrimVariantSetData>& src,
                              std::vector<PrimVariantSetData>* dst) {
  struct CopyFrame {
    const std::vector<PrimVariantSetData>* src;
    std::vector<PrimVariantSetData>* dst;
  };

  dst->resize(src.size());
  std::vector<CopyFrame> stack;
  stack.push_back(CopyFrame{&src, dst});
  while (!stack.empty()) {
    const CopyFrame frame = stack.back();
    stack.pop_back();
    for (size_t si = 0; si < frame.src->size(); ++si) {
      const PrimVariantSetData& src_set = (*frame.src)[si];
      PrimVariantSetData& dst_set = (*frame.dst)[si];
      dst_set.name = src_set.name;
      dst_set.selected = src_set.selected;
      dst_set.variants.resize(src_set.variants.size());
      for (size_t vi = 0; vi < src_set.variants.size(); ++vi) {
        const PrimVariantData& src_option = src_set.variants[vi];
        PrimVariantData& dst_option = dst_set.variants[vi];
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


PrimVariantData::PrimVariantData() = default;
PrimVariantData::PrimVariantData(const PrimVariantData& other) {
  CopyVariantDataShallow(other, this);
  CopyVariantSetsIterative(other.variantSets, &variantSets);
}
PrimVariantData::PrimVariantData(PrimVariantData&&) noexcept = default;
PrimVariantData& PrimVariantData::operator=(const PrimVariantData& other) {
  if (this != &other) {
    PrimVariantData copy(other);
    *this = std::move(copy);
  }
  return *this;
}
PrimVariantData& PrimVariantData::operator=(PrimVariantData&&) noexcept = default;

PrimVariantData::~PrimVariantData() {
  // Variant sets recursively own more PrimVariantData values. A default vector
  // teardown consumes one C++ stack frame per nested option and can overflow
  // for deeply authored API data even though writer traversal is iterative.
  // Move descendants into an explicit worklist and detach each level before
  // its ordinary destructor runs.
  std::vector<PrimVariantSetData> pending;
  pending.swap(variantSets);
  while (!pending.empty()) {
    PrimVariantSetData set = std::move(pending.back());
    pending.pop_back();
    for (PrimVariantData& option : set.variants) {
      for (PrimVariantSetData& child : option.variantSets) {
        pending.push_back(std::move(child));
      }
      option.variantSets.clear();
    }
  }
}

// ============================================================
// TypeNameTable
// ============================================================

TypeNameId TypeNameTable::intern(const std::string& name) {
  return intern_locked(name);
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
  intern("Field3DAsset");

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
// ValueBlockStorage
// ============================================================

ValueBlockStorage::ValueBlockStorage() {
  // Grow on demand: eager per-prim reservations cost KBs per prim regardless
  // of property count, which is catastrophic on high-prim-count scenes.
}

ValueBlockStorage::~ValueBlockStorage() = default;

uint32_t ValueBlockStorage::store(const Value& value) {
  uint32_t idx = static_cast<uint32_t>(values_.size());
  values_.push_back(value);
  return idx;
}

uint32_t ValueBlockStorage::store(Value&& value) {
  uint32_t idx = static_cast<uint32_t>(values_.size());
  values_.push_back(std::move(value));
  return idx;
}

const Value* ValueBlockStorage::get(uint32_t offset) const {
  if (offset >= values_.size()) return nullptr;
  return &values_[offset];
}

Value* ValueBlockStorage::get(uint32_t offset) {
  if (offset >= values_.size()) return nullptr;
  return &values_[offset];
}

size_t ValueBlockStorage::memory_usage() const {
  size_t size = safe::saturating_mul(values_.capacity(), sizeof(Value));
  for (const auto& v : values_) {
    size = safe::saturating_add(size, v.heap_memory_usage());
  }
  return size;
}

void ValueBlockStorage::clear() {
  values_.clear();
}

void ValueBlockStorage::release_payloads() {
  for (auto& v : values_) {
    // Keep AssetPath values: post-write asset collection reads them.
    if (v.type_id() == TypeId::AssetPath) continue;
    v = Value();
  }
}

// ============================================================
// TimeSampleBlock
// ============================================================

TimeSampleBlock::TimeSampleBlock() {
  // Grow on demand (was reserve(64) ~= 8.7 KB per prim with time samples).
}

TimeSampleBlock::~TimeSampleBlock() = default;

uint32_t TimeSampleBlock::find_or_store(Value value) {
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

void TimeSampleBlock::insert_sample(PropNameId name_id, double time,
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

uint32_t TimeSampleBlock::add(PropNameId name_id, double time, Value value) {
  // Store without deduplication (faster for unique values)
  uint32_t offset = static_cast<uint32_t>(values_.size());
  values_.push_back(std::move(value));
  insert_sample(name_id, time, offset);
  return offset;
}

uint32_t TimeSampleBlock::add_dedup(PropNameId name_id, double time, Value value) {
  // Lazy (crate-backed) arrays: skip content dedup. find_or_store() hashes the
  // value, and Value::hash() forces an in-place materialize — decoding every
  // array-valued time sample into the heap at parse time, which is the dominant
  // peak-RSS cost on animation-heavy scenes (e.g. Scene C). Store the lazy ref
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

const std::vector<std::pair<double, uint32_t>>* TimeSampleBlock::get(PropNameId name_id) const {
  auto it = samples_.find(name_id.id);
  if (it == samples_.end()) return nullptr;
  return &it->second;
}

const Value* TimeSampleBlock::value(uint32_t offset) const {
  if (offset >= values_.size()) return nullptr;
  return &values_[offset];
}

size_t TimeSampleBlock::remap_asset_paths(
    const std::map<std::string, std::string>& remap) {
  if (remap.empty()) return 0;
  size_t count = 0;
  for (Value& value : values_) {
    auto path = value.as_asset_path();
    if (!path) continue;
    const std::string* mapped = FindAssetPathRemap(remap, *path);
    if (!mapped) continue;
    value = Value::MakeAssetPath(*mapped);
    ++count;
  }
  return count;
}

bool TimeSampleBlock::has(PropNameId name_id) const {
  return samples_.find(name_id.id) != samples_.end();
}

bool TimeSampleBlock::remove(PropNameId name_id) {
  return samples_.erase(name_id.id) > 0;
}

void TimeSampleBlock::remap_times(double offset, double scale) {
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

SampleResult TimeSampleBlock::interpolate(PropNameId name_id, double time,
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

std::vector<PropNameId> TimeSampleBlock::properties() const {
  std::vector<PropNameId> result;
  result.reserve(samples_.size());
  for (const auto& kv : samples_) {
    result.push_back(PropNameId{kv.first});
  }
  return result;
}

size_t TimeSampleBlock::memory_usage() const {
  size_t size = sizeof(*this);
  auto add = [&size](size_t bytes) {
    size = safe::saturating_add(size, bytes);
  };

  // Values
  add(safe::saturating_mul(values_.capacity(), sizeof(Value)));
  for (const auto& v : values_) {
    add(v.heap_memory_usage());
  }

  // Samples map
  for (const auto& kv : samples_) {
    add(safe::saturating_mul(kv.second.capacity(),
                             sizeof(std::pair<double, uint32_t>)));
  }

  // Hash table
  for (const auto& kv : hash_to_offsets_) {
    add(safe::saturating_mul(kv.second.capacity(), sizeof(uint32_t)));
  }

  return size;
}

void TimeSampleBlock::clear() {
  samples_.clear();
  values_.clear();
  hash_to_offsets_.clear();
  dedup_count_ = 0;
}

void TimeSampleBlock::release_payloads() {
  for (auto& v : values_) v = Value();
  // The dedup index only accelerates future add()s; a released store is
  // write-and-discard, so free it too.
  hash_to_offsets_.clear();
}

TimeSampleBlock::Stats TimeSampleBlock::stats() const {
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


}  // namespace next
}  // namespace lightusd
