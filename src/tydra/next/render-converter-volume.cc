// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - UsdVol Volume conversion
//
// A Volume's `field:<name>` relationships each name one field prim
// (OpenVDBAsset; Field3DAsset is recognized but has no reader). The field
// attributes are read at the conversion time, the file is resolved against
// the authoring layer, and tydra/vol resolves the grid (fieldName,
// fieldIndex, type and class checks, world placement). Voxels stay in the
// scene's FieldCache until a consumer asks for them.

#include "render-converter-internal.hh"

#include "next/resolver/asset-resolver.hh"
#include "next/schema/usd-shade.hh"

#include <cstring>
#include <string>

namespace lightusd {
namespace tydra {
namespace next {

namespace {

const char kFieldPrefix[] = "field:";

std::string TokenAt(const UsdPrim& prim, const char* name, double t) {
  const ::lightusd::next::Value* v = prim.GetValueAtTime(name, t);
  if (!v) return std::string();
  if (auto s = v->as_token()) return std::string(s);
  if (auto s = v->as_string()) return std::string(s);
  return std::string();
}

bool IsFieldAssetType(const std::string& type) {
  return type == "OpenVDBAsset" || type == "Field3DAsset";
}

// Volume shader constants (ND_standard_volume, PreviewSurface-like
// spellings), as legacy tydra reads them; connected/animated inputs keep
// their defaults.
void ReadVolumeShader(const ::lightusd::next::Stage& stage, const UsdPrim& shader, double t,
                      RenderVolume* out) {
  auto scalar = [&](const char* name, float* dst) {
    ::lightusd::next::Value v;
    if (!::lightusd::next::ResolveShaderPortValue(stage, shader, name, &v, t)) return;
    if (const float* f = v.view().as_float()) *dst = *f;
    else if (const double* d = v.view().as_double()) *dst = float(*d);
  };
  auto color = [&](const char* name, float dst[3]) {
    ::lightusd::next::Value v;
    if (!::lightusd::next::ResolveShaderPortValue(stage, shader, name, &v, t)) return;
    if (const float* f = v.view().as_float3()) {
      dst[0] = f[0];
      dst[1] = f[1];
      dst[2] = f[2];
    }
  };
  scalar("inputs:density", &out->density_scale);
  color("inputs:scattering_color", out->albedo);
  color("inputs:scatter_color", out->albedo);
  color("inputs:emission_color", out->emission);
  color("inputs:emissionColor", out->emission);
  scalar("inputs:emission", &out->emission_intensity);
  scalar("inputs:emission_intensity", &out->emission_intensity);
  scalar("inputs:emissionIntensity", &out->emission_intensity);
  if (out->density_scale < 0) out->density_scale = 0;
  if (out->emission_intensity < 0) out->emission_intensity = 0;
}

}  // namespace

void ResolveVolumeMaterials(RenderScene* scene) {
  for (RenderVolume& v : scene->volumes) {
    if (v.material_path.empty()) continue;
    const auto it = scene->material_by_path.find(v.material_path);
    if (it != scene->material_by_path.end()) v.material_id = it->second;
  }
}

bool RenderSceneConverter::Impl::ConvertVolume(const ::lightusd::next::Stage& stage,
                                               const UsdPrim& prim, RenderScene* scene,
                                               RenderVolume* out) {
  if (!scene || !out || prim.GetTypeName() != "Volume") return false;
  const double t = config_.time_code;
  if (!scene->volume_cache) {
    const ::lightusd::next::AssetResolver* resolver = config_.asset_resolver;
    vol::AssetReader reader = vol::ReadFileAsset;
    if (resolver)
      reader = [resolver](const std::string& path, std::vector<uint8_t>* bytes,
                          std::string* err) { return resolver->ReadAsset(path, bytes, err); };
    scene->volume_cache = std::make_shared<vol::FieldCache>(reader);
  }
  out->prim_path = prim.GetPath().str();
  double m[16];
  if (ComputeWorldTransform(stage, prim, m, t)) std::memcpy(out->world_matrix, m, sizeof(m));

  // Authored extent (local space).
  if (const ::lightusd::next::Value* e = prim.GetValueAtTime("extent", t)) {
    auto arr = e->view().as_float_array();
    if (arr.size() >= 6) {
      for (int a = 0; a < 3; ++a) {
        out->extent_min[a] = arr[size_t(a)];
        out->extent_max[a] = arr[size_t(3 + a)];
      }
      out->has_extent = true;
    }
  }

  std::vector<vol::ResolvedField> resolved;
  for (const std::string& rel : prim.GetRelationshipNames()) {
    if (rel.compare(0, sizeof(kFieldPrefix) - 1, kFieldPrefix) != 0) continue;
    const std::string binding = rel.substr(sizeof(kFieldPrefix) - 1);
    std::vector<::lightusd::next::Path> targets;
    prim.GetForwardedRelationshipTargets(rel, &targets);
    if (targets.empty()) continue;  // blocked: no field (UsdVolVolume::GetFieldPaths)
    if (targets.size() != 1) {
      out->warnings.push_back(out->prim_path + "." + rel + ": " + std::to_string(targets.size()) +
                              " targets; a field relationship needs exactly one");
      continue;
    }
    const UsdPrim field = stage.GetPrimAtPath(targets[0].str());
    if (!field.IsValid()) {
      out->warnings.push_back(out->prim_path + "." + rel + ": target " + targets[0].str() +
                              " is not a prim");
      continue;
    }
    if (!IsFieldAssetType(field.GetTypeName())) {
      out->warnings.push_back(out->prim_path + "." + rel + ": target " + targets[0].str() +
                              " is a " + field.GetTypeName() + ", not a volume field asset");
      continue;
    }
    vol::FieldRequest q;
    q.binding = binding;
    q.field_prim = field.GetPath().str();
    q.prim_type = field.GetTypeName();
    if (const ::lightusd::next::Value* fp = field.GetValueAtTime("filePath", t))
      if (auto a = fp->as_asset_path()) q.file_path = ResolveAssetPath(std::string(a), AssetAnchorOf(field));
    q.field_name = TokenAt(field, "fieldName", t);
    if (const ::lightusd::next::Value* fi = field.GetValueAtTime("fieldIndex", t))
      if (const int32_t* i = fi->view().as_int()) q.field_index = *i;
    q.field_data_type = TokenAt(field, "fieldDataType", t);
    q.field_class = TokenAt(field, "fieldClass", t);
    const std::string hint = TokenAt(field, "vectorDataRoleHint", t);
    if (!hint.empty()) q.vector_role_hint = hint;
    double fm[16];
    if (ComputeWorldTransform(stage, field, fm, t)) std::memcpy(q.field_to_world, fm, sizeof(fm));
    vol::ResolvedField r;
    std::string err;
    if (!scene->volume_cache->Resolve(q, &r, &err)) {
      out->warnings.push_back(err);
      continue;
    }
    for (const std::string& w : r.warnings) out->warnings.push_back(w);
    resolved.push_back(r);
    RenderVolumeField f;
    f.binding = binding;
    f.field = std::move(r);
    out->fields.push_back(std::move(f));
  }
  if (!out->has_extent && vol::FieldsWorldBounds(resolved, out->extent_min, out->extent_max)) {
    out->has_extent = true;
    out->extent_is_world = true;
  }

  // Volume material.
  const UsdPrim material = ::lightusd::next::GetBoundMaterial(stage, prim);
  if (material.IsValid()) {
    out->material_path = material.GetPath().str();
    const auto it = scene->material_by_path.find(out->material_path);
    if (it != scene->material_by_path.end()) out->material_id = it->second;
    const std::string shader_path = ::lightusd::next::GetVolumeShader(stage, material);
    if (!shader_path.empty()) {
      const UsdPrim shader = stage.GetPrimAtPath(shader_path);
      if (shader.IsValid()) ReadVolumeShader(stage, shader, t, out);
    }
  }
  return true;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
