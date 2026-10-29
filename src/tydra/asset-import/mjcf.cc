// SPDX-License-Identifier: Apache-2.0
// MJCF payload builder and the import entry points.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
thread_local Reader* active_reader = nullptr;
thread_local Json* active_materials = nullptr;
thread_local std::string active_world_name = "__world";
// <body> node -> exported link name, so later passes (sites) can name the body
// that owns them even when the body name was generated.
thread_local std::map<const void*, std::string> active_body_names;
thread_local std::vector<GeomMass>* active_geom_masses = nullptr;
// Bodies authored with stacked joints (non-legacy profiles).
thread_local std::set<std::string>* stacked_joint_bodies = nullptr;
// Per-import cache of loaded mesh assets: a mesh geom is otherwise parsed
// for its visual, its collider and mass inference, and an OBJ's MTL
// materials would be registered once per parse.
thread_local std::map<std::pair<std::string, bool>, Mesh>* active_mesh_cache = nullptr;
thread_local std::map<std::string, Mesh>* active_plugin_meshes = nullptr;
thread_local std::map<std::pair<std::string, bool>, PreloadedMesh>* active_mesh_preload = nullptr;
thread_local std::map<std::string, MeshFit>* active_mesh_fits = nullptr;
thread_local bool active_fitaabb = false;
// MJCF vertex bodies absorbed into raw <flex> soft bodies (skipped by
// VisitMujocoBody).
thread_local std::set<const void*>* active_flex_bodies = nullptr;

bool BuildMujocoPayload(const std::string& xml, const fs::path& input_filename,
                        const Options& opts, Json* payload, Stats* stats,
                        std::string* err) {
  std::set<fs::path> seen;
  std::string expanded;
  if (!ExpandIncludes(xml, input_filename.parent_path(), &seen, &expanded,
                      err, input_filename.string())) {
    return false;
  }

  pugi::xml_document doc;
  const pugi::xml_parse_result parse_result = doc.load_string(expanded.c_str());
  if (!parse_result) {
    if (err)
      *err =
          std::string("MJCF XML parse failed: ") + parse_result.description();
    return false;
  }
  // The shortest "__world" + "_"*k that the text does not contain: one more
  // underscore than the longest run after any "__world" (one pass, not a
  // search per appended underscore).
  active_world_name = "__world";
  {
    size_t longest = 0;
    bool seen = false;
    for (size_t at = expanded.find("__world"); at != std::string::npos;
         at = expanded.find("__world", at + 1)) {
      size_t run = 0;
      for (size_t i = at + 7; i < expanded.size() && expanded[i] == '_'; ++i) ++run;
      longest = std::max(longest, run);
      seen = true;
    }
    if (seen) active_world_name.append(longest + 1, '_');
  }
  const auto root = doc.child("mujoco");
  if (!root) {
    if (err) *err = "Expected <mujoco> root.";
    return false;
  }

  // Expand <attach> sub-models (graft + prefix child body subtrees) before any
  // semantic parsing, so assets/defaults/tendons/etc. see the merged model.
  if (!ExpandAttachments(root, input_filename.parent_path(), err)) {
    return false;
  }

  InlineMJCFDefaults(root);
  // <geom type="sdf"> is a mesh geom whose collider MuJoCo evaluates as a
  // signed distance field (NewtonSDFCollisionAPI in USD).
  {
    std::vector<pugi::xml_node> stack{root};
    while (!stack.empty()) {
      pugi::xml_node node = stack.back();
      stack.pop_back();
      for (auto child : node.children())
        if (child.type() == pugi::node_element) stack.push_back(child);
      if (std::string(node.name()) == "geom" && ToLower(Attr(node, "type")) == "sdf" &&
          std::string(node.parent().name()) != "default") {
        node.attribute("type").set_value("mesh");
        node.append_attribute(kSdfGeomMarker).set_value("true");
      }
    }
  }
  std::map<std::string, Mesh> plugin_meshes;
  struct PluginMeshScope {
    std::map<std::string, Mesh>* previous = active_plugin_meshes;
    explicit PluginMeshScope(std::map<std::string, Mesh>* m) { active_plugin_meshes = m; }
    ~PluginMeshScope() { active_plugin_meshes = previous; }
  } plugin_mesh_scope(&plugin_meshes);
  const auto assets = CollectMujocoAssets(root, input_filename.parent_path());
  if (!active_reader->failure.empty()) {
    if (err) *err = active_reader->failure;
    return false;
  }
  const auto hfields = CollectMujocoHFields(root, input_filename.parent_path());
  // MuJoCo merges every <worldbody> block (the local one plus any pulled in via
  // <include>) into a single world; visit them all, not just the first.
  const auto worldbodies = Children(root, "worldbody");
  if (worldbodies.empty()) {
    if (err) *err = "MJCF has no <worldbody>.";
    return false;
  }

  // <compiler angle/eulerseq> + <default> tree drive orientation units and
  // class inheritance for the whole scene.
  Context ctx;
  if (const auto compiler = Child(root, "compiler")) {
    if (ToLower(Attr(compiler, "angle", "degree")) == "radian") {
      ctx.angle_to_rad = 1.0;
    }
    const std::string eseq = Attr(compiler, "eulerseq");
    if (!eseq.empty()) ctx.eulerseq = eseq;
    ctx.autolimits = MjcBoolAttr(Attr(compiler, "autolimits", "true"));
    ctx.inertiafromgeom = ToLower(Attr(compiler, "inertiafromgeom", "auto"));
    const auto igr = ParseDoubles(Attr(compiler, "inertiagrouprange"));
    if (igr.size() >= 2) {
      ctx.inertia_group_min = ToInt32(igr[0], "inertiagrouprange");
      ctx.inertia_group_max = ToInt32(igr[1], "inertiagrouprange");
    }
  }
  ctx.defaults = ParseDefaults(root);
  if (!ExpandCableComposites(root, ctx, err)) return false;
  if (!ExpandReplicates(root, ctx, err)) return false;
  // Expanded cables may nest deeper than the source XML.
  if (!ValidateXMLDepth(root.root(), opts.max_depth + kMaxCableVertices,
                        active_reader->root_source, err))
    return false;
  if (const auto tendon_root = Child(root, "tendon")) {
    for (const auto& spatial : Children(tendon_root, "spatial"))
      for (const auto& wrap : Children(spatial, "geom"))
        if (HasAttr(wrap, "geom")) ctx.wrap_geoms.insert(Attr(wrap, "geom"));
  }

  // Dissolve <frame> grouping transforms now that the angle/eulerseq context is
  // known, so body/geom/site traversal never has to look inside a <frame>.
  // MuJoCo 3.16 frame scale/massscale scale the frame's contents; the
  // flattening keeps only the pose (massscale 0 derives the mass factor
  // from scale, so it matters only with a non-unit scale).
  for (const auto& worldbody : Children(root, "worldbody")) {
    std::vector<pugi::xml_node> stack{worldbody};
    while (!stack.empty()) {
      const pugi::xml_node node = stack.back();
      stack.pop_back();
      for (auto c = node.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) stack.push_back(c);
      if (std::string(node.name()) != "frame") continue;
      const auto scale = ParseDouble3(Attr(node, "scale"), {{1.0, 1.0, 1.0}});
      const double massscale = ParseDoubleAttr(node, "massscale", 0.0);
      if (scale[0] == 1.0 && scale[1] == 1.0 && scale[2] == 1.0 &&
          (massscale == 0.0 || massscale == 1.0))
        continue;
      if (!UnsupportedXML(node, "MJCF frame scale/massscale is not applied to the frame's contents",
                          "mjcf.frame_scale_unsupported", "mjcf.frame_scale",
                          active_reader->root_source, true)) {
        if (err) *err = active_reader->failure;
        return false;
      }
    }
  }
  for (const auto& worldbody : Children(root, "worldbody")) {
    FlattenFramesIn(worldbody, ctx);
  }

  // Body names are keyed by this document's nodes: drop them when it goes.
  active_body_names.clear();
  struct BodyNamesScope {
    ~BodyNamesScope() { active_body_names.clear(); }
  } body_names_scope;
  std::map<std::string, MeshFit> mesh_fits;
  struct FitScope {
    std::map<std::string, MeshFit>* previous = active_mesh_fits;
    bool previous_aabb = active_fitaabb;
    FitScope(std::map<std::string, MeshFit>* fits, bool aabb) {
      active_mesh_fits = fits;
      active_fitaabb = aabb;
    }
    ~FitScope() {
      active_mesh_fits = previous;
      active_fitaabb = previous_aabb;
    }
  } fit_scope(&mesh_fits, Child(root, "compiler") &&
                              MjcBoolAttr(Attr(Child(root, "compiler"), "fitaabb", "false")));
  std::map<std::pair<std::string, bool>, Mesh> mesh_cache;
  struct MeshCacheScope {
    std::map<std::pair<std::string, bool>, Mesh>* previous = active_mesh_cache;
    explicit MeshCacheScope(std::map<std::pair<std::string, bool>, Mesh>* c) { active_mesh_cache = c; }
    ~MeshCacheScope() { active_mesh_cache = previous; }
  } mesh_cache_scope(&mesh_cache);
  std::map<std::pair<std::string, bool>, PreloadedMesh> mesh_preload;
  struct MeshPreloadScope {
    std::map<std::pair<std::string, bool>, PreloadedMesh>* previous = active_mesh_preload;
    explicit MeshPreloadScope(std::map<std::pair<std::string, bool>, PreloadedMesh>* p) {
      active_mesh_preload = p;
    }
    ~MeshPreloadScope() { active_mesh_preload = previous; }
  } mesh_preload_scope(&mesh_preload);
  {
    std::set<std::pair<std::string, bool>> unique;
    for (const auto& entry : assets) {
      const std::string ext = ToLower(entry.second.path.extension().string());
      if (ext == ".stl" || ext == ".obj" || ext == ".dae")
        unique.emplace(entry.second.path.string(), entry.second.smoothnormal);
    }
    PreloadMeshFiles({unique.begin(), unique.end()});
  }
  std::set<std::string> stacked;
  struct StackedScope {
    std::set<std::string>* previous = stacked_joint_bodies;
    explicit StackedScope(std::set<std::string>* s) { stacked_joint_bodies = s; }
    ~StackedScope() { stacked_joint_bodies = previous; }
  } stacked_scope(&stacked);
  Json links = Json::array();
  Json joints = Json::array();
  links.push_back(Json{{"name", active_world_name}, {"static", true}});
  Json actuators = Json::array();
  Json tendons = Json::array();
  Json equalities = Json::array();
  Json filtered_pairs = Json::array();
  Json sites = Json::array();
  Json deformables = Json::array();
  Json mjc_actuators = Json::array();
  Json keyframes = Json::array();
  Json lights = Json::array();
  Json cameras = Json::array();
  Json materials = Json::array();
  Json sensors = Json::array();
  Json contact_pairs = Json::array();
  Json raw_flexes = Json::array();
  std::set<const void*> flex_bodies;
  if (opts.profile == "newton" &&
      !CollectMujocoRawFlexesJson(root, worldbodies, ctx, &raw_flexes, &flex_bodies, err))
    return false;
  struct FlexBodyScope {
    std::set<const void*>* previous = active_flex_bodies;
    explicit FlexBodyScope(std::set<const void*>* b) { active_flex_bodies = b; }
    ~FlexBodyScope() { active_flex_bodies = previous; }
  } flex_body_scope(&flex_bodies);
  for (const auto& worldbody : worldbodies) {
    for (const auto& body : Children(worldbody, "body")) {
      if (!VisitMujocoBody(body, active_world_name, "", IdentityMatrix(),
                           assets, hfields, opts, ctx, &links, &joints, stats,
                           err)) {
        return false;
      }
    }
    CollectMujocoSitesJson(worldbody, IdentityMatrix(), ctx, &sites, stats);
    if (opts.profile == "newton" &&
        !CollectMujocoFlexcompsJson(worldbody, ctx, FlexMeshDir(root, input_filename.parent_path()),
                                    &deformables, err))
      return false;
    CollectMujocoLightsCamerasJson(worldbody, IdentityMatrix(), ctx, &lights,
                                   &cameras);
  }
  for (auto& flex : raw_flexes) deformables.push_back(std::move(flex));
  // World-fixed geoms living directly under <worldbody> (floor/ground/hfield).
  if (!AddWorldbodyGeomsLink(worldbodies, assets, hfields, opts, ctx, &links,
                             stats, err)) {
    return false;
  }
  // Mark each articulation root that owns stacked joints so the converter can
  // author newton:jointsAddMobility.
  if (!stacked.empty()) {
    std::map<std::string, std::string> parent_of;
    for (const Json& joint : joints)
      parent_of[joint["child"].get<std::string>()] = joint["parent"].get<std::string>();
    std::set<std::string> roots;
    for (const std::string& body : stacked) {
      std::string current = body;
      for (size_t hops = 0; hops <= links.size(); ++hops) {
        auto up = parent_of.find(current);
        if (up == parent_of.end() || up->second == active_world_name) break;
        current = up->second;
      }
      roots.insert(current);
    }
    for (Json& link : links)
      if (roots.count(link["name"].get<std::string>())) link["jointsAddMobility"] = true;
  }
  AddMujocoActuatorsJson(root, &actuators, &mjc_actuators, stats);
  AddMujocoTendonsJson(root, ctx, &tendons, stats);
  AddMujocoEqualityJson(root, &equalities, stats);
  AddMujocoContactExcludesJson(root, &filtered_pairs, stats);
  AddMujocoKeyframesJson(root, &keyframes, stats);
  Json skybox = Json::object();
  AddMujocoMaterialsJson(root, input_filename.parent_path(), &materials, &skybox, stats);
  for (auto& mat : *active_materials) materials.push_back(mat);
  AddMujocoSensorsJson(root, &sensors, stats);
  AddMujocoContactPairsJson(root, &contact_pairs, stats);
  Json custom = Json::object();
  AddMujocoCustomJson(root, &custom);
  Json plugins = Json::array();
  AddMujocoPluginsJson(root, &plugins);
  Json skins = Json::array();
  if (opts.profile != "legacy" && !AddMujocoSkinsJson(root, input_filename.parent_path(), &skins, err))
    return false;

  (*payload) = {{"name", Attr(root, "model", input_filename.stem().string())},
                {"upAxis", opts.up_axis}, {"profile", opts.profile},
                {"layout", opts.layout}, {"sourceFormat", "mjcf"},
                {"gravity", opts.up_axis == "Z" ? Json::array({0, 0, -1})
                                                : Json::array({0, -1, 0})}};
  // Assign the large arrays by move (a braced list would copy them).
  (*payload)["links"] = std::move(links);
  (*payload)["joints"] = std::move(joints);
  (*payload)["actuators"] = std::move(actuators);
  if (!tendons.empty()) (*payload)["tendons"] = std::move(tendons);
  if (!equalities.empty()) (*payload)["equalities"] = std::move(equalities);
  if (!filtered_pairs.empty())
    (*payload)["filteredPairs"] = std::move(filtered_pairs);
  if (!sites.empty()) (*payload)["sites"] = std::move(sites);
  if (!deformables.empty()) (*payload)["deformables"] = std::move(deformables);
  if (!skins.empty()) (*payload)["skins"] = std::move(skins);
  if (!mjc_actuators.empty())
    (*payload)["mjcActuators"] = std::move(mjc_actuators);
  Json mjc_scene = BuildMjcSceneJson(root);
  if (!mjc_scene.empty()) (*payload)["mjcScene"] = std::move(mjc_scene);
  if (!keyframes.empty()) (*payload)["keyframes"] = std::move(keyframes);
  if (!lights.empty()) (*payload)["lights"] = std::move(lights);
  if (!skybox.empty()) (*payload)["skybox"] = std::move(skybox);
  // Clipping planes are <visual><map znear/zfar> scaled by the model extent;
  // only an authored <statistic extent> makes them known before compiling.
  if (const auto statistic = Child(root, "statistic")) {
    if (HasAttr(statistic, "extent")) {
      const double extent = ParseDoubleAttr(statistic, "extent", 1.0);
      const auto map = Child(Child(root, "visual"), "map");
      const double znear = map ? ParseDoubleAttr(map, "znear", 0.01) : 0.01;
      const double zfar = map ? ParseDoubleAttr(map, "zfar", 50.0) : 50.0;
      for (auto& cam : cameras) cam["clippingRange"] = Json::array({znear * extent, zfar * extent});
    }
  }
  if (!cameras.empty()) (*payload)["cameras"] = std::move(cameras);
  if (!materials.empty()) (*payload)["materials"] = std::move(materials);
  if (!sensors.empty()) (*payload)["sensors"] = std::move(sensors);
  if (!contact_pairs.empty())
    (*payload)["contactPairs"] = std::move(contact_pairs);
  if (!custom.empty()) (*payload)["custom"] = std::move(custom);
  if (!plugins.empty()) (*payload)["plugins"] = std::move(plugins);
  const auto option = Child(root, "option");
  if (option && HasAttr(option, "gravity")) {
    auto g = ParseDouble3(Attr(option, "gravity"), {{0, 0, -9.80665}});
    double magnitude = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    (*payload)["gravityMagnitude"] = magnitude;
    if (magnitude > 0)
      for (double& v : g) v /= magnitude;
    if (opts.up_axis == "Y") g = {{g[0], g[2], -g[1]}};
    (*payload)["gravity"] = Json::array({g[0], g[1], g[2]});
  } else if (opts.profile != "legacy") {
    (*payload)["gravityMagnitude"] = 9.81;  // MuJoCo's default <option gravity="0 0 -9.81">
  }
  if (option && HasAttr(option, "timestep")) {
    (*payload)["timestep"] = ParseDoubleAttr(option, "timestep", 0.0);
  }
  return true;
}
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd

namespace lightusd {
namespace tydra {
namespace import_asset {
using namespace mjcf_detail;

bool BuildPayload(const std::string& text, const std::string& source,
                  const Options& options, Result* result, std::string* err) {
  Options effective = options;
  ImportSession session(&effective, source, options.format, err);
  effective.report->input_bytes = text.size();
  if (!result) {
    if (err) *err = "Null import result";
    return false;
  }
  *result = Result{};
  result->report = effective.report;
  if (text.size() > options.max_input || options.max_memory == 0) {
    if (err) *err = "Import input or memory limit exceeded";
    return false;
  }
  if (options.max_depth == 0 || options.max_depth > 1024) {
    if (err) *err = "Import depth limit must be in [1, 1024]";
    return false;
  }
  if (options.up_axis != "Z" && options.up_axis != "Y") {
    if (err) *err = "up_axis must be Z or Y";
    return false;
  }
  if (options.profile != "newton" && options.profile != "openusd" &&
      options.profile != "legacy") {
    if (err) *err = "profile must be newton, openusd or legacy";
    return false;
  }
  if (ToLower(options.format) == "obj" ||
      (ToLower(options.format) == "auto" &&
       ToLower(fs::path(source).extension().string()) == ".obj"))
  {
    if (!BuildOBJPayload(text, source, effective, result, err)) return false;
    session.Succeed();
    return true;
  }
  Reader reader(effective);
  if (!reader.Charge(text.size(), err)) return false;
  // Filesystem dependencies may refer back to a parent document's directory.
  reader.root_source = source;
  reader.allow_unanchored = !source.empty();
  struct Scope {
    Reader* previous;
    Json* previous_materials;
    std::string previous_world = active_world_name;
    Scope(Reader* r, Json* m)
        : previous(active_reader), previous_materials(active_materials) {
      active_reader = r;
      active_materials = m;
    }
    ~Scope() {
      active_reader = previous;
      active_materials = previous_materials;
      active_world_name = previous_world;
    }
  };
  Json obj_materials = Json::array();
  Scope scope(&reader, &obj_materials);
  std::string format = ToLower(options.format);
  if (format == "auto" &&
      ToLower(fs::path(source).extension().string()) == ".obj")
    format = "obj";
  bool ok = false;
  {
    if (!reader.ChargeArray(text.size(), 8, err)) return false;
    pugi::xml_document doc;
    auto parsed = doc.load_buffer(text.data(), text.size());
    if (!parsed) {
      if (err) *err = std::string("XML parse failed: ") + parsed.description();
      return false;
    }
    // Reject DTDs and entities rather than accepting a misleading partial
    // parse.
    if (text.find("<!DOCTYPE") != std::string::npos ||
        text.find("<!ENTITY") != std::string::npos) {
      if (err) *err = "XML DTD and entity declarations are unsupported";
      return false;
    }
    if (!ValidateXMLDepth(doc, options.max_depth, source, err)) return false;
    auto root = doc.document_element();
    std::string tag = root.name();
    if (format == "auto")
      format = tag == "mujoco" ? "mjcf" : tag == "robot" ? "urdf" : "unknown";
    effective.report->format = format;
    if (format == "mjcf" && tag == "mujoco") {
      Stats stats;
      ok = BuildMujocoPayload(text, fs::path(source), effective, &result->payload,
                              &stats, err);
    } else if (format == "urdf" && tag == "robot")
      ok = BuildURDF(root, source, effective, &result->payload, err);
    else {
      if (err) *err = "Expected OBJ, <robot> URDF or <mujoco> MJCF input";
      return false;
    }
  }
  if (!ok) return false;
  if (!reader.failure.empty()) {
    if (err) *err = reader.failure;
    return false;
  }
  // Geometry index tables refer to the shared OBJ material prefix.
  if (format != "obj" && !obj_materials.empty()) {
    Json ordered = Json::array();
    for (auto& m : obj_materials)
      if (!m.is_null()) ordered.push_back(m);
    for (auto& m : result->payload["materials"])
      if (m["name"].get<std::string>().rfind("obj_", 0) != 0)
        ordered.push_back(std::move(m));
    result->payload["materials"] = std::move(ordered);
  }
  Json material_names = Json::array();
  for (const Json& m : result->payload["materials"])
    material_names.push_back(m["name"]);
  for (Json& link : result->payload["links"])
    for (const char* kind : {"visuals", "collisions"})
      for (Json& mesh : link[kind]) {
        if (mesh.contains("geometry"))
          mesh["geometry"]["materialNames"] = material_names;
        if (format == "obj") mesh["renderOnly"] = true;
      }
  if (result->payload.contains("timestep")) {
    double dt = result->payload["timestep"].get<double>();
    if (!std::isfinite(dt) || dt <= 0 || 1.0 / dt > double(INT32_MAX)) {
      if (err) *err = "Invalid MJCF timestep";
      return false;
    }
  }
  if (!GatherTextures(reader, result, err)) return false;
  if (!reader.generated.empty()) {
    Json generated = Json::array();
    for (auto& entry : reader.generated) {
      generated.push_back(entry.first);
      result->assets[entry.first] = std::move(entry.second);
    }
    result->payload["generatedAssets"] = std::move(generated);
    reader.generated.clear();
  }
  if (!reader.preserved.empty()) result->payload["unsupported"] = std::move(reader.preserved);
  result->warnings = reader.warnings;
  result->memory_estimate = reader.bytes;
  result->meshes = std::move(reader.meshes);
  for (const char* key : {"links", "joints", "materials", "actuators", "tendons",
                         "equalities", "sites", "sensors", "contactPairs", "filteredPairs", "keyframes",
                         "deformables", "skins"})
    if (result->payload[key].is_array()) {
      effective.report->counts[key] = uint64_t(result->payload[key].size());
      if (!result->payload[key].empty()) effective.report->Feature(key, "imported", source);
    }
  session.Succeed();
  return true;
}

bool BuildPayloadFile(const std::string& filename, const Options& options,
                      Result* result, std::string* err) {
  Options effective = options;
  ImportSession session(&effective, filename, options.format, err);
  if (ToLower(options.format) == "obj" || (ToLower(options.format) == "auto" &&
      ToLower(fs::path(filename).extension().string()) == ".obj"))
  {
    if (!BuildOBJPayloadFile(filename, effective, result, err)) return false;
    session.Succeed();
    return true;
  }
  Reader reader(effective);
  std::vector<uint8_t> bytes;
  std::string resolved;
  if (!reader.Read(filename, "", &bytes, &resolved, err)) return false;
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!BuildPayload(text, resolved, effective, result, err)) return false;
  session.Succeed();
  return true;
}
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
