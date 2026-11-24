// SPDX-License-Identifier: Apache-2.0
#include "geometry-optimize.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "c/lightusd-core-c.h"
#include "diff/layer-diff.hh"
#include "eval/xform-eval.hh"
#include "layer-edit.hh"
#include "schema/usd-shade.hh"
#include "stage/stage.hh"
#include "types/type-info.hh"
#include "writer/usda-writer.hh"

namespace lightusd::usdz {
namespace {
bool Starts(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}
bool Ends(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
std::string Parent(const std::string& s) {
  const size_t slash = s.rfind('/');
  return slash == std::string::npos ? std::string() : s.substr(0, slash);
}
struct Budget {
  size_t left;
  bool exceeded{false};
  bool charge(size_t count, size_t stride) {
    if (stride && count > left / stride) {
      exceeded = true;
      return false;
    }
    left -= count * stride;
    return true;
  }
};
struct Array {
  TypeId type{TypeId::Invalid};
  size_t stride{0};
  std::vector<uint8_t> bytes;
};
struct Mesh {
  std::string path;
  bool per_edge_sharpness{false};
  PrimSpec spec;
  std::vector<PrimSpec> subsets;
  std::map<std::string, Array> arrays;
  size_t points{0}, faces{0};
};
bool ReadInts(const PrimSpec& p, const char* name, std::vector<int32_t>* out) {
  const Value* v = p.property_value(name);
  if (!v || !v->is_array() || v->type_id() != TypeId::Int) return false;
  const auto data = v->as_int_array();
  out->assign(data.begin(), data.end());
  return true;
}
// Row-vector affine transform; normals use inverse transpose, not the point
// matrix.
bool Transform(Array* array, const double* m, bool normal, bool point) {
  const TypeInfo* info = GetTypeInfo(array->type);
  if (!info || info->component_count != 3 ||
      (info->component_type != TypeId::Float &&
       info->component_type != TypeId::Double))
    return false;
  double t[16];
  if (normal)
    lu_mat4_invert(m, t);
  else
    std::copy(m, m + 16, t);
  for (size_t offset = 0; offset < array->bytes.size();
       offset += array->stride) {
    double v[3];
    if (info->component_type == TypeId::Float) {
      float f[3];
      std::memcpy(f, array->bytes.data() + offset, sizeof(f));
      for (size_t i = 0; i < 3; ++i) v[i] = f[i];
    } else
      std::memcpy(v, array->bytes.data() + offset, sizeof(v));
    double r[3];
    for (size_t j = 0; j < 3; ++j)
      r[j] = normal
                 ? v[0] * t[j * 4] + v[1] * t[j * 4 + 1] + v[2] * t[j * 4 + 2]
                 : v[0] * t[j] + v[1] * t[j + 4] + v[2] * t[j + 8] +
                       (point ? t[j + 12] : 0);
    if (normal) {
      const double length = std::hypot(r[0], r[1], r[2]);
      if (length > 0)
        for (double& x : r) x /= length;
    }
    for (double x : r)
      if (!std::isfinite(x)) return false;
    if (info->component_type == TypeId::Float) {
      float f[3];
      for (size_t i = 0; i < 3; ++i) {
        f[i] = static_cast<float>(r[i]);
        if (!std::isfinite(f[i])) return false;
      }
      std::memcpy(array->bytes.data() + offset, f, sizeof(f));
    } else
      std::memcpy(array->bytes.data() + offset, r, sizeof(r));
  }
  return true;
}
bool IsNormal(TypeId t) {
  return t == TypeId::Normal3f || t == TypeId::Normal3d ||
         t == TypeId::Normal3h;
}
bool IsPoint(TypeId t) {
  return t == TypeId::Point3f || t == TypeId::Point3d || t == TypeId::Point3h;
}
bool IsVector(TypeId t) {
  return t == TypeId::Vector3f || t == TypeId::Vector3d ||
         t == TypeId::Vector3h;
}
bool MakeArray(const Value& v, Array* a, Budget* budget) {
  const TypeInfo* info = GetTypeInfo(v.type_id());
  const TypeInfo* component =
      info ? GetTypeInfo(info->component_type) : nullptr;
  if (!v.is_array() || !info || !info->size ||
      (!(info->flags & LU_TYPE_FLAG_NUMERIC) &&
       (!component || !(component->flags & LU_TYPE_FLAG_NUMERIC))))
    return false;
  if (!budget->charge(v.array_size(), info->size)) return false;
  Value flat = v.materialized_copy();
  if (flat.array_size() && !flat.raw_data()) return false;
  a->type = v.type_id();
  a->stride = info->size;
  a->bytes.resize(v.array_size() * info->size);
  if (!a->bytes.empty())
    std::memcpy(a->bytes.data(), flat.raw_data(), a->bytes.size());
  return true;
}
Value ToValue(const Array& a) {
  return Value::MakeArrayFromRaw(a.type, a.bytes.data(),
                                 a.bytes.size() / a.stride);
}

bool Prepare(const Stage& stage, const PrimSpec& source,
             const MeshMergeOptions& options, Mesh* mesh, Budget* budget) {
  if (source.path().str() == "/" + stage.GetRootLayer()->meta().defaultPrim)
    return false;
  mesh->path = source.path().str();
  if (Parent(mesh->path).empty())
    return false;  // Public root prims remain entry points.
  const Value* source_faces = source.property_value("faceVertexCounts");
  const Value* source_points = source.property_value("points");
  if (!source_faces || !source_points ||
      source_faces->array_size() > options.max_input_faces ||
      source_points->array_size() > options.max_input_points)
    return false;
  if (!budget->charge(source.memory_usage(), 1)) return false;
  mesh->spec = source.Clone();
  mesh->spec.clear_child_indices();
  // Animated/inherited rendering state cannot be replaced by static geometry.
  for (std::string path = mesh->path; !path.empty(); path = Parent(path)) {
    const PrimSpec* p = stage.GetRootLayer()->prim_at_path(path);
    if (!p) continue;
    if (!p->meta().active || p->meta().hidden || !p->meta().loaded ||
        p->meta().instanceable || !p->meta().instance_prototype().empty() ||
        !p->meta().clips().is_empty() || !p->meta().references.empty() ||
        !p->meta().payloads.empty() || !p->meta().inherits.empty() ||
        !p->meta().specializes.empty())
      return false;
    for (const PropSlot& slot : p->properties().slots()) {
      const std::string name(GetPropNameTable().get(slot.name_id));
      if (slot.is_time_sampled() &&
          (path == mesh->path || Starts(name, "xformOp")))
        return false;
      if (name == "visibility" || name == "purpose" || Starts(name, "skel:") ||
          Starts(name, "primvars:skel:") || Starts(name, "collection:"))
        return false;
      if (path != mesh->path && Starts(name, "primvars:") &&
          !Ends(name, ":indices")) {
        const PropMeta* meta = p->property_meta(name);
        if (meta && !meta->interpolation.empty() &&
            meta->interpolation != "constant")
          continue;
        if (!mesh->spec.property(name)) {
          const Value* value = p->property_value(name);
          if (!value || slot.is_time_sampled()) return false;
          if (!budget->charge(value->heap_memory_usage() + sizeof(PropMeta), 2))
            return false;
          mesh->spec.add_property(name, *value, slot.flags);
          if (meta) mesh->spec.set_property_meta(name, PropMeta(*meta));
          const std::string indices = name + ":indices";
          if (!mesh->spec.property(indices))
            if (const Value* ix = p->property_value(indices))
              mesh->spec.add_property(indices, *ix);
        }
      }
    }
    for (const auto& rel : p->relationship_names())
      if (Starts(rel, "material:binding:") || Starts(rel, "skel:") ||
          Starts(rel, "collection:"))
        return false;
    // Variants, instances, clips and composition must be resolved by
    // flattening.
    if (!p->meta().variantSets().empty()) return false;
  }
  double world[16];
  if (!ComputeWorldTransform(stage, stage.GetPrimAtPath(mesh->path), world,
                             std::numeric_limits<double>::quiet_NaN()))
    return false;
  for (double x : world)
    if (!std::isfinite(x)) return false;
  if (world[3] != 0 || world[7] != 0 || world[11] != 0 || world[15] != 1)
    return false;
  const double det = world[0] * (world[5] * world[10] - world[6] * world[9]) -
                     world[1] * (world[4] * world[10] - world[6] * world[8]) +
                     world[2] * (world[4] * world[9] - world[5] * world[8]);
  if (!std::isfinite(det) || std::abs(det) < 1e-20) return false;
  const Value* count_value = source.property_value("faceVertexCounts");
  const Value* index_value = source.property_value("faceVertexIndices");
  if (!count_value || !index_value ||
      count_value->array_size() > options.max_input_faces ||
      !budget->charge(count_value->array_size(), sizeof(int32_t)) ||
      !budget->charge(index_value->array_size(), sizeof(int32_t)))
    return false;
  std::vector<int32_t> counts, indices;
  if (!ReadInts(source, "faceVertexCounts", &counts) ||
      !ReadInts(source, "faceVertexIndices", &indices))
    return false;
  const Value* points = source.property_value("points");
  if (!points || !points->is_array() || !IsPoint(points->type_id()))
    return false;
  mesh->faces = counts.size();
  mesh->points = points->array_size();
  if (!mesh->faces || !mesh->points || mesh->faces > options.max_input_faces ||
      mesh->points > options.max_input_points)
    return false;
  size_t corners = 0;
  for (int32_t n : counts) {
    if (n < 3 || size_t(n) > indices.size() - std::min(indices.size(), corners))
      return false;
    corners += size_t(n);
  }
  if (corners != indices.size()) return false;
  for (int32_t i : indices)
    if (i < 0 || size_t(i) >= mesh->points) return false;
  std::map<std::string, std::vector<int32_t>> optional;
  for (const char* name :
       {"cornerIndices", "creaseIndices", "creaseLengths", "holeIndices"}) {
    const Value* value = source.property_value(name);
    if (!value) continue;
    if (!budget->charge(value->array_size(), sizeof(int32_t)) ||
        !ReadInts(source, name, &optional[name]))
      return false;
  }
  for (const char* name : {"cornerIndices", "creaseIndices", "holeIndices"}) {
    const size_t limit =
        std::strcmp(name, "holeIndices") == 0 ? mesh->faces : mesh->points;
    for (int32_t index : optional[name])
      if (index < 0 || size_t(index) >= limit) return false;
  }
  size_t crease_vertices = 0, crease_edges = 0;
  for (int32_t length : optional["creaseLengths"]) {
    if (length < 2 ||
        size_t(length) >
            optional["creaseIndices"].size() -
                std::min(crease_vertices, optional["creaseIndices"].size()))
      return false;
    crease_vertices += size_t(length);
    crease_edges += size_t(length) - 1;
  }
  if (crease_vertices != optional["creaseIndices"].size()) return false;
  const Value* corners_sharp = source.property_value("cornerSharpnesses");
  if ((corners_sharp ? corners_sharp->array_size() : 0) !=
      optional["cornerIndices"].size())
    return false;
  const Value* creases_sharp = source.property_value("creaseSharpnesses");
  const size_t sharp_count = creases_sharp ? creases_sharp->array_size() : 0;
  if (sharp_count != optional["creaseLengths"].size() &&
      sharp_count != crease_edges)
    return false;
  mesh->per_edge_sharpness = sharp_count != optional["creaseLengths"].size();
  const std::set<std::string> topology = {
      "points",        "faceVertexCounts",  "faceVertexIndices",
      "cornerIndices", "cornerSharpnesses", "creaseIndices",
      "creaseLengths", "creaseSharpnesses", "holeIndices"};
  const auto slots = mesh->spec.properties().slots();
  std::vector<std::string> names;
  for (const PropSlot& slot : slots)
    names.emplace_back(GetPropNameTable().get(slot.name_id));
  for (const std::string& name : names) {
    if (Starts(name, "xformOp") || name == "extent") {
      mesh->spec.remove_property(name);
      continue;
    }
    const PropSlot* slot = mesh->spec.property(name);
    if (!slot || slot->is_time_sampled() ||
        (slot->flags & PropSlot::kFlagConnection))
      return false;
    if (Ends(name, ":indices") && Starts(name, "primvars:")) continue;
    const bool primvar = Starts(name, "primvars:") || name == "normals";
    const Value* v = mesh->spec.property_value(name);
    if (slot->flags & PropSlot::kFlagRelationship) continue;
    if (!v) return false;
    if (!topology.count(name) && !primvar) {
      if (v->is_array()) return false;
      continue;
    }
    Array a;
    if (!MakeArray(*v, &a, budget)) return false;
    const TypeId type = a.type;
    if (name == "points" || IsPoint(type) || IsNormal(type) || IsVector(type) ||
        name == "normals")
      if (!Transform(&a, world, IsNormal(type) || name == "normals",
                     name == "points" || IsPoint(type)))
        return false;
    bool constant = false;
    if (primvar) {
      const PropMeta* meta = mesh->spec.property_meta(name);
      const std::string interpolation =
          meta && !meta->interpolation.empty()
              ? meta->interpolation
              : (name == "normals" ? "varying" : "constant");
      const size_t element = meta ? size_t(std::max(0, meta->elementSize)) : 1;
      if (!element || (meta && meta->unauthoredValuesIndex >= 0)) return false;
      size_t domain = 0;
      if (interpolation == "constant") {
        domain = 1;
        constant = true;
      } else if (interpolation == "uniform")
        domain = mesh->faces;
      else if (interpolation == "vertex" || interpolation == "varying")
        domain = mesh->points;
      else if (interpolation == "faceVarying")
        domain = corners;
      else
        return false;
      if (domain > std::numeric_limits<size_t>::max() / element) return false;
      const std::string index_name = name + ":indices";
      const Value* index_value = mesh->spec.property_value(index_name);
      Array index_array;
      if (index_value) {
        if (index_value->type_id() != TypeId::Int ||
            index_value->array_size() != domain ||
            !MakeArray(*index_value, &index_array, budget))
          return false;
        const auto ix = index_value->as_int_array();
        for (int32_t index : ix)
          if (index < 0 || size_t(index) >= a.bytes.size() / a.stride / element)
            return false;
      } else if (a.bytes.size() / a.stride != domain * element)
        return false;
      if (det < 0 && interpolation == "faceVarying") {
        // Keep the indexed value pool: subdivision continuity depends on index
        // identity even when adjacent values happen to have equal coordinates.
        Array& reversed = index_value ? index_array : a;
        const size_t stride =
            index_value ? sizeof(int32_t) : element * a.stride;
        size_t offset = 0;
        for (int32_t n : counts) {
          for (size_t i = 0; i < size_t(n) / 2; ++i)
            for (size_t b = 0; b < stride; ++b)
              std::swap(
                  reversed.bytes[(offset + i) * stride + b],
                  reversed.bytes[(offset + size_t(n) - 1 - i) * stride + b]);
          offset += size_t(n);
        }
      }
      if (index_value && !constant) {
        mesh->spec.set_property_value(GetPropNameTable().intern(index_name),
                                      Value::MakeEmptyArray(TypeId::Int, 0));
        mesh->arrays.emplace(index_name, std::move(index_array));
      }
    }
    if (name == "faceVertexIndices" && det < 0) {
      size_t offset = 0;
      for (int32_t n : counts) {
        std::reverse(
            indices.begin() + static_cast<ptrdiff_t>(offset),
            indices.begin() + static_cast<ptrdiff_t>(offset + size_t(n)));
        offset += size_t(n);
      }
      std::memcpy(a.bytes.data(), indices.data(), a.bytes.size());
    }
    mesh->spec.set_property_value(
        GetPropNameTable().intern(name),
        constant ? ToValue(a) : Value::MakeEmptyArray(a.type, 0));
    if (!constant) mesh->arrays.emplace(name, std::move(a));
  }
  // Every indexed primvar must have a corresponding authored value.
  for (const std::string& name : names)
    if (Starts(name, "primvars:") && Ends(name, ":indices") &&
        !mesh->spec.property(name.substr(0, name.size() - 8)))
      return false;
  for (uint32_t child : source.child_indices()) {
    const PrimSpec* subset = stage.GetRootLayer()->prim(child);
    if (!subset || subset->type_name() != "GeomSubset" ||
        !subset->child_indices().empty())
      return false;
    for (const PropSlot& slot : subset->properties().slots())
      if (slot.is_time_sampled() || (slot.flags & PropSlot::kFlagConnection))
        return false;
    const Value* kind = subset->property_value("elementType");
    if (kind && kind->as_token() != "face") return false;
    std::vector<int32_t> faces;
    const Value* subset_indices = subset->property_value("indices");
    if (!subset_indices ||
        !budget->charge(subset_indices->array_size(), sizeof(int32_t)) ||
        !ReadInts(*subset, "indices", &faces))
      return false;
    for (int32_t face : faces)
      if (face < 0 || size_t(face) >= mesh->faces) return false;
    if (!budget->charge(subset->memory_usage(), 1)) return false;
    PrimSpec copy = subset->Clone();
    const std::string bound =
        GetInheritedBoundMaterialPath(stage, subset->path().str());
    if (!bound.empty())
      copy.set_relationship_targets("material:binding", {Path(bound)});
    mesh->subsets.push_back(std::move(copy));
  }
  const std::string material = GetInheritedBoundMaterialPath(stage, mesh->path);
  if (!material.empty())
    mesh->spec.set_relationship_targets("material:binding", {Path(material)});
  mesh->spec.set_name("Mesh");
  mesh->spec.set_path(Path("/Mesh"));
  mesh->spec.add_property("xformOpOrder",
                          Value::MakeTokenArray({"!resetXformStack!"}),
                          PropSlot::kFlagUniform);
  return true;
}
Layer Signature(const Mesh& mesh) {
  Layer layer;
  const uint32_t index = layer.add_prim(mesh.spec.Clone());
  layer.add_root(index);
  layer.finalize();
  return layer;
}
bool EqualSchema(const Mesh& a, const Mesh& b) {
  DiffOptions opts;
  opts.floatUlps = 0;
  opts.doubleUlps = 0;
  opts.timeUlps = 0;
  opts.fuzzyAssetPaths = false;
  return DiffToText(Signature(a), Signature(b), "a", "b", opts) ==
         "No differences found.\n";
}
}  // namespace

bool MergeMeshes(Stage* stage, const MeshMergeOptions& options,
                 GeometryOptimizationStats* output, std::string* warn,
                 std::string* error) {
  GeometryOptimizationStats stats;
  if (!stage || !stage->GetRootLayer() || !options.max_input_faces ||
      !options.max_input_points || !options.max_aggregate_faces ||
      options.max_input_faces > size_t(INT32_MAX) ||
      options.max_input_points > size_t(INT32_MAX) ||
      options.max_aggregate_faces > size_t(INT32_MAX) ||
      options.min_group_size < 2 ||
      options.min_group_size > size_t(INT32_MAX) ||
      !options.memory_budget_bytes) {
    if (error) *error = "Invalid mesh merge options or stage";
    return false;
  }
  Layer* layer = stage->GetRootLayer();
  Budget budget{options.memory_budget_bytes};
  std::map<std::string, std::vector<Mesh>> groups;
  // Prepare every source before modifying the layer, retaining whole-mesh
  // ownership.
  const size_t initial = layer->prim_count();
  for (size_t i = 0; i < initial; ++i) {
    const PrimSpec* p = layer->prim(static_cast<uint32_t>(i));
    if (!p || p->type_name() != "Mesh" ||
        layer->index_at_path(p->path().str()) != i)
      continue;
    ++stats.num_meshes_before;
    Mesh mesh;
    const Budget before = budget;
    if (!Prepare(*stage, *p, options, &mesh, &budget)) {
      if (budget.exceeded) {
        if (error) *error = "Mesh merge working memory budget exceeded";
        return false;
      }
      budget = before;
      continue;
    }
    if (!budget.charge(mesh.spec.memory_usage(), 8)) {
      if (error) *error = "Mesh merge working memory budget exceeded";
      return false;
    }
    ++stats.num_meshes_eligible;
    const size_t slash = mesh.path.find('/', 1);
    const std::string base =
        mesh.path.substr(0, slash) + "@" +
        std::to_string(mesh.spec.asset_anchor_id()) +
        (mesh.per_edge_sharpness ? "\nedge\n" : "\ncrease\n") +
        WriteLayerToString(Signature(mesh));
    std::string key = base;
    for (size_t suffix = 0;
         groups.count(key) && !EqualSchema(groups[key].front(), mesh); ++suffix)
      key = base + "#" + std::to_string(suffix);
    groups[key].push_back(std::move(mesh));
  }
  std::map<std::string, std::string> roots;
  std::vector<std::pair<PrimSpec, std::vector<PrimSpec>>> outputs;
  std::vector<std::string> removals;
  for (auto& group : groups) {
    auto& meshes = group.second;
    const std::string parent =
        meshes.front().path.substr(0, meshes.front().path.find('/', 1));
    std::string& root = roots[parent];
    if (root.empty()) {
      root = parent + "/__LightUSD_MeshMerge";
      for (size_t n = 1; layer->prim_at_path(root); ++n)
        root = parent + "/__LightUSD_MeshMerge_" + std::to_string(n);
    }
    for (size_t begin = 0; begin < meshes.size();) {
      size_t end = begin, faces = 0, points = 0;
      while (end < meshes.size() &&
             meshes[end].faces <= options.max_aggregate_faces - faces &&
             meshes[end].points <= size_t(INT32_MAX) - points) {
        faces += meshes[end].faces;
        points += meshes[end].points;
        ++end;
      }
      if (end - begin < options.min_group_size) {
        begin = end > begin ? end : begin + 1;
        continue;
      }
      Mesh& first = meshes[begin];
      const std::string name =
          "Merged_" + std::to_string(stats.num_mesh_aggregates);
      const std::string path = root + "/" + name;
      PrimSpec merged = first.spec.Clone();
      merged.set_name(name);
      merged.set_path(Path(path));
      std::map<std::string, Array> arrays;
      size_t point_offset = 0, face_offset = 0;
      std::vector<PrimSpec> subsets;
      for (size_t i = begin; i < end; ++i) {
        Mesh& mesh = meshes[i];
        std::map<std::string, size_t> primvar_offsets;
        for (const auto& pair : mesh.arrays) {
          if (!Starts(pair.first, "primvars:") || Ends(pair.first, ":indices"))
            continue;
          const PropMeta* meta = mesh.spec.property_meta(pair.first);
          const size_t element = meta ? size_t(meta->elementSize) : 1;
          const auto previous = arrays.find(pair.first);
          primvar_offsets[pair.first + ":indices"] =
              previous == arrays.end() ? 0
                                       : previous->second.bytes.size() /
                                             pair.second.stride / element;
        }
        for (auto& pair : mesh.arrays) {
          Array& a = pair.second;
          if (pair.first == "faceVertexIndices" ||
              pair.first == "cornerIndices" || pair.first == "creaseIndices" ||
              pair.first == "holeIndices" ||
              primvar_offsets.count(pair.first)) {
            if (a.type != TypeId::Int) {
              if (error) *error = "Invalid topology index type";
              return false;
            }
            const size_t offset =
                primvar_offsets.count(pair.first)
                    ? primvar_offsets[pair.first]
                    : (pair.first == "holeIndices" ? face_offset
                                                   : point_offset);
            for (size_t j = 0; j < a.bytes.size(); j += sizeof(int32_t)) {
              int32_t value;
              std::memcpy(&value, a.bytes.data() + j, sizeof(value));
              if (value < 0 || offset > size_t(INT32_MAX) ||
                  size_t(value) > size_t(INT32_MAX) - offset) {
                if (error) *error = "Mesh index overflow";
                return false;
              }
              value += static_cast<int32_t>(offset);
              std::memcpy(a.bytes.data() + j, &value, sizeof(value));
            }
          }
          Array& dst = arrays[pair.first];
          dst.type = a.type;
          dst.stride = a.stride;
          if (!budget.charge(a.bytes.size(), 2)) {
            if (error) *error = "Mesh merge working memory budget exceeded";
            return false;
          }
          dst.bytes.insert(dst.bytes.end(), a.bytes.begin(), a.bytes.end());
        }
        for (PrimSpec& subset : mesh.subsets) {
          std::vector<int32_t> ids;
          ReadInts(subset, "indices", &ids);
          for (int32_t& id : ids) id += static_cast<int32_t>(face_offset);
          subset.set_property_value(GetPropNameTable().intern("indices"),
                                    Value::MakeIntArray(ids));
          const std::string child_name =
              "Subset_" + std::to_string(subsets.size());
          subset.set_name(child_name);
          subset.set_path(Path(path + "/" + child_name));
          subsets.push_back(std::move(subset));
        }
        point_offset += mesh.points;
        face_offset += mesh.faces;
      }
      for (const auto& pair : arrays)
        merged.set_property_value(GetPropNameTable().intern(pair.first),
                                  ToValue(pair.second));
      outputs.emplace_back(std::move(merged), std::move(subsets));
      for (size_t i = begin; i < end; ++i) removals.push_back(meshes[i].path);
      stats.num_meshes_merged += end - begin;
      ++stats.num_mesh_aggregates;
      begin = end;
    }
  }
  if (stats.num_meshes_merged) {
    // Existing clients may have unlinked specs before invoking the optimizer.
    // Drop that storage before rebuilding the index for our appended outputs.
    detail::CompactLayer(layer);
    std::map<std::string, uint32_t> root_indices;
    for (auto& result : outputs) {
      const std::string root = Parent(result.first.path().str());
      auto found = root_indices.find(root);
      uint32_t root_index;
      if (found == root_indices.end()) {
        PrimSpec scope;
        scope.set_name(root.substr(root.rfind('/') + 1));
        scope.set_path(Path(root));
        scope.set_type_name("Scope");
        root_index = layer->add_prim(std::move(scope));
        layer->set_parent(root_index, layer->index_at_path(Parent(root)));
        root_indices.emplace(root, root_index);
      } else
        root_index = found->second;
      const uint32_t index = layer->add_prim(std::move(result.first));
      layer->set_parent(index, root_index);
      for (PrimSpec& subset : result.second) {
        const uint32_t child = layer->add_prim(std::move(subset));
        layer->set_parent(child, index);
      }
    }
    // add_prim is a builder operation; this already-finalized layer still has
    // its old path index until it is explicitly rebuilt.
    layer->build_path_index();
    for (const auto& path : removals) layer->remove_prim_at_path(path);
    detail::CompactLayer(layer);
    if (warn)
      *warn +=
          "Mesh merging changes source mesh paths and hierarchy for static "
          "delivery.\n";
  }
  stats.num_meshes_after = stats.num_meshes_before - stats.num_meshes_merged +
                           stats.num_mesh_aggregates;
  if (output) *output = stats;
  return true;
}
}  // namespace lightusd::usdz
