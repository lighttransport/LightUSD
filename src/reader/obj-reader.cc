// SPDX-License-Identifier: Apache-2.0
#include "obj-reader.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
namespace lightusd {
namespace {
namespace tn = ::lightusd;
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
// Subdivision tags (tinyobjloader subd-obj.md) -> UsdGeomMesh. Tag values use
// OpenSubdiv's OBJ numbering (interpolateboundary 1 = edge and corner, 2 = edge
// only). A mesh without accepted tags stays a polygon mesh ("none").
void AuthorSubd(PrimSpec* p, const tydra::import_asset::Mesh::Subd& s) {
  if (!s.subd) {
    SetToken(p, "subdivisionScheme", "none", true);
    return;
  }
  static const char* const kScheme[] = {"catmullClark", "loop", "bilinear"};
  static const char* const kBoundary[] = {"none", "edgeAndCorner", "edgeOnly"};
  static const char* const kFvar[] = {"none", "cornersOnly", "cornersPlus1",
                                      "cornersPlus2", "boundaries", "all"};
  static const char* const kTri[] = {"catmullClark", "smooth"};
  SetToken(p, "subdivisionScheme", kScheme[s.scheme < 0 ? 0 : s.scheme], true);
  // Absent tags take OpenSubdiv's OBJ-reader defaults (edge only, all), which
  // differ from USD's (edgeAndCorner, cornersPlus1): always author them.
  SetToken(p, "interpolateBoundary", kBoundary[s.interp_boundary < 0 ? 2 : s.interp_boundary]);
  SetToken(p, "faceVaryingLinearInterpolation",
           kFvar[s.fvar_interp_boundary < 0 ? 5 : s.fvar_interp_boundary]);
  if (s.smooth_triangles >= 0)
    SetToken(p, "triangleSubdivisionRule", kTri[s.smooth_triangles]);
  if (!s.crease_indices.empty()) {
    Set(p, "creaseIndices", Value::MakeIntArray(s.crease_indices), "int[]");
    Set(p, "creaseLengths",
        Value::MakeIntArray(std::vector<int32_t>(s.crease_sharpnesses.size(), 2)), "int[]");
    Set(p, "creaseSharpnesses", Value::MakeFloatArray(s.crease_sharpnesses), "float[]");
  }
  if (!s.corner_indices.empty()) {
    Set(p, "cornerIndices", Value::MakeIntArray(s.corner_indices), "int[]");
    Set(p, "cornerSharpnesses", Value::MakeFloatArray(s.corner_sharpnesses), "float[]");
  }
  if (!s.hole_indices.empty())
    Set(p, "holeIndices", Value::MakeIntArray(s.hole_indices), "int[]");
  // UsdGeomMesh has no creasing-method attribute (an OpenSubdiv option) and no
  // place for unknown tags: keep them as custom attributes.
  if (s.crease_method >= 0) {
    p->upsert_property("obj:subd:creaseMethod",
                       Value::MakeToken(s.crease_method == 1 ? "chaikin" : "uniform"),
                       PropSlot::kFlagCustom);
    p->set_property_type_name("obj:subd:creaseMethod", "token");
  }
  if (!s.unknown.empty()) {
    p->upsert_property("obj:subd:unknownTags",
                       Value::MakeStringLikeArray(s.unknown, TypeId::String),
                       PropSlot::kFlagCustom);
    p->set_property_type_name("obj:subd:unknownTags", "string[]");
  }
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
// MaterialX value type -> (USD type name, TypeId, value). False if the type
// has no scalar/vector USD mapping here (arrays, matrices).
bool MtlxValue(const Json& in, std::string* usd_type, tn::TypeId* id,
               tn::Value* value) {
  const std::string ty = JsonString(in, "type");
  const Json& v = in["value"];
  auto f = [&](size_t i) {
    return v.is_array() && i < v.size() ? v[i].get<float>() : 0.0f;
  };
  const size_t n = v.is_array() ? v.size() : 0;
  if (ty == "float" && n == 1) {
    *usd_type = "float"; *id = tn::TypeId::Float; *value = tn::Value(f(0));
  } else if (ty == "integer" && n == 1) {
    *usd_type = "int"; *id = tn::TypeId::Int;
    *value = tn::Value(static_cast<int32_t>(v[0].get<double>()));
  } else if (ty == "boolean" && v.is_boolean()) {
    *usd_type = "bool"; *id = tn::TypeId::Bool; *value = tn::Value(v.get<bool>());
  } else if (ty == "color3" && n == 3) {
    *usd_type = "color3f"; *id = tn::TypeId::Color3f;
    *value = tn::Value::MakeColor3f(f(0), f(1), f(2));
  } else if (ty == "color4" && n == 4) {
    *usd_type = "color4f"; *id = tn::TypeId::Color4f;
    *value = tn::Value::MakeColor4f(f(0), f(1), f(2), f(3));
  } else if (ty == "vector2" && n == 2) {
    *usd_type = "float2"; *id = tn::TypeId::Float2;
    *value = tn::Value::MakeFloat2(f(0), f(1));
  } else if (ty == "vector3" && n == 3) {
    *usd_type = "float3"; *id = tn::TypeId::Float3;
    *value = tn::Value::MakeFloat3(f(0), f(1), f(2));
  } else if (ty == "vector4" && n == 4) {
    *usd_type = "float4"; *id = tn::TypeId::Float4;
    *value = tn::Value::MakeFloat4(f(0), f(1), f(2), f(3));
  } else if (ty == "string" && v.is_string()) {
    *usd_type = "string"; *id = tn::TypeId::String;
    *value = tn::Value(v.get<std::string>());
  } else if (ty == "filename" && v.is_string()) {
    *usd_type = "asset"; *id = tn::TypeId::AssetPath;
    *value = tn::Value::MakeAssetPath(v.get<std::string>());
  } else {
    return false;
  }
  return true;
}

std::string MtlxUsdType(const std::string& ty, tn::TypeId* id) {
  static const struct { const char* mx; const char* usd; tn::TypeId id; } k[] = {
      {"float", "float", tn::TypeId::Float},       {"integer", "int", tn::TypeId::Int},
      {"boolean", "bool", tn::TypeId::Bool},       {"color3", "color3f", tn::TypeId::Color3f},
      {"color4", "color4f", tn::TypeId::Color4f},  {"vector2", "float2", tn::TypeId::Float2},
      {"vector3", "float3", tn::TypeId::Float3},   {"vector4", "float4", tn::TypeId::Float4},
      {"string", "string", tn::TypeId::String},    {"filename", "asset", tn::TypeId::AssetPath}};
  for (const auto& e : k)
    if (ty == e.mx) { *id = e.id; return e.usd; }
  *id = tn::TypeId::Token;
  return {};
}

// UsdShade MaterialX network from the MaterialX-in-MTL graph built by the OBJ

// MaterialX stdlib nodedefs (generated table shared with lightrt/mtlx-dom).
struct StdNodeDef { const char *name, *node, *out; int first, count; };
struct StdInput { const char *name, *type; };
static const StdNodeDef kStdNodeDefs[] = {
#define MTLX_STD_NODEDEF(nm, node, out, first, count) {nm, node, out, first, count},
#define MTLX_STD_INPUT(nm, ty, hd, dv, gp)
#include "../external/lightrt/mtlxrender/mtlx_stdlib_defs.inc"
#undef MTLX_STD_INPUT
#undef MTLX_STD_NODEDEF
};
static const StdInput kStdInputs[] = {
#define MTLX_STD_NODEDEF(nm, node, out, first, count)
#define MTLX_STD_INPUT(nm, ty, hd, dv, gp) {nm, ty},
#include "../external/lightrt/mtlxrender/mtlx_stdlib_defs.inc"
#undef MTLX_STD_INPUT
#undef MTLX_STD_NODEDEF
};

// Nodedef id for a node: the stdlib definition of `category` with output
// `type` whose inputs agree with the authored input types (first in library
// order on ties, e.g. ND_multiply_color3 over ND_multiply_color3FA unless
// `in2` is a float). Falls back to ND_<category>_<type> for nodes outside
// the stdlib.
std::string MtlxNodeDefId(const std::string& category, const std::string& type,
                          const Json& inputs) {
  const StdNodeDef* best = nullptr;
  for (const StdNodeDef& d : kStdNodeDefs) {
    if (category != d.node || type != d.out) continue;
    bool ok = true;
    for (const auto& in : inputs) {
      const std::string n = JsonString(in, "name"), t = JsonString(in, "type");
      for (int k = 0; k < d.count; ++k) {
        const StdInput& si = kStdInputs[d.first + k];
        if (n == si.name) { ok = t == si.type; break; }
      }
      if (!ok) break;
    }
    if (ok) { best = &d; break; }
    if (!best) best = &d;  // type-mismatched candidate beats a guessed id
  }
  return best ? best->name : "ND_" + category + "_" + type;
}

// importer: <Material>/MtlxSurface (ND_<category>_surfaceshader) fed by
// <Material>/NodeGraphs (ND_<category>_<type> shaders + o_<node> outputs).
// Defining a prim may grow the layer's prim storage, so PrimSpec pointers are
// re-acquired by path (scope->Add on an existing path returns it) after every
// definition instead of being held across them.
void AddMtlxNetwork(Scope* scope, const std::string& name,
                    const std::string& mat_path, const Json& x) {
  if (tn::PrimSpec* mat = scope->Add(name, "Material")) {
    API(mat, "MaterialXConfigAPI");
    Set(mat, "config:mtlx:version", tn::Value(JsonString(x, "version", "1.39")), "string");
  }
  const Json& surf = x["surface"];
  const std::string ng_path = mat_path + "/NodeGraphs";
  auto author_inputs = [&](tn::PrimSpec* p, const Json& inputs, bool into_surface) {
    for (const auto& in : inputs) {
      const std::string prop = "inputs:" + JsonString(in, "name");
      tn::TypeId id;
      if (in["node"].is_string()) {
        const std::string src = in["node"].get<std::string>();
        std::string usd = MtlxUsdType(JsonString(in, "type"), &id);
        if (usd.empty()) continue;
        Connect(p, prop, id, usd,
                into_surface ? ng_path + ".outputs:o_" + src
                             : ng_path + "/" + src + ".outputs:out");
        continue;
      }
      std::string usd;
      tn::Value v;
      if (!MtlxValue(in, &usd, &id, &v)) continue;
      Set(p, prop, std::move(v), usd);
      // MaterialX: a filename input without a colorspace is not converted
      // (USD's "auto" would decode 8-bit images as sRGB).
      std::string cs = JsonString(in, "colorSpace");
      if (cs.empty() && JsonString(in, "type") == "filename") cs = "lin_rec709";
      if (!cs.empty()) {
        auto& meta = p->ensure_property_meta(prop);
        meta.colorSpace = cs;
        meta.authored |= tn::PropMeta::kColorSpace;
      }
    }
  };
  if (x["nodes"].is_array() && x["nodes"].size()) {
    if (!scope->Add(name + "/NodeGraphs", "NodeGraph")) return;
    for (const auto& nd : x["nodes"]) {
      const std::string n = JsonString(nd, "name"), ty = JsonString(nd, "type");
      tn::PrimSpec* sh = scope->Add(name + "/NodeGraphs/" + n, "Shader");
      if (!sh) continue;
      SetToken(sh, "info:id", MtlxNodeDefId(JsonString(nd, "category"), ty, nd["inputs"]), true);
      author_inputs(sh, nd["inputs"], false);
      tn::TypeId id;
      const std::string usd = MtlxUsdType(ty, &id);
      if (!usd.empty()) Declare(sh, "outputs:out", id, usd);
    }
    tn::PrimSpec* ng = scope->Add(name + "/NodeGraphs", "NodeGraph");
    for (const auto& in : surf["inputs"])
      if (ng && in["node"].is_string()) {
        const std::string src = in["node"].get<std::string>();
        tn::TypeId id;
        const std::string usd = MtlxUsdType(JsonString(in, "type"), &id);
        if (!usd.empty())
          Connect(ng, "outputs:o_" + src, id, usd, ng_path + "/" + src + ".outputs:out");
      }
  }
  tn::PrimSpec* ss = scope->Add(name + "/MtlxSurface", "Shader");
  if (!ss) return;
  SetToken(ss, "info:id", "ND_" + JsonString(surf, "category") + "_surfaceshader", true);
  author_inputs(ss, surf["inputs"], true);
  Declare(ss, "outputs:surface", tn::TypeId::Token, "token");
  if (tn::PrimSpec* mat = scope->Add(name, "Material"))
    Connect(mat, "outputs:mtlx:surface", tn::TypeId::Token, "token",
            mat_path + "/MtlxSurface.outputs:surface");
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
  // The MaterialX network (if any) is authored first; `mat` is not used after
  // this point because defining prims may move it.
  if (m["mtlx"].is_object()) AddMtlxNetwork(scope, name, mat_path, m["mtlx"]);

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
  for (const auto& material : payload["materials"]) {
    prim_count += 5 + material["maps"].size() * 3;
    // MaterialX-in-MTL: NodeGraphs + MtlxSurface + one Shader per node.
    if (material["mtlx"].is_object())
      prim_count += 2 + material["mtlx"]["nodes"].size();
  }
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
      AuthorSubd(p, g.subd);
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
  tydra::import_asset::MappedAsset asset;
  std::string resolved;
  if (!reader.Map(source, "", &asset, &resolved, err)) return nullptr;
  auto layer = LoadOBJLayerFromMemory(resolved, asset.data(), asset.size(), effective,
                                      warn, err);
  if (layer) session.Succeed();
  return layer;
}
}  // namespace lightusd
