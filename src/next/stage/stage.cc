// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Stage Implementation

#include "stage.hh"
#include "../composition/composition.hh"
#include "../schema/schema-registry.hh"
#include <algorithm>
#include <unordered_set>

namespace lightusd {
namespace next {

namespace {

bool IsStaticGeometryArray(const std::string& type,
                          const PropNameId property_id) {
  const auto& names = GetPropNameTable();
  const std::string property(names.get(property_id));
  const bool primvar = property.compare(0, 9, "primvars:") == 0;
  if (type == "Mesh") {
    if (primvar) return true;
    static const PropNameId kIdPoints =
        GetPropNameTable().intern("points");
    static const PropNameId kIdNormals =
        GetPropNameTable().intern("normals");
    static const PropNameId kIdVelocities =
        GetPropNameTable().intern("velocities");
    static const PropNameId kIdAccelerations =
        GetPropNameTable().intern("accelerations");
    static const PropNameId kIdFaceVertexCounts =
        GetPropNameTable().intern("faceVertexCounts");
    static const PropNameId kIdFaceVertexIndices =
        GetPropNameTable().intern("faceVertexIndices");
    static const PropNameId kIdHoleIndices =
        GetPropNameTable().intern("holeIndices");
    static const PropNameId kIdCornerIndices =
        GetPropNameTable().intern("cornerIndices");
    static const PropNameId kIdCornerSharpnesses =
        GetPropNameTable().intern("cornerSharpnesses");
    static const PropNameId kIdCreaseIndices =
        GetPropNameTable().intern("creaseIndices");
    static const PropNameId kIdCreaseLengths =
        GetPropNameTable().intern("creaseLengths");
    static const PropNameId kIdCreaseSharpnesses =
        GetPropNameTable().intern("creaseSharpnesses");
    if ((property_id == kIdPoints) || (property_id == kIdNormals) ||
        (property_id == kIdVelocities) || (property_id == kIdAccelerations) ||
        (property_id == kIdFaceVertexCounts) ||
        (property_id == kIdFaceVertexIndices) ||
        (property_id == kIdHoleIndices) || (property_id == kIdCornerIndices) ||
        (property_id == kIdCornerSharpnesses) ||
        (property_id == kIdCreaseIndices) ||
        (property_id == kIdCreaseLengths) ||
        (property_id == kIdCreaseSharpnesses)) {
      return true;
    }
    return false;
  }
  if (type == "PointInstancer" || type == "UsdGeomPointInstancer") {
    if (primvar) return true;
    static const PropNameId kIdProtoIndices =
        GetPropNameTable().intern("protoIndices");
    static const PropNameId kIdPositions =
        GetPropNameTable().intern("positions");
    static const PropNameId kIdOrientations =
        GetPropNameTable().intern("orientations");
    static const PropNameId kIdScales =
        GetPropNameTable().intern("scales");
    static const PropNameId kIdVelocities =
        GetPropNameTable().intern("velocities");
    static const PropNameId kIdAngularVelocities =
        GetPropNameTable().intern("angularVelocities");
    static const PropNameId kIdAccelerations =
        GetPropNameTable().intern("accelerations");
    static const PropNameId kIdIds = GetPropNameTable().intern("ids");
    static const PropNameId kIdInvisibleIds =
        GetPropNameTable().intern("invisibleIds");
    if ((property_id == kIdProtoIndices) ||
        (property_id == kIdPositions) ||
        (property_id == kIdOrientations) || (property_id == kIdScales) ||
        (property_id == kIdVelocities) ||
        (property_id == kIdAngularVelocities) ||
        (property_id == kIdAccelerations) || (property_id == kIdIds) ||
        (property_id == kIdInvisibleIds)) {
      return true;
    }
    return false;
  }
  if (type == "BasisCurves" || type == "NurbsCurves" ||
      type == "Points") {
    if (primvar) return true;
    static const PropNameId kIdPoints =
        GetPropNameTable().intern("points");
    static const PropNameId kIdNormals =
        GetPropNameTable().intern("normals");
    static const PropNameId kIdWidths =
        GetPropNameTable().intern("widths");
    static const PropNameId kIdVelocities =
        GetPropNameTable().intern("velocities");
    static const PropNameId kIdAccelerations =
        GetPropNameTable().intern("accelerations");
    static const PropNameId kIdIds = GetPropNameTable().intern("ids");
    static const PropNameId kIdCurveVertexCounts =
        GetPropNameTable().intern("curveVertexCounts");
    if ((property_id == kIdPoints) || (property_id == kIdNormals) ||
        (property_id == kIdWidths) || (property_id == kIdVelocities) ||
        (property_id == kIdAccelerations) || (property_id == kIdIds) ||
        (property_id == kIdCurveVertexCounts)) {
      return true;
    }
    return false;
  }
  if (type == "GeomSubset") {
    static const PropNameId kIdIndices =
        GetPropNameTable().intern("indices");
    return property_id == kIdIndices;
  }
  if (type == "ParticleField3DGaussianSplat") {
    if (primvar) return true;
    static const PropNameId kIdPositions =
        GetPropNameTable().intern("positions");
    static const PropNameId kIdScales =
        GetPropNameTable().intern("scales");
    static const PropNameId kIdOrientations =
        GetPropNameTable().intern("orientations");
    static const PropNameId kIdOpacities =
        GetPropNameTable().intern("opacities");
    static const PropNameId kIdPositionsH =
        GetPropNameTable().intern("positionsh");
    static const PropNameId kIdScalesH =
        GetPropNameTable().intern("scalesh");
    static const PropNameId kIdOrientationsH =
        GetPropNameTable().intern("orientationsh");
    static const PropNameId kIdOpacitiesH =
        GetPropNameTable().intern("opacitiesh");
    static const PropNameId kIdSh = GetPropNameTable().intern(
        "radiance:sphericalHarmonicsCoefficients");
    static const PropNameId kIdShH = GetPropNameTable().intern(
        "radiance:sphericalHarmonicsCoefficientsh");
    return property_id == kIdPositions || property_id == kIdScales ||
           property_id == kIdOrientations || property_id == kIdOpacities ||
           property_id == kIdPositionsH || property_id == kIdScalesH ||
           property_id == kIdOrientationsH || property_id == kIdOpacitiesH ||
           property_id == kIdSh || property_id == kIdShH;
  }
  return false;
}

Stage::StaticGeometryReleaseStats ReleasePrimStaticGeometryArrays(
    PrimSpec* prim, size_t min_array_elements) {
  Stage::StaticGeometryReleaseStats stats;
  if (!prim) return stats;
  const std::string& prim_type = prim->type_name();
  for (const PropSlot& slot : prim->properties().slots()) {
    const PropNameId id = slot.name_id;
    if (!IsStaticGeometryArray(prim_type, id)) continue;
    // release_static_array_value changes only the slot's offset and its Value;
    // PropIndex storage/order is stable, so this iteration does not invalidate.
    const size_t bytes = prim->release_static_array_value(
        id, min_array_elements, &stats.element_count);
    if (bytes == 0) continue;
    ++stats.property_count;
    stats.estimated_payload_bytes += bytes;
  }
  return stats;
}

}  // namespace

// ============================================================
// UsdPrim
// ============================================================

static const std::string kEmptyString;
static const Path kEmptyPath;
static const PrimSpecMeta kDefaultMeta;

const std::string& UsdPrim::GetName() const {
  if (!spec_) return kEmptyString;
  return spec_->name();
}

const std::string& UsdPrim::GetTypeName() const {
  if (!spec_) return kEmptyString;
  return spec_->type_name();
}

const Path& UsdPrim::GetPath() const {
  if (!spec_) return kEmptyPath;
  return proxy_path_.empty() ? spec_->path() : proxy_path_;
}

PrimSpecifier UsdPrim::GetSpecifier() const {
  if (!spec_) return PrimSpecifier::Def;
  return spec_->specifier();
}

bool UsdPrim::IsActive() const {
  if (!spec_) return false;
  UsdPrim current = *this;
  while (current) {
    if (!current.spec_->meta().active) return false;
    current = current.GetParent();
  }
  return true;
}

bool UsdPrim::IsLoaded() const {
  if (!spec_) return false;
  UsdPrim current = *this;
  while (current) {
    if (!current.spec_->meta().loaded) return false;
    current = current.GetParent();
  }
  return true;
}

bool UsdPrim::IsDefined() const {
  if (!spec_) return false;
  UsdPrim current = *this;
  while (current) {
    if (current.spec_->specifier() == PrimSpecifier::Over) return false;
    current = current.GetParent();
  }
  return true;
}

bool UsdPrim::IsAbstract() const {
  if (!spec_) return false;
  UsdPrim current = *this;
  while (current) {
    if (current.spec_->specifier() == PrimSpecifier::Class) return true;
    current = current.GetParent();
  }
  return false;
}

bool UsdPrim::IsConcretelyDefined() const {
  if (!spec_) return false;
  UsdPrim current = *this;
  while (current) {
    if (current.spec_->specifier() != PrimSpecifier::Def) return false;
    current = current.GetParent();
  }
  return true;
}

bool UsdPrim::IsInModelHierarchy() const {
  if (!spec_) return false;
  const std::string& own_kind = spec_->meta().kind();
  if (own_kind != "group" && own_kind != "assembly" &&
      own_kind != "component") {
    return false;
  }

  UsdPrim ancestor = GetParent();
  while (ancestor) {
    const std::string& kind = ancestor.spec_->meta().kind();
    if (kind != "group" && kind != "assembly") return false;
    ancestor = ancestor.GetParent();
  }
  return true;
}

bool UsdPrim::HasProperty(const std::string& name) const {
  if (!spec_) return false;
  const PrimSpec* source = ChildSourceSpec();
  return spec_->property(name) != nullptr ||
         (source != spec_ && source->property(name) != nullptr) ||
         GetSchemaRegistry().FindProperty(*source, name) != nullptr;
}

bool UsdPrim::HasProperty(PropNameId name_id) const {
  if (!spec_ || !name_id.is_valid()) return false;
  const PrimSpec* source = ChildSourceSpec();
  if (spec_->property(name_id) != nullptr) return true;
  if (source != spec_ && source->property(name_id) != nullptr) return true;
  const std::string name(GetPropNameTable().get(name_id));
  return GetSchemaRegistry().FindProperty(*source, name) != nullptr;
}

bool UsdPrim::HasAuthoredProperty(const std::string& name) const {
  if (!spec_) return false;
  const PrimSpec* source = ChildSourceSpec();
  return spec_->property(name) != nullptr ||
          (source != spec_ && source->property(name) != nullptr);
}

const Value* UsdPrim::GetPropertyValue(const std::string& name) const {
  if (!spec_) return nullptr;
  if (const Value* value = spec_->property_value(name)) {
    return value->is_block() ? nullptr : value;
  }
  const PrimSpec* source = ChildSourceSpec();
  if (source != spec_) {
    if (const Value* value = source->property_value(name)) {
      return value->is_block() ? nullptr : value;
    }
  }
  if (const SchemaPropertyDefinition* def =
          GetSchemaRegistry().FindProperty(*source, name)) {
    if (def->has_fallback) return &def->fallback;
  }
  return nullptr;
}

const Value* UsdPrim::GetPropertyValue(PropNameId name_id) const {
  if (!spec_) return nullptr;
  if (const Value* value = spec_->property_value(name_id)) {
    return value->is_block() ? nullptr : value;
  }
  const PrimSpec* source = ChildSourceSpec();
  if (source != spec_) {
    if (const Value* value = source->property_value(name_id)) {
      return value->is_block() ? nullptr : value;
    }
  }
  const std::string name(GetPropNameTable().get(name_id));
  if (const SchemaPropertyDefinition* def =
          GetSchemaRegistry().FindProperty(*source, name)) {
    if (def->has_fallback) return &def->fallback;
  }
  return nullptr;
}

const Value* UsdPrim::GetPropertyValueOrEarliestTimeSample(
    const std::string& name) const {
  if (const Value* value = GetPropertyValue(name)) return value;
  return EarliestTimeSampleValue(GetPropNameTable().find(name));
}

const Value* UsdPrim::GetPropertyValueOrEarliestTimeSample(
    PropNameId name_id) const {
  if (const Value* value = GetPropertyValue(name_id)) return value;
  return EarliestTimeSampleValue(name_id);
}

const Value* UsdPrim::EarliestTimeSampleValue(PropNameId name_id) const {
  if (!spec_ || !name_id.is_valid()) return nullptr;
  const PrimSpec* owner = spec_;
  const auto* samples = owner->time_samples(name_id);
  if ((!samples || samples->empty()) && ChildSourceSpec() != spec_) {
    owner = ChildSourceSpec();
    samples = owner->time_samples(name_id);
  }
  if (!samples || samples->empty()) return nullptr;
  return owner->time_sample_value(samples->front().second);
}

std::vector<std::string> UsdPrim::GetPropertyNames() const {
  if (!spec_) return {};

  const PrimSpec* source = ChildSourceSpec();
  const auto& props = spec_->properties();
  std::vector<std::string> names;
  names.reserve(props.size() + (source == spec_ ? 0 : source->properties().size()));

  // Membership is tested with a hash set (O(1)) instead of a linear scan: this
  // runs once per queried prim on the render path, so a dense prim (hundreds of
  // primvars) used to cost O(P^2) string compares here.
  PropNameTable& table = GetPropNameTable();
  std::unordered_set<std::string> seen;
  seen.reserve(names.capacity());
  for (const auto& slot : props.slots()) {
    names.emplace_back(table.get(slot.name_id));
    seen.insert(names.back());
  }
  if (source != spec_) {
    for (const auto& slot : source->properties().slots()) {
      const std::string name(table.get(slot.name_id));
      if (seen.insert(name).second) {
        names.push_back(name);
      }
    }
  }
  for (const std::string& built_in :
       GetSchemaRegistry().PropertyNames(*source)) {
    if (seen.insert(built_in).second) {
      names.push_back(built_in);
    }
  }
  const std::vector<std::string>& authored_order =
      spec_->meta().propertyOrder();
  if (!authored_order.empty()) {
    std::vector<std::string> ordered;
    ordered.reserve(names.size());
    std::unordered_set<std::string> emitted;
    emitted.reserve(names.size());
    for (const std::string& wanted : authored_order) {
      if (seen.count(wanted) && emitted.insert(wanted).second) {
        ordered.push_back(wanted);
      }
    }
    for (const std::string& name : names) {
      if (emitted.insert(name).second) {
        ordered.push_back(name);
      }
    }
    names = std::move(ordered);
  }
  return names;
}

bool UsdPrim::HasTimeSamples(const std::string& name) const {
  if (!spec_) return false;
  PropNameId name_id = GetPropNameTable().find(name);
  if (!name_id.is_valid()) return false;
  return spec_->has_time_samples(name_id);
}

bool UsdPrim::HasTimeSamples(PropNameId name_id) const {
  if (!spec_ || !name_id.is_valid()) return false;
  return spec_->has_time_samples(name_id);
}

bool UsdPrim::HasAnyTimeSamples() const {
  if (!spec_) return false;
  return spec_->has_any_time_samples();
}

std::vector<double> UsdPrim::GetTimeSampleTimes(const std::string& name) const {
  std::vector<double> times;
  if (!spec_) return times;

  PropNameId name_id = GetPropNameTable().find(name);
  if (!name_id.is_valid()) return times;

  const auto* samples = spec_->time_samples(name_id);
  if (!samples) return times;

  times.reserve(samples->size());
  for (const auto& sample : *samples) {
    times.push_back(sample.first);
  }
  return times;
}

std::vector<double> UsdPrim::GetTimeSampleTimes(PropNameId name_id) const {
  std::vector<double> times;
  if (!spec_ || !name_id.is_valid()) return times;
  const auto* samples = spec_->time_samples(name_id);
  if (!samples) return times;

  times.reserve(samples->size());
  for (const auto& sample : *samples) {
    times.push_back(sample.first);
  }
  return times;
}

const std::vector<std::pair<double, uint32_t>>* UsdPrim::GetTimeSamples(
    PropNameId name_id) const {
  if (!spec_ || !name_id.is_valid()) return nullptr;
  return spec_->time_samples(name_id);
}

const Value* UsdPrim::GetValueAtTime(const std::string& name, double time) const {
  if (!spec_) return nullptr;

  PropNameId name_id = GetPropNameTable().find(name);
  if (!name_id.is_valid()) return nullptr;
  return GetValueAtTime(name_id, time);
}

const Value* UsdPrim::GetValueAtTime(PropNameId name_id, double time) const {
  if (!spec_ || !name_id.is_valid()) return nullptr;

  const auto* samples = spec_->time_samples(name_id);
  if (!samples || samples->empty()) {
    // Fall back to default value
    return spec_->property_value(name_id);
  }

  // Binary search for closest time
  auto it = std::lower_bound(samples->begin(), samples->end(), time,
                              [](const std::pair<double, uint32_t>& p, double t) {
                                return p.first < t;
                              });

  if (it == samples->end()) {
    // Time is past all samples, use last
    return spec_->time_sample_value(samples->back().second);
  }
  if (it == samples->begin()) {
    // Time is before all samples, use first
    return spec_->time_sample_value(it->second);
  }

  // Check if exact match or use previous
  if (it->first == time) {
    return spec_->time_sample_value(it->second);
  }

  // Return the previous sample (held interpolation)
  --it;
  return spec_->time_sample_value(it->second);
}

Value UsdPrim::GetInterpolatedValue(const std::string& name, double time) const {
  if (!spec_) return Value();

  PropNameId name_id = GetPropNameTable().find(name);
  if (!name_id.is_valid()) return Value();
  return GetInterpolatedValue(name_id, time);
}

Value UsdPrim::GetInterpolatedValue(PropNameId name_id, double time) const {
  if (!spec_ || !name_id.is_valid()) return Value();

  const auto* samples = spec_->time_samples(name_id);
  if (!samples || samples->empty()) {
    const Value* v = spec_->property_value(name_id);
    return v ? *v : Value();
  }

  auto it = std::lower_bound(samples->begin(), samples->end(), time,
                             [](const std::pair<double, uint32_t>& p, double t) {
                               return p.first < t;
                             });

  if (it == samples->end()) {
    const Value* v = spec_->time_sample_value(samples->back().second);
    return v ? *v : Value();
  }
  if (it == samples->begin() || it->first == time) {
    const Value* v = spec_->time_sample_value(it->second);
    return v ? *v : Value();
  }

  // Linearly interpolate between the bracketing samples (held for
  // non-interpolatable types, handled by LerpValue).
  auto prev = it;
  --prev;
  const Value* va = spec_->time_sample_value(prev->second);
  const Value* vb = spec_->time_sample_value(it->second);
  if (!va || !vb) return va ? *va : Value();
  const double t0 = prev->first;
  const double t1 = it->first;
  const double frac = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0;
  return LerpValue(*va, *vb, frac);
}

const std::vector<Path>* UsdPrim::GetRelationship(const std::string& name) const {
  if (!spec_) return nullptr;
  if (const std::vector<Path>* relationship = spec_->relationship(name))
    return relationship;
  return ChildSourceSpec()->relationship(name);
}

bool UsdPrim::GetForwardedRelationshipTargets(
    const std::string& name, std::vector<Path>* targets) const {
  if (!targets) return false;
  targets->clear();
  if (!spec_ || !layer_ || !GetRelationship(name)) return false;

  std::string prototype_root = prototype_root_;
  std::string instance_root = instance_root_;
  const PrimSpec* root_owner = spec_;
  if (prototype_root.empty() && !spec_->meta().instance_prototype().empty()) {
    prototype_root = spec_->meta().instance_prototype();
    instance_root = spec_->path().str();
    if (!spec_->relationship(name)) {
      if (const PrimSpec* prototype =
              layer_->prim_at_path(Path(prototype_root))) root_owner = prototype;
    }
  }
  auto map_prefix = [](const std::string& path, const std::string& from,
                       const std::string& to) {
    if (from.empty() || to.empty()) return path;
    if (path == from) return to;
    if (path.size() > from.size() && path.compare(0, from.size(), from) == 0 &&
        (path[from.size()] == '/' || path[from.size()] == '.')) {
      return to + path.substr(from.size());
    }
    return path;
  };

  std::unordered_set<std::string> visited_relationships;
  std::unordered_set<std::string> unique_targets;

  struct ForwardFrame {
    const PrimSpec* owner;
    std::string rel_name;
    size_t target_pos;
  };
  std::vector<ForwardFrame> fwd_stack;
  fwd_stack.push_back({root_owner, name, 0});
  visited_relationships.insert(root_owner->path().append_property(name).str());

  while (!fwd_stack.empty()) {
    ForwardFrame& f = fwd_stack.back();
    const std::vector<Path>* raw = f.owner->relationship(f.rel_name);
    if (!raw) { fwd_stack.pop_back(); continue; }

    if (f.target_pos >= raw->size()) {
      fwd_stack.pop_back();
      continue;
    }

    const Path& target = (*raw)[f.target_pos++];
    const std::string lookup_string =
        map_prefix(target.str(), instance_root, prototype_root);
    const std::string output_string =
        map_prefix(target.str(), prototype_root, instance_root);
    const Path lookup_target(lookup_string);
    if (target.has_property()) {
      const PrimSpec* target_prim =
          layer_->prim_at_path(lookup_target.prim_path());
      if (target_prim &&
          target_prim->relationship(lookup_target.property_name()) != nullptr) {
        if (visited_relationships.insert(lookup_string).second) {
          fwd_stack.push_back({target_prim, lookup_target.property_name(), 0});
        }
        continue;
      }
    }
    if (unique_targets.insert(output_string).second) {
      targets->push_back(Path(output_string));
    }
  }
  return true;
}

std::vector<std::string> UsdPrim::GetRelationshipNames() const {
  if (!spec_) return {};
  std::vector<std::string> names = spec_->relationship_names();
  const PrimSpec* source = ChildSourceSpec();
  if (source != spec_) {
    // O(1) dedup (was a linear scan per name).
    std::unordered_set<std::string> seen(names.begin(), names.end());
    for (const std::string& name : source->relationship_names()) {
      if (seen.insert(name).second) {
        names.push_back(name);
      }
    }
  }
  return names;
}

UsdPrim UsdPrim::GetParent() const {
  if (!spec_ || !layer_) return UsdPrim();

  if (!proxy_path_.empty() && !prototype_root_.empty()) {
    const std::string proxy = proxy_path_.str();
    const size_t slash = proxy.rfind('/');
    if (slash == std::string::npos || slash == 0) return UsdPrim();
    const std::string proxy_parent = proxy.substr(0, slash);
    if (proxy_parent == instance_root_) {
      const uint32_t index = layer_->index_at_path(instance_root_);
      const PrimSpec* instance = layer_->prim_at_path(Path(instance_root_));
      return instance ? UsdPrim(instance, layer_, index) : UsdPrim();
    }
    const std::string prototype_parent =
        prototype_root_ + proxy_parent.substr(instance_root_.size());
    const uint32_t index = layer_->index_at_path(prototype_parent);
    const PrimSpec* parent = layer_->prim_at_path(Path(prototype_parent));
    return parent ? UsdPrim(parent, layer_, index, Path(proxy_parent),
                            prototype_root_, instance_root_)
                  : UsdPrim();
  }

  // Parse parent path from current path
  const std::string& path_str = spec_->path().str();
  if (path_str.empty() || path_str == "/") return UsdPrim();

  size_t last_slash = path_str.rfind('/');
  if (last_slash == 0) {
    // Parent is pseudo-root, not a real prim
    return UsdPrim();
  }
  if (last_slash == std::string::npos) return UsdPrim();

  std::string parent_path = path_str.substr(0, last_slash);
  // O(1) via the path index. There is no linear-scan fallback: prim_at_path()
  // resolves through the very same path_to_index_ map, so a miss here means
  // the prim genuinely is not in the layer -- the old fallback loop could
  // never run (it was reached only when prim_at_path had already succeeded on
  // a lookup that index_at_path had just failed, which is impossible).
  uint32_t pidx = layer_->index_at_path(parent_path);
  if (pidx == UINT32_MAX) return UsdPrim();
  return UsdPrim(layer_->prim(pidx), layer_, pidx);
}

// For instance proxies, children are provided by the prototype prim's subtree.
const PrimSpec* UsdPrim::ChildSourceSpec() const {
  if (spec_ && layer_ && !spec_->meta().instance_prototype().empty()) {
    const PrimSpec* proto =
        layer_->prim_at_path(Path(spec_->meta().instance_prototype()));
    if (proto) return proto;
  }
  return spec_;
}

std::vector<UsdPrim> UsdPrim::GetChildren() const {
  std::vector<UsdPrim> children;
  if (!spec_ || !layer_) return children;

  const PrimSpec* src = ChildSourceSpec();
  std::string prototype_root = prototype_root_;
  std::string instance_root = instance_root_;
  if (prototype_root.empty() && !spec_->meta().instance_prototype().empty()) {
    prototype_root = spec_->meta().instance_prototype();
    instance_root = spec_->path().str();
  }
  const auto& indices = src->child_indices();
  children.reserve(indices.size());

  for (uint32_t idx : indices) {
    const PrimSpec* child = layer_->prim(idx);
    if (child) {
      if (!prototype_root.empty()) {
        children.emplace_back(
            child, layer_, idx,
            Path(instance_root + child->path().str().substr(prototype_root.size())),
            prototype_root, instance_root);
      } else {
        children.emplace_back(child, layer_, idx);
      }
    }
  }
  return children;
}

size_t UsdPrim::GetChildCount() const {
  if (!spec_) return 0;
  return ChildSourceSpec()->child_count();
}

UsdPrim UsdPrim::GetChildAt(size_t index) const {
  if (!spec_ || !layer_) return UsdPrim();
  const auto& indices = ChildSourceSpec()->child_indices();
  if (index >= indices.size()) return UsdPrim();
  uint32_t idx = indices[index];
  const PrimSpec* child = layer_->prim(idx);
  if (!child) return UsdPrim();
  std::string prototype_root = prototype_root_;
  std::string instance_root = instance_root_;
  if (prototype_root.empty() && !spec_->meta().instance_prototype().empty()) {
    prototype_root = spec_->meta().instance_prototype();
    instance_root = spec_->path().str();
  }
  if (!prototype_root.empty()) {
    return UsdPrim(
        child, layer_, idx,
        Path(instance_root + child->path().str().substr(prototype_root.size())),
        prototype_root, instance_root);
  }
  return UsdPrim(child, layer_, idx);
}

UsdPrim UsdPrim::GetChild(const std::string& name) const {
  if (!spec_ || !layer_) return UsdPrim();

  for (uint32_t idx : ChildSourceSpec()->child_indices()) {
    const PrimSpec* child = layer_->prim(idx);
    if (child && child->name() == name) {
      std::string prototype_root = prototype_root_;
      std::string instance_root = instance_root_;
      if (prototype_root.empty() && !spec_->meta().instance_prototype().empty()) {
        prototype_root = spec_->meta().instance_prototype();
        instance_root = spec_->path().str();
      }
      if (!prototype_root.empty()) {
        return UsdPrim(
            child, layer_, idx,
            Path(instance_root +
                 child->path().str().substr(prototype_root.size())),
            prototype_root, instance_root);
      }
      return UsdPrim(child, layer_, idx);
    }
  }
  return UsdPrim();
}

const PrimSpecMeta& UsdPrim::GetMeta() const {
  if (!spec_) return kDefaultMeta;
  return spec_->meta();
}

// ============================================================
// Stage
// ============================================================

Stage::Stage() = default;
Stage::~Stage() = default;

Stage::Stage(Stage&&) noexcept = default;
Stage& Stage::operator=(Stage&&) noexcept = default;

Stage Stage::Clone() const {
  Stage out;
  if (root_layer_) {
    out.SetRootLayer(root_layer_->Clone());
  }
  for (const auto& sub_layer : sub_layers_) {
    if (sub_layer) {
      out.AddSubLayer(sub_layer->Clone());
    }
  }
  // SetRootLayer derives metadata from the cloned root layer. Preserve any
  // stage-level metadata that was authored or updated after loading.
  out.meta_ = meta_;
  return out;
}

void Stage::SetRootLayer(Layer&& layer) {
  root_layer_ = std::make_unique<Layer>(std::move(layer));
  UpdateMetaFromRootLayer();
}

void Stage::AddSubLayer(Layer&& layer) {
  sub_layers_.push_back(std::make_unique<Layer>(std::move(layer)));
}

const Layer* Stage::GetSubLayer(size_t index) const {
  if (index >= sub_layers_.size()) return nullptr;
  return sub_layers_[index].get();
}

void Stage::UpdateMetaFromRootLayer() {
  if (!root_layer_) return;

  const LayerMeta& lm = root_layer_->meta();
  meta_.defaultPrim = lm.defaultPrim;
  meta_.defaultPrim_set = lm.defaultPrim_set;
  meta_.upAxis = lm.upAxis;
  meta_.metersPerUnit = lm.metersPerUnit;
  meta_.timeCodesPerSecond = lm.timeCodesPerSecond;
  meta_.startTimeCode = lm.startTimeCode;
  meta_.endTimeCode = lm.endTimeCode;
  meta_.upAxis_set = lm.upAxis_set;
  meta_.metersPerUnit_set = lm.metersPerUnit_set;
  meta_.timeCodesPerSecond_set = lm.timeCodesPerSecond_set;
  meta_.startTimeCode_set = lm.startTimeCode_set;
  meta_.endTimeCode_set = lm.endTimeCode_set;
  meta_.framesPerSecond = lm.framesPerSecond;
  meta_.framesPerSecond_set = lm.framesPerSecond_set;
  meta_.kilogramsPerUnit = lm.kilogramsPerUnit;
  meta_.kilogramsPerUnit_set = lm.kilogramsPerUnit_set;
  meta_.colorConfiguration = lm.colorConfiguration;
  meta_.colorManagementSystem = lm.colorManagementSystem;
  meta_.colorConfiguration_set = lm.colorConfiguration_set;
  meta_.colorManagementSystem_set = lm.colorManagementSystem_set;
  meta_.renderSettingsPrimPath = lm.renderSettingsPrimPath;
  meta_.renderSettingsPrimPath_set = lm.renderSettingsPrimPath_set;
  meta_.doc = lm.doc;
  meta_.comment = lm.comment;
  meta_.owner = lm.owner;
  meta_.doc_set = lm.doc_set;
  meta_.comment_set = lm.comment_set;
  meta_.owner_set = lm.owner_set;
}

UsdPrim Stage::GetPseudoRoot() const {
  // Pseudo-root is a virtual prim, we return an invalid prim
  // Users should use GetRootPrims() instead
  return UsdPrim();
}

UsdPrim Stage::GetPrimAtPath(const Path& path) const {
  return GetPrimAtPath(path.str());
}

UsdPrim Stage::GetPrimAtPath(const std::string& path) const {
  if (!root_layer_) return UsdPrim();

  // The path index already maps path -> index; use it directly. (Previously this
  // re-derived the index with a linear scan over every prim -- O(N) per call,
  // O(meshes*prims) over a render, ~8% of an isCoral render in per-mesh
  // material/texture/prototype lookups.)
  const uint32_t idx = root_layer_->index_at_path(path);
  if (idx != UINT32_MAX) {
    return UsdPrim(root_layer_->prim(idx), root_layer_.get(), idx);
  }

  // Instance-proxy lookup: the composed layer stores one prototype subtree,
  // so descendants of another instance root need not have physical PrimSpecs.
  // Find the nearest real ancestor that points at a prototype, remap the
  // requested suffix into that subtree, and return a proxy UsdPrim whose public
  // path remains in the instance namespace. Nearest-first also handles nested
  // instances whose instance root itself is physically present.
  size_t slash = path.size();
  while (slash > 1) {
    slash = path.rfind('/', slash - 1);
    if (slash == std::string::npos || slash == 0) break;
    const std::string instance_root = path.substr(0, slash);
    const uint32_t instance_idx = root_layer_->index_at_path(instance_root);
    if (instance_idx == UINT32_MAX) continue;
    const PrimSpec* instance = root_layer_->prim(instance_idx);
    if (!instance || instance->meta().instance_prototype().empty()) continue;
    const std::string& prototype_root =
        instance->meta().instance_prototype();
    const std::string prototype_path = prototype_root + path.substr(slash);
    const uint32_t prototype_idx = root_layer_->index_at_path(prototype_path);
    if (prototype_idx == UINT32_MAX) continue;
    const PrimSpec* prototype = root_layer_->prim(prototype_idx);
    if (prototype) {
      return UsdPrim(prototype, root_layer_.get(), prototype_idx, Path(path),
                     prototype_root, instance_root);
    }
  }
  return UsdPrim();
}

UsdPrim Stage::GetDefaultPrim() const {
  if (meta_.defaultPrim.empty()) return UsdPrim();
  return GetPrimAtPath("/" + meta_.defaultPrim);
}

std::vector<UsdPrim> Stage::GetRootPrims() const {
  std::vector<UsdPrim> prims;
  if (!root_layer_) return prims;

  const auto& indices = root_layer_->root_indices();
  prims.reserve(indices.size());

  for (uint32_t idx : indices) {
    const PrimSpec* spec = root_layer_->prim(idx);
    if (spec) {
      prims.emplace_back(spec, root_layer_.get(), idx);
    }
  }
  return prims;
}

bool Stage::HasPrimAtPath(const Path& path) const {
  return HasPrimAtPath(path.str());
}

bool Stage::HasPrimAtPath(const std::string& path) const {
  if (!root_layer_) return false;
  return root_layer_->prim_at_path(path) != nullptr;
}

bool Stage::TraverseImpl(uint32_t prim_index, const Layer* layer,
                          const std::function<bool(const UsdPrim&)>& callback) const {
  // Iterative DFS with explicit stack to avoid stack overflow on deep scenes.
  struct Frame {
    uint32_t idx;
    size_t child_pos;
  };
  std::vector<Frame> stack;

  const PrimSpec* spec = layer->prim(prim_index);
  if (!spec) return true;

  UsdPrim prim(spec, layer, prim_index);
  if (!callback(prim)) return false;

  stack.push_back({prim_index, 0});

  while (!stack.empty()) {
    Frame& f = stack.back();
    const PrimSpec* parent = layer->prim(f.idx);
    if (!parent) { stack.pop_back(); continue; }

    const auto& children = parent->child_indices();
    if (f.child_pos >= children.size()) {
      stack.pop_back();
      continue;
    }

    uint32_t child_idx = children[f.child_pos++];
    const PrimSpec* child_spec = layer->prim(child_idx);
    if (!child_spec) continue;

    UsdPrim child_prim(child_spec, layer, child_idx);
    if (!callback(child_prim)) return false;

    stack.push_back({child_idx, 0});
  }
  return true;
}

std::vector<UsdPrim> Stage::GetPrimsOfType(const std::string& typeName) const {
  std::vector<UsdPrim> result;

  Traverse([&](const UsdPrim& prim) {
    if (prim.GetTypeName() == typeName) {
      result.push_back(prim);
    }
    return true;  // Continue traversal
  });

  return result;
}

Layer Stage::Flatten() const {
  if (!root_layer_) return {};

  // Use the Compositor to resolve sublayers, references, payloads, etc.
  Compositor compositor;
  CompositionOptions opts;
  opts.load_payloads = true;
  compositor.SetOptions(opts);

  auto result = compositor.Compose(*root_layer_);
  if (!result) return {};

  return std::move(*result);
}

double Stage::GetStartTimeCode() const {
  return meta_.startTimeCode;
}

double Stage::GetEndTimeCode() const {
  return meta_.endTimeCode;
}

double Stage::GetTimeCodesPerSecond() const {
  return meta_.timeCodesPerSecond;
}

bool Stage::HasTimeSamples() const {
  if (!root_layer_) return false;
  const size_t prim_count = root_layer_->prim_count();
  for (size_t i = 0; i < prim_count; ++i) {
    const PrimSpec* prim = root_layer_->prim(static_cast<uint32_t>(i));
    if (!prim) continue;
    if (prim->has_any_time_samples()) return true;
  }
  return false;
}

bool Stage::HasValueClips() const {
  if (!root_layer_) return false;
  const size_t prim_count = root_layer_->prim_count();
  for (size_t i = 0; i < prim_count; ++i) {
    const PrimSpec* prim = root_layer_->prim(static_cast<uint32_t>(i));
    if (!prim) continue;
    if (prim->meta().clips().as_dictionary()) return true;
  }
  return false;
}

size_t Stage::GetPrimCount() const {
  if (!root_layer_) return 0;
  return root_layer_->prim_count();
}

size_t Stage::GetMemoryUsage() const {
  size_t size = sizeof(Stage);
  size += sizeof(StageMeta);
  size += meta_.defaultPrim.capacity();
  size += meta_.upAxis.capacity();
  size += meta_.doc.capacity();
  size += meta_.owner.capacity();

  if (root_layer_) {
    size += root_layer_->memory_usage();
  }

  for (const auto& sub : sub_layers_) {
    if (sub) {
      size += sub->memory_usage();
    }
  }

  return size;
}

Stage::Stats Stage::GetStats() const {
  Stats s{};
  s.layer_count = 1 + sub_layers_.size();

  if (root_layer_) {
    auto layer_stats = root_layer_->stats();
    s.prim_count = layer_stats.prim_count;
    s.total_properties = layer_stats.total_properties;
  }

  s.memory_bytes = GetMemoryUsage();
  return s;
}

Stage::StaticGeometryReleaseStats Stage::ReleaseStaticGeometryArrays(
    size_t min_array_elements) {
  StaticGeometryReleaseStats stats;
  stats.stage_bytes_before = GetMemoryUsage();
  if (!root_layer_) {
    stats.stage_bytes_after = stats.stage_bytes_before;
    return stats;
  }

  for (size_t i = 0; i < root_layer_->prim_count(); ++i) {
    PrimSpec* prim = root_layer_->prim_mutable(static_cast<uint32_t>(i));
    if (!prim) continue;
    const StaticGeometryReleaseStats one =
        ReleasePrimStaticGeometryArrays(prim, min_array_elements);
    stats.property_count += one.property_count;
    stats.element_count += one.element_count;
    stats.estimated_payload_bytes += one.estimated_payload_bytes;
  }
  stats.stage_bytes_after = GetMemoryUsage();
  return stats;
}

Stage::StaticGeometryReleaseStats Stage::ReleaseStaticGeometryArraysForPrim(
    const UsdPrim& prim, size_t min_array_elements) {
  if (!root_layer_ || !prim.IsValid() || prim.GetLayer() != root_layer_.get()) {
    return {};
  }
  PrimSpec* mutable_prim = root_layer_->prim_mutable(prim.GetIndex());
  if (!mutable_prim || mutable_prim != prim.GetPrimSpec()) return {};
  return ReleasePrimStaticGeometryArrays(mutable_prim, min_array_elements);
}

// ============================================================
// StageBuilder
// ============================================================

StageBuilder::StageBuilder()
    : layer_(std::make_unique<Layer>()),
      layer_builder_(std::make_unique<LayerBuilder>(*layer_)) {}

StageBuilder::~StageBuilder() = default;

void StageBuilder::SetDefaultPrim(const std::string& primName) {
  meta_.defaultPrim = primName;
  meta_.defaultPrim_set = true;
  layer_->meta().defaultPrim = primName;
  layer_->meta().defaultPrim_set = true;
}

void StageBuilder::SetUpAxis(const std::string& axis) {
  meta_.upAxis = axis;
  meta_.upAxis_set = true;
  layer_->meta().upAxis = axis;
  layer_->meta().upAxis_set = true;
}

void StageBuilder::SetMetersPerUnit(double value) {
  meta_.metersPerUnit = value;
  meta_.metersPerUnit_set = true;
  layer_->meta().metersPerUnit = value;
  layer_->meta().metersPerUnit_set = true;
}

void StageBuilder::SetTimeCodesPerSecond(double fps) {
  meta_.timeCodesPerSecond = fps;
  meta_.timeCodesPerSecond_set = true;
  layer_->meta().timeCodesPerSecond = fps;
  layer_->meta().timeCodesPerSecond_set = true;
}

void StageBuilder::SetStartTimeCode(double time) {
  meta_.startTimeCode = time;
  meta_.startTimeCode_set = true;
  layer_->meta().startTimeCode = time;
  layer_->meta().startTimeCode_set = true;
}

void StageBuilder::SetEndTimeCode(double time) {
  meta_.endTimeCode = time;
  meta_.endTimeCode_set = true;
  layer_->meta().endTimeCode = time;
  layer_->meta().endTimeCode_set = true;
}

Stage StageBuilder::Build() {
  layer_builder_->finalize();

  Stage stage;
  stage.SetRootLayer(std::move(*layer_));

  // Reset for potential reuse
  layer_ = std::make_unique<Layer>();
  layer_builder_ = std::make_unique<LayerBuilder>(*layer_);

  return stage;
}

}  // namespace next
}  // namespace lightusd
