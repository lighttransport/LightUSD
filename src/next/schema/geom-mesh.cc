// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdGeomMesh Schema Implementation

#include "geom-mesh.hh"
#include "../layer/property-index.hh"

#include <cmath>

namespace lightusd {
namespace next {

UsdGeomMesh::UsdGeomMesh(const UsdPrim& prim) : prim_(prim) {
  if (prim_.IsValid()) {
    const std::string& type = prim_.GetTypeName();
    is_mesh_ = (type == "Mesh");
  }
}

namespace {

const PropNameId& kIdFaceVertexCounts() {
  static const PropNameId id = GetPropNameTable().intern("faceVertexCounts");
  return id;
}

const PropNameId& kIdFaceVertexIndices() {
  static const PropNameId id = GetPropNameTable().intern("faceVertexIndices");
  return id;
}

const PropNameId& kIdPoints() {
  static const PropNameId id = GetPropNameTable().find("points");
  return id;
}

const PropNameId& kIdNormals() {
  static const PropNameId id = GetPropNameTable().find("normals");
  return id;
}

const PropNameId& kIdExtent() {
  static const PropNameId id = GetPropNameTable().find("extent");
  return id;
}

const PropNameId& kIdPrimvarsSt() {
  static const PropNameId id = GetPropNameTable().find("primvars:st");
  return id;
}

const PropNameId& kIdPrimvarsUv() {
  static const PropNameId id = GetPropNameTable().intern("primvars:uv");
  return id;
}

const PropNameId& kIdPrimvarsStIndices() {
  static const PropNameId id = GetPropNameTable().intern("primvars:st:indices");
  return id;
}

const PropNameId& kIdPrimvarsUvIndices() {
  static const PropNameId id = GetPropNameTable().intern("primvars:uv:indices");
  return id;
}

const PropNameId& kIdSubdivisionScheme() {
  static const PropNameId id = GetPropNameTable().find("subdivisionScheme");
  return id;
}

}  // namespace

std::vector<int> UsdGeomMesh::GetFaceVertexCounts() const {
  std::vector<int> result;
  if (!IsValid()) return result;

  const Value* val = prim_.GetPropertyValue(kIdFaceVertexCounts());
  if (!val) return result;

  const std::vector<int32_t>* arr = val->as_int_array();
  if (arr && !arr->empty()) {
    result = *arr;
  }
  return result;
}

std::vector<int> UsdGeomMesh::GetFaceVertexIndices() const {
  std::vector<int> result;
  if (!IsValid()) return result;

  const Value* val = prim_.GetPropertyValue(kIdFaceVertexIndices());
  if (!val) return result;

  const std::vector<int32_t>* arr = val->as_int_array();
  if (arr && !arr->empty()) {
    result = *arr;
  }
  return result;
}

size_t UsdGeomMesh::GetFaceCount() const {
  if (!IsValid()) return 0;

  const Value* val = prim_.GetPropertyValue(kIdFaceVertexCounts());
  if (!val) return 0;

  const std::vector<int32_t>* arr = val->as_int_array();
  return arr ? arr->size() : 0;
}

size_t UsdGeomMesh::GetPointCount() const {
  if (!IsValid()) return 0;

  const Value* val = prim_.GetPropertyValue(kIdPoints());
  if (!val) return 0;

  // Points are float3[], so divide by 3
  const std::vector<float>* arr = val->as_float_array();
  return arr ? arr->size() / 3 : 0;
}

std::vector<float> UsdGeomMesh::GetPoints() const {
  std::vector<float> result;
  if (!IsValid()) return result;

  const Value* val = prim_.GetPropertyValue(kIdPoints());
  if (!val) return result;

  const std::vector<float>* arr = val->as_float_array();
  if (arr && !arr->empty()) {
    result = *arr;
  }
  return result;
}

bool UsdGeomMesh::GetPoints(std::vector<float>& x, std::vector<float>& y, std::vector<float>& z) const {
  if (!IsValid()) return false;

  const Value* val = prim_.GetPropertyValue(kIdPoints());
  if (!val) return false;

  const std::vector<float>* arr = val->as_float_array();
  if (!arr || arr->size() < 3) return false;

  size_t point_count = arr->size() / 3;
  x.resize(point_count);
  y.resize(point_count);
  z.resize(point_count);

  for (size_t i = 0; i < point_count; ++i) {
    x[i] = (*arr)[i * 3 + 0];
    y[i] = (*arr)[i * 3 + 1];
    z[i] = (*arr)[i * 3 + 2];
  }
  return true;
}

std::vector<float> UsdGeomMesh::GetNormals() const {
  std::vector<float> result;
  if (!IsValid()) return result;

  const Value* val = prim_.GetPropertyValue(kIdNormals());
  if (!val) return result;

  const std::vector<float>* arr = val->as_float_array();
  if (arr && !arr->empty()) {
    result = *arr;
  }
  return result;
}

bool UsdGeomMesh::HasNormals() const {
  if (!IsValid()) return false;
  return prim_.HasProperty(kIdNormals());
}

bool UsdGeomMesh::GetExtent(float* min, float* max) const {
  if (!IsValid() || !min || !max) return false;

  const Value* val = prim_.GetPropertyValue(kIdExtent());
  if (!val) return false;

  // Extent is float3[2] - min and max corners
  const std::vector<float>* arr = val->as_float_array();
  if (!arr || arr->size() < 6) return false;

  min[0] = (*arr)[0]; min[1] = (*arr)[1]; min[2] = (*arr)[2];
  max[0] = (*arr)[3]; max[1] = (*arr)[4]; max[2] = (*arr)[5];
  return true;
}

std::vector<float> UsdGeomMesh::GetUVs() const {
  std::vector<float> result;
  if (!IsValid()) return result;

  // Try common UV primvar names
  const Value* val = prim_.GetPropertyValue(kIdPrimvarsSt());
  if (!val) {
    val = prim_.GetPropertyValue(kIdPrimvarsUv());
  }
  if (!val) return result;

  const std::vector<float>* arr = val->as_float_array();
  if (arr && !arr->empty()) {
    result = *arr;
  }
  return result;
}

bool UsdGeomMesh::HasUVs() const {
  if (!IsValid()) return false;
  return prim_.HasProperty(kIdPrimvarsSt()) || prim_.HasProperty(kIdPrimvarsUv());
}

std::vector<int> UsdGeomMesh::GetUVIndices() const {
  std::vector<int> result;
  if (!IsValid()) return result;

  const Value* val = prim_.GetPropertyValue(kIdPrimvarsStIndices());
  if (!val) {
    val = prim_.GetPropertyValue(kIdPrimvarsUvIndices());
  }
  if (!val) return result;

  const std::vector<int32_t>* arr = val->as_int_array();
  if (arr && !arr->empty()) {
    result = *arr;
  }
  return result;
}

std::string UsdGeomMesh::GetSubdivisionScheme() const {
  if (!IsValid()) return "none";

  const Value* val = prim_.GetPropertyValue(kIdSubdivisionScheme());
  if (!val) return "none";

  const std::string* scheme = val->as_token();
  if (scheme) return *scheme;
  return "none";
}

bool UsdGeomMesh::IsSubdivisionSurface() const {
  std::string scheme = GetSubdivisionScheme();
  return scheme != "none" && !scheme.empty();
}

std::vector<float> UsdGeomMesh::GetPointsAtTimecode(double timecode) const {
  std::vector<float> result;
  if (!IsValid()) return result;

  Value hold;
  const Value* val = nullptr;
  if (!std::isnan(timecode)) {
    Value v = prim_.GetInterpolatedValue(kIdPoints(), timecode);
    if (!v.is_empty()) {
      hold = std::move(v);
      val = &hold;
    } else {
      val = prim_.GetValueAtTime(kIdPoints(), timecode);
    }
  }
  if (!val) val = prim_.GetPropertyValue(kIdPoints());
  if (!val) return result;

  const std::vector<float>* arr = val->as_float_array();
  if (arr && !arr->empty()) {
    result = *arr;
  }
  return result;
}

bool UsdGeomMesh::HasAnimatedPoints() const {
  if (!IsValid()) return false;
  return prim_.HasTimeSamples(kIdPoints());
}

std::vector<double> UsdGeomMesh::GetPointsTimeSamples() const {
  if (!IsValid()) return {};
  return prim_.GetTimeSampleTimes(kIdPoints());
}

bool IsMesh(const UsdPrim& prim) {
  if (!prim.IsValid()) return false;
  return prim.GetTypeName() == "Mesh";
}

std::vector<UsdGeomMesh> GetAllMeshes(const Stage& stage) {
  std::vector<UsdGeomMesh> meshes;

  stage.Traverse([&meshes](const UsdPrim& prim) {
    if (IsMesh(prim)) {
      meshes.emplace_back(prim);
    }
    return true;  // continue traversal
  });

  return meshes;
}

}  // namespace next
}  // namespace lightusd
