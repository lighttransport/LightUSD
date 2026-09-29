// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdShade Schema Implementation

#include "usd-shade.hh"
#include "../layer/prim-spec.hh"

#include <algorithm>
#include <cmath>

namespace lightusd {
namespace next {

bool IsMaterial(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Material";
}

bool IsShader(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "Shader";
}

bool IsNodeGraph(const UsdPrim& prim) {
  return prim.IsValid() && prim.GetTypeName() == "NodeGraph";
}

namespace {

bool ResolveShaderPortValueRec(const Stage& stage, const UsdPrim& prim,
                               const std::string& port_name, Value* out,
                               double time, int depth, int max_depth) {
  if (!prim || !out || depth > max_depth) return false;
  const PrimSpec* spec = prim.GetPrimSpec();
  if (spec) {
    if (const std::vector<Path>* connections = spec->connection(port_name)) {
      if (!connections->empty()) {
        const Path& target = (*connections)[0];
        const std::string target_port = target.property_name();
        const UsdPrim target_prim = stage.GetPrimAtPath(target.prim_path());
        if (!target_port.empty() && target_prim &&
            ResolveShaderPortValueRec(stage, target_prim, target_port, out,
                                      time, depth + 1, max_depth)) {
          return true;
        }
      }
    }
  }

  EvalOptions options;
  options.follow_connections = false;
  options.max_connection_depth = max_depth;
  options.time = TimeQuery::Numeric(time);
  AttributeEval eval(&stage);
  EvalResult result = eval.EvalWith(prim, port_name, options);
  if (result.success) {
    *out = result.value;
    return true;
  }

  // MaterialX constant nodes author the value on inputs:value while their
  // outputs:out port is a declaration without a default value.
  if (port_name.rfind("outputs:", 0) == 0 && IsShader(prim)) {
    const Value* id_value = prim.GetPropertyValue("info:id");
    const std::string* id = id_value ? id_value->as_token() : nullptr;
    if (id && (id->find("constant") != std::string::npos ||
               id->find("Constant") != std::string::npos)) {
      return ResolveShaderPortValueRec(stage, prim, "inputs:value", out, time,
                                       depth + 1, max_depth);
    }
    if (id) {
      auto read = [&](const char* input, Value* value) {
        return ResolveShaderPortValueRec(stage, prim, input, value, time,
                                         depth + 1, max_depth);
      };
      auto components = [](const Value& value, float lanes[3], int* count) {
        if (const float* f = value.as_float()) {
          lanes[0] = lanes[1] = lanes[2] = *f; *count = 1; return true;
        }
        if (const double* d = value.as_double()) {
          lanes[0] = lanes[1] = lanes[2] = float(*d); *count = 1; return true;
        }
        if (value.to_float3(lanes)) { *count = 3; return true; }
        return false;
      };
      auto make = [](const float lanes[3], int count) {
        return count == 1 ? Value(lanes[0])
                          : Value::MakeFloat3(lanes[0], lanes[1], lanes[2]);
      };
      auto starts = [&](const char* prefix) { return id->rfind(prefix, 0) == 0; };
      if (starts("ND_add_") || starts("ND_subtract_") ||
          starts("ND_multiply_") || starts("ND_divide_") ||
          starts("ND_min_") || starts("ND_max_")) {
        Value av, bv;
        float a[3], b[3], result_lanes[3];
        int ac = 0, bc = 0;
        if (!read("inputs:in1", &av) || !read("inputs:in2", &bv) ||
            !components(av, a, &ac) || !components(bv, b, &bc)) return false;
        const int count = std::max(ac, bc);
        for (int i = 0; i < count; ++i) {
          if (starts("ND_add_")) result_lanes[i] = a[i] + b[i];
          else if (starts("ND_subtract_")) result_lanes[i] = a[i] - b[i];
          else if (starts("ND_multiply_")) result_lanes[i] = a[i] * b[i];
          else if (starts("ND_divide_"))
            result_lanes[i] = std::fabs(b[i]) > 1.0e-8f ? a[i] / b[i] : 0.0f;
          else if (starts("ND_min_")) result_lanes[i] = std::min(a[i], b[i]);
          else result_lanes[i] = std::max(a[i], b[i]);
        }
        *out = make(result_lanes, count);
        return true;
      }
      if (starts("ND_clamp_")) {
        Value vv, lv, hv;
        float v[3], lo[3], hi[3], result_lanes[3];
        int vc = 0, lc = 0, hc = 0;
        if (!read("inputs:in", &vv) || !read("inputs:low", &lv) ||
            !read("inputs:high", &hv) || !components(vv, v, &vc) ||
            !components(lv, lo, &lc) || !components(hv, hi, &hc)) return false;
        const int count = std::max(vc, std::max(lc, hc));
        for (int i = 0; i < count; ++i)
          result_lanes[i] = std::min(std::max(v[i], lo[i]), hi[i]);
        *out = make(result_lanes, count);
        return true;
      }
      if (starts("ND_mix_")) {
        Value bgv, fgv, mixv;
        float bg[3], fg[3], amount[3], result_lanes[3];
        int bgc = 0, fgc = 0, mc = 0;
        if (!read("inputs:bg", &bgv) || !read("inputs:fg", &fgv) ||
            !read("inputs:mix", &mixv) || !components(bgv, bg, &bgc) ||
            !components(fgv, fg, &fgc) || !components(mixv, amount, &mc))
          return false;
        const int count = std::max(bgc, fgc);
        for (int i = 0; i < count; ++i) {
          const float t = amount[mc == 1 ? 0 : i];
          result_lanes[i] = bg[i] * (1.0f - t) + fg[i] * t;
        }
        *out = make(result_lanes, count);
        return true;
      }
    }
  }
  return false;
}

}  // namespace

bool ResolveShaderPortValue(const Stage& stage, const UsdPrim& prim,
                            const std::string& port_name, Value* out,
                            double time, int max_depth) {
  if (max_depth < 0) return false;
  return ResolveShaderPortValueRec(stage, prim, port_name, out, time, 0,
                                   max_depth);
}

namespace {

// First purpose-preferred binding whose target resolves to a Material. A
// dangling preview-purpose target must not hide a valid all-purpose/full target
// on the same prim. Optionally reports the strength metadata of the relationship
// that actually won (rather than the first authored, possibly dangling one).
std::string GetValidBoundMaterialPath(const Stage& stage, const UsdPrim& prim,
                                      bool* stronger = nullptr,
                                      const std::string& purpose = "") {
  if (stronger) *stronger = false;
  if (!prim.IsValid()) return "";
  static const char* kPreviewBindingOrder[] = {"material:binding:preview",
                                               "material:binding",
                                               "material:binding:full"};
  static const char* kFullBindingOrder[] = {"material:binding:full",
                                            "material:binding",
                                            "material:binding:preview"};
  const PrimSpec* spec = prim.GetPrimSpec();
  const bool standardPurpose = purpose == "preview" || purpose == "full";
  const std::string purpose_rel = purpose.empty() || standardPurpose
                                      ? std::string()
                                      : "material:binding:" + purpose;
  const char* const* bindingOrder = purpose == "full" ? kFullBindingOrder
                                                       : kPreviewBindingOrder;
  const size_t count = purpose.empty() || standardPurpose ? size_t{3}
                                                          : size_t{1};
  for (size_t i = 0; i < count; ++i) {
    const char* rel = (purpose.empty() || standardPurpose)
                          ? bindingOrder[i]
                          : purpose_rel.c_str();
    const std::vector<Path>* targets = prim.GetRelationship(rel);
    if (!targets || targets->empty()) continue;
    const std::string path = (*targets)[0].str();
    if (!IsMaterial(stage.GetPrimAtPath(path))) continue;
    if (stronger && spec) {
      if (const PropMeta* pm = spec->property_meta(rel)) {
        *stronger = (pm->authored & PropMeta::kBindMaterialAs) &&
                    pm->bindMaterialAs == "strongerThanDescendants";
      }
    }
    return path;
  }
  return "";
}

}  // namespace

// ============================================================
// Material API Implementation
// ============================================================

// A material terminal ("outputs:surface" etc.) is an ATTRIBUTE CONNECTION in
// USD (`token outputs:surface.connect = </Mat/Shader.outputs:surface>`), not a
// relationship. Accept both (some hand-authored files use a rel). The returned
// path is the connected PRIM (property suffix stripped).
static std::string GetTerminalShaderPath(const UsdPrim& material,
                                         const char* output_name) {
  auto strip_prop = [](const std::string& p) {
    size_t dot = p.rfind('.');
    // Property separator only when the dot comes after the last '/'.
    size_t slash = p.rfind('/');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash)) {
      return p.substr(0, dot);
    }
    return p;
  };
  if (const PrimSpec* spec = material.GetPrimSpec()) {
    if (const std::vector<Path>* conns = spec->connection(output_name)) {
      if (!conns->empty()) return strip_prop((*conns)[0].str());
    }
  }
  const std::vector<Path>* targets = material.GetRelationship(output_name);
  if (targets && !targets->empty()) {
    return strip_prop((*targets)[0].str());
  }
  return "";
}

std::string GetSurfaceShader(const Stage& /* stage */, const UsdPrim& material) {
  if (!IsMaterial(material)) return "";
  return GetTerminalShaderPath(material, "outputs:surface");
}

std::string GetDisplacementShader(const Stage& /* stage */, const UsdPrim& material) {
  if (!IsMaterial(material)) return "";
  std::string path = GetTerminalShaderPath(material, "outputs:displacement");
  if (path.empty()) {
    // RenderMan-authored production layers commonly expose only the ri render
    // context. Preserve that terminal so renderer adapters can translate a
    // supported PxrDisplace graph instead of silently dropping displacement.
    path = GetTerminalShaderPath(material, "outputs:ri:displacement");
  }
  return path;
}

std::string GetVolumeShader(const Stage& /* stage */, const UsdPrim& material) {
  if (!IsMaterial(material)) return "";
  return GetTerminalShaderPath(material, "outputs:volume");
}

bool GetMaterialBinding(const Stage& stage, const UsdPrim& material,
                        MaterialBinding* out) {
  if (!IsMaterial(material) || !out) return false;

  out->surface_shader_path = GetSurfaceShader(stage, material);
  out->displacement_shader_path = GetDisplacementShader(stage, material);
  out->volume_shader_path = GetVolumeShader(stage, material);

  return !out->surface_shader_path.empty() ||
         !out->displacement_shader_path.empty() ||
         !out->volume_shader_path.empty();
}

UsdPrim GetBoundMaterial(const Stage& stage, const UsdPrim& prim) {
  std::string path = GetValidBoundMaterialPath(stage, prim);
  if (path.empty()) return UsdPrim();

  return stage.GetPrimAtPath(path);
}

std::string GetBoundMaterialPath(const UsdPrim& prim) {
  if (!prim.IsValid()) return "";

  // Purpose-specific bindings take precedence for a preview extractor:
  // material:binding:preview, then the all-purpose material:binding, then
  // material:binding:full (UsdShade ComputeBoundMaterial semantics for the
  // "preview" purpose).
  static const char* kBindingOrder[] = {"material:binding:preview",
                                        "material:binding",
                                        "material:binding:full"};
  for (const char* rel : kBindingOrder) {
    const std::vector<Path>* targets = prim.GetRelationship(rel);
    if (targets && !targets->empty()) {
      return (*targets)[0].str();
    }
  }
  return "";
}

namespace {

std::string ParentPathOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "";
  return path.substr(0, slash);
}

}  // namespace

bool BindingIsStrongerThanDescendants(const UsdPrim& prim) {
  static const char* kBindingOrder[] = {"material:binding:preview",
                                        "material:binding",
                                        "material:binding:full"};
  const PrimSpec* spec = prim.GetPrimSpec();
  if (!spec) return false;
  for (const char* rel : kBindingOrder) {
    const std::vector<Path>* targets = prim.GetRelationship(rel);
    if (!targets || targets->empty()) continue;
    if (const PropMeta* pm = spec->property_meta(rel)) {
      if ((pm->authored & PropMeta::kBindMaterialAs) &&
          pm->bindMaterialAs == "strongerThanDescendants") {
        return true;
      }
    }
    return false;  // binding found; default weakerThanDescendants
  }
  return false;
}

std::string GetInheritedBoundMaterialPath(const Stage& stage,
                                          const std::string& prim_path) {
  // UsdShade binding INHERITANCE: a binding authored on an ancestor applies to
  // every descendant. Walk leaf->root; the nearest binding wins by default, but
  // an ancestor marked `bindMaterialAs="strongerThanDescendants"` overrides
  // everything below it, so keep the highest such ancestor.
  std::string leaf_binding;
  std::string strongest_ancestor;
  std::string path = prim_path;
  while (!path.empty() && path != "/") {
    UsdPrim prim = stage.GetPrimAtPath(path);
    if (prim.IsValid()) {
      bool stronger = false;
      const std::string material_path =
          GetValidBoundMaterialPath(stage, prim, &stronger);
      if (!material_path.empty()) {
        if (leaf_binding.empty()) leaf_binding = material_path;
        if (path != prim_path && stronger) {
          strongest_ancestor = material_path;  // higher ancestors overwrite
        }
      }
    }
    path = ParentPathOf(path);
  }
  return strongest_ancestor.empty() ? leaf_binding : strongest_ancestor;
}

std::string GetInheritedBoundMaterialPathForPurpose(
    const Stage& stage, const std::string& prim_path,
    const std::string& purpose) {
  if (purpose.empty()) return GetInheritedBoundMaterialPath(stage, prim_path);

  std::string path = prim_path;
  std::string leaf_binding;
  std::string strongest_ancestor;
  while (!path.empty() && path != "/") {
    const UsdPrim prim = stage.GetPrimAtPath(path);
    if (prim.IsValid()) {
      bool stronger = false;
      const std::string material_path =
          GetValidBoundMaterialPath(stage, prim, &stronger, purpose);
      if (!material_path.empty()) {
        if (leaf_binding.empty()) leaf_binding = material_path;
        if (path != prim_path && stronger) {
          strongest_ancestor = material_path;
        }
      }
    }
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) break;
    path.resize(slash);
  }
  return strongest_ancestor.empty() ? leaf_binding : strongest_ancestor;
}

// ============================================================
// Shader API Implementation
// ============================================================

std::string GetShaderId(const UsdPrim& shader) {
  if (!IsShader(shader)) return "";

  const Value* result = shader.GetPropertyValue("info:id");
  if (result && result->type_id() == TypeId::Token) {
    if (const std::string* str = result->as_token()) {
      return *str;
    }
  }
  return "";
}

std::string GetShaderImplementationSource(const UsdPrim& shader) {
  if (!IsShader(shader)) return "";

  const Value* result = shader.GetPropertyValue("info:implementationSource");
  if (result && result->type_id() == TypeId::Token) {
    if (const std::string* str = result->as_token()) {
      return *str;
    }
  }
  return "id";  // Default
}

std::vector<ShaderPort> GetShaderInputs(const Stage& stage, const UsdPrim& shader,
                                         double time) {
  std::vector<ShaderPort> inputs;
  if (!IsShader(shader)) return inputs;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  // Get all property names starting with "inputs:"
  std::vector<std::string> props = shader.GetPropertyNames();
  for (const std::string& prop_name : props) {
    if (prop_name.size() <= 7 || prop_name.substr(0, 7) != "inputs:") continue;

    ShaderPort port;
    port.name = prop_name.substr(7);  // Remove "inputs:" prefix

    EvalResult result = eval.Eval(shader, prop_name);
    if (result.success) {
      port.value = result.value;
      port.type_id = result.value.type_id();
    }

    // Check for connection
    if (eval.HasConnection(shader, prop_name)) {
      port.connection_path = eval.GetConnectionPath(shader, prop_name);
      port.is_connected = true;
    }

    inputs.push_back(std::move(port));
  }

  return inputs;
}

std::vector<ShaderPort> GetShaderOutputs(const UsdPrim& shader) {
  std::vector<ShaderPort> outputs;
  if (!IsShader(shader)) return outputs;

  // Get all properties starting with "outputs:"
  std::vector<std::string> props = shader.GetPropertyNames();
  for (const std::string& prop_name : props) {
    if (prop_name.size() <= 8 || prop_name.substr(0, 8) != "outputs:") continue;

    ShaderPort port;
    port.name = prop_name.substr(8);  // Remove "outputs:" prefix

    const Value* result = shader.GetPropertyValue(prop_name);
    if (result) {
      port.value = *result;
      port.type_id = result->type_id();
    }

    outputs.push_back(std::move(port));
  }

  return outputs;
}

bool GetShaderInput(const Stage& stage, const UsdPrim& shader,
                    const std::string& input_name, ShaderPort* out,
                    double time) {
  if (!IsShader(shader) || !out) return false;

  std::string attr_name = "inputs:" + input_name;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  EvalResult result = eval.Eval(shader, attr_name);
  if (!result.success) return false;

  out->name = input_name;
  out->value = result.value;
  out->type_id = result.value.type_id();

  // Check for connection
  if (eval.HasConnection(shader, attr_name)) {
    out->connection_path = eval.GetConnectionPath(shader, attr_name);
    out->is_connected = true;
  }

  return true;
}

EvalResult EvalShaderInput(const Stage& stage, const UsdPrim& shader,
                           const std::string& input_name, double time) {
  if (!IsShader(shader)) {
    EvalResult err;
    err.success = false;
    return err;
  }

  std::string attr_name = "inputs:" + input_name;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  // This will follow connections automatically
  return eval.Eval(shader, attr_name);
}

// ============================================================
// UsdPreviewSurface Implementation
// ============================================================

bool IsPreviewSurface(const UsdPrim& shader) {
  if (!IsShader(shader)) return false;
  std::string id = GetShaderId(shader);
  return id == "UsdPreviewSurface";
}

bool GetPreviewSurfaceData(const Stage& stage, const UsdPrim& shader,
                           PreviewSurfaceData* out, double time) {
  if (!IsPreviewSurface(shader) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  // Diffuse color
  float diffuse[3];
  if (eval.EvalFloat3(shader, "inputs:diffuseColor", diffuse)) {
    out->diffuse_color[0] = diffuse[0];
    out->diffuse_color[1] = diffuse[1];
    out->diffuse_color[2] = diffuse[2];
  }

  // Emissive color
  float emissive[3];
  if (eval.EvalFloat3(shader, "inputs:emissiveColor", emissive)) {
    out->emissive_color[0] = emissive[0];
    out->emissive_color[1] = emissive[1];
    out->emissive_color[2] = emissive[2];
  }

  // Specular color
  float specular[3];
  if (eval.EvalFloat3(shader, "inputs:specularColor", specular)) {
    out->specular_color[0] = specular[0];
    out->specular_color[1] = specular[1];
    out->specular_color[2] = specular[2];
  }

  // Scalar values
  out->metallic = eval.EvalOr(shader, "inputs:metallic", 0.0f);
  out->roughness = eval.EvalOr(shader, "inputs:roughness", 0.5f);
  out->clearcoat = eval.EvalOr(shader, "inputs:clearcoat", 0.0f);
  out->clearcoat_roughness = eval.EvalOr(shader, "inputs:clearcoatRoughness", 0.01f);
  out->opacity = eval.EvalOr(shader, "inputs:opacity", 1.0f);
  out->opacity_threshold = eval.EvalOr(shader, "inputs:opacityThreshold", 0.0f);
  out->ior = eval.EvalOr(shader, "inputs:ior", 1.5f);
  out->displacement = eval.EvalOr(shader, "inputs:displacement", 0.0f);
  out->occlusion = eval.EvalOr(shader, "inputs:occlusion", 1.0f);

  // Normal
  float normal[3];
  if (eval.EvalFloat3(shader, "inputs:normal", normal)) {
    out->normal[0] = normal[0];
    out->normal[1] = normal[1];
    out->normal[2] = normal[2];
  }

  // Specular workflow
  int use_specular = eval.EvalOr(shader, "inputs:useSpecularWorkflow", 0);
  out->use_specular_workflow = (use_specular != 0);

  // Check for texture connections
  auto check_texture = [&eval, &shader](const std::string& input) -> std::string {
    std::string attr_name = "inputs:" + input;
    if (eval.HasConnection(shader, attr_name)) {
      return eval.GetConnectionPath(shader, attr_name);
    }
    return "";
  };

  out->diffuse_texture = check_texture("diffuseColor");
  out->normal_texture = check_texture("normal");
  out->metallic_texture = check_texture("metallic");
  out->roughness_texture = check_texture("roughness");
  out->emissive_texture = check_texture("emissiveColor");
  out->occlusion_texture = check_texture("occlusion");
  out->opacity_texture = check_texture("opacity");

  return true;
}

// ============================================================
// UsdUVTexture Implementation
// ============================================================

bool IsUVTexture(const UsdPrim& shader) {
  if (!IsShader(shader)) return false;
  std::string id = GetShaderId(shader);
  return id == "UsdUVTexture";
}

bool GetUVTextureData(const Stage& stage, const UsdPrim& shader,
                      UVTextureData* out, double time) {
  if (!IsUVTexture(shader) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  // File path
  std::optional<std::string> file = eval.EvalAssetPath(shader, "inputs:file");
  if (file) out->file = *file;

  // ST coordinates
  float st[2];
  if (eval.EvalFloat2(shader, "inputs:st", st)) {
    out->st[0] = st[0];
    out->st[1] = st[1];
  }

  // Wrap modes
  std::optional<std::string> wrap_s = eval.EvalToken(shader, "inputs:wrapS");
  if (wrap_s) out->wrap_s = *wrap_s;

  std::optional<std::string> wrap_t = eval.EvalToken(shader, "inputs:wrapT");
  if (wrap_t) out->wrap_t = *wrap_t;

  // Fallback value
  float fallback[4];
  if (eval.EvalFloat4(shader, "inputs:fallback", fallback)) {
    out->fallback[0] = fallback[0];
    out->fallback[1] = fallback[1];
    out->fallback[2] = fallback[2];
    out->fallback[3] = fallback[3];
  }

  // Scale
  float scale[4];
  if (eval.EvalFloat4(shader, "inputs:scale", scale)) {
    out->scale[0] = scale[0];
    out->scale[1] = scale[1];
    out->scale[2] = scale[2];
    out->scale[3] = scale[3];
  }

  // Bias
  float bias[4];
  if (eval.EvalFloat4(shader, "inputs:bias", bias)) {
    out->bias[0] = bias[0];
    out->bias[1] = bias[1];
    out->bias[2] = bias[2];
    out->bias[3] = bias[3];
  }

  // Source color space
  std::optional<std::string> color_space = eval.EvalToken(shader, "inputs:sourceColorSpace");
  if (color_space) out->source_color_space = *color_space;

  return true;
}

// ============================================================
// UsdPrimvarReader Implementation
// ============================================================

bool IsPrimvarReader(const UsdPrim& shader) {
  if (!IsShader(shader)) return false;
  std::string id = GetShaderId(shader);
  return id.size() >= 16 && id.substr(0, 16) == "UsdPrimvarReader";
}

namespace {

std::string TokenishValue(const Value* v) {
  if (!v) return "";
  if (const std::string* tok = v->as_token()) return *tok;
  if (const std::string* str = v->as_string()) return *str;
  return "";
}

}  // namespace

std::string GetPrimvarReaderVarname(const UsdPrim& shader) {
  if (!IsPrimvarReader(shader)) return "";
  return TokenishValue(shader.GetPropertyValue("inputs:varname"));
}

std::string GetPrimvarReaderVarname(const Stage& stage,
                                    const UsdPrim& shader) {
  if (!IsPrimvarReader(shader)) return "";

  // Authored value wins; otherwise follow inputs:varname connections
  // (usdMtlx/Apple flattens author e.g.
  // `token inputs:varname.connect = </Mat.inputs:frame:stPrimvarName>`).
  std::string value = TokenishValue(shader.GetPropertyValue("inputs:varname"));
  if (!value.empty()) return value;

  const ::lightusd::next::PrimSpec* spec = shader.GetPrimSpec();
  const std::vector<Path>* conns =
      spec ? spec->connection("inputs:varname") : nullptr;
  for (int hop = 0; conns && !conns->empty() && hop < 4; ++hop) {
    const std::string target = (*conns)[0].str();
    const size_t dot = target.rfind('.');
    if (dot == std::string::npos) break;
    const std::string prim_path = target.substr(0, dot);
    const std::string prop_name = target.substr(dot + 1);
    UsdPrim src = stage.GetPrimAtPath(prim_path);
    if (!src.IsValid()) break;
    value = TokenishValue(src.GetPropertyValue(prop_name));
    if (!value.empty()) return value;
    const ::lightusd::next::PrimSpec* src_spec = src.GetPrimSpec();
    conns = src_spec ? src_spec->connection(prop_name) : nullptr;
  }
  return "";
}

}  // namespace next
}  // namespace lightusd
