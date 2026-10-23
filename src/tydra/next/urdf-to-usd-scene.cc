// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 - Present Light Transport Entertainment Inc.
#include "urdf-to-usd-internal.hh"
#include "next/layer/layer.hh"
#include "next/layer/prim-spec.hh"
#include "next/types/value.hh"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace lightusd { namespace tydra { namespace next {
namespace urdf_detail {

Matrix4 LightTransformMatrix(const Matrix4 &world,
                             const std::array<double, 3> &local_dir) {
  double wd[3];
  for (int j = 0; j < 3; j++) {
    wd[j] = local_dir[0] * world[0 * 4 + j] + local_dir[1] * world[1 * 4 + j] +
            local_dir[2] * world[2 * 4 + j];
  }
  auto norm = [](double v[3]) {
    double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-12) { v[0] /= n; v[1] /= n; v[2] /= n; }
  };
  norm(wd);
  // USD light emits along -Z, so the local Z axis (world) = -dir.
  double z[3] = {-wd[0], -wd[1], -wd[2]};
  // Pick an up reference not parallel to z.
  double up[3] = {0, 0, 1};
  if (std::fabs(z[2]) > 0.95) { up[0] = 0; up[1] = 1; up[2] = 0; }
  double x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2],
                 up[0] * z[1] - up[1] * z[0]};
  norm(x);
  double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2],
                 z[0] * x[1] - z[1] * x[0]};
  Matrix4 m = IdentityMatrix();
  for (int j = 0; j < 3; j++) {
    m[0 * 4 + j] = x[j];
    m[1 * 4 + j] = y[j];
    m[2 * 4 + j] = z[j];
    m[3 * 4 + j] = world[3 * 4 + j];  // world translation
  }
  return m;
}

void AddLightFromJson(Scope *scope, std::set<std::string> *used,
                      const Json &l, size_t index) {
  const std::string name = Unique(
      JsonString(l, "name", "light_" + std::to_string(index)), "light", used);
  const std::string type = JsonString(l, "type", "spot");
  std::array<double, 3> dir{0, 0, -1};
  const std::vector<double> d = JsonDoubles(l, "dir");
  if (d.size() >= 3) dir = {d[0], d[1], d[2]};
  const Matrix4 xform =
      LightTransformMatrix(MatrixFromArray(JsonDoubles(l, "matrix")), dir);
  std::vector<float> color = JsonFloats(l, "color");
  if (color.size() < 3) color = {1.0f, 1.0f, 1.0f};
  bool castshadow = true;
  ReadBool(l, "castshadow", &castshadow);

  tn::PrimSpec *prim =
      scope->Add(name, type == "directional" ? "DistantLight" : "SphereLight");
  if (!prim) return;
  if (type != "directional") {
    Set(prim, "inputs:radius", tn::Value(0.02f), "float");  // near-point
  }
  Set(prim, "inputs:color", tn::Value::MakeColor3f(color[0], color[1], color[2]),
      "color3f");
  Set(prim, "inputs:shadow:enable", tn::Value(castshadow), "bool");
  if (type == "spot") {
    double cutoff = 45.0;
    ReadNumber(l, "cutoff", &cutoff);
    Set(prim, "inputs:shaping:cone:angle",
        tn::Value(static_cast<float>(cutoff)), "float");
    double exponent = 0.0;
    if (ReadNumber(l, "exponent", &exponent))
      Set(prim, "inputs:shaping:focus", tn::Value(static_cast<float>(exponent)), "float");
  }
  double intensity = 0.0;
  if (ReadNumber(l, "intensity", &intensity) && intensity > 0.0)
    Set(prim, "inputs:intensity", tn::Value(static_cast<float>(intensity)), "float");
  SetTransform(prim, xform);
}

void AddCameraFromJson(Scope *scope, std::set<std::string> *used,
                       const Json &c, size_t index) {
  const std::string name = Unique(
      JsonString(c, "name", "camera_" + std::to_string(index)), "camera", used);
  tn::PrimSpec *prim = scope->Add(name, "Camera");
  if (!prim) return;
  SetTransform(prim, MatrixFromArray(JsonDoubles(c, "matrix")));
  double fovy = 45.0;
  if (ReadNumber(c, "fovy", &fovy) && fovy > 0.0 && fovy < 180.0) {
    // verticalAperture = 2 * focalLength * tan(fovy/2), square sensor.
    const double f = 50.0;
    const double va =
        2.0 * f * std::tan(fovy * 0.5 * 3.14159265358979323846 / 180.0);
    Set(prim, "focalLength", tn::Value(static_cast<float>(f)), "float");
    Set(prim, "verticalAperture", tn::Value(static_cast<float>(va)), "float");
    Set(prim, "horizontalAperture", tn::Value(static_cast<float>(va)), "float");
  }
  bool ortho = false;
  if (ReadBool(c, "orthographic", &ortho) && ortho) {
    SetToken(prim, "projection", "orthographic");
  }
  const std::vector<float> clip = JsonFloats(c, "clippingRange");
  if (clip.size() == 2 && clip[0] > 0.0f && clip[1] > clip[0])
    Set(prim, "clippingRange", tn::Value::MakeFloat2(clip[0], clip[1]), "float2");
}

void AddMaterialFromJson(Scope *scope, std::set<std::string> *used,
                         const Json &m, size_t index) {
  const std::string name = Sanitize(
      JsonString(m, "name", "material_" + std::to_string(index)), "material");
  if (!used->insert(name).second) return;  // MJCF names are unique
  const std::string mat_path = "/World/Materials/" + name;
  tn::PrimSpec *mat = scope->Add(name, "Material");
  if (!mat) return;
  Connect(mat, "outputs:surface", tn::TypeId::Token, "token",
          mat_path + "/PreviewSurface.outputs:surface");

  const std::vector<float> rgba = JsonFloats(m, "rgba");
  const std::string tex_file = JsonString(m, "texture");
  tn::PrimSpec *ps = scope->Add(name + "/PreviewSurface", "Shader");
  if (!ps) return;
  SetToken(ps, "info:id", "UsdPreviewSurface", true);
  float base[3] = {0.8f, 0.8f, 0.8f};
  if (rgba.size() >= 3) {
    base[0] = rgba[0];
    base[1] = rgba[1];
    base[2] = rgba[2];
  }
  if (!tex_file.empty()) {
    Connect(ps, "inputs:diffuseColor", tn::TypeId::Color3f, "color3f",
            mat_path + "/DiffuseTexture.outputs:rgb");
  } else if (rgba.size() >= 3) {
    Set(ps, "inputs:diffuseColor",
        tn::Value::MakeColor3f(base[0], base[1], base[2]), "color3f");
  }
  if (rgba.size() >= 4) Set(ps, "inputs:opacity", tn::Value(rgba[3]), "float");
  for (const char *key : {"metallic", "roughness"}) {
    double v = 0.0;
    if (ReadNumber(m, key, &v)) {
      Set(ps, std::string("inputs:") + key, tn::Value(static_cast<float>(v)),
          "float");
    }
  }
  double emission = 0.0;
  if (ReadNumber(m, "emission", &emission) && emission > 0.0) {
    const float e = static_cast<float>(emission);
    Set(ps, "inputs:emissiveColor",
        tn::Value::MakeColor3f(base[0] * e, base[1] * e, base[2] * e),
        "color3f");
  }
  Declare(ps, "outputs:surface", tn::TypeId::Token, "token");
  for (const char* key : {"specularColor", "emissiveColor"}) {
    auto rgb = JsonFloats(m, key);
    if (rgb.size() == 3)
      Set(ps, std::string("inputs:") + key,
          tn::Value::MakeColor3f(rgb[0], rgb[1], rgb[2]), "color3f");
  }
  if (m.contains("useSpecularWorkflow"))
    Set(ps, "inputs:useSpecularWorkflow",
        tn::Value(JsonInt(m, "useSpecularWorkflow", 0)), "int");
  double ior = 1.5;
  if (ReadNumber(m, "ior", &ior))
    Set(ps, "inputs:ior", tn::Value(static_cast<float>(ior)), "float");
  if (const Json* maps = JsonArray(m, "maps")) {
    for (size_t k = 0; k < maps->size(); k++) {
      const Json& map = (*maps)[k];
      const std::string input = JsonString(map, "input"),
                        file = JsonString(map, "file");
      if (input.empty() || file.empty()) continue;
      const bool vector_input = input == "diffuseColor" ||
                                input == "specularColor" ||
                                input == "emissiveColor" || input == "normal";
      bool color = false, clamp = false;
      ReadBool(map, "color", &color);
      ReadBool(map, "clamp", &clamp);
      const std::string stem = name + "/Map_" + std::to_string(k),
                        path = mat_path + "/Map_" + std::to_string(k);
      auto* uv = scope->Add(stem + "_UV", "Shader");
      SetToken(uv, "info:id", "UsdPrimvarReader_float2", true);
      Set(uv, "inputs:varname", tn::Value(std::string("st")), "string");
      Declare(uv, "outputs:result", tn::TypeId::Float2, "float2");
      auto* transform = scope->Add(stem + "_Transform", "Shader");
      SetToken(transform, "info:id", "UsdTransform2d", true);
      Connect(transform, "inputs:in", tn::TypeId::Float2, "float2",
              path + "_UV.outputs:result");
      auto scale = JsonFloats(map, "scale"), offset = JsonFloats(map, "offset");
      if (scale.size() == 2)
        Set(transform, "inputs:scale",
            tn::Value::MakeFloat2(scale[0], scale[1]), "float2");
      if (offset.size() == 2)
        Set(transform, "inputs:translation",
            tn::Value::MakeFloat2(offset[0], offset[1]), "float2");
      Declare(transform, "outputs:result", tn::TypeId::Float2, "float2");
      auto* texture = scope->Add(stem, "Shader");
      SetToken(texture, "info:id", "UsdUVTexture", true);
      Set(texture, "inputs:file", tn::Value::MakeAssetPath(file), "asset");
      Connect(texture, "inputs:st", tn::TypeId::Float2, "float2",
              path + "_Transform.outputs:result");
      SetToken(texture, "inputs:wrapS", clamp ? "clamp" : "repeat");
      SetToken(texture, "inputs:wrapT", clamp ? "clamp" : "repeat");
      SetToken(texture, "inputs:sourceColorSpace", color ? "sRGB" : "raw");
      if (input == "normal") {
        Set(texture, "inputs:scale", tn::Value::MakeFloat4(2, 2, 2, 1),
            "float4");
        Set(texture, "inputs:bias", tn::Value::MakeFloat4(-1, -1, -1, 0),
            "float4");
      }
      Declare(texture, "outputs:rgb", tn::TypeId::Float3, "float3");
      Declare(texture, "outputs:r", tn::TypeId::Float, "float");
      Connect(
          ps, "inputs:" + input,
          input == "normal"
              ? tn::TypeId::Normal3f
              : (vector_input ? tn::TypeId::Color3f : tn::TypeId::Float),
          input == "normal" ? "normal3f" : (vector_input ? "color3f" : "float"),
          path + (vector_input ? ".outputs:rgb" : ".outputs:r"));
    }
  }

  if (tex_file.empty()) return;

  tn::PrimSpec *reader = scope->Add(name + "/stReader", "Shader");
  if (!reader) return;
  SetToken(reader, "info:id", "UsdPrimvarReader_float2", true);
  Set(reader, "inputs:varname", tn::Value(std::string("st")), "string");
  Declare(reader, "outputs:result", tn::TypeId::Float2, "float2");

  // MJCF texrepeat tiles the texture: st scaled through a UsdTransform2d.
  std::string st_source = mat_path + "/stReader.outputs:result";
  const std::vector<float> repeat = JsonFloats(m, "texrepeat");
  if (repeat.size() == 2) {
    tn::PrimSpec *transform = scope->Add(name + "/stTransform", "Shader");
    if (!transform) return;
    SetToken(transform, "info:id", "UsdTransform2d", true);
    Connect(transform, "inputs:in", tn::TypeId::Float2, "float2", st_source);
    Set(transform, "inputs:scale", tn::Value::MakeFloat2(repeat[0], repeat[1]), "float2");
    Declare(transform, "outputs:result", tn::TypeId::Float2, "float2");
    st_source = mat_path + "/stTransform.outputs:result";
  }

  tn::PrimSpec *tex = scope->Add(name + "/DiffuseTexture", "Shader");
  if (!tex) return;
  SetToken(tex, "info:id", "UsdUVTexture", true);
  Set(tex, "inputs:file", tn::Value::MakeAssetPath(tex_file), "asset");
  // UsdUVTexture declares inputs:st as float2 (texCoord2f fails OpenUSD's
  // MismatchedPropertyType validator).
  Connect(tex, "inputs:st", tn::TypeId::Float2, "float2", st_source);
  SetToken(tex, "inputs:wrapS", "repeat");
  SetToken(tex, "inputs:wrapT", "repeat");
  SetToken(tex, "inputs:sourceColorSpace", "sRGB");
  Declare(tex, "outputs:rgb", tn::TypeId::Float3, "float3");
}

}  // namespace urdf_detail

} } }  // namespace lightusd::tydra::next
