// SPDX-License-Identifier: Apache-2.0
#include "../viewer_document.hh"
#include "../public_stage_queries.hh"
// App's McpHost implementation: the tool handlers. All run on the main thread
// (the MCP server marshals tool calls into the render loop), so they freely read
// the loaded scene / DrawScene and drive the camera and selection.
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <GLFW/glfw3.h>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>

#include "app.hh"
#include "core_scene_loader.hh"
#include "light3d/math.h"
#include "skinning.hh"
#include "tydra/scene-access.hh"
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

json publicDefaultJson(const lightusd_value_view& value, lightusd_sv text) {
  if (value.is_array) return json{{"type", lightusd_type_name(value.type)}, {"count", value.count}};
  if (value.data) {
    switch (value.type) {
      case LIGHTUSD_TYPE_BOOL: return *static_cast<const bool*>(value.data);
      case LIGHTUSD_TYPE_INT: return *static_cast<const int32_t*>(value.data);
      case LIGHTUSD_TYPE_INT64: return *static_cast<const int64_t*>(value.data);
      case LIGHTUSD_TYPE_FLOAT: return *static_cast<const float*>(value.data);
      case LIGHTUSD_TYPE_DOUBLE: return *static_cast<const double*>(value.data);
      default: break;
    }
    size_t floatCount = 0, doubleCount = 0;
    switch (value.type) {
      case LIGHTUSD_TYPE_FLOAT2: case LIGHTUSD_TYPE_TEXCOORD2F: floatCount = 2; break;
      case LIGHTUSD_TYPE_FLOAT3: case LIGHTUSD_TYPE_POINT3F: case LIGHTUSD_TYPE_VECTOR3F:
      case LIGHTUSD_TYPE_NORMAL3F: case LIGHTUSD_TYPE_COLOR3F: case LIGHTUSD_TYPE_TEXCOORD3F: floatCount = 3; break;
      case LIGHTUSD_TYPE_FLOAT4: case LIGHTUSD_TYPE_COLOR4F: floatCount = 4; break;
      case LIGHTUSD_TYPE_DOUBLE2: case LIGHTUSD_TYPE_TEXCOORD2D: doubleCount = 2; break;
      case LIGHTUSD_TYPE_DOUBLE3: case LIGHTUSD_TYPE_POINT3D: case LIGHTUSD_TYPE_VECTOR3D:
      case LIGHTUSD_TYPE_NORMAL3D: case LIGHTUSD_TYPE_COLOR3D: case LIGHTUSD_TYPE_TEXCOORD3D: doubleCount = 3; break;
      case LIGHTUSD_TYPE_DOUBLE4: case LIGHTUSD_TYPE_COLOR4D: doubleCount = 4; break;
      default: break;
    }
    json components = json::array();
    if (floatCount && value.nbytes >= floatCount * sizeof(float)) {
      for (size_t i = 0; i < floatCount; ++i) {
        float component = 0.0f;
        std::memcpy(&component, static_cast<const uint8_t*>(value.data) + i * sizeof(float), sizeof(float));
        components.push_back(component);
      }
      return components;
    }
    if (doubleCount && value.nbytes >= doubleCount * sizeof(double)) {
      for (size_t i = 0; i < doubleCount; ++i) {
        double component = 0.0;
        std::memcpy(&component, static_cast<const uint8_t*>(value.data) + i * sizeof(double), sizeof(double));
        components.push_back(component);
      }
      return components;
    }
  }
  if (value.type == LIGHTUSD_TYPE_STRING || value.type == LIGHTUSD_TYPE_TOKEN ||
      value.type == LIGHTUSD_TYPE_ASSET_PATH) return PublicString(text);
  return json{{"type", lightusd_type_name(value.type)}};
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
              {"loading", loadActive_ || reconvActive_ || (reconvHasRequest_ && reconvRequested_ != reconvApplied_)}, {"has_animation", hasAnimation_},
              {"scene_generation", sceneGen_},
              {"window_generation", windowGeneration_},
              {"renderer_generation", rendererGeneration_},
              {"backend", backend_ == Backend::Metal ? "metal" :
                          backend_ == Backend::GL ? "gl" : "vk"},
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
  } else if (coreSession_) {
    out["composed"] = coreSession_->IsComposed();
    const std::vector<std::string> deferred =
        coreSession_->GetDeferredPayloadPaths();
    out["deferred_payload_count"] = deferred.size();
    json paths = json::array();
    for (const std::string& path : deferred) {
      paths.push_back(json{{"prim", path}, {"arc", "payload"}});
    }
    out["deferred_payloads"] = std::move(paths);
  }
  if (coreSession_) {
    out["stage_revision"] = coreStageRevision_;
    out["layer_dependencies"] = coreSession_->GetLayerDependencies();
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
                              {"visualization", json::array({"bodies", "colliders", "joints", "contacts"})},
                              {"loader", "core"}}},
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
  if (coreSession_) {
    auto snapshot = coreSession_->PublicSnapshot();
    lightusd::api::Stage view;
    if (lightusd::api::DocumentSnapshotStage(snapshot, &view) == LIGHTUSD_OK)
      authoredControls = ReadVcharControls(view.get());
  }

  if (tool == "vchar_physics") {
    const std::string op = args.value("op", std::string());
    if (op != "status" && op != "initialize" && op != "step" && op != "reset" && op != "hide") {
      err = "vchar_physics op must be status, initialize, step, reset, or hide";
      return json::object();
    }
    auto snapshot = coreSession_ ? coreSession_->PublicSnapshot() : lightusd::api::DocumentSnapshot{};
    lightusd::api::Stage authored;
    const uint64_t revision = lightusd::api::DocumentSnapshotRevision(snapshot);
    if (physicsSceneGen_ != sceneGen_ || physicsRevision_ != revision) {
      physicsPreview_.clear(); physicsPreviewVisible_ = false;
      gui_.setPhysicsDebugLines({});
    }
    if (op == "hide") {
      physicsPreviewVisible_ = false;
      gui_.setPhysicsDebugLines({});
      return {{"visible", false}, {"initialized", physicsPreview_.stage() != nullptr}};
    }
    if (op != "status") {
      if (lightusd::api::DocumentSnapshotStage(snapshot, &authored) != LIGHTUSD_OK) {
        err = "vchar_physics requires a loaded core document"; return json::object();
      }
      if (op == "initialize" || op == "reset" || !physicsPreview_.stage()) {
        if (!physicsPreview_.initialize(authored.get(), animTime_, &err)) return json::object();
        physicsSceneGen_ = sceneGen_; physicsRevision_ = revision;
      }
      if (op == "step") {
        if (args.contains("steps") && !args["steps"].is_number_integer()) {
          err = "vchar_physics steps must be an integer"; return json::object();
        }
        const auto requestedSteps = args.value("steps", int64_t(1));
        if (requestedSteps < 1 || requestedSteps > 600) {
          err = "vchar_physics steps must be in [1,600]"; return json::object();
        }
        if (args.contains("timestep") && !args["timestep"].is_number()) {
          err = "vchar_physics timestep must be numeric"; return json::object();
        }
        const float dt = args.value("timestep", physicsPreview_.world().timestep);
        if (!physicsPreview_.step(authored.get(), animTime_, static_cast<int>(requestedSteps), dt, &err)) return json::object();
      }
      UpdateCoreAnimatedMeshWorlds(physicsPreview_.stage(), &draw_, animTime_);
      std::vector<std::pair<int, std::array<float, 16>>> uploads;
      for (size_t i = 0; i < draw_.meshes.size(); ++i) {
        if (!draw_.meshes[i].animatedWorld) continue;
        std::array<float, 16> matrix;
        std::copy_n(draw_.meshes[i].world, 16, matrix.begin());
        uploads.push_back({static_cast<int>(i), matrix});
      }
      postGpu([this, uploads = std::move(uploads)]() {
        if (renderer_) for (const auto& upload : uploads)
          renderer_->updateMeshWorld(upload.first, upload.second.data());
      });
      coreTracerPosedTime_ = std::numeric_limits<double>::quiet_NaN();
      physicsPreviewVisible_ = true;
    }
    const auto& world = physicsPreview_.world();
    std::vector<HelperVertex> lines;
    if (physicsPreview_.stage() && physicsPreviewVisible_) {
      const float extent = draw_.hasBounds
                               ? std::max(0.01f, (draw_.aabbMax[0] - draw_.aabbMin[0] +
                                                draw_.aabbMax[1] - draw_.aabbMin[1] +
                                                draw_.aabbMax[2] - draw_.aabbMin[2]) /
                                                   300.0f)
                               : 0.05f;
      auto addLine = [&](float ax, float ay, float az, float bx, float by,
                         float bz, float r, float g, float b) {
        if (lines.size() >= 131072) return;
        for (float v : {ax, ay, az, bx, by, bz}) if (!std::isfinite(v)) return;
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
      for (int32_t i = 0; i < world.num_bodies; ++i) {
        const TydraPhysBody& body = world.bodies[i];
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
      for (int32_t i = 0; i < world.num_colliders; ++i) {
        const TydraPhysCollider& collider = world.colliders[i];
        if (collider.shape.type == TYDRA_PHYS_SHAPE_PLANE) continue;
        if (collider.body_index < 0 ||
            collider.body_index >= world.num_bodies) continue;
        TydraPhysAABB box;
        tydra_phys_collider_aabb(&collider,
            &world.bodies[collider.body_index].xform, &box);
        addBox(box, 0.15f, 0.75f, 1.0f);
      }
      for (int32_t i = 0; i < world.num_joints; ++i) {
        const TydraPhysJoint& joint = world.joints[i];
        TydraPhysTransform a = joint.local_anchor_a;
        TydraPhysTransform b = joint.local_anchor_b;
        if (joint.body_a >= 0 && joint.body_a < world.num_bodies) {
          a = tp_xform_mul(world.bodies[joint.body_a].xform, a);
        }
        if (joint.body_b >= 0 && joint.body_b < world.num_bodies) {
          b = tp_xform_mul(world.bodies[joint.body_b].xform, b);
        }
        addLine(a.position.x, a.position.y, a.position.z,
                b.position.x, b.position.y, b.position.z, 0.9f, 0.2f, 0.9f);
        const TydraPhysVec3 axis = tp_q_rotate(a.rotation, joint.axis);
        addLine(a.position.x, a.position.y, a.position.z,
                a.position.x + axis.x * extent * 2.0f,
                a.position.y + axis.y * extent * 2.0f,
                a.position.z + axis.z * extent * 2.0f, 1.0f, 0.2f, 0.8f);
      }
      for (int32_t i = 0; i < world.num_contacts; ++i) {
        const TydraPhysContact& contact = world.contacts[i];
        const float length = std::max(extent, contact.depth * 4.0f);
        addLine(contact.point.x, contact.point.y, contact.point.z,
                contact.point.x + contact.normal.x * length,
                contact.point.y + contact.normal.y * length,
                contact.point.z + contact.normal.z * length, 1.0f, 0.1f, 0.1f);
      }
      gui_.setPhysicsDebugLines(std::move(lines));
    }
    json bodies = json::array();
    for (int32_t i = 0; i < world.num_bodies; ++i) {
      const auto& body = world.bodies[i];
      bodies.push_back({{"index", i}, {"path", physicsPreview_.bodyPaths()[static_cast<size_t>(i)]},
          {"type", body.body_type == TYDRA_PHYS_BODY_DYNAMIC ? "dynamic" : body.body_type == TYDRA_PHYS_BODY_KINEMATIC ? "kinematic" : "static"},
          {"position", json::array({body.xform.position.x, body.xform.position.y, body.xform.position.z})},
          {"linear_velocity", json::array({body.linear_velocity.x, body.linear_velocity.y, body.linear_velocity.z})},
          {"angular_velocity", json::array({body.angular_velocity.x, body.angular_velocity.y, body.angular_velocity.z})},
          {"sleeping", (body.flags & TYDRA_PHYS_BODY_FLAG_SLEEPING) != 0}});
    }
    return {{"initialized", physicsPreview_.stage() != nullptr}, {"visible", physicsPreviewVisible_},
        {"body_count", world.num_bodies}, {"collider_count", world.num_colliders},
        {"joint_count", world.num_joints}, {"contact_count", world.num_contacts},
        {"timestep", world.timestep}, {"bodies", std::move(bodies)},
        {"mesh_sync", physicsPreview_.stage() != nullptr}, {"loader", "core"},
        {"note", "diagnostic solver writes an independent core preview stage; reset restores the document pose"}};
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
  for (const char* key : {"guide", "proxy", "render"}) {
    if (args.contains(key) && !args[key].is_boolean()) {
      err = "purpose flags must be boolean"; return json::object();
    }
  }
  if (args.contains("background")) {
    if (!args["background"].is_string() ||
        (args["background"] != "black" && args["background"] != "white")) {
      err = "shared background must be black or white"; return json::object();
    }
  }

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

  // Validate the complete update before mutating any display setting.
  for (const char* key : {"grid", "adaptive_quality", "skeleton"}) {
    if (args.contains(key) && !args[key].is_boolean()) {
      err = std::string(key) + " must be boolean"; return json::object();
    }
  }
  if (args.contains("camera") && !args["camera"].is_string()) {
    err = "camera must be a string"; return json::object();
  }
  if (args.contains("target_render_fps") &&
      (!args["target_render_fps"].is_number() ||
       !(args["target_render_fps"].get<double>() >= 1.0 && args["target_render_fps"].get<double>() <= 240.0))) {
    err = "target_render_fps must be in [1,240]"; return json::object();
  }
  if (args.contains("mode")) {
    bool valid = false;
    if (args["mode"].is_string())
      for (const auto& entry : kModes) valid |= args["mode"] == entry.name;
    if (!valid) { err = "unsupported render mode"; return json::object(); }
  }
  if (args.contains("transparency")) {
    bool valid = false;
    if (args["transparency"].is_string())
      for (const auto& entry : kTransparencyModes) valid |= args["transparency"] == entry.name;
    if (!valid) { err = "unsupported transparency mode"; return json::object(); }
  }

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

  const uint32_t purposeMask = gui_.purposeVisibleMask();
  gui_.setPurposeFlags(args.value("guide", bool(purposeMask & 8u)),
                       args.value("proxy", bool(purposeMask & 4u)),
                       args.value("render", bool(purposeMask & 2u)));
  if (args.contains("background")) gui_.setBackgroundPreset(args["background"] == "white");

  if (args.contains("skeleton")) gui_.setShowSkeleton(args["skeleton"].get<bool>());

  const RenderMode current = gui_.renderMode();
  const char* currentName = "unknown";
  for (const ModeName& entry : kModes) {
    if (current == entry.mode) {
      currentName = entry.name;
      break;
    }
  }
  return json{{"skeleton", gui_.showSkeletonOverlay()}, {"skeleton_control", true}, {"skeleton_policy", "overlay lines"}, {"background", gui_.backgroundPreset()}, {"guide", bool(gui_.purposeVisibleMask() & 8u)},
              {"proxy", bool(gui_.purposeVisibleMask() & 4u)},
              {"render", bool(gui_.purposeVisibleMask() & 2u)},
              {"mode", currentName}, {"camera", cameraName_},
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
  return json{{"core_skeleton_overlay_vertices", gui_.coreSkeletonOverlayVertexCount()}, {"cpu_frame_s", mcpCpuFrameSeconds_},
              {"timer_scope", "lusdview UI frame through present; includes CPU work and GPU waits"},
              {"frame_serial", mcpFrameSerial_},
              {"ui_fps", ImGui::GetIO().Framerate},
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
  if (!loaded_.comp.composed && !coreSession_) {
    err = "load_payloads: scene was not composed (no deferred payloads)";
    return json::object();
  }
  std::set<std::string> add;
  if (args.contains("paths") && args["paths"].is_array() && !args["paths"].empty()) {
    for (const auto& p : args["paths"]) {
      if (p.is_string()) add.insert(p.get<std::string>());
    }
  } else {
    if (coreSession_) {
      for (const std::string& path :
           coreSession_->GetDeferredPayloadPaths()) {
        add.insert(path);
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
  if (!useCoreLoader_ || !coreSession_) {
    err = "reload_layer: the active scene is not a retained core session";
    return json::object();
  }
  if (loadActive_) {
    err = "reload_layer: a scene update is already active";
    return json::object();
  }
  std::string layer = args.value("path", loaded_.filepath);
  const std::vector<std::string> dependencies =
      coreSession_->GetLayerDependencies();
  if (layer != loaded_.filepath &&
      std::find(dependencies.begin(), dependencies.end(), layer) ==
          dependencies.end()) {
    err = "reload_layer: path is not a dependency of the active stage";
    return json{{"dependencies", dependencies}};
  }
  startLayerReloadAsync(layer);
  return json{{"started", true}, {"path", layer},
              {"base_revision", coreStageRevision_}};
}

json App::mcpTimeline(const json& args, std::string& err) {
  const std::string op = args.value("op", std::string("get"));
  if (op == "get") return json{{"playing", animPlaying_}, {"time", animTime_},
      {"start", animStart_}, {"end", animEnd_}, {"fps", animFps_}};
  if (!hasAnimation_) {
    err = "timeline: scene has no animation";
    return json::object();
  }
  if (op == "play") {
    animPlaying_ = true;
  } else if (op == "pause") {
    animPlaying_ = false;
  } else if (op == "stop") {
    animPlaying_ = false;
    animTime_ = animStart_;
    requestReconvert(animTime_);
  } else if (op == "seek") {
    if (!args.contains("time") || !args["time"].is_number() || !std::isfinite(args["time"].get<double>())) {
      err = "timeline seek requires finite time"; return json::object();
    }
    double t = args["time"].get<double>();
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
  if (op == "resize") {
    if (!args.contains("width") || !args["width"].is_number_integer() ||
        !args.contains("height") || !args["height"].is_number_integer() ||
        args["width"].get<int>() < 16 || args["width"].get<int>() > 8192 ||
        args["height"].get<int>() < 16 || args["height"].get<int>() > 8192) {
      err = "resize requires width/height in [16,8192]"; return json::object();
    }
    gui_.setFixedRenderSize(args["width"].get<int>(), args["height"].get<int>());
  } else if (op == "get" || op == "refresh") {
    // Query only: synchronization clients must not dirty the camera.
  } else if (op == "set_camera") {
    if (!args.contains("camera") || !args["camera"].is_object()) {
      err = "set_camera requires a camera object"; return json::object();
    }
    const json& c = args["camera"];
    const auto number = [&](const char* key, float lo, float hi) {
      return c.contains(key) && c[key].is_number() &&
             std::isfinite(c[key].get<double>()) &&
             c[key].get<double>() >= lo && c[key].get<double>() <= hi;
    };
    if (!c.contains("transform") || !c["transform"].is_array() ||
        c["transform"].size() != 16 || !c.contains("shift") ||
        !c["shift"].is_array() || c["shift"].size() != 2 ||
        !c.contains("projection") || !c["projection"].is_string() ||
        (c["projection"] != "perspective" && c["projection"] != "orthographic") ||
        !number("fov_y", 5.0f, 175.0f) || !number("aspect", 0.05f, 100.0f) ||
        !number("ortho_height", 1e-5f, 1e30f) ||
        !number("near", 1e-5f, 1e30f) || !number("far", 1e-5f, 1e30f) ||
        !number("distance", 1e-3f, 1e30f) || !number("exposure", -100.0f, 100.0f) ||
        c["far"].get<double>() <= c["near"].get<double>()) {
      err = "invalid camera snapshot"; return json::object();
    }
    float transform[16];
    for (int i = 0; i < 16; ++i) {
      const auto& v = c["transform"][i];
      if (!v.is_number() || !std::isfinite(v.get<double>()) ||
          std::abs(v.get<double>()) > 1e30) {
        err = "invalid camera transform"; return json::object();
      }
      transform[i] = v.get<float>();
    }
    for (const auto& v : c["shift"]) {
      if (!v.is_number() || !std::isfinite(v.get<double>()) ||
          std::abs(v.get<double>()) > 10.0) {
        err = "invalid lens shift"; return json::object();
      }
    }
    auto candidate = camera_;
    if (!candidate.setPose(transform, c["distance"].get<float>())) {
      err = "camera transform must be rigid and right handed"; return json::object();
    }
    candidate.setProjection(c["projection"] == "orthographic"
                                ? light3d::CameraProjection::Orthographic
                                : light3d::CameraProjection::Perspective);
    candidate.setFovYDeg(c["fov_y"].get<float>());
    candidate.setOrthographicHeight(c["ortho_height"].get<float>());
    candidate.setAspectOverride(c["aspect"].get<float>());
    candidate.setAspectOverrideEnabled(true);
    candidate.setConform(light3d::CameraConform::None);
    candidate.setLensShift(c["shift"][0].get<float>(), c["shift"][1].get<float>());
    candidate.setClipPlanes(c["near"].get<float>(), c["far"].get<float>());
    candidate.setAutoClip(false);
    candidate.setExposure(c["exposure"].get<float>());
    camera_ = candidate;
    cameraName_.clear();
  } else if (op == "orbit") {
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
  // Effective projection after filmback conform, in OpenGL camera convention.
  const auto pose = camera_.view().inverse();
  const auto projection = camera_.proj(false);
  json transform = json::array();
  for (float v : pose.m) transform.push_back(v);
  constexpr float radiansToDegrees = 57.29577951308232f;
  const bool ortho = camera_.projection() == light3d::CameraProjection::Orthographic;
  json snapshot{{"transform", transform},
                {"projection", ortho ? "orthographic" : "perspective"},
                {"fov_y", ortho ? camera_.fovYDeg() : 2.0f * std::atan(1.0f / projection.m[5]) * radiansToDegrees},
                {"aspect", projection.m[5] / projection.m[0]},
                {"ortho_height", ortho ? 2.0f / projection.m[5] : camera_.orthographicHeight()},
                {"shift", {camera_.lensShiftX(), camera_.lensShiftY()}},
                {"near", camera_.nearPlane()}, {"far", camera_.farPlane()},
                {"distance", camera_.distance()}, {"exposure", camera_.exposure()}};
  return json{{"camera", snapshot}, {"protocol_version", 1},
              {"target", vec3json(camera_.target())},
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
  if (args.contains("barrier")) {
    if (!args["barrier"].is_string() || mcpBarrier_.is_null() ||
        mcpBarrier_.value("token", std::string()) != args["barrier"].get<std::string>()) {
      err = "screenshot barrier is unknown or replaced"; return json::object();
    }
    if (!mcpBarrier_.value("ready", false)) {
      err = mcpBarrier_.value("error", std::string("barrier frame is not ready")); return json::object();
    }
    if (args.contains("after_frame") && (!args["after_frame"].is_number_integer() ||
        args["after_frame"].get<int64_t>() < 0 ||
        mcpBarrier_["frame_serial"].get<uint64_t>() <= args["after_frame"].get<uint64_t>())) {
      err = "barrier frame is not newer than after_frame"; return json::object();
    }
    std::string error;
    if (!WriteScreenshotImage(path, mcpBarrierPixels_, mcpBarrierWidth_, mcpBarrierHeight_, &error)) {
      err = "barrier image write failed: " + error; return json::object();
    }
    const auto& state = mcpBarrier_["state"];
    return json{{"written", true}, {"ready", true}, {"path", path},
                {"width", mcpBarrierWidth_}, {"height", mcpBarrierHeight_},
                {"frame_serial", mcpBarrier_["frame_serial"]}, {"barrier", mcpBarrier_},
                {"rendered_camera", state["camera"]}, {"rendered_time", state["timeline"]["time"]}};
  }
  if (args.contains("after_frame")) {
    if (!args["after_frame"].is_number_unsigned() && !args["after_frame"].is_number_integer()) {
      err = "after_frame must be a nonnegative integer"; return json::object();
    }
    if (args["after_frame"].get<int64_t>() < 0) {
      err = "after_frame must be nonnegative"; return json::object();
    }
    if (mcpFrameSerial_ <= args["after_frame"].get<uint64_t>())
      return json{{"written", false}, {"ready", false}, {"reason", "waiting for a new rendered frame"}};
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
              {"path", path}, {"frame_serial", mcpFrameSerial_},
              {"rendered_time", mcpRenderedTime_}, {"rendered_camera", mcpRenderedCamera_},
              {"width", w},
              {"height", h}};
}

json App::mcpInput(const json& args, std::string& err) {
  const std::string key = args.value("key", std::string());
  if (key.empty()) {
    err = "input: 'key' is required (e.g. v|x|w|s|f|a|r|G|V|0|1|3|5|7)";
    return json::object();
  }
  std::string action;
  if (key == "v") {
    action = "wireframe=" + std::to_string(gui_.cycleWireframe());
  } else if (key == "x") {
    action = std::string("wireframe_xray=") +
             (gui_.toggleWireXray() ? "1" : "0");
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
    err = "input: unhandled key '" + key + "' (v|x|w|s|f|a|r|G|V|0|1|3|5|7)";
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
  json paths = json::array();
  for (const Gui::SelectionItem& item : report.items) {
    paths.push_back({{"path", item.path},
                     {"mesh_index", item.meshIndex},
                     {"source_prim", item.sourcePrim}});
  }
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
              {"selected", args.value("select", true) && !report.path.empty()},
              {"selected_count", report.items.size()},
              {"paths", paths}};
}

json App::mcpSelect(const json& args, std::string& err) {
  const std::string op = args.value("op", std::string("list"));
  const std::string path = args.value("path", std::string());
  auto selectionJson = [&]() {
    json items = json::array();
    for (const Gui::SelectionItem& item : gui_.selectionItems()) {
      items.push_back({{"path", item.path},
                       {"mesh_index", item.meshIndex},
                       {"source_prim", item.sourcePrim}});
    }
    return json{{"count", items.size()},
                {"focused", gui_.selectedPath()},
                {"items", items}};
  };
  if (op == "list") return selectionJson();
  if (op == "clear") {
    gui_.clearSelection();
    return selectionJson();
  }
  if (path.empty()) {
    err = "select: 'path' is required for op=" + op;
    return json::object();
  }
  if (op == "focus") {
    if (gui_.focusSelectedPath(path)) return selectionJson();
    err = "focus requires an already-selected prim"; return json::object();
  }
  if (op == "group") {
    gui_.selectDescendants(path, /*addToSelection=*/false);
    if (gui_.selectionItems().empty()) {
      err = "select: no renderable prims under " + path;
      return json::object();
    }
    return selectionJson();
  }
  if (op == "set") {
    gui_.selectByPath(path, -1);
    return selectionJson();
  }
  if (op == "add" || op == "remove") {
    const bool already = gui_.isPathSelected(path);
    if ((op == "add" && already) || (op == "remove" && !already))
      return selectionJson();
    gui_.toggleSelectionItem({path, -1, -1});
    return selectionJson();
  }
  err = "select: unknown op '" + op + "'";
  return json::object();
}

json App::mcpListPrims(const json& args, std::string&) {
  size_t cap = 1000;
  if (args.contains("max") && args["max"].is_number_integer()) {
    const long long m = args["max"].get<long long>();
    if (m > 0) cap = static_cast<size_t>(m);
  }
  json paths = json::array();
  if (coreSession_) {
    auto snapshot = coreSession_->PublicSnapshot();
    lightusd::api::Stage stage;
    if (lightusd::api::DocumentSnapshotStage(snapshot, &stage) == LIGHTUSD_OK) {
      VisitPublicPrims(stage.get(), [&](lightusd_prim prim) {
        if (paths.size() >= cap) return false;
        paths.push_back(PublicString(lightusd_prim_path(prim)));
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
  if (name == "compare_panel") {
    const std::string op=args.value("op",std::string("open"));
    const char* directory=std::getenv("LUSDVIEW_CONTROL_DIR");
    if (!directory || !*directory) { err="launch through view_control to use the comparison panel"; return json::object(); }
    if (renderThreadActive_) { err="comparison panel requires non-threaded rendering"; return json::object(); }
    if (op=="open") {
      int mode=args.value("mode",0);
      if (mode<0 || mode>4) { err="mode must be 0..4"; return json::object(); }
      if (args.contains("window_size")) {
        const auto& size=args["window_size"];
        if (!size.is_array() || size.size()!=2 || !size[0].is_number_integer() || !size[1].is_number_integer() ||
            size[0].get<int>()<640 || size[0].get<int>()>4096 || size[1].get<int>()<480 || size[1].get<int>()>2160) {
          err="window_size must be [640..4096,480..2160]"; return json::object();
        }
        if (!window_) { err="window_size requires a native window; omit it for headless comparison"; return json::object(); }
        glfwSetWindowSize(window_,size[0].get<int>(),size[1].get<int>());
      }
      gui_.openCompare(mode);
    } else if (op=="window_capture") {
      mcpCompareWindowPath_=(std::filesystem::path(directory)/"lusdview-compare-window.png").string();
      mcpCompareWindowResult_={{"pending",true},{"path",mcpCompareWindowPath_}};
    } else if (op!="status") { err="unknown compare_panel operation"; return json::object(); }
    return {{"control_directory",directory},{"window_capture",mcpCompareWindowResult_}};
  }
  if (name == "frame_barrier") {
    const std::string token = args.value("token", std::string());
    if (token.empty() || token.size() > 128) { err = "barrier requires a token of 1..128 bytes"; return json::object(); }
    if (args.value("cancel", false)) {
      bool cancelled = false;
      if (mcpBarrierRequest_ == token) { mcpBarrierRequest_.clear(); mcpBarrierSubmitting_ = nullptr; cancelled = true; }
      if (!mcpBarrier_.is_null() && mcpBarrier_.value("token", std::string()) == token) {
        mcpBarrier_ = nullptr; mcpBarrierPixels_.clear(); cancelled = true;
      }
      return json{{"token", token}, {"cancelled", cancelled}};
    }
    if (renderThreadActive_ || rtPath_ || cpuInteractive_ || cudaInteractive_ || hipInteractive_ || stereoRequested_ || rasterMotionAccum_.valid) {
      err = "frame barriers currently require the non-threaded mono raster path"; return json::object();
    }
    if (!mcpBarrierRequest_.empty() && mcpBarrierRequest_ != token) {
      err = "a frame barrier is already pending"; return json::object();
    }
    if (!mcpBarrier_.is_null() && mcpBarrier_.value("token", std::string()) == token) return mcpBarrier_;
    mcpBarrierRequest_ = token;
    return json{{"token", token}, {"pending", true}};
  }
  if (name == "view_state") {
    const json viewport = mcpViewport(json{{"op", "get"}}, err);
    const json selection = mcpSelect(json{{"op", "list"}}, err);
    json paths = json::array();
    for (const auto& item : selection["items"]) paths.push_back(item["path"]);
    int width = 0, height = 0;
    if (renderer_) renderer_->viewportSize(&width, &height);
    return json{{"protocol_version", 1}, {"camera", viewport["camera"]},
                {"selection", paths}, {"focus", selection["focused"]}, {"timeline", mcpTimeline(json{{"op", "get"}}, err)}, {"display", mcpRenderSettings(json::object(), err)},
                {"scene", {{"loaded", loaded_.ok},
                           {"loading", loadActive_ || progressiveActive_ || reconvActive_ || (reconvHasRequest_ && reconvRequested_ != reconvApplied_)},
                           {"filepath", loaded_.filepath}, {"has_animation", hasAnimation_},
                           {"scene_generation", sceneGen_}, {"stage_revision", coreStageRevision_},
                           {"renderer_generation", rendererGeneration_}, {"window_generation", windowGeneration_},
                           {"backend", BackendName(backend_)}, {"ray_tracing", rtPath_}}}, {"size", {width, height}},
                {"frame_serial", mcpFrameSerial_}, {"rendered_time", mcpRenderedTime_}, {"rendered_camera", mcpRenderedCamera_}, {"barrier", mcpBarrier_}};
  }

  if (coreSession_) {
    auto document = coreSession_->PublicSnapshot();
    lightusd::api::Stage view;
    if (lightusd::api::DocumentSnapshotStage(document, &view) != LIGHTUSD_OK) {
      err = "core session has no published stage";
      return json();
    }
    if (name == "stage_info") {
      PublicStageInfo meta;
      if (!ReadPublicStageInfo(view.get(), &meta)) { err = lightusd::api::LastError(); return json(); }
      return json{{"loaded", true},
                  {"defaultPrim", meta.defaultPrim},
                  {"upAxis", meta.upAxis},
                  {"metersPerUnit", meta.metersPerUnit},
                  {"startTimeCode", meta.startTimeCode},
                  {"endTimeCode", meta.endTimeCode},
                  {"timeCodesPerSecond", meta.timeCodesPerSecond},
                  {"primCount", meta.primCount}};
    }

    if (name == "prim_list" || name == "query_prims_by_type" ||
        name == "search") {
      const std::string root = args.value("path", std::string("/"));
      const std::string type = args.value("type", std::string());
      const std::string query = args.value("query", std::string());
      json prims = json::array();
      VisitPublicPrims(view.get(), [&](lightusd_prim prim) {
        const std::string path = PublicString(lightusd_prim_path(prim));
        if (root != "/" && path != root &&
            path.compare(0, root.size() + 1, root + "/") != 0) {
          return true;
        }
        if (!type.empty() && PublicString(lightusd_prim_type_name(prim)) != type) return true;
        if (!query.empty() && path.find(query) == std::string::npos &&
            PublicString(lightusd_prim_name(prim)).find(query) == std::string::npos) {
          return true;
        }
        prims.push_back(json{{"path", path},
                             {"name", PublicString(lightusd_prim_name(prim))},
                             {"type", PublicString(lightusd_prim_type_name(prim))},
                             {"active", bool(lightusd_prim_is_active(prim))}});
        return true;
      });
      return json{{"path", root}, {"prims", prims}, {"count", prims.size()}};
    }

    if (!args.contains("path") || !args["path"].is_string()) {
      err = "Missing 'path' argument";
      return json::object();
    }
    const std::string path = args["path"].get<std::string>();
    const lightusd_prim prim = lightusd_stage_prim_at_path(view.get(), path.c_str());
    if (!lightusd_prim_is_valid(prim)) {
      err = "Prim not found: " + path;
      return json::object();
    }

    if (name == "inspect_prim") {
      for (const char* key : {"offset", "limit"}) {
        if (args.contains(key) && (!args[key].is_number_integer() || args[key].get<int64_t>() < 0)) {
          err = "offset/limit must be nonnegative integers"; return json::object();
        }
      }
      const size_t offset = args.value("offset", size_t(0));
      const size_t limit = args.value("limit", size_t(64));
      if (limit < 1 || limit > 256) { err = "limit must be in [1,256]"; return json::object(); }
      lightusd::api::StringList propertyNames, relationshipNames;
      if (lightusd_prim_property_names(prim, propertyNames.put()) != LIGHTUSD_OK ||
          lightusd_prim_relationship_names(prim, relationshipNames.put()) != LIGHTUSD_OK) {
        err = lightusd::api::LastError(); return json::object();
      }
      std::vector<std::string> names;
      for (const auto* list : {&propertyNames, &relationshipNames})
        for (size_t i = 0; i < lightusd::api::StringListSize(*list); ++i)
          names.push_back(PublicString(lightusd::api::StringListGet(*list, i)));
      std::sort(names.begin(), names.end());
      names.erase(std::unique(names.begin(), names.end()), names.end());
      json properties = json::array();
      const size_t begin = std::min(offset, names.size());
      const size_t end = begin + std::min(limit, names.size() - begin);
      for (size_t i = begin; i < end; ++i) {
        const std::string& property = names[i];
        const uint16_t flags = lightusd_prim_property_flags(prim, property.c_str());
        json row = {{"name", property}, {"path", path + "." + property},
                    {"flags", flags}, {"custom", bool(flags & LIGHTUSD_PROP_CUSTOM)}};
        const bool relationship = lightusd_prim_has_relationship(prim, property.c_str()) != 0;
        if (relationship) {
          row["kind"] = "relationship";
          const size_t count = lightusd_rel_target_count(prim, property.c_str());
          row["target_count"] = count;
          row["targets"] = json::array();
          for (size_t j = 0; j < std::min(count, size_t(128)); ++j)
            row["targets"].push_back(PublicString(lightusd_rel_target(prim, property.c_str(), j)));
        } else {
          row["kind"] = "attribute";
          row["type"] = PublicString(lightusd_prim_property_type_name(prim, property.c_str()));
          lightusd_value_view value{};
          lightusd_sv text{};
          const bool present = lightusd_attr_inspect_default(prim, property.c_str(), &value, &text) == LIGHTUSD_OK;
          row["has_default"] = present;
          if (present) {
            size_t length = std::min(text.len, size_t(4096));
            // Keep a bounded preview on a UTF-8 codepoint boundary.
            while (text.data && length > 0 && length < text.len &&
                   (static_cast<unsigned char>(text.data[length]) & 0xc0u) == 0x80u) --length;
            text.len = length;
            row["value_preview"] = PublicDefaultSummary(value, text);
            row["value"] = publicDefaultJson(value, text);
          }
          row["time_sample_count"] = lightusd_attr_timesample_count(prim, property.c_str());
          const size_t count = lightusd_attr_connection_count(prim, property.c_str());
          row["connection_count"] = count;
          row["connections"] = json::array();
          for (size_t j = 0; j < std::min(count, size_t(128)); ++j)
            row["connections"].push_back(PublicString(lightusd_attr_connection(prim, property.c_str(), j)));
        }
        properties.push_back(std::move(row));
      }
      return json{{"path", path}, {"type", PublicString(lightusd_prim_type_name(prim))},
                  {"active", bool(lightusd_prim_is_active(prim))}, {"properties", properties},
                  {"total", names.size()}, {"offset", offset},
                  {"next_offset", end < names.size() ? json(end) : json(nullptr)},
                  {"value_policy", "native resolved defaults; arrays report type/count without materialization"},
                  {"provenance_policy", "not exposed by the native public C API"}};
    }

    if (name == "prim_get") {
      lightusd::api::StringList names;
      if (lightusd_prim_property_names(prim, names.put()) != LIGHTUSD_OK) {
        err = lightusd::api::LastError(); return json::object();
      }
      return json{{"path", path},
                  {"name", PublicString(lightusd_prim_name(prim))},
                  {"type", PublicString(lightusd_prim_type_name(prim))},
                  {"active", bool(lightusd_prim_is_active(prim))},
                  {"propertyCount", lightusd::api::StringListSize(names)},
                  {"childCount", lightusd_prim_child_count(prim)}};
    }
    if (name == "variant_list_sets" || name == "variant_get_selection") {
      json sets = json::object();
      const size_t count = lightusd_prim_variant_set_count(prim);
      for (size_t i = 0; i < count; ++i) {
        const std::string set = PublicString(lightusd_prim_variant_set_name(prim, i));
        json variants = json::array();
        const size_t variantCount = lightusd_variant_count(prim, set.c_str());
        for (size_t j = 0; j < variantCount; ++j)
          variants.push_back(PublicString(lightusd_variant_name(prim, set.c_str(), j)));
        sets[set] = json{{"selection", PublicString(lightusd_variant_selection(prim, set.c_str()))},
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

    if (name == "attr_list") {
      lightusd::api::StringList names;
      if (lightusd_prim_property_names(prim, names.put()) != LIGHTUSD_OK) {
        err = lightusd::api::LastError(); return json::object();
      }
      json attributes = json::array();
      for (size_t i = 0; i < lightusd::api::StringListSize(names); ++i) {
        const auto attr = PublicString(lightusd::api::StringListGet(names, i));
        lightusd_value_view value{};
        const bool present = lightusd_attr_inspect_default(prim, attr.c_str(), &value, nullptr) == LIGHTUSD_OK;
        attributes.push_back(json{{"name", attr},
            {"type", present ? lightusd_type_name(value.type) : "unknown"}, {"hasValue", present}});
      }
      return json{{"path", path}, {"attributes", attributes}, {"count", attributes.size()}};
    }
    if (name == "attr_get") {
      const std::string attr = args.value("attr_name", std::string());
      if (attr.empty()) { err = "Missing 'attr_name' argument"; return json::object(); }
      lightusd_value_view value{};
      lightusd_sv text{};
      if (lightusd_attr_inspect_default(prim, attr.c_str(), &value, &text) != LIGHTUSD_OK) {
        err = "Attribute not found: " + attr;
        return json::object();
      }
      return json{{"path", path}, {"attr_name", attr}, {"value", publicDefaultJson(value, text)}};
    }

    err = "Tool is not available on the read-only core document: " + name;
    return json::object();
  }

  err = loaded_.ok ? "unknown tool: " + name : std::string("No scene loaded");
  return json::object();
}

}  // namespace lusdview
