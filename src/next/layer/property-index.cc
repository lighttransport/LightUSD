// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Property Index Implementation

#include "property-index.hh"
#include <algorithm>
#include <cstring>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <mutex>
#include <shared_mutex>
#endif

namespace lightusd {
namespace next {

// ============================================================
// PropNameTable
// ============================================================

PropNameTable::PropNameTable() = default;

PropNameTable::~PropNameTable() = default;

namespace {
const char* PropertyNameKey(const void* context, size_t index, size_t* length) {
  const auto& names = *static_cast<const std::deque<std::string>*>(context);
  if (index >= names.size()) return nullptr;
  const std::string_view name = names[index];
  *length = name.size();
  return name.data();
}
}  // namespace

PropNameId PropNameTable::find_live(std::string_view name) const {
  // Empty string_view may have a null data pointer; empty names are still keys.
  const size_t id = name_to_id_.find(name.empty() ? "" : name.data(), name.size(),
                                   PropertyNameKey, &names_);
  if (id < UINT32_MAX) return PropNameId{static_cast<uint32_t>(id)};
  // The index is optional acceleration. An allocation failure must not lose
  // an interned name or give it a second ID on a subsequent lookup.
  if (name_to_id_.size() != names_.size()) {
    for (size_t i = 0; i < names_.size(); ++i) {
      if (names_[i] == name) return PropNameId{static_cast<uint32_t>(i)};
    }
  }
  return PropNameId{};
}


#if defined(LIGHTUSD_ENABLE_THREAD)
namespace {
// Order the snapshot by the pointed-to name.
struct FrozenLess {
  bool operator()(const std::pair<const std::string*, uint32_t>& a,
                  std::string_view b) const {
    return std::string_view(*a.first) < b;
  }
  bool operator()(const std::pair<const std::string*, uint32_t>& a,
                  const std::pair<const std::string*, uint32_t>& b) const {
    return *a.first < *b.first;
  }
};
}  // namespace

void PropNameTable::freeze() {
  // Build the snapshot under the write lock so it is a consistent view, then
  // publish it atomically. Readers only ever see a fully-built index.
  auto idx = std::make_shared<FrozenIndex>();
  {
    std::unique_lock<std::shared_mutex> wlk(mu_);
    idx->by_id.reserve(names_.size());
    for (const std::string& name : names_) idx->by_id.push_back(&name);
    idx->by_name.reserve(names_.size());
    for (size_t i = 0; i < names_.size(); ++i) {
      idx->by_name.emplace_back(&names_[i], static_cast<uint32_t>(i));
    }
    std::sort(idx->by_name.begin(), idx->by_name.end(), FrozenLess{});
  }
  {
    // Retain the snapshot for the table's lifetime so the raw pointer readers
    // publish below can never dangle, then publish with a release store so a
    // reader that acquires the pointer sees a fully-built index.
    std::unique_lock<std::shared_mutex> wlk(mu_);
    published_.push_back(idx);
  }
  frozen_ptr_.store(idx.get(), std::memory_order_release);
}

void PropNameTable::unfreeze() {
  // Retire the pointer only. The snapshot itself stays alive in published_:
  // a reader may have loaded it a moment ago and still be walking it.
  frozen_ptr_.store(nullptr, std::memory_order_release);
}

bool PropNameTable::is_frozen() const {
  return frozen_ptr_.load(std::memory_order_acquire) != nullptr;
}
#else
void PropNameTable::freeze() {}
void PropNameTable::unfreeze() {}
bool PropNameTable::is_frozen() const { return false; }
#endif

PropNameId PropNameTable::intern(const std::string& name) {
  const std::string_view view(name.data(), name.size());
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Frozen fast path: the common compose-time intern is a HIT (every property
  // name was interned at parse), so answer it lock-free from the snapshot.
  // A MISS falls through to the locked path below and inserts into the live
  // map — which the snapshot does not alias, so lock-free readers are
  // untouched and the snapshot stays valid (just missing this one name).
  if (const FrozenIndex* idx = frozen_ptr_.load(std::memory_order_acquire)) {
    auto it = std::lower_bound(idx->by_name.begin(), idx->by_name.end(), view,
                               FrozenLess{});
    if (it != idx->by_name.end() && *it->first == view) {
      return PropNameId{it->second};
    }
  }
  {
    std::shared_lock<std::shared_mutex> rlk(mu_);
    const PropNameId hit = find_live(view);
    if (hit.is_valid()) return hit;
  }
  std::unique_lock<std::shared_mutex> wlk(mu_);
#endif
  // Recheck under the exclusive lock; another thread may have interned it.
  const PropNameId hit = find_live(view);
  if (hit.is_valid()) return hit;
  if (names_.size() >= UINT32_MAX) return PropNameId{};
  const uint32_t id = static_cast<uint32_t>(names_.size());
  names_.push_back(name);
  if (name_to_id_.size() + 1 == names_.size())
    name_to_id_.insert(id, PropertyNameKey, &names_);
  else
    name_to_id_.rebuild(names_.size(), PropertyNameKey, &names_);
  return PropNameId{id};
}

PropNameId PropNameTable::intern(const char* name) {
  if (!name) return PropNameId{};
  return intern(std::string(name));
}

std::string_view PropNameTable::get(PropNameId id) const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  // Lock-free when the id is covered by the published snapshot. The deque
  // elements it points at never relocate, so the reference stays valid even if
  // another thread interns concurrently.
  if (const FrozenIndex* idx = frozen_ptr_.load(std::memory_order_acquire)) {
    if (id.id < idx->by_id.size()) return *idx->by_id[id.id];
  }
  {
    // Shared lock: a concurrent intern() on another thread may push_back names_
    // (parallel composition warms referenced layers on workers).
    std::shared_lock<std::shared_mutex> rlk(mu_);
    if (id.id >= names_.size()) return {};
    return names_[id.id];
  }
#endif
  if (id.id >= names_.size()) return {};
  return names_[id.id];
}

PropNameId PropNameTable::find(const std::string& name) const {
  return find(std::string_view(name.data(), name.size()));
}

PropNameId PropNameTable::find(std::string_view name) const {
#if defined(LIGHTUSD_ENABLE_THREAD)
  if (const FrozenIndex* idx = frozen_ptr_.load(std::memory_order_acquire)) {
    auto it = std::lower_bound(idx->by_name.begin(), idx->by_name.end(), name,
                               FrozenLess{});
    if (it != idx->by_name.end() && *it->first == name) {
      return PropNameId{it->second};
    }
  }
  std::shared_lock<std::shared_mutex> rlk(mu_);
  return find_live(name);
#else
  return find_live(name);
#endif
}

PropNameId PropNameTable::find(const char* name) const {
  if (!name) return PropNameId{};
  return find(std::string_view(name));
}

void PropNameTable::register_common_names() {
  // Pre-register common USD property names
  // These get the lowest IDs for fastest lookup
  id_points = intern("points");
  id_normals = intern("normals");
  id_primvars_st = intern("primvars:st");
  id_extent = intern("extent");
  id_visibility = intern("visibility");
  id_purpose = intern("purpose");
  id_xformOpOrder = intern("xformOpOrder");
  id_faceVertexCounts = intern("faceVertexCounts");
  id_faceVertexIndices = intern("faceVertexIndices");
  id_subdivisionScheme = intern("subdivisionScheme");
  id_interpolateBoundary = intern("interpolateBoundary");
  id_radius = intern("radius");
  id_width = intern("width");
  id_height = intern("height");
  id_size = intern("size");

  // Additional common names
  intern("doubleSided");
  intern("orientation");
  intern("displayColor");
  intern("displayOpacity");
  intern("velocities");
  intern("accelerations");
  intern("ids");
  intern("widths");
  intern("cornerIndices");
  intern("cornerSharpnesses");
  intern("creaseIndices");
  intern("creaseLengths");
  intern("creaseSharpnesses");
  intern("holeIndices");

  // XformOp names
  intern("xformOp:translate");
  intern("xformOp:rotateX");
  intern("xformOp:rotateY");
  intern("xformOp:rotateZ");
  intern("xformOp:rotateXYZ");
  intern("xformOp:rotateXZY");
  intern("xformOp:rotateYXZ");
  intern("xformOp:rotateYZX");
  intern("xformOp:rotateZXY");
  intern("xformOp:rotateZYX");
  intern("xformOp:scale");
  intern("xformOp:orient");
  intern("xformOp:transform");

  // Material binding
  intern("material:binding");
  intern("material:binding:preview");
  intern("material:binding:full");

  // Shader inputs/outputs
  intern("inputs:diffuseColor");
  intern("inputs:emissiveColor");
  intern("inputs:metallic");
  intern("inputs:roughness");
  intern("inputs:opacity");
  intern("inputs:normal");
  intern("inputs:occlusion");
  intern("inputs:clearcoat");
  intern("inputs:clearcoatRoughness");
  intern("inputs:ior");
  intern("inputs:specularColor");
  intern("inputs:useSpecularWorkflow");
  intern("inputs:file");
  intern("inputs:st");
  intern("inputs:wrapS");
  intern("inputs:wrapT");
  intern("inputs:fallback");
  intern("inputs:varname");
  intern("outputs:surface");
  intern("outputs:displacement");
  intern("outputs:volume");
  intern("outputs:rgb");
  intern("outputs:r");
  intern("outputs:g");
  intern("outputs:b");
  intern("outputs:a");

  // Light properties
  intern("intensity");
  intern("exposure");
  intern("color");
  intern("enableColorTemperature");
  intern("colorTemperature");
  intern("diffuse");
  intern("specular");
  intern("normalize");
  intern("inputs:intensity");
  intern("inputs:exposure");
  intern("inputs:color");
  intern("inputs:radius");
  intern("inputs:angle");
  intern("inputs:softness");
  intern("inputs:length");
  intern("inputs:width");
  intern("inputs:height");
  intern("inputs:texture:file");
  intern("inputs:texture:format");

  // Skeleton properties
  intern("joints");
  intern("jointNames");
  intern("bindTransforms");
  intern("restTransforms");
  intern("rotations");
  intern("translations");
  intern("scales");
  intern("blendShapes");
  intern("blendShapeWeights");
  intern("skeleton");
  intern("animationSource");
  intern("primvars:skel:jointIndices");
  intern("primvars:skel:jointWeights");
  intern("primvars:skel:geomBindTransform");

  // Schema-accessor names (geom-mesh / geom-xform / geom-point-instancer /
  // stage / tydra render-converter / scene-access kId* functions). These are
  // intern()ed from render-time accessors; pre-registering them keeps every
  // post-compose intern a HIT so the frozen lock-free table (see freeze())
  // is never unfrozen by a render-phase schema accessor.
  intern("primvars:uv");
  intern("primvars:uv:indices");
  intern("primvars:st:indices");
  intern("primvars:displayColor");
  intern("primvars:displayOpacity");
  intern("protoIndices");
  intern("positions");
  intern("orientations");
  intern("angularVelocities");
  intern("invisibleIds");
  intern("inactiveIds");
  intern("curveVertexCounts");
  intern("indices");
  intern("tetVertexIndices");
  intern("outputs:out");
  intern("clippingRange");
  intern("projection");
  intern("focalLength");
  intern("horizontalAperture");
  intern("verticalAperture");
  intern("focusDistance");
  intern("fStop");
  intern("shutter:open");
  intern("shutter:close");
  intern("inputs:colorTemperature");
  intern("inputs:enableColorTemperature");
  intern("inputs:diffuse");
  intern("inputs:specular");
  intern("inputs:normalize");
  intern("inputs:enableShadows");
  intern("inputs:shadow:enable");
  intern("inputs:shadow:color");
  intern("inputs:shadow:distance");
  intern("inputs:shadow:falloff");
  intern("inputs:shadow:falloffGamma");
  intern("inputs:shaping:cone:angle");
  intern("inputs:shaping:cone:softness");
  intern("inputs:shaping:focus");
  intern("inputs:shaping:focusTint");
  intern("inputs:shaping:ies:angleScale");
  intern("inputs:shaping:ies:normalize");
  intern("inputs:shaping:ies:file");
}

// Global singleton. Registration happens inside the (thread-safe) static
// local initialization, so concurrent first callers cannot observe a
// half-registered table.
PropNameTable& GetPropNameTable() {
  static PropNameTable& table = []() -> PropNameTable& {
    static PropNameTable t;
    t.register_common_names();
    return t;
  }();
  return table;
}

// ============================================================
// PropIndex
// ============================================================

void PropIndex::reserve(size_t count) {
  slots_.reserve(count);
}

void PropIndex::add(PropSlot slot) {
  slots_.push_back(slot);
  sorted_ = false;
}

const PropSlot* PropIndex::find(PropNameId name_id) const {
  if (!name_id.is_valid()) return nullptr;

  if (sorted_) {
    // Binary search
    auto it = std::lower_bound(slots_.begin(), slots_.end(), name_id,
        [](const PropSlot& slot, PropNameId id) {
          return slot.name_id < id;
        });
    if (it != slots_.end() && it->name_id == name_id) {
      return &(*it);
    }
  } else {
    // Linear search
    for (const auto& slot : slots_) {
      if (slot.name_id == name_id) {
        return &slot;
      }
    }
  }
  return nullptr;
}

PropSlot* PropIndex::find_mutable(PropNameId name_id) {
  // const_cast is safe: *this is non-const, so the slot returned by find()
  // aliases our own non-const slots_ storage.
  return const_cast<PropSlot*>(find(name_id));
}

const PropSlot* PropIndex::find(const std::string& name) const {
  PropNameId id = GetPropNameTable().find(name);
  if (!id.is_valid()) {
    // Name not in table - do linear search by string
    const auto& table = GetPropNameTable();
    for (const auto& slot : slots_) {
      if (table.get(slot.name_id) == std::string_view(name.data(), name.size())) {
        return &slot;
      }
    }
    return nullptr;
  }
  return find(id);
}

bool PropIndex::remove(PropNameId name_id) {
  if (!name_id.is_valid()) return false;
  for (auto it = slots_.begin(); it != slots_.end(); ++it) {
    if (it->name_id == name_id) {
      slots_.erase(it);  // preserves order, so sorted_ stays valid
      return true;
    }
  }
  return false;
}

void PropIndex::sort() {
  std::sort(slots_.begin(), slots_.end(),
      [](const PropSlot& a, const PropSlot& b) {
        return a.name_id < b.name_id;
      });
  sorted_ = true;
}

}  // namespace next
}  // namespace lightusd
