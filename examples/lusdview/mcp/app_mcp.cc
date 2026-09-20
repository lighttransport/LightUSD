// SPDX-License-Identifier: Apache-2.0
// App's McpHost implementation: the tool handlers. All run on the main thread
// (the MCP server marshals tool calls into the render loop), so they freely read
// the loaded scene / DrawScene and drive the camera and selection.
#include <cmath>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>

#include "app.hh"
#include "light3d/math.h"
#include "next/lightusd-next.hh"
#include "skinning.hh"
#include "tydra/scene-access.hh"
#include "tydra/mcp-tools.hh"  // lightusd::tydra::mcp::CallTool
#include "vchar_control_map.hh"
#include "../../vchar/autorigger_client.hh"

namespace lusdview {

using nlohmann::json;

namespace {
json arr3(const float a[3]) { return json::array({a[0], a[1], a[2]}); }
json vec3json(const light3d::Vec3& v) { return json::array({v.x, v.y, v.z}); }

std::string asciiLower(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

int skinMaterialScore(const DrawMaterialCPU& material) {
  const std::string text = asciiLower(material.name + " " +
      material.displayName + " " + material.absPath);
  int score = 0;
  if (text.find("skin") != std::string::npos) score += 100;
  if (text.find("face") != std::string::npos) score += 80;
  if (text.find("head") != std::string::npos) score += 60;
  if (text.find("body") != std::string::npos) score += 30;
  if (text.find("eye") != std::string::npos ||
      text.find("teeth") != std::string::npos ||
      text.find("hair") != std::string::npos) score -= 200;
  if (material.hasOpenPBRSurface) score += 10;
  return score;
}

json openPbrValues(const DrawLightRtOpenPBRCPU& p) {
  return {{"base_weight", p.baseWeight}, {"base_color", arr3(p.baseColor)},
          {"diffuse_roughness", p.diffuseRoughness}, {"metalness", p.metalness},
          {"specular_weight", p.specularWeight}, {"specular_color", arr3(p.specularColor)},
          {"specular_roughness", p.specularRoughness}, {"specular_ior", p.specularIor},
          {"transmission_weight", p.transmission}, {"transmission_color", arr3(p.transmissionColor)},
          {"transmission_depth", p.transmissionDepth}, {"transmission_scatter", arr3(p.transmissionScatter)},
          {"subsurface_weight", p.subsurface}, {"subsurface_color", arr3(p.subsurfaceColor)},
          {"subsurface_radius", arr3(p.subsurfaceRadius)}, {"subsurface_scale", p.subsurfaceScale},
          {"coat_weight", p.coatWeight}, {"coat_color", arr3(p.coatColor)},
          {"coat_roughness", p.coatRoughness}, {"coat_ior", p.coatIor},
          {"fuzz_weight", p.sheenWeight}, {"fuzz_color", arr3(p.sheenColor)},
          {"fuzz_roughness", p.sheenRoughness}, {"thin_film_weight", p.thinFilmWeight},
          {"thin_film_thickness", p.thinFilmThicknessNm}, {"thin_film_ior", p.thinFilmIor},
          {"emission_luminance", p.emission}, {"emission_color", arr3(p.emissionColor)},
          {"opacity", p.opacity}, {"has_texture_inputs", p.hasTextureInputs},
          {"has_normal_input", p.hasNormalInput}};
}

json openPbrMaterialJson(int id, const DrawMaterialCPU& mat) {
  size_t connected = 0;
  for (const DrawMaterialParamCPU& param : mat.params)
    if (param.texture >= 0 || param.renderTexture >= 0) ++connected;
  return {{"material_id", id}, {"name", mat.name}, {"display_name", mat.displayName},
          {"path", mat.absPath}, {"active_openpbr", mat.openPbrSpecularModel},
          {"dual_authored", mat.hasUsdPreviewSurface && mat.hasOpenPBRSurface},
          {"connected_parameter_count", connected},
          {"materialx_graph", mat.materialXGraph.valid},
          {"values", openPbrValues(mat.lightRtOpenPBR)}};
}

json nextValueJson(const lightusd::next::Value& value) {
  if (value.is_array()) {
    return json{{"type", lightusd::next::GetTypeName(value.type_id())},
                {"count", value.array_size()}};
  }
  if (const bool* v = value.as_bool()) return *v;
  if (const int32_t* v = value.as_int()) return *v;
  if (const int64_t* v = value.as_int64()) return *v;
  if (const float* v = value.as_float()) return *v;
  if (const double* v = value.as_double()) return *v;
  if (const std::string* v = value.as_string()) return *v;
  if (const std::string* v = value.as_token()) return *v;
  if (const std::string* v = value.as_asset_path()) return *v;
  return json{{"type", lightusd::next::GetTypeName(value.type_id())}};
}

// Build the focused-prim payload from the current selection + DrawScene.
json focusedPayload(const Gui& gui, const DrawScene& draw) {
  const int mi = gui.selectedMeshIndex();
  const std::string path = gui.selectedPath();
  if (mi < 0 && path.empty()) return json{{"focused", false}};

  json out = {{"focused", true}, {"path", path}, {"mesh_index", mi}};
  if (mi >= 0 && static_cast<size_t>(mi) < draw.meshes.size()) {
    const DrawMeshCPU& m = draw.meshes[static_cast<size_t>(mi)];
    out["name"] = m.name;
    out["vertex_count"] = m.vertices.size();
    out["triangle_count"] = m.indices.size() / 3;
    out["aabb_min"] = arr3(m.aabbMin);
    out["aabb_max"] = arr3(m.aabbMax);
    out["double_sided"] = m.doubleSided;
    out["has_vertex_opacity"] = !m.vertexAlpha.empty();
    if (!m.vertexAlpha.empty()) {
      const auto mm = std::minmax_element(m.vertexAlpha.begin(),
                                          m.vertexAlpha.end());
      out["vertex_opacity_min"] = *mm.first;
      out["vertex_opacity_max"] = *mm.second;
    }
    json world = json::array();
    for (int i = 0; i < 16; ++i) world.push_back(m.world[i]);
    out["world"] = world;
    const int matId = m.submeshes.empty() ? -1 : m.submeshes.front().materialId;
    out["submesh_count"] = m.submeshes.size();
    json mat = {{"id", matId}};
    if (matId >= 0 && static_cast<size_t>(matId) < draw.materials.size()) {
      const DrawMaterialCPU& dm = draw.materials[static_cast<size_t>(matId)];
      mat["name"] = dm.name;
      mat["base_color"] = arr3(dm.baseColor);
      mat["metallic"] = dm.metallic;
      mat["roughness"] = dm.roughness;
      mat["emissive"] = arr3(dm.emissive);
      mat["alpha"] = dm.alpha;
      mat["alpha_mode"] = dm.alphaMode;
      mat["alpha_cutoff"] = dm.alphaCutoff;
      mat["base_color_tex"] = dm.baseColorTex;
      mat["base_color_uv_set"] = dm.baseColorSample.uvSet;
      mat["opacity_tex"] = dm.opacityTex;
      mat["opacity_uv_set"] = dm.opacitySample.uvSet;
      mat["opacity_channel"] = dm.opacitySample.channel;
      if (dm.opacityTex >= 0 &&
          static_cast<size_t>(dm.opacityTex) < draw.textures.size()) {
        mat["opacity_asset"] =
            draw.textures[static_cast<size_t>(dm.opacityTex)].assetIdentifier;
      }
    }
    out["material"] = mat;
  }
  return out;
}
}  // namespace

json App::mcpLoadUsd(const json& args, std::string& err) {
  const bool hasPath = args.contains("path") && args["path"].is_string();
  const bool hasUsda = args.contains("usda") && args["usda"].is_string();
  if (hasPath == hasUsda) {
    err = "load_usd requires exactly one string 'path' or 'usda'";
    return json::object();
  }
  std::string path;
  if (hasPath) {
    path = args["path"].get<std::string>();
  } else {
    const std::string source = args["usda"].get<std::string>();
    constexpr size_t kMaxInlineUsdaBytes = 16u * 1024u * 1024u;
    if (source.empty() || source.size() > kMaxInlineUsdaBytes) {
      err = "load_usd: inline USDA must be 1 byte..16 MiB";
      return json::object();
    }
    static uint64_t serial = 0;
    const std::filesystem::path temp =
        std::filesystem::temp_directory_path() /
        ("lusdview-mcp-" +
         std::to_string(reinterpret_cast<uintptr_t>(this)) + "-" +
         std::to_string(++serial) + ".usda");
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    stream.write(source.data(), static_cast<std::streamsize>(source.size()));
    if (!stream) {
      err = "load_usd: could not materialize inline USDA";
      return json::object();
    }
    path = temp.string();
    mcpTempFiles_.push_back(temp);
  }
  startLoadAsync(path);  // async; client polls get_scene_info
  return json{{"started", true}, {"path", path}, {"inline", hasUsda}};
}

json App::mcpSceneInfo(const json&, std::string&) {
  json out = {{"loaded", loaded_.ok},
              {"loading", loadActive_},
              {"scene_generation", sceneGen_},
              {"window_generation", windowGeneration_},
              {"renderer_generation", rendererGeneration_},
              {"backend", backend_ == Backend::GL ? "gl" : "vk"},
              {"ray_tracing", rtPath_},
              {"filepath", loaded_.filepath},
              {"mesh_count", draw_.meshes.size()},
              {"triangle_count", draw_.triangleCount},
              {"material_count", draw_.materials.size()},
              {"texture_count", draw_.textures.size()},
              {"upAxis", camera_.upAxis()},
              {"has_bounds", draw_.hasBounds}};
  if (loadActive_) out["loading_path"] = loadingPath_;
  if (!loaded_.err.empty()) out["error"] = loaded_.err;
  out["aabb_min"] = arr3(draw_.aabbMin);
  out["aabb_max"] = arr3(draw_.aabbMax);
  out["skinning_requested"] = skinningModeName(skinningRequested_);
  out["skinning_effective"] = skinningModeName(skinningEffective_);
  out["skinning_reason"] = skinningReason_;
  if (draw_.truncated) out["truncated"] = true;
  if (hasAnimation_) {
    out["has_animation"] = true;
    out["time"] = animTime_;
    out["start_time"] = animStart_;
    out["end_time"] = animEnd_;
    out["fps"] = animFps_;
    out["playing"] = animPlaying_;
  }
  if (loaded_.comp.composed) {
    out["composed"] = true;
    out["deferred_payload_count"] = loaded_.comp.deferred.size();
    json deferred = json::array();
    for (const auto& d : loaded_.comp.deferred) {
      deferred.push_back(
          json{{"prim", d.primPath}, {"asset", d.assetPath}, {"arc", d.arc}});
    }
    out["deferred_payloads"] = deferred;
  } else if (nextSession_) {
    out["composed"] = nextSession_->IsComposed();
    const std::vector<lightusd::next::Path> deferred =
        nextSession_->GetDeferredPayloadPaths();
    out["deferred_payload_count"] = deferred.size();
    json paths = json::array();
    for (const lightusd::next::Path& path : deferred) {
      paths.push_back(json{{"prim", path.str()}, {"arc", "payload"}});
    }
    out["deferred_payloads"] = std::move(paths);
  }
  if (nextSession_) {
    out["stage_revision"] = nextStageRevision_;
    out["layer_dependencies"] = nextSession_->GetLayerDependencies();
  }
  return out;
}

json App::mcpListOpenPbrMaterials(const json&, std::string&) {
  json materials = json::array();
  for (size_t i = 0; i < draw_.materials.size(); ++i) {
    if (draw_.materials[i].hasOpenPBRSurface) {
      materials.push_back(openPbrMaterialJson(static_cast<int>(i),
                                              draw_.materials[i]));
    }
  }
  return {{"count", materials.size()}, {"materials", std::move(materials)}};
}

json App::mcpVirtualHuman(const std::string& tool, const json& args,
                          std::string& err) {
  std::map<std::string, json> targets;
  for (const DrawMeshCPU& mesh : draw_.meshes) {
    for (const MorphTargetCPU& morph : mesh.morphs) {
      json& target = targets[morph.name];
      if (target.empty()) {
        target = {{"name", morph.name}, {"min", -1.0}, {"max", 1.0},
                  {"default", 0.0}, {"mesh_count", 0},
                  {"affected_vertex_count", 0}};
      }
      target["mesh_count"] = target["mesh_count"].get<size_t>() + 1u;
      target["affected_vertex_count"] =
          target["affected_vertex_count"].get<size_t>() + morph.vtx.size();
    }
    for (const MorphTargetChannelsCPU& channels : mesh.morphTargetChannels) {
      if (targets.find(channels.name) == targets.end()) {
        targets[channels.name] =
            {{"name", channels.name}, {"min", -1.0}, {"max", 1.0},
             {"default", 0.0}, {"mesh_count", 1},
             {"affected_vertex_count", mesh.morphInfluence.size()}};
      }
    }
  }

  if (tool == "vchar_status") {
    size_t skinnedMeshes = 0;
    size_t curveGroups = 0;
    for (const DrawMeshCPU& mesh : draw_.meshes) {
      if (!mesh.jointWt.empty()) ++skinnedMeshes;
    }
    curveGroups = draw_.curves.size();
    return {{"profile", virtualHumanProfile_ ? "vchar" : "lusdview"},
            {"input", "USD/USDA/USDC/USDZ"},
            {"raster", json{{"opengl", true}, {"vulkan", true}}},
            {"ray_tracing", json{{"vulkan_ray_query", true},
                                  {"cuda", "deferred"}, {"hip", "deferred"}}},
            {"deformation", json{{"usdskel_skinning", true},
                                  {"blendshapes", true},
                                  {"adapters", json::array({"usdskel", "usd-overlay", "body-playback"})},
                                  {"external_adapter", "json-rpc-overlay"}}},
            {"physics", json{{"mode", "diagnostic-rigid-body-preview"},
                              {"simulation", true},
                              {"kinematic_stage_to_solver", true},
                              {"dynamic_solver_to_stage", true},
                              {"mesh_writeback", true},
                              {"visualization", json::array(
                                  {"bodies", "colliders", "joints", "contacts"})},
                              {"loader", "legacy"}}},
            {"skin", json{{"material_model", "openpbr-multilobe"},
                            {"profile", "human-skin-v1"},
                            {"sss", true}, {"fuzz", true}, {"coat", true}}},
            {"debug", json{{"skeleton", true}, {"skin_weights", true},
                            {"blend_influence", true},
                            {"normals_tangents", true}}},
            {"skinned_mesh_count", skinnedMeshes},
            {"blendshape_count", targets.size()},
            {"hair_curve_group_count", curveGroups}};
  }

  if (tool == "autorigger_inspect") {
    return {{"transport", "json-rpc-2.0-stdio"},
            {"methods", json::array({"rig.initialize", "rig.inspect",
                                      "rig.submit", "rig.status", "rig.cancel",
                                      "dna.inspect", "dna.convert", "dna.evaluate",
                                      "body.animate"})},
            {"authoritative_result", "USD overlay layer"},
            {"input_path", loaded_.filepath},
            {"facial_first", true}, {"body", "next"},
            {"configured", !autoriggerExecutable_.empty()},
            {"executable", autoriggerExecutable_}};
  }

  if (tool == "vchar_deformer") {
    const std::string op = args.value("op", std::string());
    if (op == "status") {
      return {{"adapters", json::array({
                  json{{"name", "usdskel"}, {"active", true},
                       {"capabilities", json::array({"skinning", "blendshapes", "inbetweens"})}},
                  json{{"name", "usd-overlay"}, {"active", true},
                       {"capabilities", json::array({"controls", "correctives", "physics-metadata"})}},
                  json{{"name", "body-playback"}, {"active", !autoriggerExecutable_.empty()},
                       {"capabilities", json::array({"usdskel-animation", "timeline", "contacts"})}},
                  json{{"name", "external-jsonrpc"}, {"active", !autoriggerExecutable_.empty()},
                       {"state", autoriggerExecutable_.empty() ? "not-configured" : "ready"}}})},
              {"evaluation_order", json::array({"overlay", "blendshape", "skinning", "physics"})}};
    }
    if (op == "autorig") {
      if (autoriggerExecutable_.empty() || loaded_.filepath.empty()) {
        err = "vchar_deformer autorig requires --autorigger and a loaded asset";
        return json::object();
      }
      const int timeoutMs = std::max(100, args.value("timeout_ms", 30000));
      std::filesystem::path output = args.value("output", std::string());
      if (output.empty()) output = std::filesystem::temp_directory_path() /
          ("vchar-lightrig-" + std::to_string(sceneGen_) + ".usda");
      const vchar::AutoriggerResult result = vchar::RunAutorigger(
          autoriggerExecutable_, loaded_.filepath, output.string(),
          std::chrono::milliseconds(timeoutMs));
      if (!result.ok) { err = result.timedOut ? "autorigger timed out" : result.error; return json::object(); }
      json applyArgs = {{"path", output.string()}};
      json applied = mcpVirtualHuman("apply_rig_overlay", applyArgs, err);
      if (!err.empty()) return json::object();
      applied["autorigger_response"] = result.response;
      applied["autorigger_exit_code"] = result.exitCode;
      return applied;
    }
    if (op == "apply-overlay") {
      return mcpVirtualHuman("apply_rig_overlay", args, err);
    }
    if (op == "body-playback") {
      if (autoriggerExecutable_.empty() || loaded_.filepath.empty()) {
        err = "vchar_deformer body-playback requires --autorigger and a loaded asset";
        return json::object();
      }
      const std::string jointsPath = args.value("joints_path", std::string());
      const std::string trackPath = args.value("track_path", std::string());
      const std::string rigLayer = args.value("rig_layer", loaded_.filepath);
      if (jointsPath.empty() || trackPath.empty() || rigLayer.empty()) {
        err = "body-playback requires joints_path, track_path, and rig_layer";
        return json::object();
      }
      const int timeoutMs = std::max(100, args.value("timeout_ms", 30000));
      std::filesystem::path output = args.value("output", std::string());
      if (output.empty()) output = std::filesystem::temp_directory_path() /
          ("vchar-lightrig-body-" + std::to_string(sceneGen_) + ".usda");
      const json request = { {"jsonrpc", "2.0"}, {"id", "body-playback"},
        {"method", "body.animate"}, {"params", {
          {"rig_layer", rigLayer}, {"joints_path", jointsPath},
          {"track_path", trackPath}, {"output_layer", output.string()}}} };
      const vchar::AutoriggerResult result = vchar::RunWorkerRequest(
          autoriggerExecutable_, request.dump(), std::chrono::milliseconds(timeoutMs));
      if (!result.ok) { err = result.timedOut ? "body playback worker timed out" : result.error; return json::object(); }
      const json response = json::parse(result.response, nullptr, false);
      if (response.is_discarded() || !response.contains("result") ||
          !response["result"].contains("output_layer")) {
        err = "body playback worker returned malformed response";
        return json::object();
      }
      const std::string animation = response["result"].value("output_layer", output.string());
      json applyArgs = {{"path", animation}};
      if (response["result"].contains("start_time"))
        applyArgs["animation_start"] = response["result"]["start_time"];
      if (response["result"].contains("end_time"))
        applyArgs["animation_end"] = response["result"]["end_time"];
      if (response["result"].contains("fps"))
        applyArgs["animation_fps"] = response["result"]["fps"];
      json applied = mcpVirtualHuman("apply_rig_overlay", applyArgs, err);
      if (!err.empty()) return json::object();
      playRequested_ = args.value("autoplay", false);
      applied["animation_layer"] = animation;
      applied["playback"] = { {"autoplay_requested", playRequested_},
                               {"state", playRequested_ ? "pending" : "paused"} };
      applied["worker_response"] = response["result"];
      for (const char* key : {"frame_count", "start_time", "end_time", "fps"}) {
        if (response["result"].contains(key)) applied[key] = response["result"][key];
      }
      return applied;
    }
    if (op == "dna-inspect" || op == "dna-evaluate") {
      if (autoriggerExecutable_.empty()) {
        err = "vchar_deformer DNA operations require --autorigger"; return json::object();
      }
      const std::string dnaPath = args.value("dna_path", std::string());
      if (dnaPath.empty()) { err = "vchar_deformer DNA operations require dna_path"; return json::object(); }
      json params = {{"asset_path", dnaPath}};
      if (op == "dna-evaluate") {
        params["lod"] = std::max(0, args.value("lod", 0));
        params["control_space"] = args.value("control_space", std::string("canonical"));
        params["controls"] = args.value("controls", json::array());
      }
      const json request = {{"jsonrpc","2.0"},{"id",2},
                            {"method",op == "dna-inspect" ? "dna.inspect" : "dna.evaluate"},
                            {"params",params}};
      const int timeoutMs = std::max(100, args.value("timeout_ms", 30000));
      const vchar::AutoriggerResult result = vchar::RunWorkerRequest(
          autoriggerExecutable_, request.dump(), std::chrono::milliseconds(timeoutMs));
      if (!result.ok) { err = result.timedOut ? "DNA worker timed out" : result.error; return json::object(); }
      const json response = json::parse(result.response, nullptr, false);
      if (response.is_discarded() || !response.contains("result")) {
        err = "DNA worker returned malformed response"; return json::object();
      }
      json resultPayload = response["result"];
      if (op == "dna-evaluate" && resultPayload.contains("blendshape_channels") &&
          resultPayload["blendshape_channels"].is_array()) {
        size_t applied = 0;
        for (const json& channel : resultPayload["blendshape_channels"]) {
          if (!channel.is_object() || !channel.contains("name") ||
              !channel.contains("value") || !channel["name"].is_string() ||
              !channel["value"].is_number()) continue;
          gui_.setBlendWeight(channel["name"].get<std::string>(),
                              channel["value"].get<float>());
          ++applied;
        }
        resultPayload["vchar_applied_blendshape_channels"] = applied;
      }
      return resultPayload;
    }
    err = "vchar_deformer op must be status, autorig, apply-overlay, body-playback, dna-inspect, or dna-evaluate";
    return json::object();
  }

  if (tool == "apply_rig_overlay") {
    const std::string overlay = args.value("path", std::string());
    if (overlay.empty() || loaded_.filepath.empty()) {
      err = "apply_rig_overlay requires path and a loaded base asset";
      return json::object();
    }
    if (overlay.find('@') != std::string::npos ||
        loaded_.filepath.find('@') != std::string::npos) {
      err = "apply_rig_overlay does not accept '@' in asset paths";
      return json::object();
    }
    if (!std::filesystem::exists(overlay)) {
      err = "apply_rig_overlay: overlay does not exist";
      return json::object();
    }
    static uint64_t serial = 0;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("vchar-rig-session-" + std::to_string(++serial) + ".usda");
    std::ofstream stream(root, std::ios::binary | std::ios::trunc);
    stream << "#usda 1.0\n(\n";
    if (args.contains("animation_start") && args.contains("animation_end")) {
      stream << "    timeCodesPerSecond = "
             << args.value("animation_fps", 1.0) << "\n"
             << "    startTimeCode = " << args["animation_start"] << "\n"
             << "    endTimeCode = " << args["animation_end"] << "\n";
    }
    stream << "    subLayers = [\n        @" << overlay
           << "@,\n        @" << loaded_.filepath << "@\n    ]\n)\n";
    if (!stream) {
      err = "apply_rig_overlay: could not create session root";
      return json::object();
    }
    mcpTempFiles_.push_back(root);
    startLoadAsync(root.string());
    return {{"started", true}, {"base", loaded_.filepath},
            {"overlay", overlay}, {"session_root", root.string()}};
  }

  if (tool == "vchar_debug") {
    const std::string mode = args.value("mode", std::string());
    if (args.contains("skeleton")) {
      if (!args["skeleton"].is_boolean()) {
        err = "vchar_debug skeleton must be boolean";
        return json::object();
      }
      setShowSkeleton(args["skeleton"].get<bool>());
    }
    if (!mode.empty()) {
      if (mode == "shaded") setRenderMode(RenderMode::Shaded);
      else if (mode == "skin-weights") setRenderMode(RenderMode::SkinWeights);
      else if (mode == "blend-influence") setRenderMode(RenderMode::BlendInfluence);
      else if (mode == "normals") setRenderMode(RenderMode::Normals);
      else if (mode == "tangents") setRenderMode(RenderMode::Tangent);
      else {
        err = "vchar_debug: unknown mode";
        return json::object();
      }
    }
    return {{"mode", mode.empty() ? "unchanged" : mode}, {"updated", true}};
  }

  if (tool == "vchar_skin_profile") {
    int id = -1;
    std::string selection = "explicit";
    if (args.contains("material_id")) {
      if (!args["material_id"].is_number_integer()) {
        err = "vchar_skin_profile material_id must be integer";
        return json::object();
      }
      id = args["material_id"].get<int>();
    } else {
      selection = "auto-name";
      int bestScore = 0;
      for (size_t i = 0; i < draw_.materials.size(); ++i) {
        const DrawMaterialCPU& candidate = draw_.materials[i];
        if (!candidate.hasOpenPBRSurface && !candidate.hasLightRtOpenPBR) continue;
        const int score = skinMaterialScore(candidate);
        if (score > bestScore) {
          bestScore = score;
          id = static_cast<int>(i);
        }
      }
      if (id < 0) {
        err = "vchar_skin_profile could not auto-detect a skin material; pass material_id";
        return json::object();
      }
    }
    if (id < 0 || static_cast<size_t>(id) >= draw_.materials.size() ||
        (!draw_.materials[static_cast<size_t>(id)].hasOpenPBRSurface &&
         !draw_.materials[static_cast<size_t>(id)].hasLightRtOpenPBR)) {
      err = "vchar_skin_profile requires an OpenPBR or realtime-PBR material";
      return json::object();
    }
    const float strength = std::max(
        0.0f, std::min(1.0f, args.value("strength", 0.65f)));
    const DrawMaterialCPU& material = draw_.materials[static_cast<size_t>(id)];
    DrawLightRtOpenPBRCPU p = material.lightRtOpenPBR;
    p.metalness = 0.0f;
    p.diffuseRoughness = 0.32f;
    p.specularWeight = 0.72f;
    p.specularRoughness = 0.42f;
    p.specularIor = 1.4f;
    p.subsurface = strength;
    p.subsurfaceScale = 0.12f;
    p.subsurfaceRadius[0] = 1.0f;
    p.subsurfaceRadius[1] = 0.35f;
    p.subsurfaceRadius[2] = 0.2f;
    p.subsurfaceColor[0] = std::min(1.0f, p.baseColor[0] * 1.08f);
    p.subsurfaceColor[1] = p.baseColor[1] * 0.72f;
    p.subsurfaceColor[2] = p.baseColor[2] * 0.58f;
    p.sheenWeight = 0.08f;
    p.sheenRoughness = 0.55f;
    p.sheenColor[0] = 1.0f; p.sheenColor[1] = 0.72f; p.sheenColor[2] = 0.62f;
    p.coatWeight = 0.06f;
    p.coatRoughness = 0.3f;
    lightusd::tydra::ClampRealtimePbrMaterial(&p);
    pendingOpenPbrEdit_.materialId = id;
    pendingOpenPbrEdit_.constants = p;
    pendingOpenPbrEdit_.makeConstant = false;
    hasPendingOpenPbrEdit_ = true;
    return {{"pending", true}, {"material_id", id},
            {"material_name", material.name}, {"selection", selection},
            {"profile", "human-skin-v1"},
            {"subsurface_weight", p.subsurface},
            {"subsurface_radius", json::array({p.subsurfaceRadius[0],
                                                p.subsurfaceRadius[1],
                                                p.subsurfaceRadius[2]})}};
  }

  std::vector<VcharControl> authoredControls;
  if (nextSession_) {
    const lightusd::next::StageSnapshot snapshot = nextSession_->GetSnapshot();
    if (snapshot) authoredControls = ReadVcharControls(*snapshot);
  } else if (loaded_.ok) {
    authoredControls = ReadVcharControls(loaded_.stage);
  }

  if (tool == "vchar_physics") {
    const std::string op = args.value("op", std::string());
    if (op == "hide") {
      gui_.setPhysicsDebugLines({});
      return {{"visible", false}, {"initialized", physicsWorldReady_}};
    }
    if (op == "reset" && physicsWorldReady_) {
      if (physicsInitialStage_) {
        loaded_.stage = *physicsInitialStage_;
        if (UpdateAnimatedMeshWorlds(loaded_.stage, &draw_, animTime_) && renderer_) {
          for (size_t i = 0; i < draw_.meshes.size(); ++i) {
            renderer_->updateMeshWorld(static_cast<int>(i), draw_.meshes[i].world);
          }
        }
      }
      lightusd::tydra::FreePhysWorld(&physicsWorld_);
      physicsWorldReady_ = false;
      physicsSceneGen_ = ~std::uint64_t(0);
      gui_.setPhysicsDebugLines({});
    }
    if ((op == "initialize" || op == "step" || op == "reset") &&
        (!physicsWorldReady_ || physicsSceneGen_ != sceneGen_)) {
      if (nextSession_) {
        err = "vchar_physics simulation currently requires --legacy-load; "
              "next-core physics annotation playback is not bridged yet";
        return json::object();
      }
      if (!loaded_.ok) {
        err = "vchar_physics requires a loaded USD scene";
        return json::object();
      }
      if (physicsWorldReady_) lightusd::tydra::FreePhysWorld(&physicsWorld_);
      lightusd::tydra::PhysWorldBuildOptions options;
      options.max_memory_limit_mb = 64;
      if (!lightusd::tydra::BuildPhysWorld(loaded_.stage, &physicsWorld_, &err,
                                           options)) {
        physicsWorldReady_ = false;
        return json::object();
      }
      physicsWorldReady_ = true;
      physicsSceneGen_ = sceneGen_;
      physicsInitialStage_ = std::make_unique<lightusd::Stage>(loaded_.stage);
    }
    if (op == "step") {
      int steps = args.value("steps", 1);
      steps = std::max(1, std::min(600, steps));
      if (args.contains("timestep")) {
        if (!args["timestep"].is_number()) {
          err = "vchar_physics timestep must be numeric";
          return json::object();
        }
        physicsWorld_.timestep = std::max(
            1.0e-5f, std::min(0.1f, args["timestep"].get<float>()));
      }
      if (!lightusd::tydra::SyncStageToPhysWorld(loaded_.stage, &physicsWorld_,
                                                 &err)) {
        return json::object();
      }
      for (int i = 0; i < steps; ++i) {
        if (tydra_phys_step(&physicsWorld_) != TYDRA_PHYS_OK) {
          err = "vchar_physics solver step failed";
          return json::object();
        }
      }
      if (!lightusd::tydra::SyncPhysWorldToStage(physicsWorld_, &loaded_.stage,
                                                  &err)) {
        return json::object();
      }
      if (UpdateAnimatedMeshWorlds(loaded_.stage, &draw_, animTime_) && renderer_) {
        for (size_t meshIndex = 0; meshIndex < draw_.meshes.size(); ++meshIndex) {
          renderer_->updateMeshWorld(static_cast<int>(meshIndex),
                                     draw_.meshes[meshIndex].world);
        }
      }
    } else if (op != "status" && op != "initialize" && op != "reset") {
      err = "vchar_physics op must be status, initialize, step, reset, or hide";
      return json::object();
    }

    std::vector<HelperVertex> lines;
    if (physicsWorldReady_) {
      const float extent = draw_.hasBounds
                               ? std::max(0.01f, (draw_.aabbMax[0] - draw_.aabbMin[0] +
                                                draw_.aabbMax[1] - draw_.aabbMin[1] +
                                                draw_.aabbMax[2] - draw_.aabbMin[2]) /
                                                   300.0f)
                               : 0.05f;
      auto addLine = [&](float ax, float ay, float az, float bx, float by,
                         float bz, float r, float g, float b) {
        lines.push_back(HelperVertex{{ax, ay, az}, {r, g, b}});
        lines.push_back(HelperVertex{{bx, by, bz}, {r, g, b}});
      };
      auto addBox = [&](const TydraPhysAABB& box, float r, float g, float b) {
        const float x0 = box.min.x, y0 = box.min.y, z0 = box.min.z;
        const float x1 = box.max.x, y1 = box.max.y, z1 = box.max.z;
        addLine(x0,y0,z0,x1,y0,z0,r,g,b); addLine(x0,y1,z0,x1,y1,z0,r,g,b);
        addLine(x0,y0,z1,x1,y0,z1,r,g,b); addLine(x0,y1,z1,x1,y1,z1,r,g,b);
        addLine(x0,y0,z0,x0,y1,z0,r,g,b); addLine(x1,y0,z0,x1,y1,z0,r,g,b);
        addLine(x0,y0,z1,x0,y1,z1,r,g,b); addLine(x1,y0,z1,x1,y1,z1,r,g,b);
        addLine(x0,y0,z0,x0,y0,z1,r,g,b); addLine(x1,y0,z0,x1,y0,z1,r,g,b);
        addLine(x0,y1,z0,x0,y1,z1,r,g,b); addLine(x1,y1,z0,x1,y1,z1,r,g,b);
      };
      for (int32_t i = 0; i < physicsWorld_.num_bodies; ++i) {
        const TydraPhysBody& body = physicsWorld_.bodies[i];
        const float x = body.xform.position.x;
        const float y = body.xform.position.y;
        const float z = body.xform.position.z;
        const bool sleeping = (body.flags & TYDRA_PHYS_BODY_FLAG_SLEEPING) != 0u;
        const float r = sleeping ? 0.3f : 1.0f;
        const float g = sleeping ? 0.6f : 0.45f;
        addLine(x - extent, y, z, x + extent, y, z, r, g, 0.1f);
        addLine(x, y - extent, z, x, y + extent, z, r, g, 0.1f);
        addLine(x, y, z - extent, x, y, z + extent, r, g, 0.1f);
      }
      for (int32_t i = 0; i < physicsWorld_.num_colliders; ++i) {
        const TydraPhysCollider& collider = physicsWorld_.colliders[i];
        if (collider.body_index < 0 ||
            collider.body_index >= physicsWorld_.num_bodies) continue;
        TydraPhysAABB box;
        tydra_phys_collider_aabb(&collider,
            &physicsWorld_.bodies[collider.body_index].xform, &box);
        addBox(box, 0.15f, 0.75f, 1.0f);
      }
      for (int32_t i = 0; i < physicsWorld_.num_joints; ++i) {
        const TydraPhysJoint& joint = physicsWorld_.joints[i];
        TydraPhysTransform a = joint.local_anchor_a;
        TydraPhysTransform b = joint.local_anchor_b;
        if (joint.body_a >= 0 && joint.body_a < physicsWorld_.num_bodies) {
          a = tp_xform_mul(physicsWorld_.bodies[joint.body_a].xform, a);
        }
        if (joint.body_b >= 0 && joint.body_b < physicsWorld_.num_bodies) {
          b = tp_xform_mul(physicsWorld_.bodies[joint.body_b].xform, b);
        }
        addLine(a.position.x, a.position.y, a.position.z,
                b.position.x, b.position.y, b.position.z, 0.9f, 0.2f, 0.9f);
        const TydraPhysVec3 axis = tp_q_rotate(a.rotation, joint.axis);
        addLine(a.position.x, a.position.y, a.position.z,
                a.position.x + axis.x * extent * 2.0f,
                a.position.y + axis.y * extent * 2.0f,
                a.position.z + axis.z * extent * 2.0f, 1.0f, 0.2f, 0.8f);
      }
      for (int32_t i = 0; i < physicsWorld_.num_contacts; ++i) {
        const TydraPhysContact& contact = physicsWorld_.contacts[i];
        const float length = std::max(extent, contact.depth * 4.0f);
        addLine(contact.point.x, contact.point.y, contact.point.z,
                contact.point.x + contact.normal.x * length,
                contact.point.y + contact.normal.y * length,
                contact.point.z + contact.normal.z * length, 1.0f, 0.1f, 0.1f);
      }
      gui_.setPhysicsDebugLines(std::move(lines));
    }
    json bodies = json::array();
    if (physicsWorldReady_) {
      for (int32_t i = 0; i < physicsWorld_.num_bodies; ++i) {
        const TydraPhysBody& body = physicsWorld_.bodies[i];
        bodies.push_back({{"index", i},
                          {"type", body.body_type == TYDRA_PHYS_BODY_DYNAMIC
                                       ? "dynamic"
                                       : (body.body_type == TYDRA_PHYS_BODY_KINEMATIC
                                              ? "kinematic" : "static")},
                          {"position", json::array({body.xform.position.x,
                                                    body.xform.position.y,
                                                    body.xform.position.z})},
                          {"linear_velocity", json::array({body.linear_velocity.x,
                                                            body.linear_velocity.y,
                                                            body.linear_velocity.z})},
                          {"angular_velocity", json::array({body.angular_velocity.x,
                                                             body.angular_velocity.y,
                                                             body.angular_velocity.z})},
                          {"sleeping", (body.flags & TYDRA_PHYS_BODY_FLAG_SLEEPING) != 0u}});
      }
    }
    return {{"initialized", physicsWorldReady_},
            {"visible", physicsWorldReady_},
            {"body_count", physicsWorldReady_ ? physicsWorld_.num_bodies : 0},
            {"collider_count", physicsWorldReady_ ? physicsWorld_.num_colliders : 0},
            {"joint_count", physicsWorldReady_ ? physicsWorld_.num_joints : 0},
            {"contact_count", physicsWorldReady_ ? physicsWorld_.num_contacts : 0},
            {"timestep", physicsWorldReady_ ? physicsWorld_.timestep : 0.0f},
            {"bodies", std::move(bodies)},
            {"mesh_sync", physicsWorldReady_ && !nextSession_},
            {"note", "solver bodies and authored USD transforms are synchronized"}};
  }

  if (tool == "vchar_hair_diagnostics") {
    json groups = json::array();
    size_t totalStrands = 0;
    size_t totalSamples = 0;
    for (const DrawCurvesCPU& curves : draw_.curves) {
      float minimumWidth = std::numeric_limits<float>::max();
      float maximumWidth = 0.0f;
      for (float width : curves.widths) {
        minimumWidth = std::min(minimumWidth, width);
        maximumWidth = std::max(maximumWidth, width);
      }
      const size_t strands = curves.vertexCounts.size();
      const size_t samples = curves.points.size() / 3u;
      totalStrands += strands;
      totalSamples += samples;
      groups.push_back({{"path", curves.absPath}, {"name", curves.name},
                        {"strand_count", strands}, {"sample_count", samples},
                        {"samples_per_strand", strands ? double(samples) / double(strands) : 0.0},
                        {"width_interpolation", curves.widths.empty() ? "fallback" :
                                                (curves.widths.size() == 1u ? "constant" : "varying")},
                        {"width_min", curves.widths.empty() ? 0.0f : minimumWidth},
                        {"width_max", maximumWidth},
                        {"aabb_min", arr3(curves.aabbMin)},
                        {"aabb_max", arr3(curves.aabbMax)}});
    }
    return {{"group_count", groups.size()}, {"strand_count", totalStrands},
            {"sample_count", totalSamples}, {"groups", std::move(groups)},
            {"raster_representation", "camera-facing-ribbons"},
            {"vulkan_rt_representation", "tube-proxies"}};
  }

  if (tool == "list_blendshapes" || tool == "list_facial_controls") {
    json values = json::array();
    if (tool == "list_facial_controls" && !authoredControls.empty()) {
      for (const VcharControl& control : authoredControls) {
        values.push_back({{"name", control.name},
                          {"blendshape", control.blendshape},
                          {"min", control.minimum}, {"max", control.maximum},
                          {"default", control.defaultValue}});
      }
    } else {
      for (const auto& entry : targets) values.push_back(entry.second);
    }
    return {{"count", values.size()},
            {tool == "list_blendshapes" ? "blendshapes" : "controls",
             std::move(values)},
            {"mapping", authoredControls.empty() ? "direct-blendshape-v1"
                                                   : "authored-vchar-metadata"}};
  }

  const char* field = tool == "set_facial_controls" ? "controls" : "weights";
  if (!args.contains(field) || !args[field].is_object()) {
    err = tool + " requires object '" + field + "'";
    return json::object();
  }
  json applied = json::object();
  json unknown = json::array();
  for (auto it = args[field].begin(); it != args[field].end(); ++it) {
    if (!it.value().is_number()) {
      err = tool + ": every value must be numeric";
      return json::object();
    }
    std::string targetName = it.key();
    float minimum = -1.0f;
    float maximum = 1.0f;
    if (tool == "set_facial_controls" && !authoredControls.empty()) {
      const auto control = std::find_if(
          authoredControls.begin(), authoredControls.end(),
          [&](const VcharControl& candidate) { return candidate.name == it.key(); });
      if (control == authoredControls.end()) {
        unknown.push_back(it.key());
        continue;
      }
      targetName = control->blendshape;
      minimum = control->minimum;
      maximum = control->maximum;
    }
    if (targets.find(targetName) == targets.end()) {
      unknown.push_back(it.key());
      continue;
    }
    const float weight =
        std::max(minimum, std::min(maximum, it.value().get<float>()));
    setBlendWeight(targetName, weight);
    applied[it.key()] = weight;
  }
  return {{"applied", std::move(applied)}, {"unknown", std::move(unknown)},
          {"mapping", authoredControls.empty() ? "direct-blendshape-v1"
                                                 : "authored-vchar-metadata"}};
}

json App::mcpOpenPbrMaterial(const json& args, std::string& err) {
  if (!args.contains("material_id") || !args["material_id"].is_number_integer()) {
    err = "openpbr_material requires integer material_id";
    return json::object();
  }
  const int id = args["material_id"].get<int>();
  if (id < 0 || static_cast<size_t>(id) >= draw_.materials.size() ||
      !draw_.materials[static_cast<size_t>(id)].hasOpenPBRSurface) {
    err = "openpbr_material: material_id is not OpenPBR-capable";
    return json::object();
  }
  DrawMaterialCPU& material = draw_.materials[static_cast<size_t>(id)];
  const bool makeConstant = args.value("make_constant", false);
  const bool hasValues = args.contains("values");
  if (hasValues && !args["values"].is_object()) {
    err = "openpbr_material: values must be an object";
    return json::object();
  }
  if (makeConstant || hasValues) {
    DrawLightRtOpenPBRCPU p = material.lightRtOpenPBR;
    std::unordered_map<std::string, float*> scalars = {
        {"base_weight", &p.baseWeight}, {"diffuse_roughness", &p.diffuseRoughness},
        {"metalness", &p.metalness}, {"specular_weight", &p.specularWeight},
        {"specular_roughness", &p.specularRoughness}, {"specular_ior", &p.specularIor},
        {"transmission_weight", &p.transmission}, {"transmission_depth", &p.transmissionDepth},
        {"subsurface_weight", &p.subsurface}, {"subsurface_scale", &p.subsurfaceScale},
        {"coat_weight", &p.coatWeight}, {"coat_roughness", &p.coatRoughness},
        {"coat_ior", &p.coatIor}, {"fuzz_weight", &p.sheenWeight},
        {"fuzz_roughness", &p.sheenRoughness}, {"thin_film_weight", &p.thinFilmWeight},
        {"thin_film_thickness", &p.thinFilmThicknessNm}, {"thin_film_ior", &p.thinFilmIor},
        {"emission_luminance", &p.emission}, {"opacity", &p.opacity}};
    std::unordered_map<std::string, float*> colors = {
        {"base_color", p.baseColor}, {"specular_color", p.specularColor},
        {"transmission_color", p.transmissionColor}, {"transmission_scatter", p.transmissionScatter},
        {"subsurface_color", p.subsurfaceColor}, {"subsurface_radius", p.subsurfaceRadius},
        {"coat_color", p.coatColor}, {"fuzz_color", p.sheenColor},
        {"emission_color", p.emissionColor}};
    if (hasValues) {
      for (auto it = args["values"].begin(); it != args["values"].end(); ++it) {
        auto scalar = scalars.find(it.key());
        auto color = colors.find(it.key());
        if (scalar != scalars.end() && it.value().is_number()) {
          *scalar->second = it.value().get<float>();
        } else if (color != colors.end() && it.value().is_array() &&
                   it.value().size() == 3 && it.value()[0].is_number() &&
                   it.value()[1].is_number() && it.value()[2].is_number()) {
          for (int c = 0; c < 3; ++c) color->second[c] = it.value()[c].get<float>();
        } else {
          err = "openpbr_material: invalid or unsupported value '" + it.key() + "'";
          return json::object();
        }
      }
    }
    lightusd::tydra::ClampRealtimePbrMaterial(&p);
    pendingOpenPbrEdit_.materialId = id;
    pendingOpenPbrEdit_.constants = p;
    pendingOpenPbrEdit_.makeConstant = makeConstant;
    hasPendingOpenPbrEdit_ = true;
    return {{"pending", true}, {"material", openPbrMaterialJson(id, material)}};
  }
  return openPbrMaterialJson(id, material);
}

json App::mcpSkinning(const json& args, std::string& err) {
  if (args.contains("mode")) {
    if (!args["mode"].is_string()) {
      err = "skinning: mode must be auto, cpu, or gpu";
      return json::object();
    }
    const std::string mode = args["mode"].get<std::string>();
    if (mode == "auto") {
      skinningRequested_ = SkinningMode::Auto;
    } else if (mode == "cpu") {
      skinningRequested_ = SkinningMode::CPU;
    } else if (mode == "gpu") {
      skinningRequested_ = SkinningMode::GPU;
    } else {
      err = "skinning: mode must be auto, cpu, or gpu";
      return json::object();
    }
    updateSkinningEffective();
    if (skinningEffective_ == SkinningMode::GPU) {
      cancelAndJoinReconvert();
      updateGpuSkinningFrameIfNeeded();
    } else if (hasAnimation_) {
      requestReconvert(animTime_);
    }
  }
  return json{{"requested", skinningModeName(skinningRequested_)},
              {"effective", skinningModeName(skinningEffective_)},
              {"reason", skinningReason_}};
}

json App::mcpRenderSettings(const json& args, std::string& err) {
  struct ModeName {
    const char* name;
    RenderMode mode;
  };
  static constexpr ModeName kModes[] = {
      {"shaded", RenderMode::Shaded},
      {"wireframe", RenderMode::Wireframe},
      {"normals", RenderMode::Normals},
      {"material-id", RenderMode::MaterialId},
      {"geom-normal", RenderMode::GeomNormal},
      {"uv", RenderMode::Uv},
      {"depth", RenderMode::Depth},
      {"albedo", RenderMode::Albedo},
      {"facing", RenderMode::Facing},
      {"roughness", RenderMode::Roughness},
      {"metallic", RenderMode::Metallic},
      {"emissive", RenderMode::Emissive},
      {"opacity", RenderMode::Opacity},
      {"mesh-id", RenderMode::MeshId},
      {"picking", RenderMode::MeshId},
      {"pick-id", RenderMode::MeshId},
      {"coat-normal", RenderMode::CoatNormal},
      {"coat-weight", RenderMode::CoatWeight},
      {"coat-color", RenderMode::CoatColor},
      {"coat-roughness", RenderMode::CoatRoughness},
      {"specular-f0", RenderMode::SpecularF0},
      {"ior-f0", RenderMode::IorF0},
  };
  struct TransparencyName {
    const char* name;
    TransparencyMode mode;
  };
  static constexpr TransparencyName kTransparencyModes[] = {
      {"auto", TransparencyMode::Auto},
      {"weighted", TransparencyMode::Weighted},
      {"sorted", TransparencyMode::Sorted},
  };

  if (args.contains("mode")) {
    if (!args["mode"].is_string()) {
      err = "render_settings: mode must be a string";
      return json::object();
    }
    const std::string requested = args["mode"].get<std::string>();
    bool found = false;
    for (const ModeName& entry : kModes) {
      if (requested == entry.name) {
        gui_.setRenderMode(entry.mode);
        found = true;
        break;
      }
    }
    if (!found) {
      err = "render_settings: unsupported mode '" + requested + "'";
      return json::object();
    }
  }
  if (args.contains("transparency")) {
    if (!args["transparency"].is_string()) {
      err = "render_settings: transparency must be a string";
      return json::object();
    }
    const std::string requested = args["transparency"].get<std::string>();
    bool found = false;
    for (const TransparencyName& entry : kTransparencyModes) {
      if (requested == entry.name) {
        transparencyMode_ = entry.mode;
        postGpu([this, mode = entry.mode] {
          if (renderer_) renderer_->setTransparencyMode(mode);
        });
        found = true;
        break;
      }
    }
    if (!found) {
      err = "render_settings: unsupported transparency '" + requested + "'";
      return json::object();
    }
  }
  if (args.contains("grid")) {
    if (!args["grid"].is_boolean()) {
      err = "render_settings: grid must be boolean";
      return json::object();
    }
    gui_.setShowGrid(args["grid"].get<bool>());
  }
  if (args.contains("camera")) {
    if (!args["camera"].is_string()) {
      err = "render_settings: camera must be a string";
      return json::object();
    }
    // Named USD cameras are resolved as part of the next scene load. Batch
    // manifests set this in load_settings before calling load_usd.
    cameraName_ = args["camera"].get<std::string>();
  }
  if (args.contains("adaptive_quality")) {
    if (!args["adaptive_quality"].is_boolean()) {
      err = "render_settings: adaptive_quality must be boolean";
      return json::object();
    }
    adaptiveQuality_ = args["adaptive_quality"].get<bool>();
  }
  if (args.contains("target_render_fps")) {
    if (!args["target_render_fps"].is_number()) {
      err = "render_settings: target_render_fps must be numeric";
      return json::object();
    }
    const float value = args["target_render_fps"].get<float>();
    if (!(value >= 1.0f && value <= 240.0f)) {
      err = "render_settings: target_render_fps must be in [1, 240]";
      return json::object();
    }
    adaptiveTargetFps_ = value;
  }

  const RenderMode current = gui_.renderMode();
  const char* currentName = "unknown";
  for (const ModeName& entry : kModes) {
    if (current == entry.mode) {
      currentName = entry.name;
      break;
    }
  }
  return json{{"mode", currentName}, {"camera", cameraName_},
              {"transparency", transparencyMode_ == TransparencyMode::Weighted
                                   ? "weighted"
                                   : transparencyMode_ == TransparencyMode::Sorted
                                         ? "sorted"
                                         : "auto"},
              {"adaptive_quality", adaptiveQuality_},
              {"target_render_fps", adaptiveTargetFps_},
              {"render_scale", adaptiveRenderScale_}};
}

json App::mcpShaderReload(const json& args, std::string& err) {
  finishLiveShaderReloads();
  const std::string action = args.value("action", "status");
  std::string backend = args.value("backend", "active");
  const std::string source = args.value("source", std::string());
  if (backend == "active") backend = activeLiveShaderBackend();
  if (backend.empty() && action != "status") {
    err = "shader_reload: no active GPU RT/path-trace backend";
    return json::object();
  }
  if (backend != "vulkan" && backend != "cuda" && backend != "hip" &&
      backend != "all" && !(backend.empty() && action == "status")) {
    err = "shader_reload: backend must be active, vulkan, cuda, hip, or all";
    return json::object();
  }
  if (!source.empty() && backend == "all") {
    err = "shader_reload: source cannot be shared with backend=all";
    return json::object();
  }

  auto stateJson = [&](const char* name, const LiveShaderState& state) {
    uint64_t generation = state.successes;
    double runtimeCompileMs = state.lastCompileMs;
    if (std::strcmp(name, "cuda") == 0) {
      generation = cudaTracer_.kernelGeneration();
      if (cudaTracer_.lastKernelCompileMs() > 0.0)
        runtimeCompileMs = cudaTracer_.lastKernelCompileMs();
    } else if (std::strcmp(name, "hip") == 0) {
      generation = hipTracer_.kernelGeneration();
      if (hipTracer_.lastKernelCompileMs() > 0.0)
        runtimeCompileMs = hipTracer_.lastKernelCompileMs();
    }
    return json{{"backend", name},
                {"source", state.source},
                {"watch", state.watch},
                {"pending", static_cast<bool>(state.pending)},
                {"phase", state.pending ? state.phase : "idle"},
                {"attempts", state.attempts},
                {"successes", state.successes},
                {"generation", generation},
                {"last_compile_ms", runtimeCompileMs},
                {"last_error", state.lastError}};
  };
  auto allStatus = [&]() {
    return json{{"active_backend", activeLiveShaderBackend()},
                {"transactional", true},
                {"poll_interval_ms", 250},
                {"backends", json::array({
                     stateJson("vulkan", liveVulkanShader_),
                     stateJson("cuda", liveCudaKernel_),
                     stateJson("hip", liveHipKernel_)})}};
  };

  if (action == "status") return allStatus();
  if (action == "watch") {
    if (!args.contains("watch") || !args["watch"].is_boolean()) {
      err = "shader_reload: action=watch requires boolean watch";
      return json::object();
    }
    const bool enabled = args["watch"].get<bool>();
    auto configure = [&](LiveShaderState* state) {
      if (!source.empty()) state->source = source;
      state->watch = enabled;
      state->haveTimestamp = false;
      if (enabled && !state->source.empty()) {
        std::error_code ec;
        state->timestamp =
            std::filesystem::last_write_time(state->source, ec);
        state->haveTimestamp = !ec;
        if (ec) state->lastError = "watch cannot stat source: " + state->source;
      }
    };
    if (backend == "all" || backend == "vulkan") configure(&liveVulkanShader_);
    if (backend == "all" || backend == "cuda") configure(&liveCudaKernel_);
    if (backend == "all" || backend == "hip") configure(&liveHipKernel_);
    return allStatus();
  }
  if (action != "reload") {
    err = "shader_reload: action must be status, reload, or watch";
    return json::object();
  }

  if (backend != "all") {
    if (!reloadLiveShader(backend, source, &err)) return allStatus();
    return allStatus();
  }
  std::string failures;
  for (const char* name : {"vulkan", "cuda", "hip"}) {
    std::string oneError;
    if (!reloadLiveShader(name, std::string(), &oneError)) {
      if (!failures.empty()) failures += "; ";
      failures += std::string(name) + ": " + oneError;
    }
  }
  if (!failures.empty()) err = failures;
  return allStatus();
}

json App::mcpRenderStats(const json&, std::string&) {
  const char* tier = adaptiveTier_ == 2 ? "interactive" :
                     (adaptiveTier_ == 1 ? "balanced" : "full");
  const char* cudaRequest = cudaTracer_.rtBackend() == CudaRtBackend::Optix
                                ? "optix"
                                : (cudaTracer_.rtBackend() ==
                                           CudaRtBackend::SoftwareBvh
                                       ? "software" : "auto");
  const TransparencyStatus transparency =
      renderer_ ? renderer_->transparencyStatus() : TransparencyStatus{};
  return json{{"ui_fps", ImGui::GetIO().Framerate},
              {"render_fps", renderFps_},
              {"threaded", renderThreadActive_},
              {"adaptive_quality", adaptiveQuality_},
              {"adaptive_tier", tier},
              {"render_scale", adaptiveRenderScale_},
              {"target_render_fps", adaptiveTargetFps_},
              {"transparency", transparency.requested == TransparencyMode::Weighted ? "weighted" :
                   transparency.requested == TransparencyMode::Sorted ? "sorted" : "auto"},
              {"transparency_effective", transparency.active ? "weighted" : "sorted"},
              {"transparency_phase", transparency.phase},
              {"transparency_error", transparency.error},
              {"transparency_compile_ms", transparency.compileMs},
              {"weighted_oit_supported", transparency.supported},
              {"weighted_oit_active", transparency.active},
              {"oit_attachment_bytes", transparency.attachmentBytes},
              {"cuda_rt", {{"transport", cudaTracer_.usesOptixTransport()
                                                  ? "optix" : "software-bvh"},
                           {"requested", cudaRequest},
                           {"optix_available", cudaTracer_.optixAvailable()},
                           {"optix_status", cudaTracer_.optixStatus()},
                           {"optix_abi", cudaTracer_.optixAbiVersion()},
                           {"optix_gas_bytes", cudaTracer_.optixGasBytes()},
                           {"optix_ias_bytes", cudaTracer_.optixIasBytes()},
                           {"optix_acceleration_bytes",
                            cudaTracer_.optixAccelerationBytes()},
                           {"fallback_reason",
                            cudaTracer_.optixFallbackReason()}}}};
}

json App::mcpLoadPayloads(const json& args, std::string& err) {
  if (!loaded_.comp.composed && !nextSession_) {
    err = "load_payloads: scene was not composed (no deferred payloads)";
    return json::object();
  }
  std::set<std::string> add;
  if (args.contains("paths") && args["paths"].is_array() && !args["paths"].empty()) {
    for (const auto& p : args["paths"]) {
      if (p.is_string()) add.insert(p.get<std::string>());
    }
  } else {
    if (nextSession_) {
      for (const lightusd::next::Path& path :
           nextSession_->GetDeferredPayloadPaths()) {
        add.insert(path.str());
      }
    } else {
      for (const auto& d : loaded_.comp.deferred) add.insert(d.primPath);
    }
  }
  if (add.empty()) {
    return json{{"started", false}, {"reason", "no deferred payloads"}};
  }
  startRecomposeAsync(add);  // async; client polls get_scene_info
  return json{{"started", true}, {"count", add.size()}};
}

json App::mcpReloadLayer(const json& args, std::string& err) {
  if (!useNextLoader_ || !nextSession_) {
    err = "reload_layer: the active scene is not a retained next session";
    return json::object();
  }
  if (loadActive_) {
    err = "reload_layer: a scene update is already active";
    return json::object();
  }
  std::string layer = args.value("path", loaded_.filepath);
  const std::vector<std::string> dependencies =
      nextSession_->GetLayerDependencies();
  if (layer != loaded_.filepath &&
      std::find(dependencies.begin(), dependencies.end(), layer) ==
          dependencies.end()) {
    err = "reload_layer: path is not a dependency of the active stage";
    return json{{"dependencies", dependencies}};
  }
  startLayerReloadAsync(layer);
  return json{{"started", true}, {"path", layer},
              {"base_revision", nextStageRevision_}};
}

json App::mcpTimeline(const json& args, std::string& err) {
  if (!hasAnimation_) {
    err = "timeline: scene has no animation";
    return json::object();
  }
  const std::string op = args.value("op", std::string());
  if (op == "play") {
    animPlaying_ = true;
  } else if (op == "pause") {
    animPlaying_ = false;
  } else if (op == "stop") {
    animPlaying_ = false;
    animTime_ = animStart_;
    requestReconvert(animTime_);
  } else if (op == "seek") {
    double t = args.value("time", animTime_);
    if (t < animStart_) t = animStart_;
    if (t > animEnd_) t = animEnd_;
    animTime_ = t;
    requestReconvert(animTime_);
  } else {
    err = "timeline: unknown op '" + op + "' (play|pause|stop|seek)";
    return json::object();
  }
  return json{{"playing", animPlaying_}, {"time", animTime_},
              {"start", animStart_},     {"end", animEnd_},
              {"fps", animFps_}};
}

json App::mcpGetFocusedPrim(const json&, std::string&) {
  return focusedPayload(gui_, draw_);
}

json App::mcpSetFocus(const json& args, std::string& err) {
  if (!args.contains("path") || !args["path"].is_string()) {
    err = "set_focus requires a string 'path'";
    return json::object();
  }
  gui_.selectByPath(args["path"].get<std::string>(), -1);
  return focusedPayload(gui_, draw_);
}

json App::mcpViewport(const json& args, std::string& err) {
  const std::string op = args.value("op", std::string());
  if (op == "orbit") {
    camera_.orbit(args.value("dx", 0.0f), args.value("dy", 0.0f));
  } else if (op == "pan") {
    camera_.pan(args.value("dx", 0.0f), args.value("dy", 0.0f));
  } else if (op == "dolly") {
    camera_.dolly(args.value("amount", 0.0f));
  } else if (op == "forward") {
    camera_.moveForward(std::abs(args.value("amount", 1.0f)));
  } else if (op == "backward") {
    camera_.moveForward(-std::abs(args.value("amount", 1.0f)));
  } else if (op == "fit") {
    if (draw_.hasBounds) {
      camera_.fitToScene(draw_.aabbMin, draw_.aabbMax);
    } else {
      err = "no scene bounds to fit";
    }
  } else if (op == "home") {
    camera_.setPreset(CameraViewPreset::Isometric);
    if (draw_.hasBounds) camera_.fitToScene(draw_.aabbMin, draw_.aabbMax);
  } else if (op == "isometric") {
    camera_.setPreset(CameraViewPreset::Isometric);
  } else if (op == "front") {
    camera_.setPreset(CameraViewPreset::Front);
  } else if (op == "back") {
    camera_.setPreset(CameraViewPreset::Back);
  } else if (op == "right") {
    camera_.setPreset(CameraViewPreset::Right);
  } else if (op == "left") {
    camera_.setPreset(CameraViewPreset::Left);
  } else if (op == "top") {
    camera_.setPreset(CameraViewPreset::Top);
  } else if (op == "bottom") {
    camera_.setPreset(CameraViewPreset::Bottom);
  } else if (op == "bookmark_save") {
    const int slot = args.value("slot", 1) - 1;
    if (slot < 0 || slot >= 3) {
      err = "bookmark_save: slot must be 1..3";
      return json::object();
    }
    gui_.saveCameraBookmark(slot);
  } else if (op == "bookmark_load") {
    const int slot = args.value("slot", 1) - 1;
    if (slot < 0 || slot >= 3) {
      err = "bookmark_load: slot must be 1..3";
      return json::object();
    }
    if (!gui_.loadCameraBookmark(slot)) {
      err = "bookmark_load: slot is empty";
      return json::object();
    }
  } else if (op == "focus_dof") {
    float distance = 0.0f;
    if (!gui_.focusDofOnSelection(&distance)) {
      err = "focus_dof: pick an object or select a renderable prim in front "
            "of the camera first";
      return json::object();
    }
  } else if (op == "set") {
    light3d::Vec3 tgt = camera_.target();
    if (args.contains("target") && args["target"].is_array() &&
        args["target"].size() == 3) {
      tgt.x = args["target"][0].get<float>();
      tgt.y = args["target"][1].get<float>();
      tgt.z = args["target"][2].get<float>();
    }
    camera_.setOrbit(tgt, args.value("yaw", camera_.yaw()),
                     args.value("pitch", camera_.pitch()),
                     args.value("distance", camera_.distance()));
  } else {
    err = "viewport: unknown op '" + op +
          "' (orbit|pan|dolly|forward|backward|fit|home|isometric|front|back|right|left|top|bottom|bookmark_save|bookmark_load|focus_dof|set)";
    return json::object();
  }
  // Return the resulting camera state.
  return json{{"target", vec3json(camera_.target())},
              {"yaw", camera_.yaw()},
              {"pitch", camera_.pitch()},
              {"distance", camera_.distance()},
              {"eye", vec3json(camera_.eye())},
              {"near", camera_.nearPlane()},
              {"far", camera_.farPlane()},
              {"focus_distance", gui_.dofFocusDistance()}};
}

json App::mcpScreenshot(const json& args, std::string& err) {
  const std::string path = args.value("path", std::string());
  if (path.empty()) {
    err = "screenshot: 'path' is required";
    return json::object();
  }
  if (!renderer_) {
    err = "screenshot: no renderer";
    return json::object();
  }
  // Capture the offscreen viewport (the last rendered frame). Camera ops issued
  // via the 'viewport' tool take effect on the next frame, so a typical debug
  // loop is: viewport(orbit ...) -> screenshot (separate calls = separate frames).
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!renderer_->captureViewport(&rgba, &w, &h)) {
    err = "screenshot: viewport capture failed (no rendered frame yet?)";
    return json::object();
  }
  // During the first few windowed frames (and briefly after a scene reload),
  // ImGui may report a collapsed viewport while its dock layout settles. Such
  // captures are technically non-empty -- commonly 1x10 -- but are not useful
  // screenshots and can make consecutive AOVs appear byte-identical. Report a
  // retryable not-ready result instead of writing misleading image data.
  if (w <= 1 || h <= 1) {
    return json{{"written", false},
                {"ready", false},
                {"reason", "viewport layout is not ready"},
                {"width", w},
                {"height", h}};
  }
  std::string werr;
  if (!WriteScreenshotImage(path, rgba, w, h, &werr)) {
    err = "screenshot: write failed: " + werr;
    return json::object();
  }
  return json{{"written", true},
              {"ready", true},
              {"path", path},
              {"width", w},
              {"height", h}};
}

json App::mcpInput(const json& args, std::string& err) {
  const std::string key = args.value("key", std::string());
  if (key.empty()) {
    err = "input: 'key' is required (e.g. v|w|s|f|a|r|G|V|0|1|3|5|7)";
    return json::object();
  }
  std::string action;
  if (key == "v") {
    action = "wireframe=" + std::to_string(gui_.cycleWireframe());
  } else if (key == "w") {
    camera_.moveForward(1.0f);
    action = "forward";
  } else if (key == "s") {
    camera_.moveForward(-1.0f);
    action = "backward";
  } else if (key == "f" || key == "a") {
    if (draw_.hasBounds) camera_.fitToScene(draw_.aabbMin, draw_.aabbMax);
    action = "frame_all";
  } else if (key == "i") {
    if (!gui_.isolateSelected()) {
      err = "input: no selected renderable prim to isolate";
      return json::object();
    }
    action = "show_selected_only";
  } else if (key == "u") {
    gui_.showAllRenderables();
    action = "show_all";
  } else if (key == "0") {
    camera_.setPreset(CameraViewPreset::Isometric);
    if (draw_.hasBounds) camera_.fitToScene(draw_.aabbMin, draw_.aabbMax);
    action = "home";
  } else if (key == "5") {
    camera_.setPreset(CameraViewPreset::Isometric);
    action = "isometric";
  } else if (key == "1") {
    camera_.setPreset(CameraViewPreset::Front);
    action = "front";
  } else if (key == "3") {
    camera_.setPreset(CameraViewPreset::Right);
    action = "right";
  } else if (key == "7") {
    camera_.setPreset(CameraViewPreset::Top);
    action = "top";
  } else if (key == "G" || key == "V") {
    // Scriptable window-owner switch (GL raster / Vulkan raster), so the
    // GL<->Vulkan teardown+rebuild path is reachable from a test harness.
    // Applied immediately, like 'r' above (this drain runs after present, i.e.
    // between ImGui frames -- the switch must never land mid-frame).
    applyTechniqueSwitch(key == "G" ? RenderTechnique::GLRaster
                                    : RenderTechnique::VulkanRaster);
    action = std::string("technique=") + RenderTechniqueLabel(activeTechnique_);
  } else if (key == "r") {
    // Same toggle the viewport 'r' keybinding drives: into CPU RT, or back to
    // whatever technique was active before. Applied immediately (the MCP tool
    // handlers already run on the main thread) rather than through Gui's
    // one-shot request flag, so the caller can screenshot right after.
    const RenderTechnique want = (activeTechnique_ == RenderTechnique::CpuRT)
                                     ? previousTechnique_
                                     : RenderTechnique::CpuRT;
    applyTechniqueSwitch(want);
    action = std::string("technique=") + RenderTechniqueLabel(activeTechnique_);
  } else {
    err = "input: unhandled key '" + key + "' (v|w|s|f|a|r|G|V|0|1|3|5|7)";
    return json::object();
  }
  return json{{"key", key}, {"action", action}, {"wireframe", gui_.wireframeMode()}};
}

json App::mcpMouse(const json& args, std::string& err) {
  const std::string type = args.value("type", std::string());
  if (type != "move" && type != "button") {
    err = "mouse: type must be 'move' or 'button'";
    return json::object();
  }
  const float x = args.value("x", -1.0f);
  const float y = args.value("y", -1.0f);
  int w = 0, h = 0;
  gui_.viewportPixelSize(&w, &h);
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0f || y < 0.0f ||
      x >= static_cast<float>(w) ||
      y >= static_cast<float>(h)) {
    err = "mouse: coordinates are outside the viewport";
    return json::object();
  }
  ImGuiIO& io = ImGui::GetIO();
  float screenX = x, screenY = y;
  gui_.viewportPixelToScreen(x, y, &screenX, &screenY);
  io.AddMousePosEvent(screenX, screenY);
  if (type == "button") {
    const int button = args.value("button", 0);
    if (button < 0 || button > 2) {
      err = "mouse: button must be 0 (left), 1 (middle), or 2 (right)";
      return json::object();
    }
    const int imguiButton = button == 1 ? 2 : button == 2 ? 1 : 0;
    io.AddKeyEvent(ImGuiMod_Shift, args.value("shift", false));
    io.AddKeyEvent(ImGuiMod_Ctrl, args.value("ctrl", false));
    io.AddKeyEvent(ImGuiMod_Alt, args.value("alt", false));
    io.AddMouseButtonEvent(imguiButton, args.value("down", false));
  }
  return json{{"type", type}, {"x", x}, {"y", y},
              {"viewport_width", w}, {"viewport_height", h}};
}

json App::mcpPick(const json& args, std::string& err) {
  const float x = args.value("x", -1.0f);
  const float y = args.value("y", -1.0f);
  int w = 0, h = 0;
  gui_.viewportPixelSize(&w, &h);
  if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0f || y < 0.0f ||
      x >= static_cast<float>(w) ||
      y >= static_cast<float>(h)) {
    err = "pick: coordinates are outside the viewport";
    return json::object();
  }
  const bool region = args.contains("x1") || args.contains("y1");
  const float x1 = args.value("x1", x);
  const float y1 = args.value("y1", y);
  if (region && (!std::isfinite(x1) || !std::isfinite(y1) || x1 < 0.0f ||
                 y1 < 0.0f || x1 > static_cast<float>(w) ||
                 y1 > static_cast<float>(h))) {
    err = "pick: region coordinates are outside the viewport";
    return json::object();
  }
  const Gui::PickReport report = region
      ? gui_.pickViewportRegion(x, y, x1, y1, args.value("select", true))
      : gui_.pickViewportPixel(x, y, args.value("select", true));
  return json{{"hit", !report.path.empty()},
              {"path", report.path},
              {"kind", report.kind},
              {"mesh_index", report.meshIndex},
              {"x", report.x},
              {"y", report.y},
              {"viewport_width", report.viewportWidth},
              {"viewport_height", report.viewportHeight},
              {"covered_pixels", report.coveredPixels},
              {"hit_distance", report.hitDistance},
              {"selected", args.value("select", true) && !report.path.empty()}};
}

json App::mcpListPrims(const json& args, std::string&) {
  size_t cap = 1000;
  if (args.contains("max") && args["max"].is_number_integer()) {
    const long long m = args["max"].get<long long>();
    if (m > 0) cap = static_cast<size_t>(m);
  }
  json paths = json::array();
  if (nextSession_) {
    const lightusd::next::StageSnapshot snapshot = nextSession_->GetSnapshot();
    if (snapshot) {
      snapshot->Traverse([&](const lightusd::next::UsdPrim& prim) {
        if (paths.size() >= cap) return false;
        paths.push_back(prim.GetPath().str());
        return true;
      });
    }
  } else {
    for (const auto& m : draw_.meshes) {
      if (paths.size() >= cap) break;
      paths.push_back(m.absPath);
    }
  }
  return json{{"count", paths.size()}, {"paths", paths}};
}

json App::mcpCallLibraryTool(const std::string& name, const json& args,
                             std::string& err) {
  if (nextSession_) {
    const lightusd::next::StageSnapshot snapshot = nextSession_->GetSnapshot();
    if (!snapshot) {
      err = "next session has no published stage";
      return json();
    }
    const lightusd::next::Stage& stage = *snapshot;
    if (name == "stage_info") {
      const lightusd::next::StageMeta& meta = stage.GetMeta();
      return json{{"loaded", true},
                  {"defaultPrim", meta.defaultPrim},
                  {"upAxis", meta.upAxis},
                  {"metersPerUnit", meta.metersPerUnit},
                  {"startTimeCode", meta.startTimeCode},
                  {"endTimeCode", meta.endTimeCode},
                  {"timeCodesPerSecond", meta.timeCodesPerSecond},
                  {"primCount", stage.GetPrimCount()}};
    }

    if (name == "prim_list" || name == "query_prims_by_type" ||
        name == "search") {
      const std::string root = args.value("path", std::string("/"));
      const std::string type = args.value("type", std::string());
      const std::string query = args.value("query", std::string());
      json prims = json::array();
      stage.Traverse([&](const lightusd::next::UsdPrim& prim) {
        const std::string path = prim.GetPath().str();
        if (root != "/" && path != root &&
            path.compare(0, root.size() + 1, root + "/") != 0) {
          return true;
        }
        if (!type.empty() && prim.GetTypeName() != type) return true;
        if (!query.empty() && path.find(query) == std::string::npos &&
            prim.GetName().find(query) == std::string::npos) {
          return true;
        }
        prims.push_back(json{{"path", path},
                             {"name", prim.GetName()},
                             {"type", prim.GetTypeName()},
                             {"active", prim.IsActive()}});
        return true;
      });
      return json{{"path", root}, {"prims", prims}, {"count", prims.size()}};
    }

    if (!args.contains("path") || !args["path"].is_string()) {
      err = "Missing 'path' argument";
      return json::object();
    }
    const std::string path = args["path"].get<std::string>();
    const lightusd::next::UsdPrim prim = stage.GetPrimAtPath(path);
    if (!prim.IsValid()) {
      err = "Prim not found: " + path;
      return json::object();
    }

    if (name == "prim_get") {
      return json{{"path", path},
                  {"name", prim.GetName()},
                  {"type", prim.GetTypeName()},
                  {"active", prim.IsActive()},
                  {"propertyCount", prim.GetPropertyNames().size()},
                  {"childCount", prim.GetChildCount()}};
    }
    if (name == "attr_list") {
      json attributes = json::array();
      for (const std::string& attr : prim.GetPropertyNames()) {
        const lightusd::next::Value* value = prim.GetPropertyValue(attr);
        attributes.push_back(
            json{{"name", attr},
                 {"type", value ? lightusd::next::GetTypeName(value->type_id())
                                : "unknown"},
                 {"hasValue", value != nullptr}});
      }
      return json{{"path", path},
                  {"attributes", attributes},
                  {"count", attributes.size()}};
    }
    if (name == "attr_get") {
      const std::string attr = args.value("attr_name", std::string());
      if (attr.empty()) {
        err = "Missing 'attr_name' argument";
        return json::object();
      }
      const lightusd::next::Value* value = prim.GetPropertyValue(attr);
      if (!value) {
        err = "Attribute not found: " + attr;
        return json::object();
      }
      return json{{"path", path},
                  {"attr_name", attr},
                  {"value", nextValueJson(*value)}};
    }
    if (name == "variant_list_sets" || name == "variant_get_selection") {
      json sets = json::object();
      for (const lightusd::next::VariantSetData& set :
           prim.GetMeta().variantSets()) {
        json variants = json::array();
        for (const lightusd::next::VariantData& variant : set.variants) {
          variants.push_back(variant.name);
        }
        sets[set.name] = json{{"selection", set.selected},
                              {"variants", variants}};
      }
      if (name == "variant_get_selection") {
        const std::string set = args.value("variant_set", std::string());
        return json{{"path", path},
                    {"variant_set", set},
                    {"selection", sets.contains(set)
                                      ? sets[set].value("selection", "")
                                      : ""}};
      }
      return json{{"path", path},
                  {"variantSets", sets},
                  {"count", sets.size()}};
    }
    if (name == "variant_set_selection") {
      const std::string set = args.value("variant_set", std::string());
      const std::string variant = args.value("variant", std::string());
      if (set.empty() || variant.empty()) {
        err = "variant_set_selection requires variant_set and variant";
        return json::object();
      }
      loadOpts_.variantOverrides[path][set] = variant;
      startRecomposeAsync(std::set<std::string>());
      return json{{"success", true}, {"started", true}, {"path", path}};
    }

    err = "Tool is not available on the read-only next document: " + name;
    return json::object();
  }

  // Lazily snapshot the loaded Stage into the library-tool Context (copied at
  // most once per loaded scene; the viewer's Stage is never disturbed).
  if (loaded_.ok) {
    if (mcpCtxGen_ != sceneGen_ || !mcpCtx_.stage) {
      mcpCtx_.stage = std::make_unique<lightusd::Stage>(loaded_.stage);
      mcpCtx_.stage_loaded = true;
      mcpCtxGen_ = sceneGen_;
    }
  } else {
    mcpCtx_.stage.reset();
    mcpCtx_.stage_loaded = false;
  }
  json result;
  if (!lightusd::tydra::mcp::CallTool(mcpCtx_, name, args, result, err)) {
    if (err.empty()) err = "unknown tool: " + name;
    return json::object();
  }
  return result;
}

}  // namespace lusdview
