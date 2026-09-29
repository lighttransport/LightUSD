// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Scene Access Implementation

#include "scene-access.hh"
#include "next/layer/prim-spec.hh"
#include "next/prim/path.hh"
#include <cstring>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace lightusd {
namespace tydra {
namespace next {

using ::lightusd::next::Path;
using ::lightusd::next::PrimSpec;
using ::lightusd::next::PropMeta;

//
// Prim type checking
//

bool IsMesh(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Mesh";
}

bool IsXform(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Xform";
}

bool IsCamera(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Camera";
}

bool IsMaterial(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Material";
}

bool IsShader(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Shader";
}

bool IsLight(const UsdPrim& prim) {
  if (!prim.IsValid()) return false;
  const std::string& type = prim.GetTypeName();
  return type == "DistantLight" || type == "DomeLight" ||
         type == "DomeLight_1" || type == "RectLight" || type == "DiskLight" ||
         type == "SphereLight" || type == "CylinderLight" ||
         type == "PointLight" || type == "GeometryLight" ||
         type == "PortalLight" || type == "PluginLight" ||
         type == "LightFilter" || type == "PluginLightFilter";
}

bool IsSkeleton(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Skeleton";
}

bool IsSkelRoot(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "SkelRoot";
}

bool IsGeomSubset(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "GeomSubset";
}

bool IsScope(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Scope";
}

LightKind GetLightKind(const UsdPrim& prim) {
  if (!prim.IsValid()) return LightKind::Unknown;
  const std::string& type = prim.GetTypeName();

  if (type == "DistantLight") return LightKind::DistantLight;
  if (type == "DomeLight" || type == "DomeLight_1") return LightKind::DomeLight;
  if (type == "RectLight") return LightKind::RectLight;
  if (type == "DiskLight") return LightKind::DiskLight;
  if (type == "SphereLight") return LightKind::SphereLight;
  if (type == "CylinderLight") return LightKind::CylinderLight;
  if (type == "PointLight") return LightKind::PointLight;
  if (type == "GeometryLight") return LightKind::GeometryLight;
  if (type == "PortalLight") return LightKind::PortalLight;
  if (type == "PluginLight") return LightKind::PluginLight;
  if (type == "LightFilter") return LightKind::LightFilter;
  if (type == "PluginLightFilter") return LightKind::PluginLightFilter;

  return LightKind::Unknown;
}

//
// Find prims by type
//

std::vector<UsdPrim> FindMeshes(const Stage& stage) {
  return FindPrimsByType(stage, "Mesh");
}

std::vector<UsdPrim> FindXforms(const Stage& stage) {
  return FindPrimsByType(stage, "Xform");
}

std::vector<UsdPrim> FindCameras(const Stage& stage) {
  return FindPrimsByType(stage, "Camera");
}

std::vector<UsdPrim> FindMaterials(const Stage& stage) {
  return FindPrimsByType(stage, "Material");
}

std::vector<UsdPrim> FindLights(const Stage& stage) {
  return FindPrims(stage, IsLight);
}

std::vector<UsdPrim> FindSkeletons(const Stage& stage) {
  return FindPrimsByType(stage, "Skeleton");
}

std::vector<UsdPrim> FindPrimsByType(const Stage& stage, const std::string& type_name) {
  std::vector<UsdPrim> result;
  stage.Traverse([&](const UsdPrim& prim) {
    if (prim.GetTypeName() == type_name) {
      result.push_back(prim);
    }
    return true;  // Continue traversal
  });
  return result;
}

std::vector<UsdPrim> FindPrims(const Stage& stage, PrimPredicate pred) {
  std::vector<UsdPrim> result;
  stage.Traverse([&](const UsdPrim& prim) {
    if (pred(prim)) {
      result.push_back(prim);
    }
    return true;
  });
  return result;
}

//
// Attribute access helpers
//

const Value* GetAttribute(const UsdPrim& prim, const std::string& name) {
  if (!prim.IsValid()) return nullptr;
  auto name_id = lightusd::next::GetPropNameTable().find(name);
  if (!name_id.is_valid()) return nullptr;
  return prim.GetPropertyValue(name_id);
}

const Value* GetAttributeAtTime(const UsdPrim& prim, const std::string& name, double time) {
  if (!prim.IsValid()) return nullptr;
  auto name_id = lightusd::next::GetPropNameTable().find(name);
  if (!name_id.is_valid()) return nullptr;
  return prim.GetValueAtTime(name_id, time);
}

bool GetFloat(const UsdPrim& prim, const std::string& name, float* out) {
  if (!out) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const float* f = val->as_float();
  if (f) {
    *out = *f;
    return true;
  }

  // Try double
  const double* d = val->as_double();
  if (d) {
    *out = static_cast<float>(*d);
    return true;
  }

  return false;
}

bool GetFloat3(const UsdPrim& prim, const std::string& name, float* x, float* y, float* z) {
  if (!x || !y || !z) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const float* f3 = val->as_float3();
  if (f3) {
    *x = f3[0]; *y = f3[1]; *z = f3[2];
    return true;
  }

  const double* d3 = val->as_double3();
  if (d3) {
    *x = static_cast<float>(d3[0]);
    *y = static_cast<float>(d3[1]);
    *z = static_cast<float>(d3[2]);
    return true;
  }

  return false;
}

bool GetDouble(const UsdPrim& prim, const std::string& name, double* out) {
  if (!out) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const double* d = val->as_double();
  if (d) {
    *out = *d;
    return true;
  }

  const float* f = val->as_float();
  if (f) {
    *out = *f;
    return true;
  }

  return false;
}

bool GetDouble3(const UsdPrim& prim, const std::string& name, double* x, double* y, double* z) {
  if (!x || !y || !z) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const double* d3 = val->as_double3();
  if (d3) {
    *x = d3[0]; *y = d3[1]; *z = d3[2];
    return true;
  }

  const float* f3 = val->as_float3();
  if (f3) {
    *x = f3[0]; *y = f3[1]; *z = f3[2];
    return true;
  }

  return false;
}

bool GetInt(const UsdPrim& prim, const std::string& name, int* out) {
  if (!out) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const int* i = val->as_int();
  if (i) {
    *out = *i;
    return true;
  }

  return false;
}

bool GetBool(const UsdPrim& prim, const std::string& name, bool* out) {
  if (!out) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const bool* b = val->as_bool();
  if (b) {
    *out = *b;
    return true;
  }

  return false;
}

bool GetString(const UsdPrim& prim, const std::string& name, std::string* out) {
  if (!out) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const std::string* s = val->as_string();
  if (s) {
    *out = *s;
    return true;
  }

  return false;
}

bool GetToken(const UsdPrim& prim, const std::string& name, std::string* out) {
  if (!out) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const std::string* t = val->as_token();
  if (t) {
    *out = *t;
    return true;
  }

  return false;
}

bool GetMatrix4(const UsdPrim& prim, const std::string& name, float* matrix16) {
  if (!matrix16) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const float* m = val->as_matrix4f();
  if (m) {
    std::memcpy(matrix16, m, 16 * sizeof(float));
    return true;
  }

  const double* md = val->as_matrix4d();
  if (md) {
    for (int i = 0; i < 16; ++i) {
      matrix16[i] = static_cast<float>(md[i]);
    }
    return true;
  }

  return false;
}

bool GetMatrix4d(const UsdPrim& prim, const std::string& name, double* matrix16) {
  if (!matrix16) return false;
  const Value* val = GetAttribute(prim, name);
  if (!val) return false;

  const double* m = val->as_matrix4d();
  if (m) {
    std::memcpy(matrix16, m, 16 * sizeof(double));
    return true;
  }

  const float* mf = val->as_matrix4f();
  if (mf) {
    for (int i = 0; i < 16; ++i) {
      matrix16[i] = mf[i];
    }
    return true;
  }

  return false;
}

std::vector<float> GetFloatArray(const UsdPrim& prim, const std::string& name) {
  std::vector<float> result;
  const Value* val = GetAttribute(prim, name);
  if (!val) return result;

  const std::vector<float>* arr = val->as_float_array();
  if (arr) {
    result = *arr;
  }
  return result;
}

std::vector<std::string> GetTokenArray(const UsdPrim& prim,
                                       const std::string& name) {
  std::vector<std::string> result;
  const Value* val = GetAttribute(prim, name);
  if (!val) return result;
  if (const std::vector<std::string>* arr = val->as_token_array()) {
    result = *arr;
  }
  return result;
}

const std::vector<int32_t>* GetIntArrayView(const UsdPrim& prim,
                                            const std::string& name) {
  const Value* val = GetAttribute(prim, name);
  return val ? val->as_int_array() : nullptr;
}

const std::vector<float>* GetFloatArrayView(const UsdPrim& prim,
                                            const std::string& name) {
  const Value* val = GetAttribute(prim, name);
  return val ? val->as_float_array() : nullptr;
}

std::vector<int32_t> GetIntArray(const UsdPrim& prim, const std::string& name) {
  std::vector<int32_t> result;
  const Value* val = GetAttribute(prim, name);
  if (!val) return result;

  const std::vector<int32_t>* arr = val->as_int_array();
  if (arr) {
    result = *arr;
  }
  return result;
}

//
// Relationship access
//

std::vector<std::string> GetRelationshipTargets(const UsdPrim& prim, const std::string& rel_name) {
  std::vector<std::string> result;
  if (!prim.IsValid()) return result;

  if (const std::vector<Path>* targets = prim.GetRelationship(rel_name)) {
    result.reserve(targets->size());
    for (const Path& p : *targets) {
      result.push_back(p.str());
    }
  }
  return result;
}

std::string GetBoundMaterial(const UsdPrim& prim) {
  if (!prim.IsValid()) return "";

  // Try material:binding relationship
  auto targets = GetRelationshipTargets(prim, "material:binding");
  if (!targets.empty()) {
    return targets[0];
  }

  return "";
}

std::string GetBoundSkeleton(const UsdPrim& prim) {
  if (!prim.IsValid()) return "";

  auto targets = GetRelationshipTargets(prim, "skel:skeleton");
  if (!targets.empty()) {
    return targets[0];
  }

  return "";
}

//
// Hierarchy access
//

UsdPrim GetParent(const Stage& stage, const UsdPrim& prim) {
  if (!prim.IsValid()) return UsdPrim();

  std::string path = prim.GetPath().str();
  std::string parent_path = GetParentPath(path);

  if (parent_path.empty() || parent_path == "/") {
    return UsdPrim();
  }

  return stage.GetPrimAtPath(parent_path);
}

std::vector<UsdPrim> GetChildren(const UsdPrim& prim) {
  if (!prim.IsValid()) return {};
  return prim.GetChildren();
}

std::vector<UsdPrim> GetDescendants(const UsdPrim& prim) {
  std::vector<UsdPrim> result;
  if (!prim.IsValid()) return result;

  // Iterative pre-order DFS. Stages parsed from USDA have a depth limit, but
  // callers can build Layers programmatically; recursing here made a valid
  // deep hierarchy exhaust the native/WASM stack.
  std::vector<UsdPrim> stack;
  const size_t child_count = prim.GetChildCount();
  stack.reserve(child_count);
  for (size_t i = child_count; i > 0; --i) {
    const UsdPrim child = prim.GetChildAt(i - 1);
    if (child.IsValid()) stack.push_back(child);
  }

  while (!stack.empty()) {
    UsdPrim current = stack.back();
    stack.pop_back();
    if (!current.IsValid()) continue;

    result.push_back(current);

    const size_t child_count = current.GetChildCount();
    for (size_t i = child_count; i > 0; --i) {
      const UsdPrim child = current.GetChildAt(i - 1);
      if (child.IsValid()) stack.push_back(child);
    }
  }
  return result;
}

std::string GetPrimName(const UsdPrim& prim) {
  if (!prim.IsValid()) return "";
  return prim.GetName();
}

std::string GetParentPath(const std::string& path) {
  if (path.empty() || path == "/") return "";

  size_t last_slash = path.rfind('/');
  if (last_slash == 0) return "/";
  if (last_slash == std::string::npos) return "";

  return path.substr(0, last_slash);
}

//
// GeomSubset access
//

std::vector<GeomSubset> GetGeomSubsets(const UsdPrim& mesh_prim) {
  std::vector<GeomSubset> result;
  if (!mesh_prim.IsValid()) return result;

  const size_t child_count = mesh_prim.GetChildCount();
  for (size_t i = 0; i < child_count; ++i) {
    const UsdPrim child = mesh_prim.GetChildAt(i);
    if (!child.IsValid()) continue;
    if (IsGeomSubset(child)) {
      GeomSubset gs;
      gs.name = child.GetName();
      gs.path = child.GetPath().str();

      GetToken(child, "familyName", &gs.family_name);

      // View, not a copy: the array lives in the stage's Value storage and
      // the consumer builds its own working copy regardless.
      if (const Value* iv = GetAttribute(child, "indices")) {
        gs.indices_view = iv->as_int_array();
      }

      // Get material binding
      gs.material_path = GetBoundMaterial(child);

      result.push_back(std::move(gs));
    }
  }

  return result;
}

//
// Primvar access
//

std::vector<Primvar> GetPrimvars(const UsdPrim& prim) {
  std::vector<Primvar> result;
  if (!prim.IsValid()) return result;

  // Get all properties that start with "primvars:"
  auto prop_names = prim.GetPropertyNames();
  for (const auto& name : prop_names) {
    if (name.find("primvars:") == 0 && name.find(":indices") == std::string::npos) {
      Primvar pv;
      pv.name = name.substr(9);  // Remove "primvars:" prefix
      pv.value = GetAttribute(prim, name);
      if (pv.value) {
        pv.type_id = pv.value->type_id();

        // Get interpolation: authored property metadata first (the usda/crate
        // readers store it in PropMeta), then the legacy attribute form.
        if (const PrimSpec* spec = prim.GetPrimSpec()) {
          if (const PropMeta* pm = spec->property_meta(name)) {
            if (pm->authored & PropMeta::kInterpolation) {
              pv.interpolation = pm->interpolation;
            }
          }
        }
        if (pv.interpolation.empty()) {
          std::string interp_attr = name + ":interpolation";
          GetToken(prim, interp_attr, &pv.interpolation);
        }
        pv.interpolation_authored = !pv.interpolation.empty();
        if (pv.interpolation.empty()) {
          pv.interpolation = "constant";  // USD spec default (pxr/legacy parity)
        }

        // Get indices if present (view, not a copy -- see Primvar::indices_view)
        std::string indices_attr = name + ":indices";
        if (const Value* iv = GetAttribute(prim, indices_attr)) {
          pv.indices_view = iv->as_int_array();
        }

        result.push_back(std::move(pv));
      }
    }
  }

  return result;
}

Primvar GetPrimvar(const UsdPrim& prim, const std::string& name) {
  Primvar pv;
  pv.name = name;

  std::string full_name = "primvars:" + name;
  pv.value = GetAttribute(prim, full_name);

  if (pv.value) {
    pv.type_id = pv.value->type_id();

    if (const PrimSpec* spec = prim.GetPrimSpec()) {
      if (const PropMeta* pm = spec->property_meta(full_name)) {
        if (pm->authored & PropMeta::kInterpolation) {
          pv.interpolation = pm->interpolation;
        }
      }
    }
    if (pv.interpolation.empty()) {
      std::string interp_attr = full_name + ":interpolation";
      GetToken(prim, interp_attr, &pv.interpolation);
    }
    pv.interpolation_authored = !pv.interpolation.empty();
    if (pv.interpolation.empty()) {
      pv.interpolation = "constant";  // USD spec default (pxr/legacy parity)
    }

    std::string indices_attr = full_name + ":indices";
    if (const Value* iv = GetAttribute(prim, indices_attr)) {
      pv.indices_view = iv->as_int_array();
    }
  }

  return pv;
}

//
// Blend shape access
//

namespace {

std::vector<std::string> ReadTokenArray(const UsdPrim& prim,
                                        const std::string& name) {
  std::vector<std::string> out;
  if (const Value* v = GetAttribute(prim, name)) {
    if (const std::vector<std::string>* toks = v->as_token_array()) out = *toks;
  }
  return out;
}

// Read a matrix4d[] / double[] array without materializing a conversion copy.
// Some encoders author float[] instead; the view keeps that representation and
// converts one lane at a time at the final destination.
struct DoubleArrayView {
  const std::vector<double>* doubles = nullptr;
  const std::vector<float>* floats = nullptr;

  size_t size() const {
    return doubles ? doubles->size() : (floats ? floats->size() : 0);
  }

  double operator[](size_t index) const {
    return doubles ? (*doubles)[index] : double((*floats)[index]);
  }
};

DoubleArrayView ReadDoubleArrayView(const UsdPrim& prim,
                                    const std::string& name) {
  DoubleArrayView view;
  const Value* v = GetAttribute(prim, name);
  if (!v) return view;
  view.doubles = v->as_double_array();
  if (!view.doubles) view.floats = v->as_float_array();
  return view;
}

void FillIdentity(float* m16) {
  for (int i = 0; i < 16; ++i) m16[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

}  // namespace

std::vector<BlendShapeInfo> GetBlendShapes(const UsdPrim& mesh_prim) {
  std::vector<BlendShapeInfo> result;
  if (!mesh_prim.IsValid()) return result;

  // skel:blendShapes (token[]) names paired with skel:blendShapeTargets
  // (relationship) paths. The per-shape point/normal offsets live in the
  // referenced BlendShape prims; resolving those requires a Stage, so callers
  // fetch them via the core GetBlendShapeData(stage, prim) using `path`.
  std::vector<std::string> names =
      ReadTokenArray(mesh_prim, "skel:blendShapes");
  std::vector<std::string> targets =
      GetRelationshipTargets(mesh_prim, "skel:blendShapeTargets");

  const size_t n = std::max(names.size(), targets.size());
  result.resize(n);
  for (size_t i = 0; i < n; ++i) {
    if (i < names.size()) result[i].name = std::move(names[i]);
    if (i < targets.size()) result[i].path = std::move(targets[i]);
  }
  return result;
}

//
// Skeleton access
//

bool GetSkeletonInfo(const UsdPrim& skel_prim, SkeletonInfo* out) {
  if (!out || !skel_prim.IsValid() || !IsSkeleton(skel_prim)) {
    return false;
  }

  out->name = skel_prim.GetName();
  out->path = skel_prim.GetPath().str();

  out->joint_order = ReadTokenArray(skel_prim, "joints");
  const std::vector<std::string>& joints = out->joint_order;

  const DoubleArrayView bind =
      ReadDoubleArrayView(skel_prim, "bindTransforms");
  const DoubleArrayView rest =
      ReadDoubleArrayView(skel_prim, "restTransforms");

  out->joints.resize(joints.size());
  // Parent lookup is quadratic if every hierarchical joint scans the full
  // skeleton. Keep the common small-skeleton path allocation-free, but use
  // non-owning views into `joints` for large skeletons so lookup is O(1)
  // without duplicating the authored path strings.
  std::unordered_map<std::string_view, int32_t> joint_index;
  if (joints.size() > 32) {
    joint_index.reserve(joints.size());
    joint_index.max_load_factor(0.7f);
    for (size_t i = 0; i < joints.size(); ++i) {
      joint_index.emplace(joints[i], static_cast<int32_t>(i));
    }
  }
  for (size_t i = 0; i < joints.size(); ++i) {
    JointInfo& j = out->joints[i];
    j.path = joints[i];

    // Joint paths are slash-separated (e.g. "Shoulder/Elbow/Hand"); the leaf
    // token is the name and the prefix identifies the parent joint.
    const size_t slash = joints[i].rfind('/');
    j.name = (slash == std::string::npos) ? joints[i] : joints[i].substr(slash + 1);
    j.parent_index = -1;
    if (slash != std::string::npos) {
      const std::string_view parent_path(joints[i].data(), slash);
      if (!joint_index.empty()) {
        const auto parent = joint_index.find(parent_path);
        if (parent != joint_index.end()) {
          j.parent_index = parent->second;
        }
      } else {
        for (size_t k = 0; k < joints.size(); ++k) {
          if (joints[k].size() == parent_path.size() &&
              joints[k].compare(0, parent_path.size(), parent_path.data(),
                                parent_path.size()) == 0) {
            j.parent_index = static_cast<int32_t>(k);
            break;
          }
        }
      }
    }

    size_t bind_offset = 0;
    if (bind.size() >= 16 && i <= (bind.size() - 16) / 16) {
      bind_offset = i * 16;
      for (int e = 0; e < 16; ++e) {
        j.bind_transform[e] = static_cast<float>(bind[bind_offset + e]);
      }
    } else {
      FillIdentity(j.bind_transform);
    }
    size_t rest_offset = 0;
    if (rest.size() >= 16 && i <= (rest.size() - 16) / 16) {
      rest_offset = i * 16;
      for (int e = 0; e < 16; ++e) {
        j.rest_transform[e] = static_cast<float>(rest[rest_offset + e]);
      }
    } else {
      FillIdentity(j.rest_transform);
    }
  }

  return true;
}

//
// Skin binding access
//

bool GetSkinBinding(const UsdPrim& mesh_prim, SkinBindingInfo* out) {
  if (!out || !mesh_prim.IsValid()) return false;

  bool any = false;

  const std::vector<std::string> skels =
      GetRelationshipTargets(mesh_prim, "skel:skeleton");
  if (!skels.empty()) {
    out->skeleton_path = skels[0];
    any = true;
  }

  out->joint_indices = GetIntArray(mesh_prim, "primvars:skel:jointIndices");
  out->joint_weights = GetFloatArray(mesh_prim, "primvars:skel:jointWeights");

  // Indexed primvars (`primvars:skel:jointIndices:indices`): flatten to the
  // expanded form all consumers expect. Per UsdGeomPrimvar, each index
  // addresses a GROUP of elementSize consecutive values.
  {
    const PrimSpec* spec = mesh_prim.GetPrimSpec();
    auto elem_size = [&](const char* pv_name) -> size_t {
      if (spec) {
        if (const PropMeta* pm = spec->property_meta(pv_name)) {
          if (pm->elementSize > 0) return size_t(pm->elementSize);
        }
      }
      return 1;
    };
    auto expand_indexed = [](auto& vals, const std::vector<int32_t>& idx,
                             size_t esize) {
      if (idx.empty() || vals.empty() || esize == 0 ||
          (vals.size() % esize) != 0) {
        return;
      }
      // Expansion size is authored data (indices count x elementSize) — a
      // hostile file can request terabytes, and this TU builds without
      // exceptions so an oversized reserve aborts. 2^28 lanes (~1 GiB of
      // int32) is far past any real skin (10M points x 8 influences = 80M).
      const size_t kMaxExpandedLanes = size_t(1) << 28;
      if (esize > kMaxExpandedLanes / idx.size()) return;  // keep authored
      const size_t elems = vals.size() / esize;
      typename std::remove_reference<decltype(vals)>::type expanded;
      expanded.reserve(idx.size() * esize);
      for (int32_t i : idx) {
        if (i < 0 || size_t(i) >= elems) return;  // malformed: keep authored
        expanded.insert(expanded.end(), vals.begin() + size_t(i) * esize,
                        vals.begin() + (size_t(i) + 1) * esize);
      }
      vals = std::move(expanded);
    };
    // Views: these index arrays are only read. On a dense skin each can be
    // tens of MB, and the expansion below already builds the owned result.
    static const std::vector<int32_t> kNoIdx;
    const std::vector<int32_t>* ji_idx =
        GetIntArrayView(mesh_prim, "primvars:skel:jointIndices:indices");
    const std::vector<int32_t>* jw_idx =
        GetIntArrayView(mesh_prim, "primvars:skel:jointWeights:indices");
    expand_indexed(out->joint_indices, ji_idx ? *ji_idx : kNoIdx,
                   elem_size("primvars:skel:jointIndices"));
    expand_indexed(out->joint_weights, jw_idx ? *jw_idx : kNoIdx,
                   elem_size("primvars:skel:jointWeights"));
  }
  if (!out->joint_indices.empty() || !out->joint_weights.empty()) any = true;

  // Mesh-local joint order (subset/permutation of the skeleton's joints).
  out->joint_order = GetTokenArray(mesh_prim, "skel:joints");

  // Influences per vertex = the jointIndices primvar's elementSize.
  out->influences_per_vertex = 0;
  if (const PrimSpec* spec = mesh_prim.GetPrimSpec()) {
    if (const PropMeta* pm =
            spec->property_meta("primvars:skel:jointIndices")) {
      out->influences_per_vertex = pm->elementSize;
    }
  }

  double gm[16];
  if (GetMatrix4d(mesh_prim, "primvars:skel:geomBindTransform", gm)) {
    for (int i = 0; i < 16; ++i) out->geom_bind_transform[i] = float(gm[i]);
    any = true;
  } else {
    FillIdentity(out->geom_bind_transform);
  }

  return any;
}

//
// Connection following
//

const Value* ResolveConnection(const Stage& stage, const UsdPrim& prim, const std::string& attr_name) {
  if (!prim.IsValid()) return nullptr;

  // A directly-authored value wins over a connection.
  if (const Value* val = GetAttribute(prim, attr_name)) {
    return val;
  }

  // Follow the connection chain: attr.connect -> target attribute, which may
  // itself hold a value or connect onward (e.g. shader input -> shader output
  // -> ...). Bounded depth guards against cycles.
  UsdPrim cur = prim;
  std::string attr = attr_name;
  for (int depth = 0; depth < 64; ++depth) {
    const PrimSpec* spec = cur.GetPrimSpec();
    if (!spec) break;
    const std::vector<Path>* conns = spec->connection(attr);
    if (!conns || conns->empty()) break;

    const Path& target = (*conns)[0];
    const std::string prop = target.property_name();
    if (prop.empty()) break;
    UsdPrim next = stage.GetPrimAtPath(target.prim_path());
    if (!next.IsValid()) break;

    // A value on the target ends the chain.
    if (const Value* v = GetAttribute(next, prop)) {
      return v;
    }
    cur = next;
    attr = prop;
  }
  return nullptr;
}

std::string GetConnectionPath(const UsdPrim& prim, const std::string& attr_name) {
  if (!prim.IsValid()) return "";

  const PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) return "";
  if (const std::vector<Path>* conns = spec->connection(attr_name)) {
    if (!conns->empty()) return (*conns)[0].str();
  }
  return "";
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
