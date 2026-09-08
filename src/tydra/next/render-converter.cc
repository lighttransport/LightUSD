// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Render Scene Converter Implementation

#include "safe-arithmetic.hh"
#include "core/path-expression-eval.hh"
#include "next/schema/usd-vol.hh"
#include "next/schema/usd-geom-camera.hh"
#include "render-converter.hh"
#include "render-converter-assembly.hh"
#include "render-converter-material-finalize.hh"
#include "render-converter-light-linking.hh"
#include "mem-budget.hh"
#include "next/schema/color-space.hh"
#include "next/eval/value-clip.hh"
#include "next/resolver/asset-resolver.hh"
#include "materialx.hh"
#include "next/layer/asset-anchor.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/schema/usdPhysics.hh"
#include "next/schema/usd-shade.hh"
#include "next/schema/usd-skel.hh"
#include "next/types/type-info.hh"
#include "tydra/fast-mikktspace.hh"
#include "tydra/mikktspace-tangent.hh"
#include "tydra/shape-to-mesh.hh"
#include "tydra/tangent-quantize.hh"
#include "tsd/tinysubdiv.hh"
#include "external/mapbox/earcut/earcut.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <deque>
#include <memory>
#include <unordered_map>
#include <optional>
#include <set>
#include <sstream>
#include <iomanip>
#include <atomic>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif
#include <unordered_set>

namespace lightusd {
namespace tydra {
namespace next {

using ::lightusd::next::Stage;
using ::lightusd::next::UsdPrim;
using ::lightusd::next::Value;

namespace {
std::string LeafNameFromJointPath(const std::string& path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos) return path;
  if (pos + 1 >= path.size()) return "";
  return path.substr(pos + 1);
}
bool JointTokenMatches(const SkeletonJoint& joint, const std::string& token) {
  if (token.empty()) return false;
  if (joint.path == token || joint.name == token) return true;
  if (LeafNameFromJointPath(joint.path) == token) return true;
  if (joint.path.size() > token.size() &&
      joint.path.compare(joint.path.size() - token.size(), token.size(),
                         token) == 0) {
    const size_t sep = joint.path.size() - token.size();
    return sep == 0 || joint.path[sep - 1] == '/';
  }
  return false;
}

std::string FirstValidBoundMaterialPath(const Stage& stage,
                                        const ::lightusd::next::UsdPrim& prim,
                                        const std::string& purpose) {
  static const char* kPreviewOrder[] = {"material:binding:preview",
                                        "material:binding",
                                        "material:binding:full"};
  static const char* kFullOrder[] = {"material:binding:full",
                                     "material:binding",
                                     "material:binding:preview"};
  const char* const* bindingOrder = purpose == "full" ? kFullOrder
                                                       : kPreviewOrder;
  for (size_t i = 0; i < 3; ++i) {
    const char* rel = bindingOrder[i];
    const std::vector<::lightusd::next::Path>* targets =
        prim.GetRelationship(rel);
    if (!targets || targets->empty()) continue;
    const std::string p = (*targets)[0].str();
    if (!p.empty() && ::lightusd::tydra::next::IsMaterial(
                          stage.GetPrimAtPath(p))) {
      return p;
    }
  }
  return "";
}

std::string FindInheritedMaterialBinding(const Stage& stage,
                                         const std::string& prim_path,
                                         const std::string& purpose) {
  // Core UsdShade resolution now validates targets at every purpose/ancestor
  // step and associates bindMaterialAs with the relationship that actually
  // won. Keep one implementation shared by the converter and applications.
  return ::lightusd::next::GetInheritedBoundMaterialPathForPurpose(
      stage, prim_path, purpose.empty() ? "preview" : purpose);
}


constexpr int kMaxMtlxConstantDepth = 64;

// 2GB is the typical hard limit for legacy WebAssembly linear memory growth in
// non-shared-memory builds. Keep a conservative per-mesh budget for temporary
// triangulation artifacts to avoid allocator abort on pathological data.







const ::lightusd::next::PropNameId& kIdOutputsOut() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("outputs:out");
  return id;
}



const ::lightusd::next::PropNameId& kIdFaceVertexCounts() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("faceVertexCounts");
  return id;
}

const ::lightusd::next::PropNameId& kIdFaceVertexIndices() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("faceVertexIndices");
  return id;
}





const ::lightusd::next::PropNameId& kIdHoleIndices() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("holeIndices");
  return id;
}

const ::lightusd::next::PropNameId& kIdPoints() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("points");
  return id;
}

const ::lightusd::next::PropNameId& kIdTetVertexIndices() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("tetVertexIndices");
  return id;
}

const ::lightusd::next::PropNameId& kIdNormals() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("normals");
  return id;
}

const ::lightusd::next::PropNameId& kIdDisplayColor() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("primvars:displayColor");
  return id;
}

const ::lightusd::next::PropNameId& kIdDisplayOpacity() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("primvars:displayOpacity");
  return id;
}

const ::lightusd::next::PropNameId& kIdPrimvarsSt() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("primvars:st");
  return id;
}

const ::lightusd::next::PropNameId& kIdExtent() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("extent");
  return id;
}

const ::lightusd::next::PropNameId& kIdCurveVertexCounts() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("curveVertexCounts");
  return id;
}

const ::lightusd::next::PropNameId& kIdWidths() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("widths");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsColor() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:color");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsIntensity() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:intensity");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsExposure() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:exposure");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsNormalize() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:normalize");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsEnableColorTemperature() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:enableColorTemperature");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsColorTemperature() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:colorTemperature");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsDiffuse() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:diffuse");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsSpecular() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:specular");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShapingConeAngle() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shaping:cone:angle");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShapingFocus() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shaping:focus");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShapingFocusTint() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shaping:focusTint");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShapingConeSoftness() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shaping:cone:softness");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShapingIesAngleScale() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shaping:ies:angleScale");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShapingIesNormalize() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shaping:ies:normalize");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShapingIesFile() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shaping:ies:file");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsRadius() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:radius");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsWidth() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:width");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsHeight() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:height");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsLength() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:length");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsAngle() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:angle");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsTextureFormat() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:texture:format");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShadowEnable() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shadow:enable");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsEnableShadows() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:enableShadows");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShadowColor() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shadow:color");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShadowDistance() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shadow:distance");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShadowFalloff() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shadow:falloff");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsShadowFalloffGamma() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:shadow:falloffGamma");
  return id;
}

const ::lightusd::next::PropNameId& kIdInputsTextureFile() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:texture:file");
  return id;
}

const ::lightusd::next::PropNameId& kIdProjection() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("projection");
  return id;
}

const ::lightusd::next::PropNameId& kIdFocalLength() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("focalLength");
  return id;
}

const ::lightusd::next::PropNameId& kIdHorizontalAperture() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("horizontalAperture");
  return id;
}

const ::lightusd::next::PropNameId& kIdVerticalAperture() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("verticalAperture");
  return id;
}

const ::lightusd::next::PropNameId& kIdFocusDistance() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("focusDistance");
  return id;
}

const ::lightusd::next::PropNameId& kIdShutterOpen() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("shutter:open");
  return id;
}

const ::lightusd::next::PropNameId& kIdShutterClose() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("shutter:close");
  return id;
}

const ::lightusd::next::PropNameId& kIdFStop() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("fStop");
  return id;
}

bool GetBool(const UsdPrim& prim, const ::lightusd::next::PropNameId& name_id,
             bool* out) {
  if (!out) return false;
  const Value* val = prim.GetPropertyValue(name_id);
  if (!val) return false;
  const bool* b = val->as_bool();
  if (!b) return false;
  *out = *b;
  return true;
}

bool GetFloat(const UsdPrim& prim, const ::lightusd::next::PropNameId& name_id,
              float* out) {
  if (!out) return false;
  const Value* val = prim.GetPropertyValue(name_id);
  if (!val) return false;
  const float* f = val->as_float();
  if (f) {
    *out = *f;
    return true;
  }
  const double* d = val->as_double();
  if (d) {
    *out = static_cast<float>(*d);
    return true;
  }
  return false;
}

bool GetFloat3(const UsdPrim& prim, const ::lightusd::next::PropNameId& name_id,
               float* x, float* y, float* z) {
  if (!x || !y || !z) return false;
  const Value* val = prim.GetPropertyValue(name_id);
  if (!val) return false;
  const float* f3 = val->as_float3();
  if (f3) {
    x[0] = f3[0];
    y[0] = f3[1];
    z[0] = f3[2];
    return true;
  }
  const double* d3 = val->as_double3();
  if (d3) {
    x[0] = static_cast<float>(d3[0]);
    y[0] = static_cast<float>(d3[1]);
    z[0] = static_cast<float>(d3[2]);
    return true;
  }
  return false;
}

bool GetDouble(const UsdPrim& prim,
               const ::lightusd::next::PropNameId& name_id, double* out) {
  if (!out) return false;
  const Value* val = prim.GetPropertyValue(name_id);
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

bool GetToken(const UsdPrim& prim,
             const ::lightusd::next::PropNameId& name_id, std::string* out) {
  if (!out) return false;
  const Value* val = prim.GetPropertyValue(name_id);
  if (!val) return false;
  const std::string* token = val->as_token();
  if (token) {
    *out = *token;
    return true;
  }
  const std::string* str = val->as_string();
  if (!str) return false;
  *out = *str;
  return true;
}

bool GetBool(const UsdPrim& prim, const char* name, bool* out) {
  const auto name_id = ::lightusd::next::GetPropNameTable().find(name);
  if (!name_id.is_valid()) return false;
  return GetBool(prim, name_id, out);
}

bool GetFloat(const UsdPrim& prim, const char* name, float* out) {
  const auto name_id = ::lightusd::next::GetPropNameTable().find(name);
  if (!name_id.is_valid()) return false;
  return GetFloat(prim, name_id, out);
}

bool GetDouble(const UsdPrim& prim, const char* name, double* out) {
  const auto name_id = ::lightusd::next::GetPropNameTable().find(name);
  if (!name_id.is_valid()) return false;
  return GetDouble(prim, name_id, out);
}

bool GetToken(const UsdPrim& prim, const char* name, std::string* out) {
  const auto name_id = ::lightusd::next::GetPropNameTable().find(name);
  if (!name_id.is_valid()) return false;
  return GetToken(prim, name_id, out);
}

bool GetBool(const UsdPrim& prim, const std::string& name, bool* out) {
  return GetBool(prim, name.c_str(), out);
}

bool GetDouble(const UsdPrim& prim, const std::string& name, double* out) {
  return GetDouble(prim, name.c_str(), out);
}

bool GetToken(const UsdPrim& prim, const std::string& name, std::string* out) {
  return GetToken(prim, name.c_str(), out);
}

size_t AuthoredArraySize(const UsdPrim& prim,
                        const ::lightusd::next::PropNameId& name_id) {
  const Value* value = prim.GetPropertyValue(name_id);
  return value && value->is_array() ? value->array_size() : 0;
}

bool WouldOverflowSizeMul(size_t a, size_t b) {
  if (a == 0 || b == 0) return false;
  return a > (std::numeric_limits<size_t>::max() / b);
}

std::string SourcePrimPathFromConnection(const std::string& connection_path) {
  size_t dot_pos = connection_path.find(".outputs:");
  if (dot_pos == std::string::npos) {
    dot_pos = connection_path.find(".inputs:");
  }
  if (dot_pos == std::string::npos) {
    dot_pos = connection_path.rfind('.');
  }
  if (dot_pos == std::string::npos) {
    return connection_path;
  }
  return connection_path.substr(0, dot_pos);
}

bool SplitConnectionPath(const std::string& connection_path,
                         std::string* prim_path,
                         std::string* prop_name) {
  if (!prim_path || !prop_name) return false;
  size_t dot_pos = connection_path.find(".outputs:");
  if (dot_pos == std::string::npos) {
    dot_pos = connection_path.find(".inputs:");
  }
  if (dot_pos == std::string::npos) {
    dot_pos = connection_path.rfind('.');
  }
  if (dot_pos == std::string::npos) return false;

  *prim_path = connection_path.substr(0, dot_pos);
  *prop_name = connection_path.substr(dot_pos + 1);
  return !prim_path->empty() && !prop_name->empty();
}

bool IsTextureEndpoint(const Stage& stage, const UsdPrim& prim,
                       double time_code) {
  if (!prim.IsValid()) return false;
  std::string id;
  GetToken(prim, "info:id", &id);
  if (id == "UsdUVTexture" || id == "HwPtexTexture" ||
      id.rfind("HwPtexTexture", 0) == 0 || id == "PxrPtexture" ||
      id == "image" || id == "tiledimage" ||
      id.rfind("ND_image_", 0) == 0 ||
      id.rfind("ND_tiledimage_", 0) == 0) {
    return true;
  }
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  return eval.EvalAssetPath(prim, "inputs:file").has_value() ||
         eval.EvalString(prim, "inputs:file").has_value() ||
         eval.EvalAssetPath(prim, "inputs:filename").has_value() ||
         eval.EvalString(prim, "inputs:filename").has_value();
}

const std::vector<::lightusd::next::Path>* PrimaryDataInputConnection(
    const UsdPrim& prim) {
  const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) return nullptr;

  static const char* kPreferredInputs[] = {
      "inputs:in", "inputs:in1", "inputs:dispScalar", "inputs:inputRGB",
      "inputs:fg", "inputs:bg"};
  for (const char* preferred : kPreferredInputs) {
    const std::vector<::lightusd::next::Path>* connections =
        spec->connection(preferred);
    if (connections && !connections->empty()) return connections;
  }

  auto is_factor_input = [](const std::string& name) {
    return name == "inputs:mix" || name == "inputs:amount" ||
           name == "inputs:weight" || name == "inputs:factor" ||
           name == "inputs:alpha" || name == "inputs:mask";
  };
  for (const std::string& property : prim.GetPropertyNames()) {
    if (property.rfind("inputs:", 0) != 0 || is_factor_input(property)) {
      continue;
    }
    const std::vector<::lightusd::next::Path>* connections =
        spec->connection(property);
    if (connections && !connections->empty()) return connections;
  }
  return nullptr;
}

bool ResolveConnectedEndpoint(const Stage& stage,
                              const std::string& connection_path,
                              double time_code,
                              std::string* endpoint_path) {
  if (!endpoint_path) return false;
  std::string current = connection_path;
  std::set<std::string> visited;
  for (int depth = 0; depth <= kMaxMtlxConstantDepth; ++depth) {
    if (!visited.insert(current).second) return false;

    std::string prim_path;
    std::string prop_name;
    if (!SplitConnectionPath(current, &prim_path, &prop_name)) return false;
    UsdPrim prim = stage.GetPrimAtPath(prim_path);
    if (!prim.IsValid()) return false;
    if (IsTextureEndpoint(stage, prim, time_code)) {
      *endpoint_path = current;
      return true;
    }

    const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* connections =
        spec ? spec->connection(prop_name) : nullptr;
    if (!connections || connections->empty()) {
      connections = PrimaryDataInputConnection(prim);
    }
    if (!connections || connections->empty()) {
      *endpoint_path = current;
      return true;
    }
    current = (*connections)[0].str();
  }
  return false;
}

bool FindConnectedUtilityScalar(const Stage& stage, const UsdPrim& shader,
                                const std::string& shader_input,
                                const std::string& node_id_prefix,
                                const std::string& node_input,
                                double time_code, float* out) {
  if (!out || !shader.IsValid()) return false;
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  const std::string property = "inputs:" + shader_input;
  if (!eval.HasConnection(shader, property)) return false;
  std::string current = eval.GetConnectionPath(shader, property);
  std::set<std::string> visited;
  for (int depth = 0; depth <= kMaxMtlxConstantDepth; ++depth) {
    if (!visited.insert(current).second) return false;
    std::string prim_path;
    std::string prop_name;
    if (!SplitConnectionPath(current, &prim_path, &prop_name)) return false;
    const UsdPrim node = stage.GetPrimAtPath(prim_path);
    if (!node.IsValid()) return false;
    std::string id;
    GetToken(node, "info:id", &id);
    if (id.rfind(node_id_prefix, 0) == 0) {
      if (std::optional<float> value =
              eval.EvalFloat(node, "inputs:" + node_input)) {
        *out = *value;
        return true;
      }
      return false;
    }
    const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec();
    const std::vector<::lightusd::next::Path>* connections =
        spec ? spec->connection(prop_name) : nullptr;
    if (!connections || connections->empty()) {
      connections = PrimaryDataInputConnection(node);
    }
    if (!connections || connections->empty()) return false;
    current = (*connections)[0].str();
  }
  return false;
}

bool ResolveConnectedValue(const Stage& stage,
                           const std::string& connection_path,
                           double time_code,
                           Value* out) {
  if (!out) return false;

  std::string endpoint;
  if (!ResolveConnectedEndpoint(stage, connection_path, time_code, &endpoint)) {
    return false;
  }
  std::string prim_path;
  std::string prop_name;
  if (!SplitConnectionPath(endpoint, &prim_path, &prop_name)) return false;

  UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;

  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  ::lightusd::next::EvalOptions opts = eval.GetOptions();
  opts.follow_connections = false;
  ::lightusd::next::EvalResult result = eval.EvalWith(prim, prop_name, opts);
  if (result.success) {
    *out = std::move(result.value);
    return true;
  }

  return false;
}

RenderTexture::Channel ChannelFromConnection(
    const std::string& connection_path, const UsdPrim& texture_prim) {
  size_t pos = connection_path.find(".outputs:");
  if (pos == std::string::npos) {
    return RenderTexture::Channel::RGBA;
  }

  const std::string channel = connection_path.substr(pos + 9);
  if (channel == "r" || channel == "x") return RenderTexture::Channel::R;
  if (channel == "g" || channel == "y") return RenderTexture::Channel::G;
  if (channel == "b" || channel == "z") return RenderTexture::Channel::B;
  if (channel == "a" || channel == "w") return RenderTexture::Channel::A;
  if (channel == "rgb" || channel == "xyz") return RenderTexture::Channel::RGB;

  // MaterialX image nodes conventionally expose a generic `outputs:out`.
  // Recover its scalar/vector shape from the synthesized output value/type so
  // roughness and metallic maps sample R while color/normal maps sample RGB.
  if (channel == "out" && texture_prim.IsValid()) {
    std::string type;
    if (const Value* value = texture_prim.GetPropertyValue(kIdOutputsOut())) {
      if (const std::string* token = value->as_token()) type = *token;
      else if (const std::string* str = value->as_string()) type = *str;
    }
    if (type.empty()) {
      if (const ::lightusd::next::PrimSpec* spec =
              texture_prim.GetPrimSpec()) {
        if (const std::string* declared =
                spec->property_type_name("outputs:out")) {
          type = *declared;
        }
      }
    }
    if (type == "float" || type == "integer" || type == "boolean") {
      return RenderTexture::Channel::R;
    }
    if (type == "color3" || type == "color3f" || type == "vector3" ||
        type == "vector3f") {
      return RenderTexture::Channel::RGB;
    }
  }
  return RenderTexture::Channel::RGBA;
}

WrapMode ParseWrapMode(const std::string& token) {
  // UsdUVTexture uses repeat/clamp/mirror/black, while MaterialX image nodes
  // call the equivalent modes periodic/clamp/mirror/constant. Keep the
  // translation at the RenderTexture boundary so every backend sees one
  // canonical enum.
  if (token == "repeat" || token == "periodic") return WrapMode::Repeat;
  if (token == "clamp") return WrapMode::Clamp;
  if (token == "mirror") return WrapMode::Mirror;
  if (token == "black" || token == "constant") return WrapMode::Black;
  // UsdUVTexture's wrapS/wrapT fallback is "useMetadata"; with no texture
  // metadata the effective mode is clamp-to-edge (legacy tydra behavior) —
  // NOT repeat, which visibly tiles textures authored to clamp.
  return WrapMode::Clamp;
}

ColorSpace ParseColorSpace(const std::string& token) {
  if (token == "raw") return ColorSpace::Raw;
  if (token == "linear" || token == "Linear" || token == "lin_srgb" ||
      token == "lin_rec709" || token == "scene-linear Rec.709-sRGB") {
    return ColorSpace::Linear;
  }
  if (token == "sRGB" || token == "srgb" || token == "srgb_texture") {
    return ColorSpace::sRGB;
  }
  if (token == "acescg" || token == "ACEScg") return ColorSpace::ACEScg;
  if (token == "rec709" || token == "Rec709") return ColorSpace::Rec709;
  if (token == "rec2020" || token == "Rec2020" ||
      token == "lin_rec2020") return ColorSpace::Rec2020;
  if (token == "displayP3" || token == "DisplayP3" ||
      token == "lin_displayp3" || token == "srgb_displayp3") {
    return ColorSpace::DisplayP3;
  }
  return ColorSpace::Unknown;
}

bool IsColorShaderInput(const UsdPrim& prim, const std::string& attr_name,
                        const std::string& param_name) {
  if (const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec()) {
    if (const std::string* type = spec->property_type_name(attr_name)) {
      if (type->rfind("color3", 0) == 0 || type->rfind("color4", 0) == 0) {
        return true;
      }
    }
  }
  static const std::set<std::string> kColorInputs = {
      "diffuseColor", "emissiveColor", "specularColor", "base_color",
      "baseColor", "specular_color", "transmission_color",
      "subsurface_color", "sheen_color", "coat_color", "emission_color"};
  return kColorInputs.count(param_name) != 0;
}

bool MaterialXConfiguredColorSpace(const UsdPrim& prim, std::string* out) {
  if (!out || !prim.IsValid()) return false;
  std::string id;
  if (!GetToken(prim, "info:id", &id) ||
      (id.rfind("ND_", 0) != 0 && id != "image" &&
       id != "tiledimage" && id != "open_pbr_surface" &&
       id != "standard_surface")) {
    return false;
  }
  for (UsdPrim current = prim; current.IsValid(); current = current.GetParent()) {
    const Value* value =
        current.GetPropertyValue("config:mtlx:colorspace");
    if (!value) continue;
    const std::string* token = value->as_token();
    if (!token) token = value->as_string();
    if (token && !token->empty()) {
      *out = ::lightusd::color::CanonicalizeToken(*token);
      return true;
    }
  }
  return false;
}

bool ResolveConnectedColorSource(const Stage& stage,
                                 const std::string& connection_path,
                                 double time_code, UsdPrim* source_prim,
                                 std::string* source_property) {
  if (!source_prim || !source_property) return false;
  std::string endpoint;
  if (!ResolveConnectedEndpoint(stage, connection_path, time_code, &endpoint)) {
    return false;
  }
  std::string prim_path;
  std::string property;
  if (!SplitConnectionPath(endpoint, &prim_path, &property)) return false;
  UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;

  // Prefer metadata on the resolved output itself. MaterialX constant and
  // utility nodes usually put it on their value/data input instead, so scan
  // those inputs before falling back to the terminal shader attribute.
  if (const ::lightusd::next::PropMeta* meta =
          prim.GetPropertyMeta(property)) {
    if ((meta->authored & ::lightusd::next::PropMeta::kColorSpace) != 0u) {
      *source_prim = prim;
      *source_property = property;
      return true;
    }
  }
  std::string value_input;
  for (const std::string& candidate : prim.GetPropertyNames()) {
    if (candidate.rfind("inputs:", 0) != 0) continue;
    if (value_input.empty() || candidate == "inputs:value" ||
        candidate == "inputs:in") {
      value_input = candidate;
    }
    if (const ::lightusd::next::PropMeta* meta =
            prim.GetPropertyMeta(candidate)) {
      if ((meta->authored & ::lightusd::next::PropMeta::kColorSpace) != 0u) {
        *source_prim = prim;
        *source_property = candidate;
        return true;
      }
    }
  }
  if (!value_input.empty()) {
    *source_prim = prim;
    *source_property = value_input;
    return true;
  }
  *source_prim = prim;
  *source_property = property;
  return true;
}

void ConvertShaderColorToWorking(const UsdPrim& prim,
                                 const std::string& attr_name,
                                 const std::string& param_name,
                                 const RenderScene* scene,
                                 ShaderParam* param) {
  if (!scene || !param || param->is_texture() ||
      !IsColorShaderInput(prim, attr_name, param_name)) {
    return;
  }
  std::string source;
  bool authored = false;
  if (!::lightusd::next::color_management::ComputeColorSpaceName(
          prim, attr_name, &source, &authored)) {
    return;
  }
  if (!authored) {
    (void)MaterialXConfiguredColorSpace(prim, &source);
  }
  ::lightusd::color::ColorTransform transform;
  if (!::lightusd::next::color_management::BuildColorTransform(
          prim, source, scene->working_color_space, &transform)) {
    return;
  }
  float rgb[3] = {param->value.x, param->value.y, param->value.z};
  ::lightusd::color::TransformRGB(transform, rgb);
  param->value.x = rgb[0];
  param->value.y = rgb[1];
  param->value.z = rgb[2];
}

void SetParamFloat(ShaderParam* out, float x) {
  out->texture_id = -1;
  out->value = Float4(x, 0.0f, 0.0f, 0.0f);
}

void SetParamFloat3(ShaderParam* out, float x, float y, float z) {
  out->texture_id = -1;
  out->value = Float4(x, y, z, 1.0f);
}

void SetParamFloat4(ShaderParam* out, float x, float y, float z, float w) {
  out->texture_id = -1;
  out->value = Float4(x, y, z, w);
}

bool ValueToShaderParam(const Value& value, ShaderParam* out) {
  if (!out || value.is_empty() || value.is_array()) return false;

  if (const float* v = value.as_float()) {
    SetParamFloat(out, *v);
    return true;
  }
  if (const double* v = value.as_double()) {
    SetParamFloat(out, static_cast<float>(*v));
    return true;
  }
  if (const int32_t* v = value.as_int()) {
    SetParamFloat(out, static_cast<float>(*v));
    return true;
  }
  if (const uint32_t* v = value.as_uint()) {
    SetParamFloat(out, static_cast<float>(*v));
    return true;
  }
  if (const bool* v = value.as_bool()) {
    SetParamFloat(out, *v ? 1.0f : 0.0f);
    return true;
  }
  if (const float* v = value.as_float2()) {
    SetParamFloat4(out, v[0], v[1], 0.0f, 1.0f);
    return true;
  }
  if (const float* v = value.as_float3()) {
    SetParamFloat3(out, v[0], v[1], v[2]);
    return true;
  }
  if (const float* v = value.as_float4()) {
    SetParamFloat4(out, v[0], v[1], v[2], v[3]);
    return true;
  }
  if (const double* v = value.as_double2()) {
    SetParamFloat4(out, static_cast<float>(v[0]), static_cast<float>(v[1]),
                   0.0f, 1.0f);
    return true;
  }
  if (const double* v = value.as_double3()) {
    SetParamFloat3(out, static_cast<float>(v[0]), static_cast<float>(v[1]),
                   static_cast<float>(v[2]));
    return true;
  }
  if (const double* v = value.as_double4()) {
    SetParamFloat4(out, static_cast<float>(v[0]), static_cast<float>(v[1]),
                   static_cast<float>(v[2]), static_cast<float>(v[3]));
    return true;
  }
  // Half-precision shader inputs (half/half2/half3/half4 + role types) store
  // raw half-bit lanes; widen through the converting reads.
  {
    float h[4];
    if (value.to_float(h)) {
      SetParamFloat(out, h[0]);
      return true;
    }
    if (value.to_float2(h)) {
      SetParamFloat4(out, h[0], h[1], 0.0f, 1.0f);
      return true;
    }
    if (value.to_float3(h)) {
      SetParamFloat3(out, h[0], h[1], h[2]);
      return true;
    }
    if (value.to_float4(h)) {
      SetParamFloat4(out, h[0], h[1], h[2], h[3]);
      return true;
    }
  }

  return false;
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
  }
  return out;
}

std::string ConnectionNodeName(const std::string& path) {
  const size_t slash = path.rfind('/');
  const size_t dot = path.rfind('.');
  if (slash == std::string::npos || dot == std::string::npos || dot <= slash)
    return {};
  return path.substr(slash + 1, dot - slash - 1);
}

std::string ConnectionOutputName(const std::string& path) {
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return {};
  std::string out = path.substr(dot + 1);
  if (out.compare(0, 8, "outputs:") == 0) out.erase(0, 8);
  return out;
}

std::string ConnectionPropertyName(const std::string& path) {
  const size_t dot = path.rfind('.');
  return dot == std::string::npos ? std::string() : path.substr(dot + 1);
}

void EmitNextGraphValue(std::ostream& os, const Value& value) {
  if (const std::string* v = value.as_asset_path()) {
    os << '"' << JsonEscape(*v) << '"';
    return;
  }
  if (const std::string* v = value.as_string()) {
    os << '"' << JsonEscape(*v) << '"';
    return;
  }
  if (const std::string* v = value.as_token()) {
    os << '"' << JsonEscape(*v) << '"';
    return;
  }
  ShaderParam p;
  if (!ValueToShaderParam(value, &p)) { os << "null"; return; }
  int lanes = 1;
  if (value.as_float2() || value.as_double2()) lanes = 2;
  else if (value.as_float3() || value.as_double3()) lanes = 3;
  else if (value.as_float4() || value.as_double4()) lanes = 4;
  const float v[4] = {p.value.x, p.value.y, p.value.z, p.value.w};
  if (lanes == 1) { os << v[0]; return; }
  os << '[';
  for (int i = 0; i < lanes; ++i) { if (i) os << ','; os << v[i]; }
  os << ']';
}

// Preserve the programmable MaterialX graph in the same compact JSON schema
// consumed by the shared lusdview graph compiler. The next converter already
// resolves simple constants and images; this record retains the full utility
// node topology for descriptor-backed renderers instead of silently baking it.
std::string BuildNextMaterialXGraphJson(const Stage& stage,
                                        const UsdPrim& shader,
                                        bool volume_graph = false) {
  const UsdPrim material = shader.GetParent();
  if (!material.IsValid()) return {};
  std::vector<UsdPrim> graphs;
  ::lightusd::next::AttributeEval shader_connections(&stage);
  for (const std::string& prop : shader.GetPropertyNames()) {
    if (prop.compare(0, 7, "inputs:") != 0 ||
        !shader_connections.HasConnection(shader, prop)) continue;
    const std::string connection =
        shader_connections.GetConnectionPath(shader, prop);
    UsdPrim candidate = stage.GetPrimAtPath(
        SourcePrimPathFromConnection(connection));
    if (::lightusd::next::IsNodeGraph(candidate) &&
        std::none_of(graphs.begin(), graphs.end(), [&](const UsdPrim& item) {
          return item.GetPath() == candidate.GetPath();
        })) {
      graphs.push_back(candidate);
    }
  }
  for (size_t i = 0; i < material.GetChildCount(); ++i) {
    if (!graphs.empty()) break;
    UsdPrim child = material.GetChildAt(i);
    if (::lightusd::next::IsNodeGraph(child)) { graphs.push_back(child); break; }
  }
  const bool direct_graph = graphs.empty();
  const bool graph_forest = graphs.size() > 1;
  const std::string graph_name = direct_graph
      ? material.GetName() + "_direct_graph"
      : (graph_forest ? material.GetName() + "_graphs"
                      : graphs.front().GetName());
  std::vector<UsdPrim> graph_nodes;
  std::function<void(const UsdPrim&)> collect_nodes = [&](const UsdPrim& parent) {
    for (size_t ci = 0; ci < parent.GetChildCount(); ++ci) {
      const UsdPrim child = parent.GetChildAt(ci);
      if (::lightusd::next::IsShader(child)) graph_nodes.push_back(child);
      else if (::lightusd::next::IsNodeGraph(child)) collect_nodes(child);
    }
  };
  if (direct_graph) {
    std::unordered_set<std::string> visited;
    std::function<void(const UsdPrim&)> collect_upstream =
        [&](const UsdPrim& node) {
          if (!node.IsValid() || !::lightusd::next::IsShader(node) ||
              node.GetPath() == shader.GetPath() ||
              !visited.insert(node.GetPath().str()).second) return;
          ::lightusd::next::AttributeEval eval(&stage);
          for (const std::string& prop : node.GetPropertyNames()) {
            if (prop.compare(0, 7, "inputs:") != 0 ||
                !eval.HasConnection(node, prop)) continue;
            collect_upstream(stage.GetPrimAtPath(SourcePrimPathFromConnection(
                eval.GetConnectionPath(node, prop))));
          }
          graph_nodes.push_back(node);
        };
    for (const std::string& prop : shader.GetPropertyNames()) {
      if (prop.compare(0, 7, "inputs:") != 0 ||
          !shader_connections.HasConnection(shader, prop)) continue;
      collect_upstream(stage.GetPrimAtPath(SourcePrimPathFromConnection(
          shader_connections.GetConnectionPath(shader, prop))));
    }
  } else {
    for (const UsdPrim& graph : graphs) collect_nodes(graph);
  }
  std::unordered_map<std::string, std::string> graph_node_names;
  if (direct_graph) {
    for (const UsdPrim& node : graph_nodes) {
      std::string relative = node.GetPath().str();
      const std::string material_path = material.GetPath().str();
      if (relative.compare(0, material_path.size(), material_path) == 0)
        relative.erase(0, material_path.size());
      while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
      std::replace(relative.begin(), relative.end(), '/', '_');
      graph_node_names[node.GetPath().str()] =
          relative.empty() ? node.GetName() : relative;
    }
  } else for (const UsdPrim& graph : graphs) {
    const std::string graph_path = graph.GetPath().str();
    for (const UsdPrim& node : graph_nodes) {
      std::string relative = node.GetPath().str();
      if (relative.compare(0, graph_path.size(), graph_path) != 0) continue;
      relative.erase(0, graph_path.size());
      while (!relative.empty() && relative.front() == '/') relative.erase(0, 1);
      std::replace(relative.begin(), relative.end(), '/', '_');
      if (graph_forest) relative = graph.GetName() + '_' + relative;
      graph_node_names[node.GetPath().str()] =
          relative.empty() ? node.GetName() : relative;
    }
  }
  // A connection may target an output on a nested NodeGraph. Chase those
  // forwarding outputs until the actual Shader output is reached; the packed
  // runtime has no graph-boundary node and should see the flattened topology.
  auto resolve_connection = [&](std::string connection) {
    for (int depth = 0; depth < 16; ++depth) {
      const UsdPrim source = stage.GetPrimAtPath(
          SourcePrimPathFromConnection(connection));
      if (!source.IsValid() || !::lightusd::next::IsNodeGraph(source)) break;
      const std::string property = ConnectionPropertyName(connection);
      if (property.empty()) break;
      ::lightusd::next::AttributeEval eval(&stage);
      if (!eval.HasConnection(source, property)) break;
      const std::string forwarded = eval.GetConnectionPath(source, property);
      if (forwarded.empty() || forwarded == connection) break;
      connection = forwarded;
    }
    return connection;
  };
  // Unconnected surface inputs are already preserved by the typed material
  // converter. Keep them out of the executable graph: synthesizing Constant
  // nodes for every direct value duplicated work at each hit and could replace
  // the authoritative typed block with graph-evaluator defaults. Only authored
  // connections belong in this runtime record.
  std::ostringstream os;
  os << "{\"version\":\"1.39\",\"nodegraph\":{\"name\":\""
     << JsonEscape(graph_name) << "\",\"inputs\":[],\"nodes\":[";
  bool first_node = true;
  for (const UsdPrim& node : graph_nodes) {
    std::string node_id;
    GetToken(node, "info:id", &node_id);
    std::string category = node_id;
    if (category.compare(0, 3, "ND_") == 0) {
      category.erase(0, 3);
      const size_t suffix = category.rfind('_');
      if (suffix != std::string::npos) category.erase(suffix);
    }
    if (!first_node) os << ',';
    first_node = false;
    os << "{\"name\":\"" << JsonEscape(graph_node_names[node.GetPath().str()])
       << "\",\"category\":\"" << JsonEscape(category)
       << "\",\"type\":\"" << JsonEscape(node_id)
       << "\",\"inputs\":[";
    bool first_input = true;
    ::lightusd::next::AttributeEval eval(&stage);
    for (const std::string& prop : node.GetPropertyNames()) {
      if (prop.compare(0, 7, "inputs:") != 0) continue;
      if (!first_input) os << ',';
      first_input = false;
      os << "{\"name\":\"" << JsonEscape(prop.substr(7)) << '"';
      if (eval.HasConnection(node, prop)) {
        const std::string connection = resolve_connection(
            eval.GetConnectionPath(node, prop));
        const std::string source_path = SourcePrimPathFromConnection(connection);
        const auto source_name = graph_node_names.find(source_path);
        os << ",\"nodename\":\""
           << JsonEscape(source_name == graph_node_names.end()
                             ? ConnectionNodeName(connection)
                             : source_name->second)
           << "\",\"output\":\"" << JsonEscape(ConnectionOutputName(connection)) << '"';
      } else if (const Value* value = node.GetPropertyValueOrEarliestTimeSample(prop)) {
        os << ",\"value\":"; EmitNextGraphValue(os, *value);
        if (const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec()) {
          if (const std::string* type = spec->property_type_name(prop))
            os << ",\"type\":\"" << JsonEscape(*type) << '"';
        }
      }
      os << '}';
    }
    os << "]}";
  }
  os << "],\"outputs\":[";
  bool first_output = true;
  ::lightusd::next::AttributeEval shader_eval(&stage);
  auto runtime_input_name = [&](const std::string& name) {
    if (!volume_graph) return name;
    if (name == "density") return std::string("volume_density");
    if (name == "scattering_color" || name == "scatter_color")
      return std::string("volume_albedo");
    if (name == "emission_color" || name == "emissionColor")
      return std::string("volume_emission_color");
    if (name == "emission" || name == "emission_intensity" ||
        name == "emissionIntensity") return std::string("volume_emission_scale");
    if (name == "anisotropy" || name == "scatter_anisotropy" ||
        name == "scattering_anisotropy") return std::string("volume_anisotropy");
    return name;
  };
  if (direct_graph) {
    for (const std::string& prop : shader.GetPropertyNames()) {
      if (prop.compare(0, 7, "inputs:") != 0 ||
          !shader_eval.HasConnection(shader, prop)) continue;
      const std::string connection = resolve_connection(
          shader_eval.GetConnectionPath(shader, prop));
      const std::string source_path = SourcePrimPathFromConnection(connection);
      const auto source_name = graph_node_names.find(source_path);
      if (source_name == graph_node_names.end()) continue;
      if (!first_output) os << ',';
      first_output = false;
      os << "{\"name\":\"" << JsonEscape(runtime_input_name(prop.substr(7)))
         << "\",\"nodename\":\"" << JsonEscape(source_name->second)
         << "\",\"output\":\"" << JsonEscape(ConnectionOutputName(connection))
         << "\"}";
    }
  } else for (const UsdPrim& graph : graphs) {
    ::lightusd::next::AttributeEval graph_eval(&stage);
    for (const std::string& prop : graph.GetPropertyNames()) {
      if (prop.compare(0, 8, "outputs:") != 0 ||
          !graph_eval.HasConnection(graph, prop)) continue;
      const std::string connection = resolve_connection(
          graph_eval.GetConnectionPath(graph, prop));
      const std::string source_path = SourcePrimPathFromConnection(connection);
      const auto source_name = graph_node_names.find(source_path);
      if (!first_output) os << ',';
      first_output = false;
      os << "{\"name\":\""
         << JsonEscape((graph_forest ? graph.GetName() + '_' : std::string()) +
                       prop.substr(8))
         << "\",\"nodename\":\""
         << JsonEscape(source_name == graph_node_names.end()
                           ? ConnectionNodeName(connection)
                           : source_name->second)
         << "\",\"output\":\"" << JsonEscape(ConnectionOutputName(connection)) << "\"}";
    }
  }
  os << "]},\"connections\":[";
  bool first_connection = true;
  for (const std::string& prop : shader.GetPropertyNames()) {
    if (prop.compare(0, 7, "inputs:") != 0 ||
        !shader_eval.HasConnection(shader, prop)) continue;
    const std::string connection = shader_eval.GetConnectionPath(shader, prop);
    const UsdPrim source = stage.GetPrimAtPath(
        SourcePrimPathFromConnection(connection));
    if (direct_graph) {
      if (!::lightusd::next::IsShader(source) ||
          graph_node_names.find(source.GetPath().str()) == graph_node_names.end())
        continue;
      if (!first_connection) os << ',';
      first_connection = false;
      os << "{\"input\":\"" << JsonEscape(runtime_input_name(prop.substr(7)))
         << "\",\"nodegraph\":\"" << JsonEscape(graph_name)
         << "\",\"output\":\"" << JsonEscape(prop.substr(7)) << "\"}";
      continue;
    }
    if (!::lightusd::next::IsNodeGraph(source)) continue;
    const auto graph_it = std::find_if(
        graphs.begin(), graphs.end(), [&](const UsdPrim& graph) {
          return graph.GetPath() == source.GetPath();
        });
    if (graph_it == graphs.end()) continue;
    if (!first_connection) os << ',';
    first_connection = false;
      os << "{\"input\":\"" << JsonEscape(runtime_input_name(prop.substr(7)))
       << "\",\"nodegraph\":\"" << JsonEscape(graph_name)
       << "\",\"output\":\""
       << JsonEscape((graph_forest ? source.GetName() + '_' : std::string()) +
                     ConnectionOutputName(connection)) << "\"}";
  }
  os << "]}";
  return first_node || first_output || first_connection ? std::string() : os.str();
}

struct MtlxConstantValue {
  std::array<float, 4> value{{0.0f, 0.0f, 0.0f, 0.0f}};
  int components = 0;
  bool color_managed = false;

  float component(int i) const {
    return value[static_cast<size_t>(i < components ? i : 0)];
  }
};

bool ValueToMtlxConstant(const Value& value, MtlxConstantValue* out) {
  if (!out) return false;
  ShaderParam param;
  if (!ValueToShaderParam(value, &param)) return false;
  out->value = {{param.value.x, param.value.y, param.value.z, param.value.w}};
  if (value.as_float3() || value.as_double3()) out->components = 3;
  else if (value.as_float4() || value.as_double4()) out->components = 4;
  else if (value.as_float2() || value.as_double2()) out->components = 2;
  else {
    float widened[4];
    if (value.to_float4(widened)) out->components = 4;
    else if (value.to_float3(widened)) out->components = 3;
    else if (value.to_float2(widened)) out->components = 2;
    else out->components = 1;
  }
  return true;
}

MtlxConstantValue MtlxBinary(const MtlxConstantValue& a,
                             const MtlxConstantValue& b,
                             const std::function<float(float, float)>& op) {
  MtlxConstantValue out;
  out.components = std::max(a.components, b.components);
  out.color_managed = a.color_managed || b.color_managed;
  for (int i = 0; i < out.components; ++i) {
    out.value[static_cast<size_t>(i)] = op(a.component(i), b.component(i));
  }
  return out;
}

bool EvalMtlxConstantConnection(const Stage& stage,
                                const std::string& connection,
                                double time_code,
                                const std::string& evaluation_space,
                                MtlxConstantValue* out,
                                std::set<std::string>* visiting,
                                int depth);

bool TransformMtlxColorInput(const UsdPrim& node,
                             const std::string& property,
                             const std::string& evaluation_space,
                             MtlxConstantValue* value) {
  if (!value) return false;
  if (value->components < 3 || evaluation_space.empty()) return true;
  const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec();
  const std::string* type = spec ? spec->property_type_name(property) : nullptr;
  if (!type || (type->rfind("color3", 0) != 0 &&
                type->rfind("color4", 0) != 0)) {
    return true;
  }

  std::string source;
  bool authored = false;
  if (!::lightusd::next::color_management::ComputeColorSpaceName(
          node, property, &source, &authored)) {
    return false;
  }
  if (!authored) source = evaluation_space;
  ::lightusd::color::ColorTransform transform;
  if (!::lightusd::next::color_management::BuildColorTransform(
          node, source, evaluation_space, &transform)) {
    return false;
  }
  float rgb[3] = {value->value[0], value->value[1], value->value[2]};
  ::lightusd::color::TransformRGB(transform, rgb);
  value->value[0] = rgb[0];
  value->value[1] = rgb[1];
  value->value[2] = rgb[2];
  value->color_managed =
      transform.source.kind != ::lightusd::color::ColorSpaceKind::Data;
  return true;
}

bool EvalMtlxInput(const Stage& stage, const UsdPrim& node,
                   const std::string& input, double time_code,
                   const std::string& evaluation_space,
                   MtlxConstantValue* out, std::set<std::string>* visiting,
                   int depth) {
  if (!out || depth > kMaxMtlxConstantDepth) return false;
  const std::string property = "inputs:" + input;
  const ::lightusd::next::PrimSpec* spec = node.GetPrimSpec();
  const std::vector<::lightusd::next::Path>* connections =
      spec ? spec->connection(property) : nullptr;
  if (connections && !connections->empty()) {
    return EvalMtlxConstantConnection(stage, (*connections)[0].str(),
                                      time_code, evaluation_space, out,
                                      visiting, depth + 1);
  }
  ::lightusd::next::AttributeEval eval(&stage);
  eval.SetTime(time_code);
  ::lightusd::next::EvalOptions options = eval.GetOptions();
  options.follow_connections = false;
  const ::lightusd::next::EvalResult result =
      eval.EvalWith(node, property, options);
  return result.success && ValueToMtlxConstant(result.value, out) &&
         TransformMtlxColorInput(node, property, evaluation_space, out);
}

bool EvalMtlxConstantNode(const Stage& stage, const UsdPrim& node,
                          double time_code,
                          const std::string& evaluation_space,
                          MtlxConstantValue* out,
                          std::set<std::string>* visiting, int depth) {
  if (!out || !node.IsValid() || depth > kMaxMtlxConstantDepth || !visiting) {
    return false;
  }
  const std::string key = node.GetPath().str();
  if (!visiting->insert(key).second) return false;
  struct VisitGuard {
    std::set<std::string>* set;
    std::string key;
    ~VisitGuard() { set->erase(key); }
  } guard{visiting, key};

  std::string id;
  GetToken(node, "info:id", &id);
  auto input = [&](const char* name, MtlxConstantValue* value) {
    return EvalMtlxInput(stage, node, name, time_code, evaluation_space, value,
                         visiting, depth + 1);
  };
  auto starts = [&id](const char* prefix) { return id.rfind(prefix, 0) == 0; };

  if (id == "ND_constant_float" || id == "ND_constant_color3" ||
      id == "ND_constant_vector3" || id == "ND_constant_color4") {
    return input("value", out);
  }

  if (starts("ND_add_") || starts("ND_subtract_") ||
      starts("ND_multiply_") || starts("ND_divide_") ||
      starts("ND_min_") || starts("ND_max_") || starts("ND_power_")) {
    MtlxConstantValue a, b;
    if (!input("in1", &a) || !input("in2", &b)) return false;
    if (starts("ND_add_")) *out = MtlxBinary(a, b, [](float x, float y) { return x + y; });
    else if (starts("ND_subtract_")) *out = MtlxBinary(a, b, [](float x, float y) { return x - y; });
    else if (starts("ND_multiply_")) *out = MtlxBinary(a, b, [](float x, float y) { return x * y; });
    else if (starts("ND_divide_")) *out = MtlxBinary(a, b, [](float x, float y) { return std::abs(y) > 1.0e-8f ? x / y : 0.0f; });
    else if (starts("ND_min_")) *out = MtlxBinary(a, b, [](float x, float y) { return std::min(x, y); });
    else if (starts("ND_max_")) *out = MtlxBinary(a, b, [](float x, float y) { return std::max(x, y); });
    else *out = MtlxBinary(a, b, [](float x, float y) { return std::pow(x, y); });
    return true;
  }

  if (starts("ND_mix_")) {
    MtlxConstantValue bg, fg, amount;
    if (!input("bg", &bg) || !input("fg", &fg) ||
        !input("mix", &amount)) return false;
    const float t = amount.component(0);
    *out = MtlxBinary(bg, fg, [t](float x, float y) {
      return x * (1.0f - t) + y * t;
    });
    return true;
  }

  if (starts("ND_clamp_")) {
    MtlxConstantValue value, low, high;
    if (!input("in", &value) || !input("low", &low) ||
        !input("high", &high)) return false;
    *out = value;
    out->color_managed = value.color_managed || low.color_managed ||
                         high.color_managed;
    for (int i = 0; i < out->components; ++i) {
      out->value[static_cast<size_t>(i)] = std::min(
          std::max(value.component(i), low.component(i)), high.component(i));
    }
    return true;
  }

  if (starts("ND_remap_")) {
    MtlxConstantValue value, in_low, in_high, out_low, out_high;
    if (!input("in", &value) || !input("inlow", &in_low) ||
        !input("inhigh", &in_high) || !input("outlow", &out_low) ||
        !input("outhigh", &out_high)) return false;
    *out = value;
    out->color_managed = value.color_managed || in_low.color_managed ||
                         in_high.color_managed || out_low.color_managed ||
                         out_high.color_managed;
    for (int i = 0; i < out->components; ++i) {
      const float denom = in_high.component(i) - in_low.component(i);
      const float t = std::abs(denom) > 1.0e-8f
                          ? (value.component(i) - in_low.component(i)) / denom
                          : 0.0f;
      out->value[static_cast<size_t>(i)] =
          out_low.component(i) + t * (out_high.component(i) - out_low.component(i));
    }
    return true;
  }

  if (starts("ND_combine3_")) {
    MtlxConstantValue a, b, c;
    if (!input("in1", &a) || !input("in2", &b) || !input("in3", &c)) {
      return false;
    }
    out->components = 3;
    out->value = {{a.component(0), b.component(0), c.component(0), 0.0f}};
    out->color_managed =
        a.color_managed || b.color_managed || c.color_managed;
    return true;
  }

  if (starts("ND_extract_")) {
    MtlxConstantValue value, index;
    if (!input("in", &value) || !input("index", &index)) return false;
    int component = static_cast<int>(index.component(0));
    if (component < 0 || component >= value.components) component = 0;
    out->components = 1;
    out->value[0] = value.value[static_cast<size_t>(component)];
    out->color_managed = value.color_managed;
    return true;
  }

  if (starts("ND_normalize_")) {
    MtlxConstantValue value;
    if (!input("in", &value)) return false;
    float length_squared = 0.0f;
    for (int i = 0; i < value.components; ++i) {
      const float component = value.component(i);
      length_squared += component * component;
    }
    const float length = std::sqrt(length_squared);
    *out = value;
    if (length > 1.0e-7f) {
      for (int i = 0; i < out->components; ++i) {
        out->value[static_cast<size_t>(i)] /= length;
      }
    }
    return true;
  }

  // Constant-fold the common MaterialX unary math family. These nodes occur
  // frequently between DCC-authored controls and surface inputs; treating
  // them as unsupported discarded an otherwise fully evaluable material.
  if (starts("ND_absval_") || starts("ND_floor_") ||
      starts("ND_ceil_") || starts("ND_round_") ||
      starts("ND_sqrt_") || starts("ND_exp_") ||
      starts("ND_ln_") || starts("ND_sin_") || starts("ND_cos_") ||
      starts("ND_tan_")) {
    MtlxConstantValue value;
    if (!input("in", &value)) return false;
    *out = value;
    for (int i = 0; i < out->components; ++i) {
      const float x = value.component(i);
      float y = x;
      if (starts("ND_absval_")) y = std::fabs(x);
      else if (starts("ND_floor_")) y = std::floor(x);
      else if (starts("ND_ceil_")) y = std::ceil(x);
      else if (starts("ND_round_")) y = std::round(x);
      else if (starts("ND_sqrt_")) y = std::sqrt(std::max(0.0f, x));
      else if (starts("ND_exp_")) y = std::exp(x);
      else if (starts("ND_ln_")) y = x > 0.0f ? std::log(x) : 0.0f;
      else if (starts("ND_sin_")) y = std::sin(x);
      else if (starts("ND_cos_")) y = std::cos(x);
      else if (starts("ND_tan_")) y = std::tan(x);
      out->value[static_cast<size_t>(i)] = y;
    }
    return true;
  }

  if (starts("ND_ifgreater_") || starts("ND_ifgreatereq_") ||
      starts("ND_ifequal_")) {
    MtlxConstantValue value1, value2, when_true, when_false;
    if (!input("value1", &value1) || !input("value2", &value2) ||
        !input("in1", &when_true) || !input("in2", &when_false)) {
      return false;
    }
    const float a = value1.component(0);
    const float b = value2.component(0);
    bool condition = false;
    if (starts("ND_ifgreatereq_")) {
      condition = a >= b;
    } else if (starts("ND_ifgreater_")) {
      condition = a > b;
    } else {
      const float scale = std::max({std::fabs(a), std::fabs(b), 1.0f});
      condition = std::fabs(a - b) <=
                  std::numeric_limits<float>::epsilon() * scale;
    }
    *out = condition ? when_true : when_false;
    return true;
  }

  if (starts("ND_convert_")) return input("in", out);

  if (id == "ND_hsv_adjust_color3" || id == "ND_hsvadjust_color3") {
    MtlxConstantValue color, hue, saturation, value, factor;
    // The standard MaterialX hsvadjust amount is a direct hue offset with
    // (0, 1, 1) as its identity. Blender's separate-input variant exposes a
    // UI control where 0.5 is neutral instead.
    const float hue_neutral =
        id == "ND_hsvadjust_color3" ? 0.0f : 0.5f;
    if (!input("in", &color)) return false;
    if (id == "ND_hsvadjust_color3") {
      MtlxConstantValue amount;
      if (!input("amount", &amount) || amount.components < 3) return false;
      hue.components = saturation.components = value.components =
          factor.components = 1;
      hue.value[0] = amount.component(0);
      saturation.value[0] = amount.component(1);
      value.value[0] = amount.component(2);
      factor.value[0] = 1.0f;
    } else if (!input("hue", &hue) ||
               !input("saturation", &saturation) ||
               !input("value", &value) || !input("fac", &factor)) {
      return false;
    }
    const float r = color.component(0), g = color.component(1), b = color.component(2);
    const float maximum = std::max({r, g, b});
    const float minimum = std::min({r, g, b});
    const float delta = maximum - minimum;
    float h = 0.0f;
    if (delta > 1.0e-7f) {
      if (r >= maximum) h = (g - b) / delta;
      else if (g >= maximum) h = 2.0f + (b - r) / delta;
      else h = 4.0f + (r - g) / delta;
      h /= 6.0f;
      if (h < 0.0f) h += 1.0f;
    }
    float s = maximum > 0.0f ? delta / maximum : 0.0f;
    float v = maximum;
    h = std::fmod(h + (hue.component(0) - hue_neutral) + 1.0f, 1.0f);
    s *= saturation.component(0);
    v *= value.component(0);
    float adjusted[3] = {v, v, v};
    if (s > 0.0f) {
      const float hh = h * 6.0f;
      const int sector = static_cast<int>(std::floor(hh)) % 6;
      const float fraction = hh - std::floor(hh);
      const float p = v * (1.0f - s);
      const float q = v * (1.0f - s * fraction);
      const float t = v * (1.0f - s * (1.0f - fraction));
      const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t},
                                 {p, q, v}, {t, p, v}, {v, p, q}};
      for (int i = 0; i < 3; ++i) adjusted[i] = table[sector][i];
    }
    const float mix = factor.component(0);
    out->components = 3;
    out->color_managed = color.color_managed;
    for (int i = 0; i < 3; ++i) {
      out->value[static_cast<size_t>(i)] =
          color.component(i) * (1.0f - mix) + adjusted[i] * mix;
    }
    return true;
  }
  return false;
}

bool EvalMtlxConstantConnection(const Stage& stage,
                                const std::string& connection,
                                double time_code,
                                const std::string& evaluation_space,
                                MtlxConstantValue* out,
                                std::set<std::string>* visiting,
                                int depth) {
  if (!out || depth > kMaxMtlxConstantDepth) return false;
  std::string prim_path, property;
  if (!SplitConnectionPath(connection, &prim_path, &property)) return false;
  const UsdPrim prim = stage.GetPrimAtPath(prim_path);
  if (!prim.IsValid()) return false;
  const ::lightusd::next::PrimSpec* spec = prim.GetPrimSpec();
  const std::vector<::lightusd::next::Path>* forwarded =
      spec ? spec->connection(property) : nullptr;
  if (forwarded && !forwarded->empty()) {
    return EvalMtlxConstantConnection(stage, (*forwarded)[0].str(), time_code,
                                      evaluation_space, out, visiting,
                                      depth + 1);
  }
  if (::lightusd::next::IsShader(prim)) {
    return EvalMtlxConstantNode(stage, prim, time_code, evaluation_space, out,
                                visiting, depth + 1);
  }
  return false;
}













// O(1) copy-on-write share. This is the copy engine behind every
// point-instance mesh clone: the topology, UV sets, colors and primvars of a
// clone are byte-identical to the prototype's and were previously duplicated
// in full, so N instances of a 50k-tri prototype cost N x the WHOLE mesh
// rather than N x (points+normals+tangents) + 1 x the rest. ChunkedArray
// detaches on the first write through either holder, so a consumer that does
// mutate a clone still gets private storage.










// Value-clip metadata parsing / active-clip selection / stage->clip time
// mapping now come from the CORE resolver (next/eval/value-clip.hh:
// ParseValueClipSets + ResolveValueClipFromSets) — Tydra previously owned a
// duplicate of these semantics that had drifted (no jump-discontinuity
// handling, stale out-of-range mapping, no clipSets ordering edits, no
// manifest gating, no nested-clip recursion).
























std::vector<std::string> ReadRelationshipTargets(const UsdPrim& prim,
                                                 const std::string& name) {
  std::vector<std::string> out;
  const std::vector<::lightusd::next::Path>* targets =
      prim.GetRelationship(name);
  if (!targets) return out;
  out.reserve(targets->size());
  for (const ::lightusd::next::Path& target : *targets) {
    out.push_back(target.str());
  }
  return out;
}







size_t SaturatingAdd(size_t a, size_t b) {
  if (b > std::numeric_limits<size_t>::max() - a) {
    return std::numeric_limits<size_t>::max();
  }
  return a + b;
}

size_t SaturatingMul(size_t a, size_t b) {
  if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
    return std::numeric_limits<size_t>::max();
  }
  return a * b;
}

bool HasAuthoredExtent(const UsdPrim& prim) {
  return AuthoredArraySize(prim, kIdExtent()) >= 2;
}

GeometryInfo BuildGeometryInfo(const UsdPrim& prim, GeometryKind kind,
                               int32_t id) {
  GeometryInfo info;
  info.kind = kind;
  info.id = id;
  info.prim_path = prim.GetPath().str();
  info.type_name = prim.GetTypeName();
  info.point_count = AuthoredArraySize(prim, kIdPoints());
  info.has_authored_extent = HasAuthoredExtent(prim);

  size_t estimate = 0;
  if (kind == GeometryKind::Mesh) {
    info.index_count = AuthoredArraySize(prim, kIdFaceVertexIndices());
    // Account for decoded source arrays, render arrays, triangulation, and
    // face-varying remaps. This intentionally errs high for residency policy.
    estimate = SaturatingMul(info.point_count, 24);
    estimate = SaturatingAdd(estimate,
                             SaturatingMul(info.index_count, 16));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdFaceVertexCounts()), 8));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdNormals()), 24));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdPrimvarsSt()), 16));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdDisplayColor()), 24));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdDisplayOpacity()), 8));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdTetVertexIndices()), 16));
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdHoleIndices()), 8));
  } else if (kind == GeometryKind::Points) {
    estimate = SaturatingMul(info.point_count, 48);
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdWidths()), 8));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayColor()), 16));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayOpacity()), 8));
  } else {
    estimate = SaturatingMul(info.point_count, 40);
    estimate = SaturatingAdd(
        estimate,
        SaturatingMul(AuthoredArraySize(prim, kIdCurveVertexCounts()), 8));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdWidths()), 12));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayColor()), 20));
    estimate = SaturatingAdd(
        estimate, SaturatingMul(AuthoredArraySize(prim, kIdDisplayOpacity()), 8));
  }
  info.estimated_resident_bytes = estimate;
  return info;
}










// First bound material path whose target actually resolves to a Material,
// walking the purpose order (preview, all-purpose, full). A dangling
// purpose-specific rel must fall through to the weaker-purpose rel on the
// SAME prim, not reject the prim (GetBoundMaterialPath returns only the
// first authored rel).




}  // namespace

//
// Constructor / Destructor
//

ConverterConfig MakeHardenedConverterConfig(size_t max_memory) {
  ConverterConfig config;
  // Zero must never turn a hardened request into the legacy unlimited mode.
  max_memory = std::max<size_t>(max_memory, 1);
  config.max_render_depth = 256;
  // A finite record ceiling prevents adversarial programmatic stages from
  // turning the catalog itself into an unbounded allocation. Applications may
  // raise it deliberately after constructing the preset.
  config.max_render_records = 1u << 20;
  config.animation.max_value_clip_samples = 10000;
  config.execution.max_threads = 1;
  config.execution.max_in_flight_bytes = max_memory;
  config.execution.callback_concurrency =
      ::lightusd::next::CallbackConcurrency::Serialized;
  return config;
}

RenderSceneConverter::RenderSceneConverter(const ConverterConfig& config)
    : config_(config) {}

RenderSceneConverter::~RenderSceneConverter() = default;

GeometryInfo RenderSceneConverter::GetGeometryInfo(const UsdPrim& prim,
                                                    GeometryKind kind,
                                                    int32_t id) const {
  return BuildGeometryInfo(prim, kind, id);
}



namespace {

// Release arrays that are not needed by the metadata-only RenderScene path.
// Material binding still needs polygon counts/triangle offsets and material UV
// promotion still needs texcoords/primvars, so conversion uses this in two
// phases. Assignment from an empty ChunkedArray releases chunks immediately;
// ChunkedArray::clear() intentionally retains them for reuse.
void ReleaseMeshGeometry(RenderMesh* mesh, bool keep_binding_inputs,
                         bool keep_triangulation = false) {
  if (!mesh) return;

  mesh->face_vertex_indices = UInt32Chunked();
  mesh->points = FloatChunked();
  mesh->normals = FloatChunked();
  mesh->tangents = FloatChunked();
  mesh->colors = FloatChunked();
  mesh->opacities = FloatChunked();
  if (!keep_triangulation) {
    mesh->triangulated_indices = UInt32Chunked();
    mesh->triangulated_face_vertex_indices = UInt32Chunked();
  }

  if (keep_binding_inputs) return;

  mesh->face_vertex_counts = UInt32Chunked();
  mesh->texcoords_0 = FloatChunked();
  mesh->texcoords_1 = FloatChunked();
  std::vector<VertexAttribute>().swap(mesh->primvars);
  std::vector<uint32_t>().swap(mesh->face_triangle_offsets);
  std::vector<int32_t>().swap(mesh->sanitize_face_remap);
  std::vector<uint32_t>().swap(mesh->hole_faces);
}

void ReleaseSourceMeshStaticArrays(Stage& stage, const UsdPrim& mesh_prim) {
  constexpr size_t kLowmemStaticReleaseThreshold = 1;
  stage.ReleaseStaticGeometryArraysForPrim(mesh_prim,
                                          kLowmemStaticReleaseThreshold);
}

}  // namespace

//
// Main conversion
//





StreamConvertResult RenderSceneConverter::ConvertToSink(Stage& stage,
                                                        SceneSink* sink) {
  StreamConvertResult result;
  ResetOperationState();
  ResetImageIdCache();
  if (!sink) {
    result.error = "ConvertToSink: null scene sink";
    return result;
  }
  const auto cancellation_requested = [&]() {
    return config_.cancel_callback && config_.cancel_callback();
  };
  if (cancellation_requested()) {
    result.cancelled = true;
    result.error = "conversion cancelled";
    return result;
  }

  RenderScene catalog;
  const auto meta = stage.GetMeta();
  catalog.name = meta.defaultPrim;
  catalog.default_prim = meta.defaultPrim;
  catalog.meters_per_unit = static_cast<float>(meta.metersPerUnit);
  catalog.up_axis = meta.upAxis == "Z" ? RenderScene::UpAxis::Z
                                        : RenderScene::UpAxis::Y;
  catalog.start_time = meta.startTimeCode;
  catalog.end_time = meta.endTimeCode;
  catalog.frames_per_second = meta.timeCodesPerSecond;
  ::lightusd::next::color_management::RenderingColorConfig color_config;
  std::string color_warning;
  (void)::lightusd::next::color_management::ResolveRenderingColorConfig(
      stage, config_.material.render_settings_path, &color_config,
      &color_warning);
  catalog.render_settings_path = color_config.render_settings_path;
  catalog.working_color_space = color_config.working_space;
  ::lightusd::color::ColorSpaceDesc display_linear;
  ::lightusd::color::ColorTransform working_to_display;
  if (::lightusd::color::GetBuiltinColorSpace("lin_rec709_scene",
                                              &display_linear) &&
      ::lightusd::color::BuildColorTransform(
          color_config.working_definition, display_linear,
          &working_to_display)) {
    for (size_t i = 0; i < 9; ++i) {
      catalog.working_to_display_linear[i] = working_to_display.matrix[i];
    }
  }
  if (!color_warning.empty()) AddWarning(color_warning);

  RenderExtractOptions xopts;
  xopts.time_code = config_.time_code;
  xopts.max_depth = config_.max_render_depth;
  xopts.max_records = config_.max_render_records;
  xopts.collect_other = true;
  RenderExtractResult extracted;
  CollectRenderPrims(stage, xopts, &extracted);
  if (extracted.limit_exceeded) {
    result.error = "render extraction limit exceeded";
    return result;
  }
  const bool emit_animations =
      config_.animation.enabled &&
      (stage.HasTimeSamples() || stage.HasValueClips());
  const bool has_time_samples = emit_animations;
  if (cancellation_requested()) {
    result.cancelled = true;
    result.error = "conversion cancelled";
    return result;
  }
  BuildNodeHierarchy(extracted, &catalog);
  ExtractPhysicsAnnotations(stage, &catalog);
  catalog.meshes.reserve(extracted.meshes.size());
  const size_t point_record_count = extracted.points.size();
  catalog.points.reserve(point_record_count);
  catalog.curves.reserve(extracted.curves.size());
  catalog.point_instancers.reserve(extracted.point_instancers.size());
  catalog.lights.reserve(extracted.lights.size());
  catalog.cameras.reserve(extracted.cameras.size());
  catalog.skeletons.reserve(extracted.skeletons.size());
  catalog.materials.reserve(extracted.materials.size());
  catalog.mesh_by_path.reserve(extracted.meshes.size());
  catalog.point_instancer_by_path.reserve(extracted.point_instancers.size());
  catalog.points_by_path.reserve(point_record_count);
  catalog.curves_by_path.reserve(extracted.curves.size());
  catalog.material_by_path.reserve(extracted.materials.size());
  catalog.unsupported_renderables.reserve(
      std::min<size_t>(extracted.records.size(), 128));
  if (emit_animations) {
    catalog.animations.reserve(extracted.records.size());
  }
  for (const RenderPrimRecord& rec : extracted.records) {
    if (rec.type_name != "Points" &&
        rec.type_name != "ParticleField3DGaussianSplat" &&
        IsUnsupportedRenderableTypeName(rec.type_name)) {
      UnsupportedRenderable unsupported;
      unsupported.prim_path = rec.path;
      unsupported.type_name = rec.type_name;
      unsupported.reason = "recognized but not converted to render geometry";
      catalog.unsupported_renderables.push_back(std::move(unsupported));
      AddWarning("Unsupported renderable prim '" + rec.path +
                          "' of type '" + rec.type_name + "'");
    }

    if (!has_time_samples ||
        (!stage.HasValueClips() &&
         (!rec.prim.GetPrimSpec() ||
          !rec.prim.GetPrimSpec()->has_any_time_samples()))) {
      continue;
    }
    AnimationClip clip;
    if (ConvertAnimation(stage, rec.prim, &clip)) {
      const auto node = catalog.node_by_path.find(rec.path);
      if (node != catalog.node_by_path.end()) {
        for (AnimationChannel& channel : clip.channels) {
          channel.target_node = node->second;
        }
      }
      catalog.animations.push_back(std::move(clip));
    }
  }

  // Materials and their image/texture descriptors are a small catalog and
  // must be assigned before geometry is emitted.
  for (const RenderPrimRecord& rec : extracted.materials) {
    RenderMaterial material;
    if (ConvertMaterial(stage, rec.prim, &material, &catalog)) {
      const int32_t id = static_cast<int32_t>(catalog.materials.size());
      catalog.material_by_path[material.prim_path] = id;
      catalog.materials.push_back(std::move(material));
    } else {
      AddWarning("Failed to convert material: " + rec.path);
    }
  }

  for (const RenderPrimRecord& rec : extracted.skeletons) {
    Skeleton skeleton;
    if (ConvertSkeleton(rec.prim, &skeleton)) {
      const int32_t id = static_cast<int32_t>(catalog.skeletons.size());
      catalog.skeletons.push_back(std::move(skeleton));
      AssignNodeDataId(&catalog, rec.path, id);
    }
  }
  ResolveSkeletalAnimationTargets(&catalog);

  for (const RenderPrimRecord& rec : extracted.lights) {
    RenderLight light;
    if (!ConvertLight(rec.prim, &light)) continue;
    for (int i = 0; i < 16; ++i) light.transform.m[i] = float(rec.world[i]);
    if (light.type == LightType::Dome) {
      std::string texture;
      if (const Value* value = rec.prim.GetPropertyValue(kIdInputsTextureFile())) {
        if (const std::string* p = value->as_asset_path()) texture = *p;
        else if (const std::string* p = value->as_string()) texture = *p;
        else if (const std::string* p = value->as_token()) texture = *p;
      }
      light.params.dome.texture_id =
          texture.empty() ? -1
                          : ResolveImageId(&catalog, texture, ColorSpace::Linear,
                                           AssetAnchorOf(rec.prim));
    }
    const int32_t id = static_cast<int32_t>(catalog.lights.size());
    catalog.lights.push_back(std::move(light));
    AssignNodeDataId(&catalog, rec.path, id);
  }
  for (const RenderPrimRecord& rec : extracted.cameras) {
    RenderCamera camera;
    if (!ConvertCamera(stage, rec.prim, &camera)) continue;
    for (int i = 0; i < 16; ++i) camera.transform.m[i] = float(rec.world[i]);
    const int32_t id = static_cast<int32_t>(catalog.cameras.size());
    catalog.cameras.push_back(std::move(camera));
    AssignNodeDataId(&catalog, rec.path, id);
  }

  // Reserve stable geometry IDs with lightweight placeholders. This is enough
  // for native/PointInstancer prototype binding without retaining geometry.
  catalog.meshes.resize(extracted.meshes.size());
  for (size_t i = 0; i < extracted.meshes.size(); ++i) {
    RenderMesh& placeholder = catalog.meshes[i];
    placeholder.name = extracted.meshes[i].prim.GetName();
    placeholder.prim_path = extracted.meshes[i].path;
    catalog.mesh_by_path[placeholder.prim_path] = static_cast<int32_t>(i);
    AssignMeshMaterialBinding(stage, catalog, &placeholder);
    AssignNodeDataId(&catalog, placeholder.prim_path, static_cast<int32_t>(i));
  }
  for (const RenderPrimRecord& rec : extracted.points) {
    const int32_t id = static_cast<int32_t>(catalog.points.size());
    RenderPoints placeholder;
    placeholder.name = rec.prim.GetName();
    placeholder.prim_path = rec.path;
    catalog.points_by_path[rec.path] = id;
    catalog.points.push_back(std::move(placeholder));
    AssignNodeDataId(&catalog, rec.path, id);
  }
  catalog.curves.resize(extracted.curves.size());
  for (size_t i = 0; i < extracted.curves.size(); ++i) {
    RenderCurves& placeholder = catalog.curves[i];
    placeholder.name = extracted.curves[i].prim.GetName();
    placeholder.prim_path = extracted.curves[i].path;
    const std::string material =
        FindInheritedMaterialBinding(stage, placeholder.prim_path,
                                     config_.material.binding_purpose);
    const auto found = catalog.material_by_path.find(material);
    if (found != catalog.material_by_path.end()) {
      placeholder.material_id = found->second;
    }
    catalog.curves_by_path[placeholder.prim_path] = static_cast<int32_t>(i);
    AssignNodeDataId(&catalog, placeholder.prim_path, static_cast<int32_t>(i));
  }

  for (const RenderPrimRecord& rec : extracted.point_instancers) {
    RenderPointInstancer instancer;
    if (!ConvertPointInstancer(rec.prim, &instancer)) {
      AddWarning("Failed to convert PointInstancer: " + rec.path);
      continue;
    }
    const int32_t id = static_cast<int32_t>(catalog.point_instancers.size());
    catalog.point_instancer_by_path[instancer.prim_path] = id;
    ResolvePointInstancerPrototypeBindings(&catalog, &instancer);
    if (config_.point_instancer.build_instance_draws ||
        config_.point_instancer.duplicate_meshes) {
      AppendPointInstanceDraws(id, &instancer, &catalog);
    }
    catalog.point_instancers.push_back(std::move(instancer));
    AssignNodeDataId(&catalog, rec.path, id);
  }
  AssignPointInstanceDrawMaterials(&catalog);

  RenderScene binding_catalog;
  binding_catalog.material_by_path = catalog.material_by_path;
  std::unordered_map<std::string, int32_t> skeleton_by_path;
  skeleton_by_path.reserve(extracted.skeletons.size());
  skeleton_by_path.max_load_factor(0.7f);
  for (size_t i = 0; i < catalog.skeletons.size(); ++i) {
    skeleton_by_path[catalog.skeletons[i].prim_path] = static_cast<int32_t>(i);
  }

  if (!sink->BeginScene(std::move(catalog))) {
    result.error = "scene sink rejected catalog";
    return result;
  }
  // The catalog and all non-geometry metadata are now owned by the sink.
  // Release the traversal-order duplicate before the first large geometry
  // payload is decoded; kind-specific lists still provide the conversion
  // order below.
  std::vector<RenderPrimRecord>().swap(extracted.records);

  const auto abort = [&](const std::string& error, bool cancelled) {
    sink->AbortScene();
    result.cancelled = cancelled;
    result.error = error;
    result.warnings = std::move(warnings_);
  };

  for (size_t i = 0; i < extracted.meshes.size(); ++i) {
    const UsdPrim& prim = extracted.meshes[i].prim;
    if (cancellation_requested()) {
      abort("conversion cancelled", true);
      return result;
    }
    const GeometryInfo info =
        GetGeometryInfo(prim, GeometryKind::Mesh, static_cast<int32_t>(i));
    const GeometryDisposition disposition = sink->SelectGeometry(info);
    if (disposition == GeometryDisposition::Cancel) {
      abort("conversion cancelled by scene sink", true);
      return result;
    }
    if (disposition == GeometryDisposition::Skip) continue;

    RenderMesh mesh;
    bool converted = false;
    if (disposition == GeometryDisposition::Proxy) {
      converted = ConvertExtentProxy(prim, &mesh);
      if (!converted) {
        AddWarning("Skipping proxy without a valid extent: " +
                            prim.GetPath().str());
        continue;
      }
    } else {
      converted = ConvertRenderableMesh(stage, prim, &mesh);
    }
    if (!converted || mesh.has_alloc_failure()) {
      AddWarning("Failed to convert renderable mesh prim: " +
                          prim.GetPath().str());
      continue;
    }
    if (!config_.mesh.retain_geometry) {
      ReleaseSourceMeshStaticArrays(stage, prim);
    }
    AssignMeshMaterialBinding(stage, binding_catalog, &mesh);
    if (mesh.skin) {
      const auto skeleton = skeleton_by_path.find(mesh.skin->skeleton_path);
      if (skeleton != skeleton_by_path.end()) {
        mesh.skin->skeleton_id = skeleton->second;
      }
    }
    mesh.compact();
    if (!sink->AddMesh(static_cast<int32_t>(i), std::move(mesh))) {
      abort("scene sink rejected mesh", false);
      return result;
    }
    ++result.mesh_count;
  }

  int32_t points_id = 0;
  for (const RenderPrimRecord& rec : extracted.points) {
    if (cancellation_requested()) {
      abort("conversion cancelled", true);
      return result;
    }
    const GeometryInfo info =
        GetGeometryInfo(rec.prim, GeometryKind::Points, points_id);
    const GeometryDisposition disposition = sink->SelectGeometry(info);
    if (disposition == GeometryDisposition::Cancel) {
      abort("conversion cancelled by scene sink", true);
      return result;
    }
    if (disposition != GeometryDisposition::Full) {
      if (disposition == GeometryDisposition::Proxy) {
        AddWarning("Skipping Points proxy without mesh expansion: " +
                            rec.path);
      }
      ++points_id;
      continue;
    }
    RenderPoints points;
    if (ConvertPoints(stage, rec.prim, &points)) {
      if (points.has_alloc_failure()) {
        AddWarning("Out of memory converting Points '" + rec.path +
                            "'; the prim was skipped");
        ++points_id;
        continue;
      }
      if (!config_.mesh.retain_geometry) {
        ReleaseSourceMeshStaticArrays(stage, rec.prim);
      }
      points.compact();
      if (!sink->AddPoints(points_id, std::move(points))) {
        abort("scene sink rejected points", false);
        return result;
      }
      ++result.point_count;
    }
    ++points_id;
  }
  for (size_t i = 0; i < extracted.curves.size(); ++i) {
    if (cancellation_requested()) {
      abort("conversion cancelled", true);
      return result;
    }
    const GeometryInfo info = GetGeometryInfo(
        extracted.curves[i].prim, GeometryKind::Curves,
        static_cast<int32_t>(i));
    const GeometryDisposition disposition = sink->SelectGeometry(info);
    if (disposition == GeometryDisposition::Cancel) {
      abort("conversion cancelled by scene sink", true);
      return result;
    }
    if (disposition != GeometryDisposition::Full) {
      if (disposition == GeometryDisposition::Proxy) {
        AddWarning("Skipping Curves proxy without mesh expansion: " +
                            extracted.curves[i].path);
      }
      continue;
    }
    RenderCurves curves;
    if (!ConvertCurves(extracted.curves[i].prim, &curves)) continue;
    if (curves.has_alloc_failure()) {
      AddWarning("Out of memory converting curves '" +
                          extracted.curves[i].path + "'; the prim was skipped");
      continue;
    }
    if (!config_.mesh.retain_geometry) {
      ReleaseSourceMeshStaticArrays(stage, extracted.curves[i].prim);
    }
    const std::string material =
        FindInheritedMaterialBinding(stage, curves.prim_path,
                                     config_.material.binding_purpose);
    const auto found = binding_catalog.material_by_path.find(material);
    if (found != binding_catalog.material_by_path.end()) {
      curves.material_id = found->second;
    }
    curves.compact();
    if (!sink->AddCurves(static_cast<int32_t>(i), std::move(curves))) {
      abort("scene sink rejected curves", false);
      return result;
    }
    ++result.curve_count;
  }

  if (!sink->EndScene()) {
    abort("scene sink failed to finalize", false);
    return result;
  }
  result.success = true;
  result.warnings = std::move(warnings_);
  return result;
}



//
// Mesh conversion
//

bool RenderSceneConverter::ConvertRenderableMesh(const Stage& stage,
                                                 const UsdPrim& prim,
                                                 RenderMesh* out) {
  if (!prim.IsValid() || !IsMeshRenderableTypeName(prim.GetTypeName())) {
    SetLastError("Invalid renderable mesh prim");
    return false;
  }
  return prim.GetTypeName() == "Mesh" ? ConvertMesh(stage, prim, out)
                                      : ConvertGeomPrimitive(prim, out);
}



// Cumulative memory guard for the expensive conversion phases.
//
// The converter's own limits (ProbeAlloc, kMaxTempAllocBytes,
// kMaxTriangulationCornerCount) are all PER-PRIM and fixed, so they cannot see
// a scene of 100k individually-small meshes summing past the cap: every prim
// passes its probe and the process still OOMs. MemBudget::WouldExceed() is
// RSS-based, so it observes everything actually resident -- including every
// ChunkedArray chunk -- without routing the converter's allocations through a
// throwing allocator (the wasm build is -fno-exceptions).
//
// Throttled: WouldExceed() reads /proc/self/statm, far too expensive per mesh.
// A cheap running total triggers a real check only once enough new bytes have
// been requested, or every kBudgetCheckStride calls.
bool RenderSceneConverter::BudgetWouldExceed(size_t estimate,
                                             const char* phase) {
  constexpr size_t kBudgetCheckStride = 256;
  constexpr size_t kBudgetCheckBytes = 8u * 1024u * 1024u;

  // Callable from the parallel per-record conversion phases (e.g. mesh
  // conversion, see ConvertMeshesParallel), so budget_*_ bookkeeping and the
  // resulting warning are both taken under state_mu_. Locks warnings_
  // directly rather than through AddWarning() -- AddWarning() takes the same
  // mutex, and it is non-recursive.
  ConverterStateLock lk(state_mu_);

  if (budget_exceeded_) return true;  // latched: stay degraded for this run

  const size_t operation_limit = config_.execution.max_in_flight_bytes;
  if (operation_limit != 0 &&
      (estimate > operation_limit ||
       budget_accounted_bytes_ > operation_limit - estimate)) {
    budget_exceeded_ = true;
    warnings_.push_back(std::string("Operation memory limit reached during ") +
                        phase + "; remaining geometry is skipped");
    return true;
  }
  budget_accounted_bytes_ = SaturatingAdd(budget_accounted_bytes_, estimate);

  budget_pending_bytes_ = SaturatingAdd(budget_pending_bytes_, estimate);
  const bool due = (++budget_check_counter_ % kBudgetCheckStride == 0) ||
                   budget_pending_bytes_ >= kBudgetCheckBytes;
  if (!due) return false;

  const size_t pending = budget_pending_bytes_;
  budget_pending_bytes_ = 0;
  std::string why;
  if (!MemBudget::Get().WouldExceed(pending, &why)) return false;

  budget_exceeded_ = true;
  warnings_.push_back(std::string("Memory budget reached during ") + phase +
                      "; remaining geometry is skipped (" + why + ")");
  return true;
}

void RenderSceneConverter::ResetOperationState() {
  warnings_.clear();
  budget_accounted_bytes_ = 0;
  budget_pending_bytes_ = 0;
  budget_check_counter_ = 0;
  budget_exceeded_ = false;
}

void RenderSceneConverter::AddWarning(std::string msg) {
  ConverterStateLock lk(state_mu_);
  warnings_.push_back(std::move(msg));
}

void RenderSceneConverter::SetLastError(std::string msg) {
  ConverterStateLock lk(state_mu_);
  last_error_ = std::move(msg);
}


//
// Triangulation
//




//
// Camera conversion
//


//
// Skeleton conversion
//

bool RenderSceneConverter::LoadTexture(const std::string& asset_path, TextureImage* out) {
  if (!out) return false;

  // Use custom loader if provided
  if (config_.material.custom_texture_loader) {
    return config_.material.custom_texture_loader(asset_path, out);
  }

  // Built-in loader is metadata-only by design. Applications that need decoded
  // pixels should provide `MaterialConfig::custom_texture_loader`.
  out->resolved_path = asset_path;

  return true;
}

//
// Utility functions
//

void ComputeTriangleNormal(const float* p0, const float* p1, const float* p2, float* normal) {
  float e1[3] = {p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]};
  float e2[3] = {p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2]};

  normal[0] = e1[1]*e2[2] - e1[2]*e2[1];
  normal[1] = e1[2]*e2[0] - e1[0]*e2[2];
  normal[2] = e1[0]*e2[1] - e1[1]*e2[0];

  float len = std::sqrt(normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2]);
  if (len > 1e-8f) {
    normal[0] /= len;
    normal[1] /= len;
    normal[2] /= len;
  }
}


ConvertResult RenderSceneConverter::Convert(const Stage& stage) {
  ConvertResult result;
  ResetOperationState();
  ResetImageIdCache();

  // Built with -fno-exceptions: the conversion helpers report failures via
  // return codes / the warnings_ list rather than throwing, so no try/catch.
  {
    // Report progress
    if (config_.progress_callback) {
      config_.progress_callback(0.0f, "Starting conversion...");
    }

    // Set scene metadata
    auto meta = stage.GetMeta();
    result.scene.name = meta.defaultPrim;
    result.scene.default_prim = meta.defaultPrim;
    result.scene.meters_per_unit = static_cast<float>(meta.metersPerUnit);
    result.scene.up_axis = (meta.upAxis == "Z") ?
                           RenderScene::UpAxis::Z : RenderScene::UpAxis::Y;
    result.scene.start_time = meta.startTimeCode;
    result.scene.end_time = meta.endTimeCode;
    result.scene.frames_per_second = meta.timeCodesPerSecond;
    ::lightusd::next::color_management::RenderingColorConfig color_config;
    std::string color_warning;
    (void)::lightusd::next::color_management::ResolveRenderingColorConfig(
        stage, config_.material.render_settings_path, &color_config,
        &color_warning);
    result.scene.render_settings_path = color_config.render_settings_path;
    result.scene.working_color_space = color_config.working_space;
    ::lightusd::color::ColorSpaceDesc display_linear;
    ::lightusd::color::ColorTransform working_to_display;
    if (::lightusd::color::GetBuiltinColorSpace("lin_rec709_scene",
                                                &display_linear) &&
        ::lightusd::color::BuildColorTransform(
            color_config.working_definition, display_linear,
            &working_to_display)) {
      for (size_t i = 0; i < 9; ++i) {
        result.scene.working_to_display_linear[i] =
            working_to_display.matrix[i];
      }
    }
    if (!color_warning.empty()) AddWarning(color_warning);

    RenderExtractOptions xopts;
    xopts.time_code = config_.time_code;
    xopts.max_depth = config_.max_render_depth;
    xopts.max_records = config_.max_render_records;
    xopts.collect_other = true;
    RenderExtractResult extracted;
    CollectRenderPrims(stage, xopts, &extracted);
    if (extracted.limit_exceeded) {
      result.error = "render extraction limit exceeded";
      return result;
    }

    // Build node hierarchy first
    if (config_.progress_callback) {
      config_.progress_callback(0.1f, "Building node hierarchy...");
    }
    BuildNodeHierarchy(extracted, &result.scene);
    ExtractPhysicsAnnotations(stage, &result.scene);
    result.scene.unsupported_renderables.reserve(
        std::min<size_t>(extracted.records.size(), 128));
    const bool emit_animations =
        config_.animation.enabled &&
        (stage.HasTimeSamples() || stage.HasValueClips());
    if (emit_animations) {
      result.scene.animations.reserve(extracted.records.size());
    }
    const size_t point_prim_count = extracted.points.size();
    result.scene.meshes.reserve(extracted.meshes.size());
    result.scene.points.reserve(point_prim_count);
    result.scene.curves.reserve(extracted.curves.size());
    result.scene.point_instancers.reserve(extracted.point_instancers.size());
    result.scene.lights.reserve(extracted.lights.size());
    result.scene.cameras.reserve(extracted.cameras.size());
    result.scene.materials.reserve(extracted.materials.size());
    result.scene.skeletons.reserve(extracted.skeletons.size());
    result.scene.mesh_by_path.reserve(extracted.meshes.size());
    result.scene.points_by_path.reserve(point_prim_count);
    result.scene.curves_by_path.reserve(extracted.curves.size());
    result.scene.point_instancer_by_path.reserve(extracted.point_instancers.size());
    result.scene.material_by_path.reserve(extracted.materials.size());

    const bool has_time_samples = emit_animations;
    for (const RenderPrimRecord& rec : extracted.records) {
      if (rec.type_name != "Points" &&
          rec.type_name != "ParticleField3DGaussianSplat" &&
          IsUnsupportedRenderableTypeName(rec.type_name)) {
        UnsupportedRenderable unsupported;
        unsupported.prim_path = rec.path;
        unsupported.type_name = rec.type_name;
        unsupported.reason =
            "recognized by extraction but not converted to render geometry";
        result.scene.unsupported_renderables.push_back(unsupported);
        AddWarning("Unsupported renderable prim '" + rec.path +
                            "' of type '" + rec.type_name + "'");
      }

      if (!has_time_samples ||
          (!stage.HasValueClips() &&
           (!rec.prim.GetPrimSpec() ||
            !rec.prim.GetPrimSpec()->has_any_time_samples()))) {
        continue;
      }
      AnimationClip clip;
      if (ConvertAnimation(stage, rec.prim, &clip)) {
        const auto node_it = result.scene.node_by_path.find(rec.path);
        if (node_it != result.scene.node_by_path.end()) {
          for (AnimationChannel& channel : clip.channels) {
            channel.target_node = node_it->second;
          }
        }
        result.scene.animations.push_back(std::move(clip));
      }
    }

    // Convert meshes
    //
    // Each mesh converts independently of every other (ConvertRenderableMesh
    // reads the composed stage and writes only into its own RenderMesh out-
    // param -- verified no other prim's data or scene-wide converter state is
    // touched); the only shared state it reaches is warnings_/last_error_/the
    // BudgetWouldExceed counters, all funneled through AddWarning()/
    // SetLastError()/BudgetWouldExceed() under state_mu_ above. So the actual
    // per-mesh conversion runs on a worker pool, in batches sized to the
    // hardware; scene bookkeeping (id assignment, mesh_by_path, node linking,
    // geometry-retention trimming) stays serial and in original prim order so
    // output stays byte-identical to the single-threaded path.
    //
    // The budget check is still made on the main thread, one mesh at a time,
    // before it is added to a batch -- same "decide with real RSS, then do
    // the work" ordering as the serial loop had, just batched at worker-pool
    // granularity instead of per-mesh.
    float mesh_progress_start = 0.2f;
    float mesh_progress_end = 0.5f;

    const size_t mesh_count = extracted.meshes.size();
    size_t mesh_workers = 1;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (config_.execution.max_threads >= 0) {
      if (config_.execution.max_threads == 0) {
        const unsigned hw_threads = std::thread::hardware_concurrency();
        mesh_workers = std::max<size_t>(
            1, std::min<size_t>(hw_threads ? hw_threads : 4, 16));
      } else {
        mesh_workers = std::min<size_t>(
            static_cast<size_t>(config_.execution.max_threads),
            static_cast<size_t>(::lightusd::next::kMaxExecutionThreads));
      }
    } else if (config_.max_worker_threads > 0) {
      mesh_workers = std::min<size_t>(
          config_.max_worker_threads,
          static_cast<size_t>(::lightusd::next::kMaxExecutionThreads));
    } else {
      const unsigned hw_threads = std::thread::hardware_concurrency();
      mesh_workers =
          std::max<size_t>(1, std::min<size_t>(hw_threads ? hw_threads : 4, 16));
    }
#endif
    ::lightusd::next::TaskArena task_arena(mesh_workers);

    // Dynamic work distribution: instead of fixed-size batch waves (where one
    // oversized mesh idled the other workers until the wave barrier), all
    // mesh indices are pushed through a single arena.Run() pass and claimed
    // via an atomic cursor. Each output writes into its own slot, and the
    // serial merge below walks slots in prim order -- so results stay
    // byte-identical to the previous batched scheduler regardless of which
    // worker ran what.
    //
    // Per-item geometry size estimates are computed up front (one serial
    // pass) so the budget gate inside each task no longer re-walks the prim's
    // authored arrays on the hot path.
    std::vector<size_t> mesh_estimates(mesh_count);
    for (size_t i = 0; i < mesh_count; ++i) {
      mesh_estimates[i] =
          BuildGeometryInfo(extracted.meshes[i].prim, GeometryKind::Mesh, -1)
              .estimated_resident_bytes;
    }

    std::vector<RenderMesh> mesh_out(mesh_count);
    std::vector<uint8_t> mesh_ok(mesh_count, 0);
    std::atomic<bool> mesh_budget_hit{false};

    task_arena.Run(mesh_count, [&](size_t mi) {
      if (mesh_budget_hit.load(std::memory_order_relaxed)) {
        return;
      }
      // Cumulative budget guard: skip the rest of the geometry rather than
      // let a scene of many individually-small meshes OOM the process. Same
      // "decide with real accounting, then do the work" ordering as before,
      // just executed by whichever worker claims the item (BudgetWouldExceed
      // is state_mu_-guarded and latches).
      if (BudgetWouldExceed(mesh_estimates[mi], "mesh conversion")) {
        mesh_budget_hit.store(true, std::memory_order_relaxed);
        return;
      }
      mesh_ok[mi] =
          ConvertRenderableMesh(stage, extracted.meshes[mi].prim,
                                &mesh_out[mi]) ? 1 : 0;
    });

    const bool budget_latched = mesh_budget_hit.load(std::memory_order_relaxed);

    // Serial merge in original prim order. Progress is reported here on the
    // calling thread as items complete (the previous scheduler reported
    // dispatch progress from the batch-fill loop; both are advisory).
    for (size_t mi = 0; mi < mesh_count; ++mi) {
      if (!mesh_ok[mi]) {
        if (budget_latched) {
          // Remaining geometry was skipped by the memory budget.
          break;
        }
        AddWarning("Failed to convert renderable mesh prim: " +
                            extracted.meshes[mi].prim.GetPath().str());
        continue;
      }
      if (config_.progress_callback) {
        float p = mesh_progress_start +
                  (mesh_progress_end - mesh_progress_start) * mi /
                      std::max<size_t>(mesh_count, 1);
        config_.progress_callback(p,
                                  "Converting mesh: " +
                                      extracted.meshes[mi].prim.GetName());
      }

      const UsdPrim& mesh_prim = extracted.meshes[mi].prim;
      RenderMesh& mesh = mesh_out[mi];
      if (mesh.has_alloc_failure()) {
        // ConvertGeomPrimitive does not run ConvertMesh's alloc check.
        AddWarning("Out of memory converting prim '" +
                            mesh_prim.GetPath().str() +
                            "'; the prim was skipped");
        continue;
      }
      const bool analytic = mesh_prim.GetTypeName() != "Mesh";
      if (!config_.mesh.retain_geometry &&
          !(analytic && config_.mesh.retain_analytic_geometry)) {
        // Retain only the small inputs still required by the later material
        // binding and UV-selection passes. This bounds conversion memory by
        // one source mesh instead of accumulating the whole render scene.
        ReleaseMeshGeometry(&mesh, true,
                            config_.mesh.retain_triangulation);
      }
      // Release chunk-allocation slack before retaining: thousands of small
      // meshes each holding 64KB-minimum chunks otherwise OOM wasm32.
      mesh.compact();
      int32_t mesh_id = static_cast<int32_t>(result.scene.meshes.size());
      result.scene.mesh_by_path[mesh.prim_path] = mesh_id;
      result.scene.meshes.push_back(std::move(mesh));
      AssignNodeDataId(&result.scene, mesh_prim.GetPath().str(), mesh_id);
    }

    // Points and Curves conversion is as independent per-prim as mesh
    // conversion (verified: ConvertPoints/ConvertCurves touch no converter
    // state beyond warnings_/last_error_, already funneled through
    // AddWarning()/SetLastError() under state_mu_), so run them across the
    // same worker pool with dynamic index distribution (single Run pass; no
    // fixed-size batch waves). Bookkeeping (id assignment, *_by_path, node
    // linking, compact/release) stays serial and in original record order.
    {
      const size_t n_points = extracted.points.size();
      std::vector<RenderPoints> points_out(n_points);
      std::vector<uint8_t> points_ok(n_points, 0);
      task_arena.Run(n_points, [&](size_t bi) {
        points_ok[bi] = ConvertPoints(stage,
                                      extracted.points[bi].prim,
                                      &points_out[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n_points; ++bi) {
        const auto& rec = extracted.points[bi];
        RenderPoints& points = points_out[bi];
        if (points_ok[bi]) {
          if (points.has_alloc_failure()) {
            AddWarning("Out of memory converting Points '" + rec.path +
                                "'; the prim was skipped");
            continue;
          }
          int32_t points_id = static_cast<int32_t>(result.scene.points.size());
          points.compact();
          result.scene.points_by_path[points.prim_path] = points_id;
          result.scene.points.push_back(std::move(points));
          AssignNodeDataId(&result.scene, rec.path, points_id);
        } else {
          AddWarning("Failed to convert Points: " + rec.path);
        }
      }
    }

    {
      const size_t n_curves = extracted.curves.size();
      std::vector<RenderCurves> curves_out(n_curves);
      std::vector<uint8_t> curves_ok(n_curves, 0);
      task_arena.Run(n_curves, [&](size_t bi) {
        curves_ok[bi] =
            ConvertCurves(extracted.curves[bi].prim, &curves_out[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n_curves; ++bi) {
        const auto& rec = extracted.curves[bi];
        RenderCurves& curves = curves_out[bi];
        if (curves_ok[bi]) {
          if (curves.has_alloc_failure()) {
            AddWarning("Out of memory converting curves '" + rec.path +
                                "'; the prim was skipped");
            continue;
          }
          int32_t curves_id = static_cast<int32_t>(result.scene.curves.size());
          curves.compact();
          result.scene.curves_by_path[curves.prim_path] = curves_id;
          result.scene.curves.push_back(std::move(curves));
          AssignNodeDataId(&result.scene, rec.path, curves_id);
        } else {
          AddWarning("Failed to convert curves prim: " + rec.path);
        }
      }
    }

    // ConvertPointInstancer itself (reading positions/orientations/scales/
    // velocities/ids arrays) touches no shared converter or scene state, so
    // it runs in the worker batch; prototype-binding resolution, instance
    // draw expansion and id/scene bookkeeping all mutate result.scene and
    // stay serial in original record order.
    for (size_t batch_start = 0; batch_start < extracted.point_instancers.size();
         batch_start += mesh_workers) {
      const size_t batch_end = std::min(batch_start + mesh_workers,
                                        extracted.point_instancers.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderPointInstancer> batch_inst(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        batch_ok[bi] = ConvertPointInstancer(
                           extracted.point_instancers[batch_start + bi].prim,
                           &batch_inst[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        const auto& rec = extracted.point_instancers[batch_start + bi];
        RenderPointInstancer& instancer = batch_inst[bi];
        if (!batch_ok[bi]) {
          AddWarning("Failed to convert PointInstancer: " + rec.path);
          continue;
        }
        int32_t instancer_id =
            static_cast<int32_t>(result.scene.point_instancers.size());
        result.scene.point_instancer_by_path[instancer.prim_path] = instancer_id;
        if (!instancer.valid) {
          AddWarning("Invalid PointInstancer data at " +
                              instancer.prim_path + ": " +
                              instancer.validation_error);
        }
        ResolvePointInstancerPrototypeBindings(&result.scene, &instancer);
        for (size_t proto_i = 0; proto_i < instancer.prototype_paths.size();
             ++proto_i) {
          if (proto_i >= instancer.prototype_node_ids.size() ||
              instancer.prototype_node_ids[proto_i] < 0) {
            AddWarning("Unresolved PointInstancer prototype at " +
                                instancer.prim_path + ": " +
                                instancer.prototype_paths[proto_i]);
          } else if (instancer.prototype_mesh_count(proto_i) == 0) {
            AddWarning("PointInstancer prototype has no meshes at " +
                                instancer.prim_path + ": " +
                                instancer.prototype_paths[proto_i]);
          }
        }
        if (config_.point_instancer.build_instance_draws ||
            config_.point_instancer.duplicate_meshes) {
          AppendPointInstanceDraws(instancer_id, &instancer, &result.scene);
        }
        result.scene.point_instancers.push_back(std::move(instancer));
        AssignNodeDataId(&result.scene, rec.path, instancer_id);
      }
    }

    // Convert materials. Each ConvertMaterial call is given its own
    // per-worker LOCAL scratch RenderScene (tl_material_local_scope_ set for
    // the duration -- see MaterialLocalScope below), so the graph-walk/
    // texture-registration work runs in parallel; a serial merge afterward
    // dedups/appends each worker's local images/textures into the shared
    // scene (in original material order, replicating exactly what the
    // fully-serial dedup would have done) and remaps every
    // ShaderParam::texture_id from the local scratch's numbering to the
    // final shared numbering.
    float mat_progress_start = 0.5f;
    float mat_progress_end = 0.7f;

    for (size_t batch_start = 0; batch_start < extracted.materials.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.materials.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderMaterial> batch_material(n);
      std::vector<RenderScene> batch_local_scene(n);
      std::vector<uint8_t> batch_ok(n, 0);
      for (size_t bi = 0; bi < n; ++bi) {
        // Only the color-managed fields ConvertMaterial/ExtractShaderParam
        // read are seeded; everything else on the scratch scene starts
        // empty and is discarded after merge.
        batch_local_scene[bi].working_color_space =
            result.scene.working_color_space;
        if (config_.progress_callback) {
          const size_t i = batch_start + bi;
          float p = mat_progress_start +
                    (mat_progress_end - mat_progress_start) * i /
                        std::max<size_t>(extracted.materials.size(), 1);
          config_.progress_callback(
              p, "Converting material: " +
                     extracted.materials[i].prim.GetName());
        }
      }
      auto convert_one = [&](size_t bi) {
        MaterialLocalScope scope;
        const UsdPrim& mat_prim = extracted.materials[batch_start + bi].prim;
        batch_ok[bi] = ConvertMaterial(stage, mat_prim, &batch_material[bi],
                                       &batch_local_scene[bi]) ? 1 : 0;
      };
#if defined(LIGHTUSD_ENABLE_THREAD)
      const bool callbacks_may_run_concurrently =
          config_.execution.callback_concurrency ==
          ::lightusd::next::CallbackConcurrency::Concurrent;
      const bool resolver_has_callbacks =
          config_.asset_resolver && config_.asset_resolver->HasUserCallbacks();
      const bool material_parallel_safe =
          callbacks_may_run_concurrently ||
          (!config_.material.custom_texture_loader &&
           !resolver_has_callbacks);
      if (n == 1 || !material_parallel_safe) {
#else
      if (true) {
#endif
        for (size_t bi = 0; bi < n; ++bi) convert_one(bi);
      } else {
#if defined(LIGHTUSD_ENABLE_THREAD)
        task_arena.Run(n, convert_one);
#endif
      }

      for (size_t bi = 0; bi < n; ++bi) {
        const UsdPrim& mat_prim = extracted.materials[batch_start + bi].prim;
        RenderMaterial& material = batch_material[bi];
        RenderScene& local = batch_local_scene[bi];

        // Dedup images against the shared scene (same key ConvertMaterial
        // itself uses), in the local scratch's own append order -- this
        // reproduces the exact sequence of "check cache, maybe append"
        // decisions the fully-serial converter would have made.
        std::vector<int32_t> image_remap(local.images.size(), -1);
        for (size_t ii = 0; ii < local.images.size(); ++ii) {
          TextureImage& img = local.images[ii];
          const std::string resolved_path = img.resolved_path;
          const ColorSpace cs = img.color_space;
          int32_t id = FindCachedImageId(&result.scene, resolved_path, cs);
          if (id < 0) {
            id = static_cast<int32_t>(result.scene.images.size());
            result.scene.images.push_back(std::move(img));
            RememberImageId(&result.scene, resolved_path, cs, id);
          }
          image_remap[ii] = id;
        }
        // Textures are never deduped against each other (matches the
        // original: every ExtractShaderParam call appends a fresh
        // RenderTexture), just index-remapped and appended.
        std::vector<int32_t> texture_remap(local.textures.size(), -1);
        for (size_t ti = 0; ti < local.textures.size(); ++ti) {
          RenderTexture& tex = local.textures[ti];
          if (tex.image_id >= 0 &&
              static_cast<size_t>(tex.image_id) < image_remap.size()) {
            tex.image_id = image_remap[static_cast<size_t>(tex.image_id)];
          }
          texture_remap[ti] = static_cast<int32_t>(result.scene.textures.size());
          result.scene.textures.push_back(std::move(tex));
        }
        RemapMaterialTextureIds(&material, texture_remap);

        if (batch_ok[bi]) {
          int32_t mat_id = static_cast<int32_t>(result.scene.materials.size());
          result.scene.material_by_path[material.prim_path] = mat_id;
          result.scene.materials.push_back(std::move(material));
        } else {
          AddWarning("Failed to convert material: " + mat_prim.GetPath().str());
        }
      }
    }

    AssignMaterialBindings(stage, &result.scene);
    // 2-arg on purpose: promotion compares against the mesh's ACTUAL
    // texcoords_0/1 names now (53415635e), which supersedes the old
    // `default_uv` / `default_uv + "1"` heuristic and its config field.
    PromoteMaterialUVPrimvars(&result.scene, &warnings_);
    if (!config_.mesh.retain_geometry) {
      for (RenderMesh& mesh : result.scene.meshes) {
        const UsdPrim source = stage.GetPrimAtPath(mesh.prim_path);
        const bool analytic = source.IsValid() && source.GetTypeName() != "Mesh";
        if (analytic && config_.mesh.retain_analytic_geometry) continue;
        ReleaseMeshGeometry(&mesh, false,
                            config_.mesh.retain_triangulation);
      }
    }
    AssignPointInstanceDrawMaterials(&result.scene);
    if (config_.point_instancer.duplicate_meshes) {
      DuplicatePointInstanceMeshes(&result.scene);
    }

    // Convert lights. ConvertLight itself touches no shared converter/scene
    // state (verified), so it runs in the worker batch; the DomeLight
    // environment-texture lookup calls ResolveImageId, which mutates the
    // scene-wide image dedup cache (image_id_by_key_/image_id_cache_) and
    // result.scene.images -- that stays serial in the merge step, same as
    // id assignment.
    for (size_t batch_start = 0; batch_start < extracted.lights.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.lights.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderLight> batch_light(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        batch_ok[bi] = ConvertLight(
                           extracted.lights[batch_start + bi].prim,
                           &batch_light[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        if (!batch_ok[bi]) continue;
        const auto& rec = extracted.lights[batch_start + bi];
        RenderLight& light = batch_light[bi];
        for (int i = 0; i < 16; ++i) {
          light.transform.m[i] = static_cast<float>(rec.world[i]);
        }
        // DomeLight environment texture -> image, id stored in params.dome.
        if (light.type == LightType::Dome) {
          light.params.dome.texture_id = -1;
          std::string tex;
          const Value* fv = rec.prim.GetPropertyValue(kIdInputsTextureFile());
          if (fv) {
            if (const std::string* ap = fv->as_asset_path()) tex = *ap;
            else if (const std::string* s = fv->as_string()) tex = *s;
            else if (const std::string* t = fv->as_token()) tex = *t;
          }
          if (!tex.empty()) {
            light.params.dome.texture_id = ResolveImageId(
                &result.scene, tex, ColorSpace::Linear, AssetAnchorOf(rec.prim));
          }
        }
        int32_t light_id = static_cast<int32_t>(result.scene.lights.size());
        result.scene.lights.push_back(std::move(light));
        AssignNodeDataId(&result.scene, rec.path, light_id);
      }
    }

    // Convert cameras. ConvertCamera touches no converter state beyond
    // warnings_/last_error_ (already mutex-guarded), so batch it the same
    // way as mesh/points/curves conversion.
    for (size_t batch_start = 0; batch_start < extracted.cameras.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.cameras.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderCamera> batch_camera(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        batch_ok[bi] = ConvertCamera(
                           stage, extracted.cameras[batch_start + bi].prim,
                           &batch_camera[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        if (!batch_ok[bi]) continue;
        const auto& rec = extracted.cameras[batch_start + bi];
        RenderCamera& camera = batch_camera[bi];
        for (int i = 0; i < 16; ++i) {
          camera.transform.m[i] = static_cast<float>(rec.world[i]);
        }
        int32_t camera_id = static_cast<int32_t>(result.scene.cameras.size());
        result.scene.cameras.push_back(std::move(camera));
        AssignNodeDataId(&result.scene, rec.path, camera_id);
      }
    }

    // Convert skeletons. ConvertSkeleton itself (joint hierarchy + bind/rest
    // transform computation) is the expensive part and touches no shared
    // state, so it runs in the worker batch; the skel:animationSource
    // ancestor walk and id/scene bookkeeping stay serial in original order.
    for (size_t batch_start = 0; batch_start < extracted.skeletons.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.skeletons.size());
      const size_t n = batch_end - batch_start;
      std::vector<Skeleton> batch_skel(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        batch_ok[bi] = ConvertSkeleton(
                           extracted.skeletons[batch_start + bi].prim,
                           &batch_skel[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        if (!batch_ok[bi]) continue;
        const auto& rec = extracted.skeletons[batch_start + bi];
        Skeleton& skeleton = batch_skel[bi];
        // skel:animationSource may be authored on the SkelRoot (or another
        // ancestor) instead of the Skeleton itself; every descendant
        // Skeleton inherits it (UsdSkel binding inheritance).
        if (skeleton.animation_source_path.empty()) {
          UsdPrim anc = GetParent(stage, rec.prim);
          while (anc.IsValid()) {
            const std::vector<std::string> sources =
                ReadRelationshipTargets(anc, "skel:animationSource");
            if (!sources.empty()) {
              skeleton.animation_source_path = sources[0];
              break;
            }
            if (::lightusd::tydra::next::IsSkelRoot(anc)) break;
            anc = GetParent(stage, anc);
          }
        }
        int32_t skeleton_id = static_cast<int32_t>(result.scene.skeletons.size());
        result.scene.skeletons.push_back(std::move(skeleton));
        AssignNodeDataId(&result.scene, rec.path, skeleton_id);
      }
    }

    // Resolve mesh skin bindings to skeleton ids (skeletons converted above).
    std::unordered_map<std::string, int32_t> skeleton_id_by_path;
    skeleton_id_by_path.reserve(extracted.skeletons.size());
    for (size_t si = 0; si < extracted.skeletons.size(); ++si) {
      skeleton_id_by_path.emplace(extracted.skeletons[si].path,
                                  static_cast<int32_t>(si));
    }
    std::vector<std::unordered_map<std::string, int32_t>> skeleton_joint_lookup;
    if (!result.scene.skeletons.empty()) {
      skeleton_joint_lookup.resize(result.scene.skeletons.size());
      for (size_t si = 0; si < result.scene.skeletons.size(); ++si) {
        const auto& skel = result.scene.skeletons[si];
        if (skel.joints.empty()) continue;
        auto& lookup = skeleton_joint_lookup[si];
        lookup.reserve(skel.joints.size());
        lookup.max_load_factor(0.7f);
        for (size_t ji = 0; ji < skel.joints.size(); ++ji) {
          const SkeletonJoint& joint = skel.joints[ji];
          const std::string& key = joint.path.empty() ? joint.name : joint.path;
          if (!key.empty()) {
            lookup.emplace(key, static_cast<int32_t>(ji));
          }
        }
      }
    }
    for (RenderMesh& mesh : result.scene.meshes) {
      if (!mesh.skin || mesh.skin->skeleton_path.empty()) continue;
      const auto sit = skeleton_id_by_path.find(mesh.skin->skeleton_path);
      if (sit != skeleton_id_by_path.end()) {
        mesh.skin->skeleton_id = sit->second;
      }

      // Mesh-local `skel:joints`: jointIndices index into the mesh's own
      // (subset/permuted) joint list — remap them onto the skeleton's joint
      // order. Unmatched tokens zero the influence weight rather than
      // silently deforming by joint 0.
      if (!mesh.skin->mesh_joint_order.empty() &&
          mesh.skin->skeleton_id >= 0) {
        const Skeleton& skel =
            result.scene.skeletons[static_cast<size_t>(mesh.skin->skeleton_id)];
        const auto& joint_lookup =
            skeleton_joint_lookup[static_cast<size_t>(mesh.skin->skeleton_id)];
        std::vector<int32_t> remap(mesh.skin->mesh_joint_order.size(), -1);
        bool identity = true;
        for (size_t k = 0; k < mesh.skin->mesh_joint_order.size(); ++k) {
          const std::string& token = mesh.skin->mesh_joint_order[k];
          auto it = joint_lookup.find(token);
          if (it != joint_lookup.end()) {
            remap[k] = it->second;
          } else {
            for (size_t ji = 0; ji < skel.joints.size(); ++ji) {
              if (JointTokenMatches(skel.joints[ji], token)) {
                remap[k] = static_cast<int32_t>(ji);
                break;
              }
            }
          }
          if (remap[k] != static_cast<int32_t>(k)) identity = false;
          if (remap[k] < 0) {
            AddWarning("Mesh " + mesh.prim_path +
                                " skel:joints token '" + token +
                                "' not found in skeleton " + skel.prim_path);
          }
        }
        if (!identity) {
          const size_t n = mesh.skin->joint_indices.size();
          for (size_t k = 0; k < n; ++k) {
            const uint16_t local = mesh.skin->joint_indices[k];
            const int32_t target =
                local < remap.size() ? remap[local] : -1;
            if (target >= 0 && target <= 65535) {
              mesh.skin->joint_indices.mutable_at(k) = static_cast<uint16_t>(target);
            } else {
              mesh.skin->joint_indices.mutable_at(k) = 0;
              if (k < mesh.skin->joint_weights.size()) {
                mesh.skin->joint_weights.mutable_at(k) = 0.0f;
              }
            }
          }
        }
      }
    }

    ResolveSkeletalAnimationTargets(&result.scene);

    ResolveLightLinking(stage, &result.scene);

    if (config_.progress_callback) {
      config_.progress_callback(1.0f, "Conversion complete");
    }

    result.success = true;
    result.warnings = std::move(warnings_);
  }

  return result;
}




}  // namespace next
}  // namespace tydra
}  // namespace lightusd
