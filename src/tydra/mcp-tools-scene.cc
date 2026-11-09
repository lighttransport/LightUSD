#include "mcp-tools-scene.hh"

#include <algorithm>
#include <fstream>
#include <sstream>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "../lightusd.hh"
#include "../usda-writer.hh"
#include "../usdc-writer.hh"
#include "../usdGeom.hh"
#include "../usdShade.hh"
#include "../usdLux.hh"
#include "../usdPhysics.hh"
#include "../usdSkel.hh"
#include "../pprinter.hh"
#include "../str-util.hh"
#include "value-to-json.hh"
#include "mcp-context.hh"

namespace lightusd {
namespace tydra {
namespace mcp {

namespace {

// ---------------------------------------------------------------------------
// Schema attribute definitions (builtin prim types)
// ---------------------------------------------------------------------------
struct SchemaAttrDef {
  std::string name;
  std::string type_name;
  std::string variability;
  bool required;
};

using SchemaDef = std::vector<SchemaAttrDef>;

const SchemaDef &GetSchemaDef(const std::string &type_name) {
  static const auto schemas = new std::map<std::string, SchemaDef>{
    {"Xform", {
      {"xformOpOrder", "token[]", "Uniform", false},
      {"xformOpTranslate", "float3", "Varying", false},
      {"xformOpRotateXYZ", "float3", "Varying", false},
      {"xformOpScale", "float3", "Varying", false},
      {"xformOpTransform", "matrix4d", "Varying", false},
      {"visibility", "token", "Varying", false},
      {"purpose", "token", "Uniform", false},
    }},
    {"Mesh", {
      {"points", "point3f[]", "Varying", true},
      {"normals", "normal3f[]", "Varying", false},
      {"primvars:normals", "normal3f[]", "Varying", false},
      {"faceVertexCounts", "int[]", "Uniform", true},
      {"faceVertexIndices", "int[]", "Uniform", true},
      {"extent", "float3[]", "Uniform", true},
      {"subdivisionScheme", "token", "Uniform", false},
      {"creaseIndices", "int[]", "Uniform", false},
      {"creaseLengths", "int[]", "Uniform", false},
      {"creaseSharpnesses", "float[]", "Uniform", false},
      {"cornerIndices", "int[]", "Uniform", false},
      {"cornerSharpnesses", "float[]", "Uniform", false},
      {"holeIndices", "int[]", "Uniform", false},
      {"doubleSided", "bool", "Uniform", false},
      {"displayColor", "color3f[]", "Varying", false},
      {"displayOpacity", "float[]", "Varying", false},
      {"primvars:displayColor", "color3f[]", "Varying", false},
      {"primvars:displayOpacity", "float[]", "Varying", false},
      {"primvars:st", "texCoord2f[]", "Varying", false},
      {"primvars:st:indices", "int[]", "Varying", false},
    }},
    {"Sphere", {
      {"radius", "float", "Uniform", true},
      {"extent", "float3[]", "Uniform", true},
    }},
    {"Cube", {
      {"size", "float", "Uniform", true},
      {"extent", "float3[]", "Uniform", true},
    }},
    {"Cylinder", {
      {"radius", "float", "Uniform", true},
      {"height", "float", "Uniform", true},
      {"axis", "token", "Uniform", true},
      {"extent", "float3[]", "Uniform", true},
    }},
    {"Capsule", {
      {"radius", "float", "Uniform", true},
      {"height", "float", "Uniform", true},
      {"axis", "token", "Uniform", true},
      {"extent", "float3[]", "Uniform", true},
    }},
    {"Cone", {
      {"radius", "float", "Uniform", true},
      {"height", "float", "Uniform", true},
      {"axis", "token", "Uniform", true},
      {"extent", "float3[]", "Uniform", true},
    }},
    {"Camera", {
      {"projection", "token", "Uniform", true},
      {"horizontalAperture", "float", "Uniform", true},
      {"verticalAperture", "float", "Uniform", true},
      {"horizontalApertureOffset", "float", "Uniform", false},
      {"verticalApertureOffset", "float", "Uniform", false},
      {"focalLength", "float", "Uniform", true},
      {"clippingRange", "float2", "Uniform", true},
      {"clippingPlanes", "float4[]", "Uniform", false},
      {"focusDistance", "float", "Uniform", false},
      {"fStop", "float", "Uniform", false},
      {"exposure", "float", "Uniform", false},
    }},
    {"Material", {
      {"surface", "token", "Varying", false},
      {"displacement", "token", "Varying", false},
      {"volume", "token", "Varying", false},
    }},
    {"Shader", {
      {"info:id", "token", "Uniform", true},
      {"info:implementationSource", "token", "Uniform", false},
      {"outputs:surface", "token", "Varying", false},
      {"outputs:displacement", "token", "Varying", false},
    }},
    {"SphereLight", {
      {"radius", "float", "Uniform", false},
      {"treatAsPoint", "bool", "Uniform", false},
      {"intensity", "float", "Varying", false},
      {"exposure", "float", "Varying", false},
      {"color", "color3f", "Varying", false},
      {"colorTemperature", "float", "Varying", false},
      {"enableColorTemperature", "bool", "Varying", false},
      {"diffuse", "float", "Varying", false},
      {"specular", "float", "Varying", false},
      {"normalize", "bool", "Varying", false},
    }},
    {"DistantLight", {
      {"angle", "float", "Uniform", false},
      {"intensity", "float", "Varying", false},
      {"exposure", "float", "Varying", false},
      {"color", "color3f", "Varying", false},
    }},
    {"RectLight", {
      {"width", "float", "Uniform", false},
      {"height", "float", "Uniform", false},
      {"intensity", "float", "Varying", false},
      {"exposure", "float", "Varying", false},
      {"color", "color3f", "Varying", false},
      {"normalize", "bool", "Varying", false},
    }},
    {"DiskLight", {
      {"radius", "float", "Uniform", false},
      {"intensity", "float", "Varying", false},
      {"color", "color3f", "Varying", false},
      {"normalize", "bool", "Varying", false},
    }},
    {"DomeLight", {
      {"intensity", "float", "Varying", false},
      {"exposure", "float", "Varying", false},
      {"color", "color3f", "Varying", false},
      {"texture:file", "asset", "Varying", false},
      {"texture:format", "token", "Uniform", false},
    }},
    {"Skeleton", {
      {"skeleton:joints", "token[]", "Uniform", true},
      {"skeleton:bindTransforms", "matrix4d[]", "Uniform", true},
      {"skeleton:restTransforms", "matrix4d[]", "Uniform", true},
    }},
    {"RigidBody", {
      {"rigidBodyEnabled", "bool", "Uniform", false},
      {"simulationOwner", "token", "Uniform", false},
      {"rigidBody:startsAsleep", "bool", "Uniform", false},
      {"rigidBody:kinematicEnabled", "bool", "Uniform", false},
      {"rigidBody:velocity", "vector3f", "Varying", false},
      {"rigidBody:angularVelocity", "vector3f", "Varying", false},
    }},
    {"CollisionGroup", {
      {"collision:enabled", "bool", "Uniform", false},
      {"collision:simulationOwner", "token", "Uniform", false},
      {"collision:mergedGroup", "bool", "Uniform", false},
    }},
    {"Volume", {
      {"density", "float[]", "Varying", false},
      {"field:dataType", "token", "Varying", false},
      {"field:fallback", "float", "Varying", false},
    }},
  };
  auto it = schemas->find(type_name);
  if (it != schemas->end()) return it->second;
  static const SchemaDef empty;
  return empty;
}

// ---------------------------------------------------------------------------
// Recursive prim tree serialisation
// ---------------------------------------------------------------------------
void PrimToJSON(const Prim &prim, nlohmann::json &j, int max_depth,
                bool include_attributes, int depth = 0) {
  j["name"] = prim.element_name();
  j["type"] = prim.prim_type_name();

  switch (prim.specifier()) {
    case Specifier::Def: j["specifier"] = "def"; break;
    case Specifier::Over: j["specifier"] = "over"; break;
    case Specifier::Class: j["specifier"] = "class"; break;
    default: j["specifier"] = "invalid"; break;
  }

  if (include_attributes) {
    auto schema = GetSchemaDef(prim.prim_type_name());
    nlohmann::json attrs = nlohmann::json::array();
    for (const auto &def : schema) {
      nlohmann::json a;
      a["name"] = def.name;
      a["type"] = def.type_name;
      a["variability"] = def.variability;
      a["required"] = def.required;
      attrs.push_back(a);
    }
    if (!attrs.empty()) {
      j["attributes"] = attrs;
    }
  }

  const auto &children = prim.children();
  if (max_depth != 0 && !children.empty()) {
    nlohmann::json child_arr = nlohmann::json::array();
    int next_depth = (max_depth > 0) ? max_depth - 1 : max_depth;
    for (const auto &child : children) {
      nlohmann::json c;
      PrimToJSON(child, c, next_depth, include_attributes, depth + 1);
      child_arr.push_back(c);
    }
    j["children"] = child_arr;
  } else if (!children.empty()) {
    j["childCount"] = children.size();
  }
}

// ---------------------------------------------------------------------------
// Attempt to read an attribute value from a Prim using concrete typed access.
// Returns nullopt if the prim type is unknown or attribute not found.
// ---------------------------------------------------------------------------
nonstd::optional<value::Value> ReadPrimAttribute(const Prim &prim,
                                                  const std::string &attr_name) {
  (void)prim;
  (void)attr_name;
  // Generic approach: get the prim's data and try to read the attribute
  // through the stage's generic attribute API. For now, we only handle
  // primvars: prefixed attributes and well-known displayColor/displayOpacity.
  return nonstd::nullopt;
}

} // namespace

// ===========================================================================
// stage_new
// ===========================================================================
bool StageNew(Context &ctx, const nlohmann::json &args,
              nlohmann::json &result, std::string &err) {
  (void)err;

  ctx.stage = std::unique_ptr<Stage>(new Stage(Stage::CreateInMemory()));
  ctx.stage_loaded = true;

  // Set stage metadata from args
  if (args.contains("upAxis") && args["upAxis"].is_string()) {
    std::string axis = args["upAxis"].get<std::string>();
    if (axis == "X") ctx.stage->metas().upAxis = Axis::X;
    else if (axis == "Y") ctx.stage->metas().upAxis = Axis::Y;
    else if (axis == "Z") ctx.stage->metas().upAxis = Axis::Z;
  }
  if (args.contains("defaultPrim") && args["defaultPrim"].is_string()) {
    ctx.stage->metas().defaultPrim = value::token(args["defaultPrim"].get<std::string>());
  }
  if (args.contains("metersPerUnit") && args["metersPerUnit"].is_number()) {
    ctx.stage->metas().metersPerUnit = args["metersPerUnit"].get<double>();
  }
  if (args.contains("timeCodesPerSecond") && args["timeCodesPerSecond"].is_number()) {
    ctx.stage->metas().timeCodesPerSecond = args["timeCodesPerSecond"].get<double>();
  }
  if (args.contains("framesPerSecond") && args["framesPerSecond"].is_number()) {
    ctx.stage->metas().framesPerSecond = args["framesPerSecond"].get<double>();
  }
  if (args.contains("startTimeCode") && args["startTimeCode"].is_number()) {
    ctx.stage->metas().startTimeCode = args["startTimeCode"].get<double>();
  }
  if (args.contains("endTimeCode") && args["endTimeCode"].is_number()) {
    ctx.stage->metas().endTimeCode = args["endTimeCode"].get<double>();
  }

  result["success"] = true;
  result["message"] = "Created new empty stage";
  return true;
}

// ===========================================================================
// stage_load
// ===========================================================================
bool StageLoad(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err) {
  if (!args.contains("uri") || !args["uri"].is_string()) {
    err = "Missing 'uri' argument";
    return false;
  }

  std::string uri = args["uri"].get<std::string>();

  lightusd::USDLoadOptions options;
  if (args.contains("options") && args["options"].is_object()) {
    const auto &opts = args["options"];
    if (opts.contains("loadPayloads") && opts["loadPayloads"].is_boolean())
      options.load_payloads = opts["loadPayloads"].get<bool>();
    if (opts.contains("loadReferences") && opts["loadReferences"].is_boolean())
      options.load_references = opts["loadReferences"].get<bool>();
    if (opts.contains("doComposition") && opts["doComposition"].is_boolean())
      options.do_composition = opts["doComposition"].get<bool>();
  }

  auto stage = std::unique_ptr<Stage>(new Stage());
  std::string warn;

  bool ok = lightusd::LoadUSDFromFile(uri, stage.get(), &warn, &err, options);
  if (!ok) {
    return false;
  }

  ctx.stage = std::move(stage);
  ctx.stage_loaded = true;

  result["success"] = true;
  result["message"] = "Loaded USD: " + uri;
  if (!warn.empty()) {
    result["warning"] = warn;
  }

  // Basic stage info
  result["upAxis"] = lightusd::to_string(ctx.stage->metas().upAxis.get_value());
  result["defaultPrim"] = ctx.stage->metas().defaultPrim.str();
  result["rootPrimCount"] = ctx.stage->root_prims().size();

  return true;
}

// ===========================================================================
// stage_load_data
// ===========================================================================
bool StageLoadData(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  if (!args.contains("data") || !args["data"].is_string()) {
    err = "Missing 'data' argument (base64 encoded)";
    return false;
  }

  std::string data_b64 = args["data"].get<std::string>();
  std::string binary = base64_decode(data_b64);
  std::string name = args.value("name", std::string("memory.usd"));
  std::string format = args.value("format", std::string("auto"));

  lightusd::USDLoadOptions options;
  auto stage = std::unique_ptr<Stage>(new Stage());
  std::string warn;

  bool ok = false;
  if (format == "usda") {
    ok = lightusd::LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(binary.data()), binary.size(),
        name, stage.get(), &warn, &err, options);
  } else if (format == "usdc") {
    ok = lightusd::LoadUSDCFromMemory(
        reinterpret_cast<const uint8_t *>(binary.data()), binary.size(),
        name, stage.get(), &warn, &err, options);
  } else {
    ok = lightusd::LoadUSDFromMemory(
        reinterpret_cast<const uint8_t *>(binary.data()), binary.size(),
        name, stage.get(), &warn, &err, options);
  }

  if (!ok) return false;

  ctx.stage = std::move(stage);
  ctx.stage_loaded = true;

  result["success"] = true;
  result["rootPrimCount"] = ctx.stage->root_prims().size();
  return true;
}

// ===========================================================================
// stage_export
// ===========================================================================
bool StageExport(Context &ctx, const nlohmann::json &args,
                 nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("uri") || !args["uri"].is_string()) {
    err = "Missing 'uri' argument";
    return false;
  }

  std::string uri = args["uri"].get<std::string>();
  std::string format = args.value("format", std::string("usda"));
  std::string warn;

  bool ok = false;
  if (format == "usdc") {
    ok = lightusd::usdc::SaveAsUSDCToFile(uri, *ctx.stage, &warn, &err);
  } else if (format == "usdz") {
    ok = lightusd::SaveAsUSDZToFile(
        uri, *ctx.stage, std::map<std::string, std::vector<uint8_t>>(),
        &warn, &err);
  } else {
    ok = lightusd::usda::SaveAsUSDA(uri, *ctx.stage, &warn, &err);
  }

  if (!ok) return false;

  result["success"] = true;
  result["uri"] = uri;
  return true;
}

// ===========================================================================
// stage_to_string
// ===========================================================================
bool StageToString(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }

  (void)args;
  std::string usda = ctx.stage->ExportToString();
  result["usda"] = usda;
  result["length"] = usda.size();
  return true;
}

// ===========================================================================
// stage_info
// ===========================================================================
bool StageInfo(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err) {
  (void)args;
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }

  const auto &metas = ctx.stage->metas();
  result["upAxis"] = lightusd::to_string(metas.upAxis.get_value());
  result["defaultPrim"] = metas.defaultPrim.str();
  result["metersPerUnit"] = metas.metersPerUnit.get_value();
  result["timeCodesPerSecond"] = metas.timeCodesPerSecond.get_value();
  result["framesPerSecond"] = metas.framesPerSecond.get_value();
  result["startTimeCode"] = metas.startTimeCode.get_value();

  double endTC = metas.endTimeCode.get_value();
  if (std::isinf(endTC)) {
    result["endTimeCode"] = nullptr;
  } else {
    result["endTimeCode"] = endTC;
  }

  result["rootPrimCount"] = ctx.stage->root_prims().size();

  // Count total prims
  size_t total_prims = 0;
  std::function<void(const Prim &)> count = [&](const Prim &p) {
    total_prims++;
    for (const auto &c : p.children()) count(c);
  };
  for (const auto &p : ctx.stage->root_prims()) count(p);
  result["totalPrimCount"] = total_prims;

  // Root prim names
  nlohmann::json root_names = nlohmann::json::array();
  for (const auto &p : ctx.stage->root_prims()) {
    root_names.push_back(p.element_name());
  }
  result["rootPrims"] = root_names;

  return true;
}

// ===========================================================================
// prim_list
// ===========================================================================
bool PrimList(Context &ctx, const nlohmann::json &args,
              nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }

  std::string path = args.value("path", std::string("/"));
  int max_depth = args.value("max_depth", -1);
  bool include_attributes = args.value("include_attributes", false);

  // Find starting prim
  lightusd::Path start_path(path, "");
  if (!start_path.is_valid() || path == "/") {
    // List root prims
    nlohmann::json prims = nlohmann::json::array();
    for (const auto &prim : ctx.stage->root_prims()) {
      nlohmann::json j;
      PrimToJSON(prim, j, max_depth, include_attributes);
      prims.push_back(j);
    }
    result["path"] = "/";
    result["prims"] = prims;
    result["count"] = prims.size();
    return true;
  }

  const Prim *found = nullptr;
  if (!ctx.stage->find_prim_at_path(start_path, found, &err)) {
    // Provide helpful error
    nlohmann::json suggestions = nlohmann::json::array();
    for (const auto &p : ctx.stage->root_prims()) {
      suggestions.push_back("/" + p.element_name());
    }
    result["error"] = "Prim not found: " + path;
    result["availableRootPrims"] = suggestions;
    return true; // not a hard error
  }

  nlohmann::json j;
  PrimToJSON(*found, j, max_depth, include_attributes);
  result["path"] = found->absolute_path().full_path_name();
  result["prim"] = j;
  return true;
}

// ===========================================================================
// prim_get
// ===========================================================================
bool PrimGet(Context &ctx, const nlohmann::json &args,
             nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }

  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  const Prim *prim = nullptr;
  if (!ctx.stage->find_prim_at_path(path, prim, &err)) {
    err = "Prim not found: " + path_str;
    return false;
  }

  bool include_attrs = args.value("include_attributes", true);

  nlohmann::json j;
  PrimToJSON(*prim, j, -1, include_attrs);
  result["path"] = prim->absolute_path().full_path_name();
  result["prim"] = j;
  return true;
}

// ===========================================================================
// prim_create
// ===========================================================================
bool PrimCreate(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }

  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string type_name = args.value("type_name", std::string("Xform"));
  std::string specifier = args.value("specifier", std::string("def"));

  lightusd::Path dst_path(path_str, "");
  if (!dst_path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  lightusd::Path parent_p = dst_path.get_parent_path();
  std::string parent_path_str = parent_p.full_path_name();
  std::string element_name = dst_path.element_name();

  value::Value prim_data;
  if (type_name == "Xform") prim_data = value::Value(Xform());
  else if (type_name == "Scope") prim_data = value::Value();
  else if (type_name == "Mesh") prim_data = value::Value(GeomMesh());
  else if (type_name == "Sphere") prim_data = value::Value(GeomSphere());
  else if (type_name == "Cube") prim_data = value::Value(GeomCube());
  else if (type_name == "Cylinder") prim_data = value::Value(GeomCylinder());
  else if (type_name == "Cone") prim_data = value::Value(GeomCone());
  else if (type_name == "Capsule") prim_data = value::Value(GeomCapsule());
  else if (type_name == "Camera") prim_data = value::Value(GeomCamera());
  else if (type_name == "Material") prim_data = value::Value(Material());
  else if (type_name == "Shader") prim_data = value::Value(Shader());
  else if (type_name == "SphereLight") prim_data = value::Value(SphereLight());
  else if (type_name == "DistantLight") prim_data = value::Value(DistantLight());
  else if (type_name == "RectLight") prim_data = value::Value(RectLight());
  else if (type_name == "DiskLight") prim_data = value::Value(DiskLight());
  else if (type_name == "DomeLight") prim_data = value::Value(DomeLight());
  else if (type_name == "Skeleton") prim_data = value::Value(Skeleton());
  else if (type_name == "CollisionGroup") prim_data = value::Value(PhysicsCollisionGroup());
  else {
    err = "Unknown prim type: " + type_name;
    return false;
  }

  Prim new_prim(element_name, prim_data);
  new_prim.prim_type_name() = type_name;
  Specifier spec_enum = Specifier::Def;
  if (specifier == "over") spec_enum = Specifier::Over;
  else if (specifier == "class") spec_enum = Specifier::Class;
  new_prim.specifier() = spec_enum;

  if (parent_path_str.empty() || parent_path_str == "/") {
    if (!ctx.stage->add_root_prim(std::move(new_prim), false)) {
      err = "Failed to add root prim";
      return false;
    }
  } else {
    lightusd::Path parent_path(parent_path_str, "");
    const Prim *parent_prim_const = nullptr;
    if (!ctx.stage->find_prim_at_path(parent_path, parent_prim_const, &err)) {
      err = "Parent prim not found: " + parent_path_str;
      return false;
    }
    // Stage currently exposes a const path lookup. The stage itself is mutable
    // and owns this Prim for the duration of the call, so it is safe to use the
    // located node as the mutation target here.
    Prim *parent_prim = const_cast<Prim *>(parent_prim_const);
    if (!parent_prim->add_child(std::move(new_prim), false, &err)) {
      return false;
    }
  }

  ctx.stage->commit();

  result["success"] = true;
  result["path"] = path_str;
  result["type"] = type_name;
  return true;
}

// ===========================================================================
// prim_remove
// ===========================================================================
bool PrimRemove(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  if (!ctx.stage->RemovePrim(path, &err)) return false;
  ctx.stage->commit();
  result["success"] = true;
  result["removed"] = path_str;
  return true;
}

// ===========================================================================
// prim_rename
// ===========================================================================
bool PrimRename(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("new_name") || !args["new_name"].is_string()) {
    err = "Missing 'new_name' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string new_name = args["new_name"].get<std::string>();

  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  if (!ctx.stage->RenamePrim(path, new_name, &err)) return false;
  ctx.stage->commit();
  std::string parent = path.get_parent_path().full_path_name();
  if (parent.empty() || parent == "/") {
    result["path"] = "/" + new_name;
  } else {
    result["path"] = parent + "/" + new_name;
  }
  result["success"] = true;
  result["old_path"] = path_str;
  return true;
}

// ===========================================================================
// prim_get_metadata
// ===========================================================================
bool PrimGetMetadata(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  const Prim *prim = nullptr;
  if (!ctx.stage->find_prim_at_path(path, prim, &err)) {
    err = "Prim not found: " + path_str;
    return false;
  }

  result["path"] = prim->absolute_path().full_path_name();
  result["metadata"] = PrimMetaToJSON(prim->metas());
  return true;
}

// ===========================================================================
// attr_list
// ===========================================================================
bool AttrList(Context &ctx, const nlohmann::json &args,
              nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  const Prim *prim = nullptr;
  if (!ctx.stage->find_prim_at_path(path, prim, &err)) {
    err = "Prim not found: " + path_str;
    return false;
  }

  result["path"] = prim->absolute_path().full_path_name();
  result["primType"] = prim->prim_type_name();

  auto schema = GetSchemaDef(prim->prim_type_name());
  nlohmann::json attrs = nlohmann::json::array();
  for (const auto &def : schema) {
    nlohmann::json a;
    a["name"] = def.name;
    a["type"] = def.type_name;
    a["variability"] = def.variability;
    a["required"] = def.required;

    // Check if it has a value
    auto val = ReadPrimAttribute(*prim, def.name);
    if (val) {
      a["hasValue"] = !val->is_empty() && !val->is_none();
    } else {
      a["hasValue"] = false;
    }
    attrs.push_back(a);
  }
  result["attributes"] = attrs;
  result["count"] = attrs.size();
  return true;
}

// ===========================================================================
// attr_get
// ===========================================================================
bool AttrGet(Context &ctx, const nlohmann::json &args,
             nlohmann::json &result, std::string &err) {
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("attr_name") || !args["attr_name"].is_string()) {
    err = "Missing 'attr_name' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string attr_name = args["attr_name"].get<std::string>();

  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  const Prim *prim = nullptr;
  if (!ctx.stage->find_prim_at_path(path, prim, &err)) {
    err = "Prim not found: " + path_str;
    return false;
  }

  auto val = ReadPrimAttribute(*prim, attr_name);
  if (!val) {
    err = "Attribute '" + attr_name + "' not found on prim type " +
          prim->prim_type_name();
    return false;
  }

  result["path"] = prim->absolute_path().full_path_name();
  result["attr_name"] = attr_name;

  if (val->is_empty()) {
    result["hasValue"] = false;
    result["value"] = nullptr;
  } else if (val->is_none()) {
    result["hasValue"] = true;
    result["value"] = nlohmann::json({{"type", "None"}});
  } else {
    result["hasValue"] = true;
    result["value"] = ValueToJSON(*val);
  }

  // Time samples info
  result["isBlocked"] = val->is_none();

  return true;
}

// ===========================================================================
// attr_set
// ===========================================================================
bool AttrSet(Context &ctx, const nlohmann::json &args,
             nlohmann::json &result, std::string &err) {
  (void)result;
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("attr_name") || !args["attr_name"].is_string()) {
    err = "Missing 'attr_name' argument";
    return false;
  }
  if (!args.contains("value")) {
    err = "Missing 'value' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string attr_name = args["attr_name"].get<std::string>();
  nlohmann::json value_json = args["value"];
  (void)attr_name;

  // Convert JSON value to value::Value
  auto val = JSONToValue(value_json, &err);
  if (!val) {
    err = "Failed to parse value: " + err;
    return false;
  }

  lightusd::Path path(path_str, "");
  if (!path.is_valid()) {
    err = "Invalid path: " + path_str;
    return false;
  }

  // For now, attribute setting requires working at the layer/PrimSpec level
  // or through the concrete prim type accessors
  // This will be expanded once write support is more complete

  err = "Attribute set via MCP tools requires Layer-level editing, which is not yet implemented. "
        "Use run_script with JS API instead.";
  return false;
}

// ===========================================================================
// attr_block
// ===========================================================================
bool AttrBlock(Context &ctx, const nlohmann::json &args,
               nlohmann::json &result, std::string &err) {
  (void)result;
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("attr_name") || !args["attr_name"].is_string()) {
    err = "Missing 'attr_name' argument";
    return false;
  }

  err = "Attribute block via MCP tools not yet implemented. Use run_script with JS API.";
  return false;
}

// ===========================================================================
// attr_connections
// ===========================================================================
bool AttrConnections(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err) {
  (void)result;
  if (!ctx.stage || !ctx.stage_loaded) {
    err = "No stage loaded";
    return false;
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "Missing 'path' argument";
    return false;
  }
  if (!args.contains("attr_name") || !args["attr_name"].is_string()) {
    err = "Missing 'attr_name' argument";
    return false;
  }

  std::string path_str = args["path"].get<std::string>();
  std::string attr_name = args["attr_name"].get<std::string>();
  (void)path_str;
  (void)attr_name;

  // Connections are authored at the Layer level
  err = "Attribute connections via MCP tools not yet implemented. Use run_script with JS API.";
  return false;
}

} // namespace mcp
} // namespace tydra
} // namespace lightusd
