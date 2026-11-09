// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment, Inc.

#include "usdz-geometry-optimize.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/attribute.hh"
#include "core/path.hh"
#include "core/prim-spec.hh"
#include "core/property.hh"
#include "core/relationship.hh"
#include "layer.hh"
#include "value-types.hh"
#include "xform.hh"

namespace lightusd {
namespace usdz {
namespace {

std::string ViewToString(tstring_view v) {
  return std::string(v.data(), v.size());
}

std::string JoinPrimPath(const std::string &parent, const std::string &name) {
  if (parent.empty() || parent == "/") {
    return "/" + name;
  }
  return parent + "/" + name;
}

bool IsAttrStaticValue(const Property &prop) {
  if (!prop.is_attribute()) {
    return false;
  }
  const Attribute *attr = prop.get_attribute_or_null();
  return attr && attr->has_value() && !attr->has_timesamples() &&
         !attr->is_connection() && !attr->is_blocked();
}

bool StartsWithPath(const std::string &path, const std::string &prefix) {
  return path == prefix ||
         (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
          path[prefix.size()] == '/');
}

template <typename T>
bool GetStaticValue(const PrimSpec &ps, const std::string &name, T *out) {
  auto it = ps.props().find(name);
  if (it == ps.props().end() || !IsAttrStaticValue(it->second)) {
    return false;
  }
  return it->second.get_attribute().get_value(out);
}

bool HasTimesamples(const PrimSpec &ps) {
  for (const auto &kv : ps.props()) {
    if (kv.second.is_attribute()) {
      const Attribute *attr = kv.second.get_attribute_or_null();
      if (attr && attr->has_timesamples()) {
        return true;
      }
    }
  }
  return false;
}

bool ReadSingleRelationshipTarget(const PrimSpec &ps, const std::string &name,
                                  std::string *target_path) {
  if (!target_path) {
    return false;
  }
  target_path->clear();
  auto it = ps.props().find(name);
  if (it == ps.props().end()) {
    return true;
  }
  if (!it->second.is_relationship()) {
    return false;
  }
  std::vector<Path> targets = it->second.get_relationTargets();
  if (targets.empty()) {
    return true;
  }
  if (targets.size() != 1 || !targets[0].prop_part().empty()) {
    return false;
  }
  *target_path = ViewToString(targets[0].prim_part());
  return true;
}

bool IsSupportedMeshPropName(const std::string &name) {
  static const char *kAllowed[] = {
      "points",
      "faceVertexCounts",
      "faceVertexIndices",
      "material:binding",
      "xformOpOrder",
      "xformOp:translate",
      "xformOp:transform",
      "extent",
      "apiSchemas",
      "assetInfo",
      "normals",
      "primvars:displayColor",
      "primvars:displayOpacity",
      "subdivisionScheme",
      "subsetFamily:materialBind:familyType",
  };
  for (const char *allowed : kAllowed) {
    if (name == allowed) {
      return true;
    }
  }
  return name.find("primvars:st") == 0;
}

bool HasUnsupportedMeshProps(const PrimSpec &ps) {
  for (const auto &kv : ps.props()) {
    if (!IsSupportedMeshPropName(kv.first)) {
      return true;
    }
  }
  return false;
}

bool HasAuthoredInheritedVisualState(const PrimSpec &ps) {
  return ps.props().find("visibility") != ps.props().end() ||
         ps.props().find("purpose") != ps.props().end();
}

bool HasSupportedSubdivisionScheme(const PrimSpec &ps) {
  auto it = ps.props().find("subdivisionScheme");
  if (it == ps.props().end()) {
    return true;
  }
  value::token scheme;
  return IsAttrStaticValue(it->second) &&
         it->second.get_attribute().get_value(&scheme) &&
         scheme.str() == "none";
}

bool IsIdentity(const value::matrix4d &m) {
  return is_identity(m);
}

bool GetStaticLocalMatrix(const PrimSpec &ps, value::matrix4d *out) {
  if (!out) {
    return false;
  }
  *out = value::matrix4d::identity();

  std::vector<value::token> order;
  auto order_it = ps.props().find("xformOpOrder");
  if (order_it != ps.props().end()) {
    if (!IsAttrStaticValue(order_it->second) ||
        !order_it->second.get_attribute().get_value(&order)) {
      return false;
    }
  }

  if (order.empty()) {
    if (ps.props().find("xformOp:translate") == ps.props().end() &&
        ps.props().find("xformOp:transform") == ps.props().end()) {
      return true;
    }
    if (ps.props().find("xformOp:translate") != ps.props().end()) {
      order.emplace_back("xformOp:translate");
    }
    if (ps.props().find("xformOp:transform") != ps.props().end()) {
      order.emplace_back("xformOp:transform");
    }
  }

  value::matrix4d local = value::matrix4d::identity();
  for (const value::token &tok : order) {
    const std::string op = tok.str();
    if (op == "!resetXformStack!") {
      local = value::matrix4d::identity();
      continue;
    }
    if (op == "xformOp:translate") {
      value::double3 t{};
      value::float3 tf{};
      value::matrix4d m = value::matrix4d::identity();
      auto it = ps.props().find(op);
      if (it == ps.props().end() || !IsAttrStaticValue(it->second)) {
        return false;
      }
      if (it->second.get_attribute().get_value(&t)) {
        m.m[3][0] = t[0];
        m.m[3][1] = t[1];
        m.m[3][2] = t[2];
      } else if (it->second.get_attribute().get_value(&tf)) {
        m.m[3][0] = static_cast<double>(tf[0]);
        m.m[3][1] = static_cast<double>(tf[1]);
        m.m[3][2] = static_cast<double>(tf[2]);
      } else {
        return false;
      }
      local = value::Mult(local, m);
    } else if (op == "xformOp:transform") {
      value::matrix4d m{};
      auto it = ps.props().find(op);
      if (it == ps.props().end() || !IsAttrStaticValue(it->second) ||
          !it->second.get_attribute().get_value(&m)) {
        return false;
      }
      local = value::Mult(local, m);
    } else {
      return false;
    }
  }

  *out = local;
  return true;
}

struct MeshSource {
  std::vector<PrimSpec> *siblings{nullptr};
  size_t sibling_index{0};
  std::string path;
  bool keep_for_material{false};
};

struct MeshFragment {
  MeshSource source;
  std::string material_path;
  value::matrix4d world_matrix{value::matrix4d::identity()};
  std::vector<value::point3f> points;
  std::vector<int32_t> face_counts;
  std::vector<int32_t> face_indices;
  std::vector<value::normal3f> normals;
  std::map<std::string, std::vector<value::texcoord2f>> texcoords;
  std::vector<value::color3f> display_color;
  std::vector<float> display_opacity;
};

struct TraverseState {
  value::matrix4d world_matrix{value::matrix4d::identity()};
  std::string material_path;
  bool xform_valid{true};
  bool inherited_visual_state_valid{true};
  bool material_binding_valid{true};
};

bool IsPerVertexInterpolation(Interpolation interpolation) {
  return interpolation == Interpolation::Vertex ||
         interpolation == Interpolation::Varying;
}

template <typename T>
struct RemappableAttribute {
  std::vector<T> values;
  std::vector<int32_t> indices;
  Interpolation interpolation{Interpolation::Vertex};
};

template <typename T>
bool LoadRemappableAttribute(const PrimSpec &ps, const std::string &name,
                             size_t num_points, size_t num_face_vertices,
                             RemappableAttribute<T> *out) {
  if (!out) {
    return false;
  }
  auto it = ps.props().find(name);
  if (it == ps.props().end()) {
    return true;
  }
  if (!IsAttrStaticValue(it->second) ||
      !it->second.get_attribute().get_value(&out->values)) {
    return false;
  }
  bool has_interpolation = it->second.get_attribute().metas().has_interpolation();
  if (has_interpolation) {
    out->interpolation =
        it->second.get_attribute().metas().get_interpolation_enum();
  }

  auto indices_it = ps.props().find(name + ":indices");
  size_t indexed_element_count = 0;
  if (indices_it != ps.props().end()) {
    if (!IsAttrStaticValue(indices_it->second) ||
        !indices_it->second.get_attribute().get_value(&out->indices)) {
      return false;
    }
    indexed_element_count = out->indices.size();
    for (int32_t idx : out->indices) {
      if (idx < 0 || static_cast<size_t>(idx) >= out->values.size()) {
        return false;
      }
    }
  }

  if (!has_interpolation) {
    const size_t authored_count =
        out->indices.empty() ? out->values.size() : indexed_element_count;
    if (authored_count == num_face_vertices) {
      out->interpolation = Interpolation::FaceVarying;
    } else if (authored_count == num_points) {
      out->interpolation = Interpolation::Vertex;
    } else {
      return false;
    }
  }

  const bool face_varying =
      out->interpolation == Interpolation::FaceVarying;
  const bool per_vertex = IsPerVertexInterpolation(out->interpolation);
  if (!face_varying && !per_vertex) {
    return false;
  }

  const size_t element_count = face_varying ? num_face_vertices : num_points;
  if (!out->indices.empty() && out->indices.size() != element_count) {
    return false;
  }
  if (out->indices.empty() && out->values.size() != element_count) {
    return false;
  }

  return true;
}

template <typename T>
bool HasRemappableAttribute(const RemappableAttribute<T> &attr) {
  return !attr.values.empty();
}

template <typename T>
bool RemapAttributeValue(const RemappableAttribute<T> &attr, size_t fv,
                         size_t point_index, T *out) {
  if (!out || attr.values.empty()) {
    return false;
  }
  size_t element_index = 0;
  if (attr.interpolation == Interpolation::FaceVarying) {
    element_index = fv;
  } else if (IsPerVertexInterpolation(attr.interpolation)) {
    element_index = point_index;
  } else {
    return false;
  }

  if (!attr.indices.empty()) {
    if (element_index >= attr.indices.size()) {
      return false;
    }
    const int32_t value_index = attr.indices[element_index];
    if (value_index < 0 || static_cast<size_t>(value_index) >= attr.values.size()) {
      return false;
    }
    *out = attr.values[static_cast<size_t>(value_index)];
    return true;
  }

  if (element_index >= attr.values.size()) {
    return false;
  }
  *out = attr.values[element_index];
  return true;
}

bool TransformNormalWithInverse(const value::matrix4d &inv,
                                const value::normal3f &n,
                                value::normal3f *out) {
  if (!out) {
    return false;
  }
  double x = inv.m[0][0] * double(n[0]) + inv.m[0][1] * double(n[1]) +
             inv.m[0][2] * double(n[2]);
  double y = inv.m[1][0] * double(n[0]) + inv.m[1][1] * double(n[1]) +
             inv.m[1][2] * double(n[2]);
  double z = inv.m[2][0] * double(n[0]) + inv.m[2][1] * double(n[1]) +
             inv.m[2][2] * double(n[2]);
  const double len = std::sqrt(x * x + y * y + z * z);
  if (len > 1.0e-20) {
    x /= len;
    y /= len;
    z /= len;
  }
  *out = value::normal3f{float(x), float(y), float(z)};
  return true;
}

bool ExtractMeshCandidate(PrimSpec *ps, const std::string &path,
                          std::vector<PrimSpec> *siblings,
                          size_t sibling_index,
                          const TraverseState &state,
                          const UsdzConvertOptions &options,
                          std::vector<MeshFragment> *fragments) {
  if (!ps || !fragments || !state.xform_valid ||
      !state.inherited_visual_state_valid || !state.material_binding_valid ||
      !siblings) {
    return false;
  }
  if (ps->typeName() != "Mesh") {
    return false;
  }
  if (HasTimesamples(*ps) || HasUnsupportedMeshProps(*ps) ||
      !HasSupportedSubdivisionScheme(*ps)) {
    return false;
  }

  std::vector<value::point3f> points;
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  if (!GetStaticValue(*ps, "points", &points) ||
      !GetStaticValue(*ps, "faceVertexCounts", &counts) ||
      !GetStaticValue(*ps, "faceVertexIndices", &indices)) {
    return false;
  }
  if (points.empty() || counts.empty() || indices.empty()) {
    return false;
  }
  int64_t index_count = 0;
  for (int32_t count : counts) {
    if (count < 3) {
      return false;
    }
    index_count += count;
  }
  if (index_count != static_cast<int64_t>(indices.size())) {
    return false;
  }
  for (int32_t idx : indices) {
    if (idx < 0 || static_cast<size_t>(idx) >= points.size()) {
      return false;
    }
  }
  if (counts.size() >
          static_cast<size_t>(std::max(0, options.mesh_merge_max_input_faces)) ||
      points.size() >
          static_cast<size_t>(std::max(0, options.mesh_merge_max_input_points))) {
    return false;
  }

  RemappableAttribute<value::normal3f> normals;
  if (!LoadRemappableAttribute(*ps, "normals", points.size(), indices.size(),
                               &normals)) {
    return false;
  }

  std::map<std::string, RemappableAttribute<value::texcoord2f>> texcoords;
  for (const auto &kv : ps->props()) {
    if (kv.first.find("primvars:st") != 0 || kv.first.find(":indices") != std::string::npos) {
      continue;
    }
    RemappableAttribute<value::texcoord2f> uv;
    if (!LoadRemappableAttribute(*ps, kv.first, points.size(), indices.size(),
                                 &uv)) {
      return false;
    }
    texcoords[kv.first] = std::move(uv);
  }

  std::vector<value::color3f> display_color;
  auto color_it = ps->props().find("primvars:displayColor");
  if (color_it != ps->props().end()) {
    if (!IsAttrStaticValue(color_it->second) ||
        !color_it->second.get_attribute().metas().has_interpolation() ||
        color_it->second.get_attribute().metas().get_interpolation_enum() !=
            Interpolation::Constant ||
        !color_it->second.get_attribute().get_value(&display_color) ||
        display_color.size() != 1) {
      return false;
    }
  }
  std::vector<float> display_opacity;
  auto opacity_it = ps->props().find("primvars:displayOpacity");
  if (opacity_it != ps->props().end()) {
    if (!IsAttrStaticValue(opacity_it->second) ||
        !opacity_it->second.get_attribute().metas().has_interpolation() ||
        opacity_it->second.get_attribute().metas().get_interpolation_enum() !=
            Interpolation::Constant ||
        !opacity_it->second.get_attribute().get_value(&display_opacity) ||
        display_opacity.size() != 1) {
      return false;
    }
  }

  std::vector<std::pair<std::string, std::vector<int32_t>>> subsets;
  std::vector<uint8_t> face_coverage(counts.size(), uint8_t{0});
  for (const PrimSpec &child : ps->children()) {
    if (child.typeName() != "GeomSubset") {
      continue;
    }
    value::token element_type;
    if (!GetStaticValue(child, "elementType", &element_type) ||
        element_type.str() != "face") {
      return false;
    }
    std::vector<int32_t> subset_faces;
    if (!GetStaticValue(child, "indices", &subset_faces) ||
        subset_faces.empty()) {
      return false;
    }
    for (int32_t face : subset_faces) {
      if (face < 0 || static_cast<size_t>(face) >= counts.size()) {
        return false;
      }
      uint8_t &covered = face_coverage[static_cast<size_t>(face)];
      if (covered != 0) {
        return false;
      }
      covered = uint8_t{1};
    }
    std::string subset_binding;
    if (!ReadSingleRelationshipTarget(child, "material:binding",
                                      &subset_binding)) {
      return false;
    }
    subsets.emplace_back(subset_binding.empty() ? state.material_path
                                                : subset_binding,
                         std::move(subset_faces));
  }
  if (subsets.empty()) {
    std::vector<int32_t> all_faces;
    all_faces.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); i++) {
      all_faces.push_back(static_cast<int32_t>(i));
    }
    subsets.emplace_back(state.material_path, std::move(all_faces));
  } else {
    std::vector<int32_t> uncovered_faces;
    for (size_t i = 0; i < face_coverage.size(); i++) {
      if (face_coverage[i] == 0) {
        uncovered_faces.push_back(static_cast<int32_t>(i));
      }
    }
    if (!uncovered_faces.empty()) {
      subsets.emplace_back(state.material_path, std::move(uncovered_faces));
    }
  }

  for (const auto &subset : subsets) {
    if (!subset.first.empty() && StartsWithPath(subset.first, path)) {
      return false;
    }
  }

  std::vector<size_t> face_offsets(counts.size(), 0);
  size_t off = 0;
  for (size_t i = 0; i < counts.size(); i++) {
    face_offsets[i] = off;
    off += static_cast<size_t>(counts[i]);
  }

  for (const auto &subset : subsets) {
    MeshFragment fragment;
    fragment.source.siblings = siblings;
    fragment.source.sibling_index = sibling_index;
    fragment.source.path = path;
    fragment.material_path = subset.first;
    fragment.source.keep_for_material =
        !fragment.material_path.empty() &&
        StartsWithPath(fragment.material_path, path);
    fragment.world_matrix = state.world_matrix;
    fragment.display_color = display_color;
    fragment.display_opacity = display_opacity;

    int32_t next_index = 0;
    for (int32_t face : subset.second) {
      const size_t face_index = static_cast<size_t>(face);
      const int32_t count = counts[face_index];
      fragment.face_counts.push_back(count);
      const size_t face_offset = face_offsets[face_index];
      for (int32_t j = 0; j < count; j++) {
        const size_t fv = face_offset + static_cast<size_t>(j);
        const int32_t point_index = indices[fv];
        fragment.points.push_back(points[static_cast<size_t>(point_index)]);
        fragment.face_indices.push_back(next_index++);
        if (HasRemappableAttribute(normals)) {
          value::normal3f normal{};
          if (!RemapAttributeValue(normals, fv,
                                   static_cast<size_t>(point_index),
                                   &normal)) {
            return false;
          }
          fragment.normals.push_back(normal);
        }
        for (const auto &uv_kv : texcoords) {
          value::texcoord2f uv{};
          if (!RemapAttributeValue(uv_kv.second, fv,
                                   static_cast<size_t>(point_index), &uv)) {
            return false;
          }
          fragment.texcoords[uv_kv.first].push_back(uv);
        }
      }
    }
    fragments->push_back(std::move(fragment));
  }

  return !subsets.empty();
}

void CollectMeshesRec(PrimSpec *ps, const std::string &path,
                      std::vector<PrimSpec> *siblings, size_t sibling_index,
                      TraverseState state,
                      const UsdzConvertOptions &options,
                      std::vector<MeshFragment> *out,
                      GeometryOptimizationStats *stats,
                      int depth) {
  if (!ps || !out) {
    return;
  }
  if (depth > 512) {
    return;
  }
  const bool active = !ps->metas().has_active() || ps->metas().get_active();
  if (!active) {
    return;
  }

  std::string binding;
  if (!ReadSingleRelationshipTarget(*ps, "material:binding", &binding)) {
    state.material_binding_valid = false;
  } else if (!binding.empty()) {
    state.material_path = binding;
  }
  if (HasAuthoredInheritedVisualState(*ps)) {
    state.inherited_visual_state_valid = false;
  }

  value::matrix4d local = value::matrix4d::identity();
  if (!GetStaticLocalMatrix(*ps, &local)) {
    state.xform_valid = false;
  } else if (state.xform_valid && !IsIdentity(local)) {
    state.world_matrix = value::Mult(state.world_matrix, local);
  }

  if (ps->typeName() == "Mesh") {
    if (stats) {
      stats->num_meshes_before++;
    }
    const size_t before = out->size();
    if (ExtractMeshCandidate(ps, path, siblings, sibling_index, state, options,
                             out)) {
      if (stats) {
        stats->num_meshes_eligible++;
      }
      if (out->size() == before) {
        return;
      }
    } else if (stats) {
      stats->num_meshes_skipped++;
    }
  }

  std::vector<PrimSpec> &children = ps->children();
  for (size_t i = 0; i < children.size(); i++) {
    CollectMeshesRec(&children[i], JoinPrimPath(path, children[i].name()),
                     &children, i, state, options, out, stats, depth + 1);
  }
}

std::vector<MeshFragment> CollectMeshes(Layer *layer,
                                        const UsdzConvertOptions &options,
                                        GeometryOptimizationStats *stats) {
  std::vector<MeshFragment> out;
  if (!layer) {
    return out;
  }
  for (auto &kv : layer->primspecs()) {
    TraverseState state;
    CollectMeshesRec(&kv.second, "/" + kv.second.name(), nullptr, 0, state,
                     options, &out, stats, /*depth*/ 0);
  }
  return out;
}

std::string MakeGroupKey(const MeshFragment &fragment) {
  std::ostringstream ss;
  ss << fragment.material_path;
  ss << "|n=" << (!fragment.normals.empty());
  ss << "|uv";
  for (const auto &kv : fragment.texcoords) {
    ss << "," << kv.first;
  }
  ss << "|dc=" << fragment.display_color.size();
  if (!fragment.display_color.empty()) {
    ss << ":" << fragment.display_color[0].r << "," << fragment.display_color[0].g
       << "," << fragment.display_color[0].b;
  }
  ss << "|do=" << fragment.display_opacity.size();
  if (!fragment.display_opacity.empty()) {
    ss << ":" << fragment.display_opacity[0];
  }
  return ss.str();
}

std::string MakeUniqueRootName(const Layer &layer) {
  const char *base = "__LightUSD_MeshMerge";
  if (!layer.has_primspec(base)) {
    return std::string(base);
  }
  for (size_t i = 1; i < 100000; i++) {
    const std::string name = std::string(base) + "_" + std::to_string(i);
    if (!layer.has_primspec(name)) {
      return name;
    }
  }
  return std::string();
}

Property MakeAttributeProperty(const std::vector<value::point3f> &value) {
  Attribute attr;
  attr.set_value(value);
  return Property(std::move(attr));
}

Property MakeAttributeProperty(const std::vector<int32_t> &value) {
  Attribute attr;
  attr.set_value(value);
  return Property(std::move(attr));
}

Property MakeAttributeProperty(const std::vector<value::normal3f> &value,
                               Interpolation interpolation) {
  Attribute attr;
  attr.set_value(value);
  attr.metas().set_interpolation_enum(interpolation);
  return Property(std::move(attr));
}

Property MakeAttributeProperty(const std::vector<value::texcoord2f> &value,
                               Interpolation interpolation) {
  Attribute attr;
  attr.set_value(value);
  attr.metas().set_interpolation_enum(interpolation);
  return Property(std::move(attr));
}

Property MakeAttributeProperty(const std::vector<value::color3f> &value,
                               Interpolation interpolation) {
  Attribute attr;
  attr.set_value(value);
  attr.metas().set_interpolation_enum(interpolation);
  return Property(std::move(attr));
}

Property MakeAttributeProperty(const std::vector<float> &value,
                               Interpolation interpolation) {
  Attribute attr;
  attr.set_value(value);
  attr.metas().set_interpolation_enum(interpolation);
  return Property(std::move(attr));
}

Property MakeTokenAttributeProperty(const std::string &value,
                                    bool uniform = true) {
  Attribute attr;
  attr.set_value(value::token(value));
  attr.variability() = uniform ? Variability::Uniform : Variability::Varying;
  return Property(std::move(attr));
}

Property MakeMaterialBindingProperty(const std::string &material_path) {
  Relationship rel;
  if (!material_path.empty()) {
    rel.set(Path(material_path, ""));
  }
  return Property(std::move(rel));
}

bool AppendAggregateMesh(const std::vector<const MeshFragment *> &group,
                         size_t aggregate_index, PrimSpec *root,
                         GeometryOptimizationStats *stats) {
  if (!root || group.size() < 2) {
    return false;
  }

  std::vector<value::point3f> points;
  std::vector<int32_t> counts;
  std::vector<int32_t> indices;
  size_t face_count = 0;
  size_t point_count = 0;
  for (const MeshFragment *fragment : group) {
    face_count += fragment->face_counts.size();
    point_count += fragment->points.size();
  }
  if (point_count > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return false;
  }
  if (face_count > static_cast<size_t>((std::numeric_limits<size_t>::max)() / 3)) {
    return false;
  }
  points.reserve(point_count);
  counts.reserve(face_count);
  indices.reserve(face_count * 3);

  int32_t point_base = 0;
  std::vector<value::normal3f> normals;
  std::map<std::string, std::vector<value::texcoord2f>> texcoords;
  const bool has_normals = !group.front()->normals.empty();
  if (has_normals) {
    normals.reserve(point_count);
  }
  for (const auto &kv : group.front()->texcoords) {
    texcoords[kv.first].reserve(point_count);
  }

  for (const MeshFragment *fragment : group) {
    for (const value::point3f &p : fragment->points) {
      points.push_back(transform(fragment->world_matrix, p));
    }
    counts.insert(counts.end(), fragment->face_counts.begin(),
                  fragment->face_counts.end());
    for (int32_t idx : fragment->face_indices) {
      indices.push_back(point_base + idx);
    }
    if (has_normals) {
      value::matrix4d normal_matrix_inverse;
      if (!inverse(fragment->world_matrix, normal_matrix_inverse)) {
        return false;
      }
      for (const value::normal3f &n : fragment->normals) {
        value::normal3f normal{};
        if (!TransformNormalWithInverse(normal_matrix_inverse, n, &normal)) {
          return false;
        }
        normals.push_back(normal);
      }
    }
    for (const auto &uv_kv : fragment->texcoords) {
      std::vector<value::texcoord2f> &dst = texcoords[uv_kv.first];
      dst.insert(dst.end(), uv_kv.second.begin(), uv_kv.second.end());
    }
    point_base += static_cast<int32_t>(fragment->points.size());
  }

  PrimSpec mesh(Specifier::Def, "Mesh",
                "Merged_" + std::to_string(aggregate_index));
  mesh.props()["points"] = MakeAttributeProperty(points);
  mesh.props()["faceVertexCounts"] = MakeAttributeProperty(counts);
  mesh.props()["faceVertexIndices"] = MakeAttributeProperty(indices);
  mesh.props()["subdivisionScheme"] = MakeTokenAttributeProperty("none");
  if (!normals.empty()) {
    mesh.props()["normals"] =
        MakeAttributeProperty(normals, Interpolation::FaceVarying);
  }
  for (const auto &kv : texcoords) {
    mesh.props()[kv.first] =
        MakeAttributeProperty(kv.second, Interpolation::FaceVarying);
  }
  if (!group.front()->display_color.empty()) {
    mesh.props()["primvars:displayColor"] =
        MakeAttributeProperty(group.front()->display_color,
                              Interpolation::Constant);
  }
  if (!group.front()->display_opacity.empty()) {
    mesh.props()["primvars:displayOpacity"] =
        MakeAttributeProperty(group.front()->display_opacity,
                              Interpolation::Constant);
  }
  if (!group.front()->material_path.empty()) {
    mesh.props()["material:binding"] =
        MakeMaterialBindingProperty(group.front()->material_path);
  }

  root->children().push_back(std::move(mesh));
  if (stats) {
    stats->num_mesh_aggregates++;
    stats->num_faces_merged += counts.size();
    stats->num_points_merged += points.size();
  }
  return true;
}

void DeactivateMergedMeshes(const std::vector<MeshSource> &sources) {
  std::unordered_map<std::vector<PrimSpec> *, std::vector<size_t>> by_parent;
  for (const MeshSource &source : sources) {
    if (source.siblings && !source.keep_for_material) {
      by_parent[source.siblings].push_back(source.sibling_index);
    }
  }
  for (auto &kv : by_parent) {
    std::vector<size_t> indices = std::move(kv.second);
    std::sort(indices.begin(), indices.end(), std::greater<size_t>());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (size_t idx : indices) {
      if (idx < kv.first->size()) {
        PrimSpec &ps = (*kv.first)[idx];
        ps.metas().set_active(false);
        ps.props().erase("points");
        ps.props().erase("faceVertexCounts");
        ps.props().erase("faceVertexIndices");
        ps.props().erase("normals");
        for (auto it = ps.props().begin(); it != ps.props().end();) {
          if (it->first.find("primvars:") == 0) {
            it = ps.props().erase(it);
          } else {
            ++it;
          }
        }
      }
    }
  }
}

size_t CountMeshesRec(const PrimSpec &ps, int depth) {
  if (depth > 512) {
    return 0;
  }
  const bool active = !ps.metas().has_active() || ps.metas().get_active();
  size_t count = (active && ps.typeName() == "Mesh" &&
                  ps.props().find("faceVertexCounts") != ps.props().end())
                     ? 1
                     : 0;
  for (const PrimSpec &child : ps.children()) {
    count += CountMeshesRec(child, depth + 1);
  }
  return count;
}

size_t CountMeshes(const Layer &layer) {
  size_t count = 0;
  for (const auto &kv : layer.primspecs()) {
    count += CountMeshesRec(kv.second, /*depth*/ 0);
  }
  return count;
}

}  // namespace

bool OptimizeGeometryInLayer(const UsdzConvertOptions &options, Layer *layer,
                             GeometryOptimizationStats *stats,
                             std::string *warn, std::string *err) {
  if (!layer) {
    if (err) {
      *err = "OptimizeGeometryInLayer: layer is null.";
    }
    return false;
  }
  if (stats) {
    *stats = GeometryOptimizationStats{};
  }

  if (options.geometry_optimization == GeometryOptimizationMode::Off) {
    if (stats) {
      stats->num_meshes_before = CountMeshes(*layer);
      stats->num_meshes_after = stats->num_meshes_before;
    }
    return true;
  }

  if (warn) {
    *warn += "Geometry optimization mergeMeshes breaks original mesh prim "
             "paths and hierarchy for realtime delivery.\n";
  }

  std::vector<MeshFragment> candidates = CollectMeshes(layer, options, stats);

  std::map<std::string, std::vector<const MeshFragment *>> groups;
  for (const MeshFragment &candidate : candidates) {
    groups[MakeGroupKey(candidate)].push_back(&candidate);
  }

  const std::string root_name = MakeUniqueRootName(*layer);
  if (root_name.empty()) {
    if (err) {
      *err = "OptimizeGeometryInLayer: failed to choose aggregate root name.";
    }
    return false;
  }
  PrimSpec root(Specifier::Def, "Xform", root_name);

  std::vector<const MeshFragment *> merged_fragments;
  size_t aggregate_index = 0;
  const size_t min_group =
      static_cast<size_t>(std::max(2, options.mesh_merge_min_group_size));
  const size_t max_aggregate_faces =
      static_cast<size_t>(std::max(1, options.mesh_merge_max_aggregate_faces));

  for (const auto &kv : groups) {
    std::vector<const MeshFragment *> chunk;
    size_t chunk_faces = 0;
    for (const MeshFragment *candidate : kv.second) {
      const size_t faces = candidate->face_counts.size();
      if (!chunk.empty() && chunk_faces + faces > max_aggregate_faces) {
        if (chunk.size() >= min_group &&
            AppendAggregateMesh(chunk, aggregate_index++, &root, stats)) {
          merged_fragments.insert(merged_fragments.end(), chunk.begin(),
                                  chunk.end());
        }
        chunk.clear();
        chunk_faces = 0;
      }
      chunk.push_back(candidate);
      chunk_faces += faces;
    }
    if (chunk.size() >= min_group &&
        AppendAggregateMesh(chunk, aggregate_index++, &root, stats)) {
      merged_fragments.insert(merged_fragments.end(), chunk.begin(),
                              chunk.end());
    }
  }

  if (!root.children().empty()) {
    if (!layer->emplace_primspec(root_name, std::move(root))) {
      if (err) {
        *err = "OptimizeGeometryInLayer: failed to add aggregate root.";
      }
      return false;
    }
    std::map<std::string, size_t> source_fragment_count;
    std::map<std::string, size_t> merged_fragment_count;
    std::map<std::string, MeshSource> sources;
    for (const MeshFragment &fragment : candidates) {
      source_fragment_count[fragment.source.path]++;
      sources[fragment.source.path] = fragment.source;
    }
    for (const MeshFragment *fragment : merged_fragments) {
      merged_fragment_count[fragment->source.path]++;
    }

    std::vector<MeshSource> erase_sources;
    for (const auto &kv : sources) {
      if (merged_fragment_count[kv.first] == source_fragment_count[kv.first]) {
        erase_sources.push_back(kv.second);
      }
    }
    DeactivateMergedMeshes(erase_sources);
    if (stats) {
      for (const MeshSource &source : erase_sources) {
        if (!source.keep_for_material) {
          stats->num_meshes_merged++;
        }
      }
    }
  }

  if (stats) {
    stats->num_meshes_after = CountMeshes(*layer);
  }
  return true;
}

}  // namespace usdz
}  // namespace lightusd
