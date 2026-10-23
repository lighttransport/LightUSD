// SPDX-License-Identifier: Apache-2.0
#include "obj-reader.hh"
#include "../crate/crate-data-source.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
namespace lightusd {
namespace next {
namespace {
namespace tn = ::lightusd::next;
using Json = tydra::import_asset::Json;
std::string JsonString(const Json& j, const char* k,
                       const std::string& fallback = {}) {
  return j[k].is_string() ? j[k].get<std::string>() : fallback;
}
std::vector<float> JsonFloats(const Json& j, const char* k) {
  std::vector<float> out;
  for (const auto& v : j[k]) out.push_back(v.get<float>());
  return out;
}
std::vector<int32_t> JsonInts(const Json& j, const char* k) {
  std::vector<int32_t> out;
  for (const auto& v : j[k]) out.push_back(v.get<int32_t>());
  return out;
}
const Json* JsonArray(const Json& j, const char* k) {
  return j[k].is_array() ? &j[k] : nullptr;
}
int32_t JsonInt(const Json& j, const char* k, int32_t fallback) {
  return j[k].is_number() ? j[k].get<int32_t>() : fallback;
}
bool ReadNumber(const Json& j, const char* k, double* out) {
  if (!j[k].is_number()) return false;
  *out = j[k].get<double>();
  return true;
}
bool ReadBool(const Json& j, const char* k, bool* out) {
  if (!j[k].is_boolean()) return false;
  *out = j[k].get<bool>();
  return true;
}
std::string Sanitize(std::string s, const std::string& fallback) {
  for (char& c : s)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
  if (s.empty()) s = fallback;
  if (std::isdigit(static_cast<unsigned char>(s[0]))) s = "_" + s;
  return s;
}
struct UniqueNames {
  std::set<std::string> used;
  std::map<std::string, size_t> next_suffix;
  std::string Get(const std::string& label) {
    const auto base = Sanitize(label, "mesh");
    if (used.insert(base).second) return base;
    auto& next = next_suffix[base];
    if (!next) next = 1;
    std::string name;
    do { name = base + "_" + std::to_string(next++); }
    while (!used.insert(name).second);
    return name;
  }
};

PrimSpec* Define(Layer* layer, const std::string& path,
                 const std::string& type) {
  auto id = layer->define_prim_at_path(path, type);
  return id == UINT32_MAX ? nullptr : layer->prim_mutable(id);
}
void Set(PrimSpec* prim, const std::string& name, Value value,
         const std::string& type, bool uniform = false) {
  prim->upsert_property(name, std::move(value),
                        uniform ? PropSlot::kFlagUniform : 0);
  prim->set_property_type_name(name, type);
}
void SetToken(PrimSpec* p, const std::string& n, const std::string& v,
              bool u = false) {
  Set(p, n, Value::MakeToken(v), "token", u);
}
void Declare(PrimSpec* p, const std::string& n, TypeId t,
             const std::string& type) {
  auto id = GetPropNameTable().intern(n);
  if (!p->property(id)) p->add_property_slot(id, t, 0);
  p->set_property_type_name(n, type);
}
void Connect(PrimSpec* p, const std::string& n, TypeId t,
             const std::string& type, const std::string& target) {
  Declare(p, n, t, type);
  p->set_connection_targets(n, {Path(target)});
  auto& edit = p->ensure_connection_edit(n);
  edit = ArcListOp();
  edit.authored = true;
  edit.is_explicit = true;
}
void API(PrimSpec* p, const std::string& name) {
  p->meta().apiSchemas().push_back(name);
  p->meta().apiSchemasQualifier() = "prepend";
  p->meta().setApiSchemasAuthored();
}
void Interpolation(PrimSpec* p, const std::string& name,
                   const std::string& value, int elements = 1) {
  auto& m = p->ensure_property_meta(name);
  m.interpolation = value;
  m.elementSize = elements;
  m.authored |= PropMeta::kInterpolation | PropMeta::kElementSize;
}
struct Scope {
  Layer* layer;
  PrimSpec* Add(const std::string& name, const std::string& type) {
    return Define(layer, "/World/Materials/" + name, type);
  }
};
void RetainAssets(tydra::import_asset::Result& result, Layer* layer) {
  auto retained = std::make_shared<ImportedAssets>();
  for (const auto& material : result.payload["materials"])
    for (const auto& map : material["maps"]) {
      if (!map["package"].is_string()) continue;
      auto found = result.assets.find(map["package"].get<std::string>());
      if (found != result.assets.end())
        retained->by_identifier[map["file"].get<std::string>()] = found->second;
    }
  if (!retained->by_identifier.empty())
    layer->meta().imported_assets = std::move(retained);
}
void AddMaterialFromJson(Scope* scope, std::set<std::string>* used,
                         const Json& m, size_t index) {
  const std::string name = Sanitize(
      JsonString(m, "name", "material_" + std::to_string(index)), "material");
  if (!used->insert(name).second) return;  // MJCF names are unique
  const std::string mat_path = "/World/Materials/" + name;
  tn::PrimSpec* mat = scope->Add(name, "Material");
  if (!mat) return;
  Connect(mat, "outputs:surface", tn::TypeId::Token, "token",
          mat_path + "/PreviewSurface.outputs:surface");

  const std::vector<float> rgba = JsonFloats(m, "rgba");
  const std::string tex_file = JsonString(m, "texture");
  tn::PrimSpec* ps = scope->Add(name + "/PreviewSurface", "Shader");
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
  for (const char* key : {"metallic", "roughness"}) {
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
  for (const char* key : {"clearcoat", "clearcoatRoughness"}) {
    double v = 0.0;
    if (ReadNumber(m, key, &v))
      Set(ps, std::string("inputs:") + key, tn::Value(static_cast<float>(v)), "float");
  }
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
      std::string color_space = JsonString(map, "colorSpace");
      if (color_space == "linear") color_space = "raw";
      if (color_space != "sRGB" && color_space != "raw") color_space = color ? "sRGB" : "raw";
      SetToken(texture, "inputs:sourceColorSpace", color_space);
      if (input == "normal") {
        // MTL -bm scales the tangent-space XY of the normal map.
        double bm = 1.0;
        ReadNumber(map, "bumpMultiplier", &bm);
        const float s2 = static_cast<float>(2.0 * bm), b1 = static_cast<float>(-bm);
        Set(texture, "inputs:scale", tn::Value::MakeFloat4(s2, s2, 2, 1), "float4");
        Set(texture, "inputs:bias", tn::Value::MakeFloat4(b1, b1, -1, 0), "float4");
      }
      // -imfchan r|g|b|m|l|z: scalar maps read the selected channel (m is
      // the matte/alpha channel; luminance and depth fall back to red).
      const std::string channel = JsonString(map, "channel");
      const std::string output = channel == "g" ? "g" : channel == "b" ? "b"
                                 : channel == "m" ? "a" : "r";
      Declare(texture, "outputs:rgb", tn::TypeId::Float3, "float3");
      Declare(texture, "outputs:" + output, tn::TypeId::Float, "float");
      Connect(
          ps, "inputs:" + input,
          input == "normal"
              ? tn::TypeId::Normal3f
              : (vector_input ? tn::TypeId::Color3f : tn::TypeId::Float),
          input == "normal" ? "normal3f" : (vector_input ? "color3f" : "float"),
          path + (vector_input ? ".outputs:rgb" : ".outputs:" + output));
    }
  }

  if (tex_file.empty()) return;

  tn::PrimSpec* reader = scope->Add(name + "/stReader", "Shader");
  if (!reader) return;
  SetToken(reader, "info:id", "UsdPrimvarReader_float2", true);
  Set(reader, "inputs:varname", tn::Value(std::string("st")), "string");
  Declare(reader, "outputs:result", tn::TypeId::Float2, "float2");

  tn::PrimSpec* tex = scope->Add(name + "/DiffuseTexture", "Shader");
  if (!tex) return;
  SetToken(tex, "info:id", "UsdUVTexture", true);
  Set(tex, "inputs:file", tn::Value::MakeAssetPath(tex_file), "asset");
  // UsdUVTexture declares inputs:st as float2 (texCoord2f fails OpenUSD's
  // MismatchedPropertyType validator).
  Connect(tex, "inputs:st", tn::TypeId::Float2, "float2",
          mat_path + "/stReader.outputs:result");
  SetToken(tex, "inputs:wrapS", "repeat");
  SetToken(tex, "inputs:wrapT", "repeat");
  SetToken(tex, "inputs:sourceColorSpace", "sRGB");
  Declare(tex, "outputs:rgb", tn::TypeId::Float3, "float3");
}

}  // namespace
static bool ConvertOBJSceneToLayer(tydra::import_asset::OBJ& obj,
    const Json& payload, Layer* layer, std::string* err) {
  if (!layer) {
    if (err) *err = "Null OBJ layer";
    return false;
  }
  // PrimSpec pointers remain live while their shaders/subsets are emitted.
  size_t prim_count = 5;
  for (const auto& material : payload["materials"])
    prim_count += 5 + material["maps"].size() * 3;
  for (const auto& mesh : obj.meshes) {
    std::set<int32_t> bindings;
    for (int32_t id : mesh.materials)
      if (id >= 0 && size_t(id) < payload["materials"].size()) bindings.insert(id);
    prim_count += 1 + bindings.size();
  }
  if (prim_count >= UINT32_MAX) {
    if (err) *err = "OBJ prim count limit exceeded";
    return false;
  }
  layer->reserve(prim_count);
  layer->meta().defaultPrim = "World";
  layer->meta().upAxis = JsonString(payload, "upAxis", "Z");
  layer->meta().upAxis_set = true;
  if (payload["unsupported"].is_array() && !payload["unsupported"].empty()) {
    layer->meta().customLayerData = Value::MakeDictionary();
    layer->meta().customLayerData.as_dictionary()->set("lightusdImportUnsupported",
        Value(payload["unsupported"].dump()));
    layer->meta().customLayerData_set = true;
  }
  PrimSpec* world = Define(layer, "/World", "Xform");
  // Non-legacy profiles state the units explicitly (OBJ carries none; USD's
  // fallback would be centimeters) and mark the asset root as a component.
  if (JsonString(payload, "profile", "legacy") != "legacy") {
    layer->meta().metersPerUnit = 1.0;
    layer->meta().metersPerUnit_set = true;
    if (world) world->meta().kind() = "component";
  }
  Define(layer, "/World/Links", "Xform");
  Define(layer, "/World/Links/Geometry", "Xform");
  Scope scope{layer};
  UniqueNames materials, names;
  std::set<std::string> used_materials;
  std::vector<std::string> material_paths;
  size_t mi = 0;
  for (const auto& input : payload["materials"]) {
    Json material = input;
    auto name = materials.Get(JsonString(material, "name", "material"));
    material["name"] = name;
    material_paths.push_back("/World/Materials/" + name);
    AddMaterialFromJson(&scope, &used_materials, material, mi++);
  }
  for (auto& g : obj.meshes) {
    auto& positions = g.positions;
    const size_t num_points = positions.size() / 3;
    const size_t corners = g.indices.size();
    if (positions.empty() || positions.size() % 3) {
      if (err) *err = "Invalid OBJ points";
      return false;
    }
    const auto& type = g.type;
    const auto& label = g.name;
    auto path = "/World/Links/Geometry/" + names.Get(label);
    auto* p = Define(layer, path, type);
    if (!p) {
      if (err) *err = "Cannot define OBJ prim";
      return false;
    }
    p->meta().displayName() = label;
    Set(p, "points", Value::MakeFloatCompArray(positions, TypeId::Point3f, 3),
        "point3f[]");
    std::vector<float> extent(6);
    for (size_t c = 0; c < 3; ++c) extent[c] = extent[c + 3] = positions[c];
    for (size_t i = 3; i < positions.size(); ++i) {
      extent[i % 3] = std::min(extent[i % 3], positions[i]);
      extent[3 + i % 3] = std::max(extent[3 + i % 3], positions[i]);
    }
    Set(p, "extent",
        Value::MakeFloatCompArray(std::move(extent), TypeId::Float3, 3),
        "float3[]");
    if (type == "Mesh") {
      SetToken(p, "subdivisionScheme", "none", true);
      Set(p, "faceVertexCounts",
          Value::MakeIntArray(g.counts), "int[]");
      Set(p, "faceVertexIndices", Value::MakeIntArray(g.indices),
          "int[]");
    } else if (type == "BasisCurves") {
      SetToken(p, "type", "linear", true);
      SetToken(p, "wrap", "nonperiodic", true);
      Set(p, "curveVertexCounts",
          Value::MakeIntArray(g.counts), "int[]");
    }
    // USD owns compact numeric buffers; no JSON value per coordinate.
    std::vector<float>().swap(g.positions);
    std::vector<int32_t>().swap(g.counts);
    std::vector<int32_t>().swap(g.indices);
    std::vector<int32_t>().swap(g.source_vertices);
    // Compact topology: indexed face-varying normals and UVs (primvars:normals
    // overrides the unindexable normals attribute).
    for (bool uv : {false, true}) {
      auto& table = uv ? g.uvs : g.normals;
      auto& ids = uv ? g.uv_indices : g.normal_indices;
      if (ids.empty()) continue;
      const size_t width = uv ? 2 : 3;
      bool valid = ids.size() == corners && table.size() % width == 0;
      for (int32_t id : ids) valid &= id >= 0 && size_t(id) < table.size() / width;
      if (!valid) {
        if (err) *err = "Invalid OBJ indexed primvar";
        return false;
      }
      const char* name = uv ? "primvars:st" : "primvars:normals";
      Set(p, name,
          Value::MakeFloatCompArray(std::move(table), uv ? TypeId::Texcoord2f : TypeId::Normal3f,
                                    static_cast<uint32_t>(width)),
          uv ? "texCoord2f[]" : "normal3f[]");
      Interpolation(p, name, "faceVarying");
      Set(p, std::string(name) + ":indices", Value::MakeIntArray(std::move(ids)), "int[]");
      std::vector<float>().swap(table);
    }
    for (const char* key : {"normals", "uvs", "colors"}) {
      auto& values = std::string(key) == "uvs" ? g.uvs :
                     std::string(key) == "colors" ? g.colors : g.normals;
      const bool uv = std::string(key) == "uvs",
                 color = std::string(key) == "colors";
      if (values.size() != num_points * (uv ? 2 : 3)) continue;
      const auto name = uv      ? "primvars:st"
                        : color ? "primvars:displayColor"
                                : "normals";
      Set(p, name,
          Value::MakeFloatCompArray(std::move(values),
                                    uv      ? TypeId::Texcoord2f
                                    : color ? TypeId::Color3f
                                            : TypeId::Normal3f,
                                    uv ? 2 : 3),
          uv      ? "texCoord2f[]"
          : color ? "color3f[]"
                  : "normal3f[]");
      Interpolation(p, name, "vertex");
      std::vector<float>().swap(values);
    }
    const int32_t influences = g.influences;
    if (influences > 0) {
      auto& ids = g.joint_indices;
      auto& weights = g.joint_weights;
      if (ids.size() != weights.size() ||
          weights.size() / size_t(influences) != num_points ||
          weights.size() % size_t(influences)) {
        if (err) *err = "Invalid OBJ skin array sizes";
        return false;
      }
      API(p, "SkelBindingAPI");
      Set(p, "primvars:skel:jointIndices",
          Value::MakeIntArray(std::move(ids)), "int[]");
      Set(p, "primvars:skel:jointWeights",
          Value::MakeFloatArray(std::move(weights)), "float[]");
      Interpolation(p, "primvars:skel:jointIndices", "vertex", influences);
      Interpolation(p, "primvars:skel:jointWeights", "vertex", influences);
    }
    std::vector<int32_t>().swap(g.joint_indices);
    std::vector<float>().swap(g.joint_weights);
    const auto& material_ids = g.materials;
    std::map<int32_t, std::vector<int32_t>> faces;
    for (size_t f = 0; f < material_ids.size(); ++f)
      if (material_ids[f] >= 0 &&
          size_t(material_ids[f]) < material_paths.size())
        faces[material_ids[f]].push_back(static_cast<int32_t>(f));
    for (const auto& face : faces) {
      auto* subset =
          Define(layer, path + "/material_" + std::to_string(face.first),
                 "GeomSubset");
      SetToken(subset, "elementType", "face", true);
      SetToken(subset, "familyName", "materialBind", true);
      Set(subset, "indices", Value::MakeIntArray(face.second), "int[]");
      API(subset, "MaterialBindingAPI");
      subset->set_relationship_targets(
          "material:binding", {Path(material_paths[size_t(face.first)])});
    }
    if (!faces.empty())
      SetToken(p, "subsetFamily:materialBind:familyType", "nonOverlapping",
               true);
  }
  return true;
}
// Compatibility bridge for callers already holding the robot JSON contract.
bool ConvertOBJPayloadToLayer(const Json& payload, Layer* layer, std::string* err) {
  tydra::import_asset::OBJ obj;
  for (const auto& link : payload["links"])
    for (const auto& visual : link["visuals"]) {
      const auto& g = visual["geometry"];
      tydra::import_asset::Mesh mesh;
      mesh.name = JsonString(visual, "name", "mesh");
      mesh.type = JsonString(visual, "type", "Mesh");
      mesh.positions = JsonFloats(g, "positions");
      mesh.normals = JsonFloats(g, "normals");
      mesh.uvs = JsonFloats(g, "uvs");
      mesh.colors = JsonFloats(g, "colors");
      mesh.counts = JsonInts(g, "faceVertexCounts");
      mesh.indices = JsonInts(g, "indices");
      mesh.materials = JsonInts(g, "materialIndices");
      mesh.joint_indices = JsonInts(g, "jointIndices");
      mesh.joint_weights = JsonFloats(g, "jointWeights");
      mesh.influences = JsonInt(g, "influences", 0);
      obj.meshes.push_back(std::move(mesh));
    }
  return ConvertOBJSceneToLayer(obj, payload, layer, err);
}

std::shared_ptr<Layer> LoadOBJLayerFromMemory(
    const std::string& source, const uint8_t* data, size_t size,
    const tydra::import_asset::Options& options, std::string* warn,
    std::string* err) {
  auto effective = options;
  tydra::import_asset::ImportSession session(&effective, source, "obj", err);
  if ((!data && size) || size > options.max_input || size > options.max_memory) {
    if (err) *err = !data && size ? "Null OBJ data" : "OBJ input limit exceeded";
    return nullptr;
  }
  tydra::import_asset::Result result;
  tydra::import_asset::OBJ obj;
  if (!tydra::import_asset::BuildOBJScene(
          std::string_view(size ? reinterpret_cast<const char*>(data) : "", size),
          source, effective, &obj, &result, err)) return nullptr;
  auto layer = std::make_shared<Layer>();
  if (!ConvertOBJSceneToLayer(obj, result.payload, layer.get(), err)) return nullptr;
  RetainAssets(result, layer.get());
  if (warn) *warn += result.warnings;
  session.Succeed();
  return layer;
}
std::shared_ptr<Layer> LoadOBJLayerFromFile(
    const std::string& source, const tydra::import_asset::Options& options,
    std::string* warn, std::string* err) {
  auto effective = options;
  tydra::import_asset::ImportSession session(&effective, source, "obj", err);
  tydra::import_asset::Reader reader(effective);
  // A local file without resolver hooks is memory-mapped instead of copied,
  // so a multi-gigabyte OBJ is not duplicated in anonymous memory.
  if (!effective.open && !effective.read) {
    tydra::import_asset::AssetSource asset;
    std::string why;
    if (reader.Open(source, "", &asset, &why)) {
      if (auto mapping = CrateDataSource::MmapFile(asset.identifier)) {
        if (!reader.Account(source, "", asset.identifier, mapping->size(), err)) return nullptr;
        auto layer = LoadOBJLayerFromMemory(asset.identifier, mapping->base(), mapping->size(),
                                            effective, warn, err);
        if (layer) session.Succeed();
        return layer;
      }
    }
  }
  std::vector<uint8_t> bytes;
  std::string resolved;
  if (!reader.Read(source, "", &bytes, &resolved, err)) return nullptr;
  auto layer = LoadOBJLayerFromMemory(resolved, bytes.data(), bytes.size(), effective,
                                      warn, err);
  if (layer) session.Succeed();
  return layer;
}
}  // namespace next
}  // namespace lightusd
