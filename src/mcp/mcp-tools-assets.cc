// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// Layer, asset, screenshot and selection tools of the next MCP server. The
// asset/screenshot/selection tools keep the legacy server's contract; loaded
// layers are uncomposed C stage handles.
#include <string>

#include "c-stage-bridge.hh"
#include "mcp-tools.hh"
#include "mcp-util.hh"
#include "next/stage/stage.hh"

namespace lightusd {
namespace mcp {

namespace {

bool LoadLayer(Context &ctx, const json &args, const uint8_t *data, size_t size,
               const std::string *uri, json &result, std::string &err) {
  const std::string name = args["name"].get<std::string>();
  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  opts.composed = 0;  // a layer, not a composed stage
  lightusd_stage *stage = nullptr;
  const lightusd_status st =
      uri ? lightusd_stage_load(uri->c_str(), &opts, &stage)
          : lightusd_stage_load_from_memory(data, size, &opts, &stage);
  if (st != LIGHTUSD_OK) {
    err = std::string("Failed to load layer: ") + lightusd_last_error();
    return false;
  }
  std::string uuid = FindLayerUUID(ctx, name);
  if (uuid.empty()) uuid = GenerateUUID();
  LayerEntry entry;
  entry.name = name;
  entry.uri = uri ? *uri : name;
  entry.description = args.value("description", std::string{});
  entry.stage.reset(stage);
  ctx.layers[uuid] = std::move(entry);
  SetTextContent(result, uuid);
  return true;
}

// Layer entry by `uuid` or `name` argument.
LayerEntry *FindLayer(Context &ctx, const json &args, std::string &err) {
  std::string uuid = args.value("uuid", std::string{});
  const std::string name = args.value("name", std::string{});
  if (uuid.empty() && name.empty()) {
    err = "Either `name` or `uuid` arg required\n";
    return nullptr;
  }
  if (uuid.empty()) uuid = FindLayerUUID(ctx, name);
  auto it = ctx.layers.find(uuid);
  if (it == ctx.layers.end()) {
    err = "Layer not found: " + (uuid.empty() ? name : uuid);
    return nullptr;
  }
  return &it->second;
}

}  // namespace

bool LoadUSDLayerFromFile(Context &ctx, const json &args, json &result,
                          std::string &err) {
#if defined(__EMSCRIPTEN__)
  (void)ctx;
  (void)args;
  (void)result;
  err = "Loading from a file URI is not supported in this build";
  return false;
#else
  if (!args.contains("uri") || !args["uri"].is_string()) {
    err = "`uri` param not found.";
    return false;
  }
  if (!args.contains("name") || !args["name"].is_string()) {
    err = "`name` param not found.";
    return false;
  }
  const std::string uri = args["uri"].get<std::string>();
  return LoadLayer(ctx, args, nullptr, 0, &uri, result, err);
#endif
}

bool LoadUSDLayerFromData(Context &ctx, const json &args, json &result,
                          std::string &err) {
  if (!args.contains("data") || !args["data"].is_string()) {
    err = "`data` param not found.";
    return false;
  }
  if (!args.contains("name") || !args["name"].is_string()) {
    err = "`name` param not found.";
    return false;
  }
  std::string binary;
  if (!DecodeBase64(args["data"].get<std::string>(), &binary, &err)) return false;
  return LoadLayer(ctx, args, reinterpret_cast<const uint8_t *>(binary.data()),
                   binary.size(), nullptr, result, err);
}

bool ToUSDA(Context &ctx, const json &args, json &result, std::string &err) {
  if (!args.contains("name") || !args["name"].is_string()) {
    err = "`name` param not found.";
    return false;
  }
  LayerEntry *layer = FindLayer(ctx, args, err);
  if (!layer) return false;
  lightusd_string *s = nullptr;
  if (lightusd_stage_export_usda(layer->stage.stage, &s) != LIGHTUSD_OK) {
    err = std::string("export failed: ") + lightusd_last_error();
    return false;
  }
  const lightusd_sv v = lightusd_string_view(s);
  const std::string text(v.data ? v.data : "", v.len);
  lightusd_string_destroy(s);
  SetTextContent(result, text, "text/plain");
  return true;
}

bool ListPrimSpecs(Context &ctx, const json &args, json &result, std::string &err) {
  LayerEntry *layer = FindLayer(ctx, args, err);
  if (!layer) return false;
  result["content"] = json::array();
  for (const next::UsdPrim &p : NativeStage(layer->stage)->GetRootPrims()) {
    result["content"].push_back({{"type", "text"}, {"text", p.GetName()}});
  }
  return true;
}

bool DebugPrimSpecDump(Context &ctx, const json &args, json &result, std::string &err) {
  const std::string path = args.value("path", std::string{});
  if (path.empty()) {
    err = "`path` arg required\n";
    return false;
  }
  LayerEntry *layer = FindLayer(ctx, args, err);
  if (!layer) return false;
  const next::UsdPrim prim = NativeStage(layer->stage)->GetPrimAtPath(path);
  if (!prim) {
    err = "PrimSpec not found at " + path;
    return false;
  }
  int max_depth = -1;
  if (args.contains("max_depth") && args["max_depth"].is_number_integer()) {
    max_depth = args["max_depth"].get<int>();
  }
  json dump = PrimToJSON(prim, layer->stage.stage, max_depth, true);
  dump["metadata"] = PrimMetaToJSON(prim);
  SetTextContent(result, dump.dump(2), "application/json");
  return true;
}

bool StoreAsset(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err) {
  if (!args.contains("data")) {
    err = "`data` param not found.";
    return false;
  }
  if (!args.contains("name")) {
    err = "`name` param not found.";
    return false;
  }

  std::string name = args["name"];
  const std::string &data = args["data"];
  std::string description = args.value("description", std::string{});

  if (data.size() > kMaxBase64InputBytes) {
    err = "`data` payload exceeds size limit.";
    return false;
  }

  std::string uuid = GenerateUUID();

  Asset asset;
  asset.name = name;
  asset.data = data;
  asset.description = description;
  asset.uuid = uuid;

  if (args.contains("preview") && args["preview"].is_object()) {
    const auto &preview = args["preview"];
    if (preview.contains("data") && preview.contains("mimeType")) {
      std::string preview_data = preview["data"];
      if (preview_data.size() > kMaxBase64InputBytes) {
        err = "`preview.data` payload exceeds size limit.";
        return false;
      }
      asset.preview.data = std::move(preview_data);
      asset.preview.mimeType = preview["mimeType"];
      if (preview.contains("name")) {
        asset.preview.name = preview["name"];
      }
    }
  }

  if (args.contains("pivot_position") && args["pivot_position"].is_array() && args["pivot_position"].size() == 3) {
    asset.pivot_position[0] = args["pivot_position"][0];
    asset.pivot_position[1] = args["pivot_position"][1];
    asset.pivot_position[2] = args["pivot_position"][2];
  }
  if (args.contains("bmin") && args["bmin"].is_array() && args["bmin"].size() == 3) {
    asset.bmin[0] = args["bmin"][0];
    asset.bmin[1] = args["bmin"][1];
    asset.bmin[2] = args["bmin"][2];
  }
  if (args.contains("bmax") && args["bmax"].is_array() && args["bmax"].size() == 3) {
    asset.bmax[0] = args["bmax"][0];
    asset.bmax[1] = args["bmax"][1];
    asset.bmax[2] = args["bmax"][2];
  }

  ctx.assets.insert_or_assign(name, std::move(asset));

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = uuid;

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}

bool ReadAsset(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
               std::string &err) {
  if (!args.contains("name")) {
    err = "`name` param not found.";
    return false;
  }

  std::string name = args["name"];
  int instance_id = -1;
  if (args.contains("instance_id")) {
    instance_id = args["instance_id"];
  }

  AssetSelection asset_selection;
  bool found_selection = false;

  for (const auto &selection : ctx.selected_assets) {
    if (selection.asset_name == name &&
        (instance_id == -1 || selection.instance_id == instance_id)) {
      asset_selection = selection;
      found_selection = true;
      break;
    }
  }

  if (!found_selection) {
    err = "Asset selection not found for name: " + name;
    return false;
  }

  if (!ctx.assets.count(asset_selection.asset_name)) {
    err = "Asset not found: " + name;
    return false;
  }

  const Asset &asset = ctx.assets.at(asset_selection.asset_name);

  nlohmann::json asset_data;
  asset_data["name"] = asset.name;
  asset_data["data"] = asset.data;
  asset_data["description"] = asset.description;
  asset_data["uuid"] = asset.uuid;

  asset_data["instance_id"] = asset_selection.instance_id;
  asset_data["position"] = nlohmann::json::array(
      {asset_selection.position[0], asset_selection.position[1],
       asset_selection.position[2]});
  asset_data["scale"] = nlohmann::json::array(
      {asset_selection.scale[0], asset_selection.scale[1],
       asset_selection.scale[2]});
  asset_data["rotation"] = nlohmann::json::array(
      {asset_selection.rotation[0], asset_selection.rotation[1],
       asset_selection.rotation[2]});

  asset_data["pivot_position"] = nlohmann::json::array(
      {asset.pivot_position[0], asset.pivot_position[1],
       asset.pivot_position[2]});
  asset_data["bmin"] = nlohmann::json::array(
      {asset.bmin[0], asset.bmin[1], asset.bmin[2]});
  asset_data["bmax"] = nlohmann::json::array(
      {asset.bmax[0], asset.bmax[1], asset.bmax[2]});

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = asset_data.dump();

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}

bool ReadAssetPreview(Context &ctx, const nlohmann::json &args,
                      nlohmann::json &result, std::string &err) {
  if (!args.contains("name")) {
    err = "`name` param not found.";
    return false;
  }

  std::string name = args["name"];

  if (!ctx.assets.count(name)) {
    err = "Asset not found: " + name;
    return false;
  }

  const auto &asset = ctx.assets.at(name);

  if (asset.preview.data.empty()) {
    err = "Asset '" + name + "' has no preview image\n";
    return false;
  }

  result["content"] = nlohmann::json::array();

  nlohmann::json content;
  content["type"] = "image";
  content["data"] = asset.preview.data;
  content["mimeType"] = asset.preview.mimeType;

  result["content"].push_back(content);

  return true;
}

bool GetAssetDescription(Context &ctx, const nlohmann::json &args,
                         nlohmann::json &result, std::string &err) {
  if (!args.contains("name")) {
    err = "`name` param not found.";
    return false;
  }

  std::string name = args.at("name");
  bool include_preview = false;
  if (args.contains("include_preview")) {
    include_preview = args["include_preview"];
  }

  if (!ctx.assets.count(name)) {
    err = "Asset not found: " + name;
    return false;
  }

  const auto &asset = ctx.assets.at(name);

  nlohmann::json asset_info;
  asset_info["name"] = name;
  asset_info["asset_name"] = asset.name;
  asset_info["description"] = asset.description;
  asset_info["uuid"] = asset.uuid;

  asset_info["pivot_position"] = nlohmann::json::array(
      {asset.pivot_position[0], asset.pivot_position[1],
       asset.pivot_position[2]});
  asset_info["bmin"] = nlohmann::json::array(
      {asset.bmin[0], asset.bmin[1], asset.bmin[2]});
  asset_info["bmax"] = nlohmann::json::array(
      {asset.bmax[0], asset.bmax[1], asset.bmax[2]});

  if (include_preview && !asset.preview.data.empty()) {
    asset_info["preview"] = nlohmann::json::object();
    asset_info["preview"]["data"] = asset.preview.data;
    asset_info["preview"]["mimeType"] = asset.preview.mimeType;
    if (!asset.preview.name.empty()) {
      asset_info["preview"]["name"] = asset.preview.name;
    }
  }

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = asset_info.dump();

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}

bool GetAllAssetDescriptions(Context &ctx, const nlohmann::json &args,
                             nlohmann::json &result, std::string &err) {
  (void)args;
  (void)err;

  bool include_preview = false;
  if (args.contains("include_preview")) {
    include_preview = args["include_preview"];
  }

  result["content"] = nlohmann::json::array();

  for (const auto &it : ctx.assets) {
    nlohmann::json asset_info;
    asset_info["name"] = it.first;
    asset_info["asset_name"] = it.second.name;
    asset_info["description"] = it.second.description;
    asset_info["uuid"] = it.second.uuid;

    asset_info["pivot_position"] = nlohmann::json::array(
        {it.second.pivot_position[0], it.second.pivot_position[1],
         it.second.pivot_position[2]});
    asset_info["bmin"] = nlohmann::json::array(
        {it.second.bmin[0], it.second.bmin[1], it.second.bmin[2]});
    asset_info["bmax"] = nlohmann::json::array(
        {it.second.bmax[0], it.second.bmax[1], it.second.bmax[2]});

    if (include_preview && !it.second.preview.data.empty()) {
      asset_info["preview"] = nlohmann::json::object();
      asset_info["preview"]["data"] = it.second.preview.data;
      asset_info["preview"]["mimeType"] = it.second.preview.mimeType;
      if (!it.second.preview.name.empty()) {
        asset_info["preview"]["name"] = it.second.preview.name;
      }
    }

    nlohmann::json content;
    content["type"] = "text";
    content["text"] = asset_info.dump();

    result["content"].push_back(content);
  }

  return true;
}

bool ListScreenshots(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err) {
  (void)args;
  (void)err;

  result["content"] = nlohmann::json::array();

  for (const auto &it : ctx.screenshots) {
    nlohmann::json content;
    content["type"] = "text";
    content["text"] = it.first;
    result["content"].push_back(content);
  }

  return true;
}

bool SaveScreenshot(Context &ctx, const nlohmann::json &args,
                    nlohmann::json &result, std::string &err) {
  if (!args.contains("name")) {
    err = "`name` param not found.";
    return false;
  }
  if (!args.contains("data")) {
    err = "`data` param not found.";
    return false;
  }
  if (!args.contains("mimeType")) {
    err = "`mimeType` param not found.";
    return false;
  }

  std::string name = args["name"];
  std::string data = args["data"];
  if (data.size() > kMaxBase64InputBytes) {
    err = "`data` payload exceeds size limit.";
    return false;
  }
  std::string mimeType = args["mimeType"];

  Screenshot screenshot;
  screenshot.uuid = GenerateUUID();
  screenshot.data = data;
  screenshot.mimeType = mimeType;

  ctx.screenshots[name] = screenshot;

  result["content"] = nlohmann::json::array();

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = screenshot.uuid;
  result["content"].push_back(content);

  return true;
}

bool ReadScreenshot(Context &ctx, const nlohmann::json &args,
                    nlohmann::json &result, std::string &err) {
  if (!args.contains("name")) {
    err = "`name` param not found.";
    return false;
  }

  std::string name = args["name"];

  if (!ctx.screenshots.count(name)) {
    err = "Screenshot not found: " + name;
    return false;
  }

  const auto &screenshot = ctx.screenshots.at(name);

  result["content"] = nlohmann::json::array();

  nlohmann::json content;
  content["type"] = "image";
  content["data"] = screenshot.data;
  content["mimeType"] = screenshot.mimeType;

  content["annotations"] = nlohmann::json::object();
  content["annotations"]["audience"] = nlohmann::json::array();
  content["annotations"]["audience"].push_back("user");
  content["annotations"]["priority"] = 0.9;

  result["content"].push_back(content);

  return true;
}

bool GetUSDDescription(Context &ctx, const nlohmann::json &args,
                       nlohmann::json &result, std::string &err) {
  if (!args.contains("name")) {
    err = "`name` param not found.";
    return false;
  }

  std::string name = args.at("name");

  std::string uuid = FindLayerUUID(ctx, name);

  if (!ctx.layers.count(uuid)) {
    err = "Internal error. No corresponding Layer found\n";
    return false;
  }

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = ctx.layers.at(uuid).description;

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}

bool GetAllUSDDescriptions(Context &ctx, const nlohmann::json &args,
                           nlohmann::json &result, std::string &err) {
  (void)args;
  (void)err;

  result["content"] = nlohmann::json::array();

  for (const auto &it : ctx.layers) {
    nlohmann::json content;
    content["type"] = "text";
    content["text"] = it.second.name + ":" + it.second.description;

    result["content"].push_back(content);
  }

  return true;
}

bool SelectAssets(Context &ctx, const nlohmann::json &args,
                  nlohmann::json &result, std::string &err) {
  if (!args.contains("assets")) {
    err = "`assets` param not found.";
    return false;
  }

  if (!args["assets"].is_array()) {
    err = "`assets` must be an array.";
    return false;
  }

  const auto &assets = args["assets"];

  ctx.selected_assets.clear();

  for (const auto &asset_obj : assets) {
    if (!asset_obj.is_object()) {
      err = "Each asset must be an object.";
      return false;
    }

    if (!asset_obj.contains("name")) {
      err = "Asset object must contain 'name' field.";
      return false;
    }

    std::string name = asset_obj["name"];

    if (!ctx.assets.count(name)) {
      err = "Asset not found: " + name;
      return false;
    }

    int instance_id = 0;
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> rotation = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> pivot_position = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> bmin = {-1.0f, -1.0f, -1.0f};
    std::array<float, 3> bmax = {1.0f, 1.0f, 1.0f};

    if (asset_obj.contains("instance_id") &&
        asset_obj["instance_id"].is_number_integer()) {
      instance_id = asset_obj["instance_id"];
    }

    if (asset_obj.contains("position") &&
        asset_obj["position"].is_array() &&
        asset_obj["position"].size() == 3) {
      position[0] = asset_obj["position"][0];
      position[1] = asset_obj["position"][1];
      position[2] = asset_obj["position"][2];
    }

    if (asset_obj.contains("scale") && asset_obj["scale"].is_array() &&
        asset_obj["scale"].size() == 3) {
      scale[0] = asset_obj["scale"][0];
      scale[1] = asset_obj["scale"][1];
      scale[2] = asset_obj["scale"][2];
    }

    if (asset_obj.contains("rotation") &&
        asset_obj["rotation"].is_array() &&
        asset_obj["rotation"].size() == 3) {
      rotation[0] = asset_obj["rotation"][0];
      rotation[1] = asset_obj["rotation"][1];
      rotation[2] = asset_obj["rotation"][2];
    }

    if (asset_obj.contains("pivot_position") &&
        asset_obj["pivot_position"].is_array() &&
        asset_obj["pivot_position"].size() == 3) {
      pivot_position[0] = asset_obj["pivot_position"][0];
      pivot_position[1] = asset_obj["pivot_position"][1];
      pivot_position[2] = asset_obj["pivot_position"][2];
    }

    if (asset_obj.contains("bmin") && asset_obj["bmin"].is_array() &&
        asset_obj["bmin"].size() == 3) {
      bmin[0] = asset_obj["bmin"][0];
      bmin[1] = asset_obj["bmin"][1];
      bmin[2] = asset_obj["bmin"][2];
    }

    if (asset_obj.contains("bmax") && asset_obj["bmax"].is_array() &&
        asset_obj["bmax"].size() == 3) {
      bmax[0] = asset_obj["bmax"][0];
      bmax[1] = asset_obj["bmax"][1];
      bmax[2] = asset_obj["bmax"][2];
    }

    AssetSelection selection;
    selection.asset_name = name;
    selection.instance_id = instance_id;
    selection.position = position;
    selection.scale = scale;
    selection.rotation = rotation;
    ctx.selected_assets.push_back(selection);
  }

  result["content"] = nlohmann::json::array();

  return true;
}

bool GetSelectedAssets(Context &ctx, const nlohmann::json &args,
                       nlohmann::json &result, std::string &err) {
  (void)err;
  (void)args;

  result["content"] = nlohmann::json::array();
  for (const auto &selection : ctx.selected_assets) {
    nlohmann::json asset_info;
    asset_info["name"] = selection.asset_name;
    asset_info["instance_id"] = selection.instance_id;

    nlohmann::json content;
    content["type"] = "text";
    content["text"] = asset_info.dump();
    result["content"].push_back(content);
  }

  return true;
}

}  // namespace mcp
}  // namespace lightusd
