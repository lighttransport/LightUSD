// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PrimSpec Implementation

#include "prim-spec.hh"
#include "layer.hh"
#include "int64-listop.hh"
#include "prim/identifier.hh"
#include "core/safe-arithmetic.hh"
#include <algorithm>
#include <cstring>
#include <list>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#endif

namespace lightusd {
namespace next {

PrimMetadataRare::PrimMetadataRare(const PrimMetadataRare &o)
      : doc(o.doc),
        comment(o.comment),
        doc_authored(o.doc_authored),
        comment_authored(o.comment_authored),
        displayName(o.displayName),
        display_name_authored(o.display_name_authored),
        permission(o.permission),
        displayGroupOrder(o.displayGroupOrder),
        displayGroupOrderAuthored(o.displayGroupOrderAuthored),
        variantSets(o.variantSets),
        variantSetNameEdits(o.variantSetNameEdits),
        variantSelections(o.variantSelections),
        variantSelectionsAuthored(o.variantSelectionsAuthored),
        relocates(o.relocates),
        relocatesAuthored(o.relocatesAuthored),
        assetInfo(o.assetInfo),
        sdrMetadata(o.sdrMetadata),
        clips(o.clips),
        clipSetEdits(o.clipSetEdits),
        clipShadowedProps(o.clipShadowedProps),
        assetInfoAuthored(o.assetInfoAuthored),
        sdrMetadataAuthored(o.sdrMetadataAuthored),
        clipsAuthored(o.clipsAuthored),
        primOrder(o.primOrder),
        propertyOrder(o.propertyOrder),
        primOrderAuthored(o.primOrderAuthored),
        propertyOrderAuthored(o.propertyOrderAuthored),
        unknownMeta(o.unknownMeta),
        unknownFields(o.unknownFields) {}

PrimMetadataRare::PrimMetadataRare() = default;

PrimMetadataRare::~PrimMetadataRare() = default;

PrimMetadataCold::PrimMetadataCold(const PrimMetadataCold &o)
      : kind(o.kind),
        customData(o.customData),
        instance_prototype(o.instance_prototype),
        apiSchemas(o.apiSchemas),
        kind_authored(o.kind_authored),
        apiSchemasAuthored(o.apiSchemasAuthored),
        customDataAuthored(o.customDataAuthored),
        arc_edits(o.arc_edits ? new ArcListOps(*o.arc_edits) : nullptr),
        api_schema_edits(o.api_schema_edits
                             ? new ApiSchemaListOp(*o.api_schema_edits)
                             : nullptr),
        rare(o.rare ? new PrimMetadataRare(*o.rare) : nullptr) {}

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

namespace {
std::vector<int64_t> SortedIds(std::vector<int64_t> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

// Deterministic O(n log n), avoiding attacker-selected integer hash collisions.
// Most authored explicit ID lists are sorted and use the allocation-free path.
void UniqueIds(std::vector<int64_t>* ids, const std::vector<int64_t>& last) {
  if (std::is_sorted(ids->begin(), ids->end())) {
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
    return;
  }
  std::vector<size_t> rank(ids->size());
  for (size_t i = 0; i < rank.size(); ++i) rank[i] = i;
  std::sort(rank.begin(), rank.end(), [&](size_t a, size_t b) {
    return (*ids)[a] < (*ids)[b] || ((*ids)[a] == (*ids)[b] && a < b);
  });
  std::vector<uint8_t> keep(ids->size(), 0);
  for (size_t i = 0; i < rank.size();) {
    size_t end = i + 1;
    while (end < rank.size() && (*ids)[rank[end]] == (*ids)[rank[i]]) ++end;
    keep[std::binary_search(last.begin(), last.end(), (*ids)[rank[i]])
             ? rank[end - 1] : rank[i]] = 1;
    i = end;
  }
  size_t count = 0;
  for (size_t i = 0; i < ids->size(); ++i)
    if (keep[i]) (*ids)[count++] = (*ids)[i];
  ids->resize(count);
}

std::vector<int64_t> ApplyInt64Op(const Int64ListOp& op,
                                 const std::vector<int64_t>& base) {
  auto run = [&](size_t i) {
    auto a = op.items[i].as_int64_array();
    std::vector<int64_t> values;
    if (a && !a->empty()) {
      values.resize(a->size());
      std::copy_n(a->data(), values.size(), values.data());
    }
    return values;
  };
  if (op.is_explicit()) {
    auto result = run(0);
    UniqueIds(&result, {});
    return result;
  }
  const auto deleted = SortedIds(run(4)), appended = run(3);
  const auto move_back = SortedIds(appended);
  const auto is_appended = [&](int64_t id) {
    return std::binary_search(move_back.begin(), move_back.end(), id);
  };
  std::vector<int64_t> result;
  for (int64_t id : run(2)) if (!is_appended(id)) result.push_back(id);
  for (int64_t id : base)
    if (!std::binary_search(deleted.begin(), deleted.end(), id) && !is_appended(id))
      result.push_back(id);
  for (int64_t id : run(1)) if (!is_appended(id)) result.push_back(id);
  result.insert(result.end(), appended.begin(), appended.end());
  UniqueIds(&result, move_back);

  const auto order = run(5);
  if (order.empty()) return result;
  const auto ordered = SortedIds(order);
  struct Block { int64_t id; size_t begin, end; };
  std::vector<Block> blocks;
  size_t prefix = result.size();
  for (size_t i = 0; i < result.size(); ++i) {
    if (!std::binary_search(ordered.begin(), ordered.end(), result[i])) continue;
    if (!blocks.empty()) blocks.back().end = i;
    else prefix = i;
    blocks.push_back({result[i], i, result.size()});
  }
  std::sort(blocks.begin(), blocks.end(),
            [](const Block& a, const Block& b) { return a.id < b.id; });
  std::vector<uint8_t> emitted(blocks.size(), 0);
  std::vector<int64_t> reordered(result.begin(), result.begin() + static_cast<ptrdiff_t>(prefix));
  for (int64_t id : order) {
    auto block = std::lower_bound(blocks.begin(), blocks.end(), id,
        [](const Block& b, int64_t value) { return b.id < value; });
    if (block == blocks.end() || block->id != id) continue;
    const size_t index = static_cast<size_t>(block - blocks.begin());
    if (emitted[index]) continue;
    emitted[index] = 1;
    reordered.insert(reordered.end(), result.begin() + static_cast<ptrdiff_t>(block->begin),
                     result.begin() + static_cast<ptrdiff_t>(block->end));
  }
  return reordered;
}
}  // namespace

size_t Int64ListOpsMemoryUsage(const Int64ListOpStack& stack) {
  size_t bytes = safe::saturating_add(sizeof(Int64ListOpStack),
      safe::saturating_mul(stack.ops.capacity(), sizeof(Int64ListOp)));
  for (const auto& op : stack.ops)
    for (const auto& items : op.items)
      bytes = safe::saturating_add(bytes, items.heap_memory_usage());
  return bytes;
}

Value ApplyInt64ListOps(const Int64ListOpStack& stack) {
  std::vector<int64_t> result;
  for (auto op = stack.ops.rbegin(); op != stack.ops.rend(); ++op)
    result = ApplyInt64Op(*op, result);
  return Value::MakeInt64Array(result);
}

DecodedExtensionField MakeInt64ListOpField(std::string name, Int64ListOp op) {
  for (size_t r = 0; r < op.items.size(); ++r)
    if ((op.bits & kInt64ListOpBits[r]) && op.items[r].is_empty())
      op.items[r] = Value::MakeInt64Array({});
  DecodedExtensionField field;
  field.name = std::move(name);
  auto stack = std::make_shared<Int64ListOpStack>();
  stack->ops.push_back(std::move(op));
  field.value = ApplyInt64ListOps(*stack);
  field.int64_list_op = std::move(stack);
  return field;
}

void ResolveInt64ListOpFields(std::vector<DecodedExtensionField>* fields) {
  for (auto& field : *fields) {
    if (!field.int64_list_op) continue;
    Int64ListOp op;
    op.bits = 3;
    op.items[0] = field.value;
    auto resolved = std::make_shared<Int64ListOpStack>();
    resolved->ops.push_back(std::move(op));
    field.int64_list_op = std::move(resolved);
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
    } else if (existing->int64_list_op && field.int64_list_op) {
      if (std::any_of(existing->int64_list_op->ops.begin(),
                      existing->int64_list_op->ops.end(),
                      [](const Int64ListOp& op) { return op.is_explicit(); }))
        continue;
      auto stack = std::make_shared<Int64ListOpStack>(*existing->int64_list_op);
      stack->ops.insert(stack->ops.end(), field.int64_list_op->ops.begin(),
                        field.int64_list_op->ops.end());
      existing->value = ApplyInt64ListOps(*stack);
      existing->int64_list_op = std::move(stack);
    } else if (existing->value.is_dictionary() && field.value.is_dictionary()) {
      MergeWeakerDictionaryValue(&existing->value, field.value);
      // The authored source represented only the stronger dictionary. Force
      // writers to regenerate the merged UnregisteredValue source.
      if (existing->unregistered) existing->unregistered_source.clear();
    }
  }
}

void StoreRawMetadataOpinion(
    std::vector<std::pair<std::string, std::string>>* fields,
    std::string name, std::string value) {
  if (!fields) return;
  const size_t separator = name.find(' ');
  const bool explicit_value = separator == std::string::npos;
  const std::string key = explicit_value ? name : name.substr(separator + 1);
  if (explicit_value) {
    fields->erase(std::remove_if(fields->begin(), fields->end(), [&](const auto& field) {
      const size_t split = field.first.find(' ');
      return field.first == key ||
          (split != std::string::npos && field.first.substr(split + 1) == key);
    }), fields->end());
  } else {
    if (std::any_of(fields->begin(), fields->end(),
        [&](const auto& field) { return field.first == key; })) return;
  }
  const auto existing = std::find_if(fields->begin(), fields->end(),
      [&](const auto& field) { return field.first == name; });
  if (existing == fields->end()) fields->emplace_back(std::move(name), std::move(value));
  else existing->second = std::move(value);
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
  dst->authoredPropertyNames = src.authoredPropertyNames;
  dst->relationships = src.relationships;
  dst->relationshipFlags = src.relationshipFlags;
  dst->references = src.references;
  dst->payloads = src.payloads;
  dst->inherits = src.inherits;
  dst->specializes = src.specializes;
  dst->variantSelections = src.variantSelections;
  dst->variantSetNameEdits = src.variantSetNameEdits;
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


std::vector<std::string> PrimVariantData::property_names() const {
  std::vector<std::string> names = authoredPropertyNames;
  auto add = [&](const std::string& name) {
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  };
  for (const auto& property : properties) add(property.name);
  std::vector<std::string> rels;
  for (const auto& rel : relationships) rels.push_back(rel.first);
  std::sort(rels.begin(), rels.end());
  for (const auto& rel : rels) add(rel);
  if (content) {
    if (const PrimSpec* self = content->prim_at_path("/__self__"))
      for (const auto& name : self->authored_property_names()) add(name);
  }
  return names;
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

#if defined(LIGHTUSD_ENABLE_THREAD)
TypeNameTable::TypeNameTable()
    : by_id_(new std::atomic<const std::string*>[UINT16_MAX]),
      slots_(new std::atomic<uint32_t>[kSlots]) {
  for (size_t i = 0; i < UINT16_MAX; ++i) by_id_[i].store(nullptr, std::memory_order_relaxed);
  for (size_t i = 0; i < kSlots; ++i) slots_[i].store(0, std::memory_order_relaxed);
}

// Runs under the exclusive lock right after names_.push_back.
void TypeNameTable::Publish(void* self) {
  TypeNameTable& t = *static_cast<TypeNameTable*>(self);
  const size_t id = t.names_.size() - 1;
  const std::string* name = &t.names_[id];
  t.by_id_[id].store(name, std::memory_order_release);
  size_t i = std::hash<std::string>{}(*name) & (kSlots - 1);
  while (t.slots_[i].load(std::memory_order_relaxed) != 0) i = (i + 1) & (kSlots - 1);
  t.slots_[i].store(static_cast<uint32_t>(id + 1), std::memory_order_release);
}

TypeNameId TypeNameTable::find_published(const std::string& name) const {
  for (size_t i = std::hash<std::string>{}(name) & (kSlots - 1);;
       i = (i + 1) & (kSlots - 1)) {
    const uint32_t v = slots_[i].load(std::memory_order_acquire);
    if (v == 0) return TypeNameId{};
    const std::string* p = by_id_[v - 1].load(std::memory_order_acquire);
    if (p && *p == name) return make_id(v - 1);
  }
}
#else
TypeNameTable::TypeNameTable() = default;
#endif

TypeNameId TypeNameTable::intern(const std::string& name) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  const TypeNameId hit = find_published(name);
  if (hit.is_valid()) return hit;
  return intern_locked(name, &TypeNameTable::Publish, this);
#else
  return intern_locked(name);
#endif
}

const std::string& TypeNameTable::get(TypeNameId id) const {
  static const std::string empty;
  if (!id.is_valid()) return empty;
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Every id was published before intern() returned it.
  const std::string* p = by_id_[id.id].load(std::memory_order_acquire);
  return p ? *p : empty;
#else
  if (id.id >= names_.size()) return empty;
  return names_[id.id];
#endif
}

TypeNameId TypeNameTable::find(const std::string& name) const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  const TypeNameId hit = find_published(name);
  if (hit.is_valid()) return hit;
  std::shared_lock<std::shared_mutex> rlk(mu_);
#endif
  const TypeNameId h = find_live(name);
  if (h.is_valid()) return h;
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

void ValueBlockStorage::reserve(size_t count) { values_.reserve(count); }

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
