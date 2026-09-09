#include "mcp-tools.hh"

#include <limits>
#include <string>

#include "mcp-context.hh"
#include "mcp-server.hh"
#include "mcp-tools-scene.hh"
#include "mcp-tools-query.hh"
#include "mcp-tools-composition.hh"
#include "mcp-tools-validate.hh"
#include "mcp-tools-usdz.hh"
#include "mcp-tools-diff.hh"
#include "mcp-js-bridge.hh"
#include "pprinter.hh"
#include "layer.hh"
#include "security-policy.hh"
#include "str-util.hh"
#include "lightusd.hh"
#include "uuid-gen.hh"
#include "value-to-json.hh"
#include "common-macros.inc"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

#include "external/jsonhpp/nlohmann/json.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace lightusd {
namespace tydra {
namespace mcp {

namespace {

// ---------------------------------------------------------------------------
// Base64 decode helper (kept from original)
// ---------------------------------------------------------------------------
inline bool decode_data(const std::string &data, std::string *binary,
                        std::string *err) {
  if (!binary) {
    if (err) (*err) = "Internal error: null output buffer for decode_data.";
    return false;
  }

  if (data.size() > security_policy::kMCPMaxBase64InputBytes) {
    if (err) (*err) = "Input base64 payload is too large.";
    return false;
  }

  size_t decoded_size = 0;
  if (!security_policy::EstimateBase64DecodedSize(data, &decoded_size)) {
    if (err) (*err) = "Invalid base64 data.";
    return false;
  }

  if (decoded_size > security_policy::kMCPMaxBase64DecodedBytes) {
    if (err) (*err) = "Decoded payload exceeds size limit.";
    return false;
  }

  (*binary) = base64_decode(data);
  if ((*binary).size() != decoded_size) {
    if (err) (*err) = "Invalid base64 data.";
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// UUID lookup helper
// ---------------------------------------------------------------------------
static std::string FindUUID(
    const std::string &name,
    const lightusd::HashMap<std::string, USDLayer> &layers) {
  for (const auto &it : layers) {
    if (it.second.name == name) {
      return it.first;
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
// Forward declarations for existing tool implementations
// (kept from the original file structure)
// ---------------------------------------------------------------------------
bool GetVersion(nlohmann::json &result);
bool GetUSDDescription(Context &ctx, const nlohmann::json &args,
                       nlohmann::json &result, std::string &err);
bool GetAllUSDDescriptions(Context &ctx, const nlohmann::json &args,
                            nlohmann::json &result, std::string &err);
#if !defined(__EMSCRIPTEN__)
bool LoadUSDLayerFromFile(Context &ctx, const nlohmann::json &args,
                           nlohmann::json &result, std::string &err);
#endif
bool LoadUSDLayerFromData(Context &ctx, const nlohmann::json &args,
                           nlohmann::json &result, std::string &err);
bool StoreAsset(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err);
bool ReadAsset(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
               std::string &err);
bool ReadAssetPreview(Context &ctx, const nlohmann::json &args,
                       nlohmann::json &result, std::string &err);
bool GetAllAssetDescriptions(Context &ctx, const nlohmann::json &args,
                              nlohmann::json &result, std::string &err);
bool GetAssetDescription(Context &ctx, const nlohmann::json &args,
                          nlohmann::json &result, std::string &err);
bool ListPrimSpecs(Context &ctx, const nlohmann::json &args,
                    nlohmann::json &result, std::string &err);
bool DebugPrimSpecDump(Context &ctx, const nlohmann::json &args,
                       nlohmann::json &result, std::string &err);
bool ToUSDA(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
            std::string &err);
bool SaveScreenshot(Context &ctx, const nlohmann::json &args,
                    nlohmann::json &result, std::string &err);
bool ListScreenshots(Context &ctx, const nlohmann::json &args,
                     nlohmann::json &result, std::string &err);
bool ReadScreenshot(Context &ctx, const nlohmann::json &args,
                    nlohmann::json &result, std::string &err);
bool SelectAssets(Context &ctx, const nlohmann::json &args,
                  nlohmann::json &result, std::string &err);
bool GetSelectedAssets(Context &ctx, const nlohmann::json &args,
                        nlohmann::json &result, std::string &err);

// ===========================================================================
// Legacy tool implementations (preserved from original MCP server)
// ===========================================================================

bool GetVersion(nlohmann::json &result) {
  std::string ver_str = std::to_string(lightusd::version_major) + "." +
                        std::to_string(lightusd::version_minor) + "." +
                        std::to_string(lightusd::version_micro);
  std::string rev = lightusd::version_rev;

  if (rev.size()) {
    ver_str += "." + rev;
  }

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = ver_str;

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}

#if !defined(__EMSCRIPTEN__)
bool LoadUSDLayerFromFile(Context &ctx, const nlohmann::json &args,
                          nlohmann::json &result, std::string &err) {
  DCOUT("args " << args);
  if (!args.contains("uri")) {
    DCOUT("uri param not found");
    err = "`uri` param not found.";
    return false;
  }

  if (!args.contains("name")) {
    DCOUT("name param not found");
    err = "`name` param not found.";
    return false;
  }

  std::string uri = args["uri"];
  std::string name = args["name"];
  std::string description = args.value("description", std::string{});

  Layer layer;
  std::string warn;
  USDLoadOptions options;
  if (!LoadLayerFromFile(uri, &layer, &warn, &err, options)) {
    DCOUT("Failed to load layer from file: " << err);
    err = "Failed to load layer from file: " + err + "\n";
    return false;
  }

  if (!warn.empty()) {
    result["warnings"] = warn;
  }

  std::string uuid = FindUUID(name, ctx.layers);

  if (uuid.empty()) {
    uuid = generateUUID();
  }

  USDLayer usd_layer;
  usd_layer.uri = uri;
  usd_layer.name = name;
  usd_layer.layer = std::move(layer);
  usd_layer.description = description;

  ctx.layers.insert_or_assign(uuid, std::move(usd_layer));

  DCOUT("loaded USD as Layer");

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = uuid;

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}
#endif

bool LoadUSDLayerFromData(Context &ctx, const nlohmann::json &args,
                          nlohmann::json &result, std::string &err) {
  DCOUT("args " << args);
  if (!args.contains("data")) {
    DCOUT("data param not found");
    err = "`data` param not found.";
    return false;
  }
  if (!args.contains("name")) {
    DCOUT("name param not found");
    err = "`name` param not found.";
    return false;
  }

  std::string name = args["name"];
  const std::string &data = args["data"];
  std::string description = args.value("description", std::string{});

  std::string binary;
  if (!decode_data(data, &binary, &err)) {
    return false;
  }

  Layer layer;
  std::string warn;
  USDLoadOptions options;
  if (!LoadLayerFromMemory(reinterpret_cast<const uint8_t *>(binary.c_str()),
                           binary.size(), name, &layer, &warn, &err, options)) {
    DCOUT("Failed to load layer from Data: " << err);
    err = "Failed to load layer from Data: " + err + "\n";
    return false;
  }

  if (!warn.empty()) {
    result["warnings"] = warn;
  }

  std::string uuid = FindUUID(name, ctx.layers);

  if (uuid.empty()) {
    uuid = generateUUID();
  }

  USDLayer usd_layer;
  usd_layer.name = name;
  usd_layer.uri = name;
  usd_layer.description = description;
  usd_layer.layer = std::move(layer);

  ctx.layers.insert_or_assign(uuid, std::move(usd_layer));

  DCOUT("loaded USD as Layer");

  nlohmann::json content;
  content["type"] = "text";
  content["text"] = uuid;

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}

bool StoreAsset(Context &ctx, const nlohmann::json &args,
                nlohmann::json &result, std::string &err) {
  DCOUT("args " << args);
  if (!args.contains("data")) {
    DCOUT("data param not found");
    err = "`data` param not found.";
    return false;
  }
  if (!args.contains("name")) {
    DCOUT("name param not found");
    err = "`name` param not found.";
    return false;
  }

  std::string name = args["name"];
  const std::string &data = args["data"];
  std::string description = args.value("description", std::string{});

  if (data.size() > security_policy::kMCPMaxBase64InputBytes) {
    err = "`data` payload exceeds size limit.";
    return false;
  }

  std::string uuid = generateUUID();

  MCPAsset asset;
  asset.name = name;
  asset.data = data;
  asset.description = description;
  asset.uuid = uuid;

  if (args.contains("preview") && args["preview"].is_object()) {
    const auto &preview = args["preview"];
    if (preview.contains("data") && preview.contains("mimeType")) {
      std::string preview_data = preview["data"];
      if (preview_data.size() > security_policy::kMCPMaxBase64InputBytes) {
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
  DCOUT("args " << args);
  if (!args.contains("name")) {
    DCOUT("name param not found");
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

  const MCPAsset &asset = ctx.assets.at(asset_selection.asset_name);

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
  DCOUT("args " << args);
  if (!args.contains("name")) {
    DCOUT("name param not found");
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
  DCOUT("args " << args);
  if (!args.contains("name")) {
    DCOUT("name param not found");
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

bool ListPrimSpecs(Context &ctx, const nlohmann::json &args,
                   nlohmann::json &result, std::string &err) {
  DCOUT("args " << args);

  std::string uuid = args.value("uuid", std::string{});
  std::string name = args.value("name", std::string{});

  if (uuid.empty() && name.empty()) {
    err = "Either `name` or `uuid` arg required\n";
    return false;
  }

  if (uuid.empty()) {
    uuid = FindUUID(name, ctx.layers);
  }

  if (!ctx.layers.count(uuid)) {
    DCOUT("Layer not found: " << uuid);
    err = "Layer not found: " + uuid;
    return false;
  }

  const USDLayer &usd_layer = ctx.layers.at(uuid);

  result["content"] = nlohmann::json::array();

  for (const auto &ps : usd_layer.layer.primspecs()) {
    nlohmann::json content;
    content["type"] = "text";
    content["text"] = ps.first;
    result["content"].push_back(content);
  }

  return true;
}

bool DebugPrimSpecDump(Context &ctx, const nlohmann::json &args,
                       nlohmann::json &result, std::string &err) {
  DCOUT("args " << args);

  std::string uuid = args.value("uuid", std::string{});
  std::string name = args.value("name", std::string{});
  std::string path = args.value("path", std::string{});
  uint32_t max_depth = 1;
  if (args.contains("max_depth") && args["max_depth"].is_number_integer()) {
    int depth = args["max_depth"];
    if (depth >= 0) {
      max_depth = static_cast<uint32_t>(depth);
    }
  }

  if (uuid.empty() && name.empty()) {
    err = "Either `name` or `uuid` arg required\n";
    return false;
  }
  if (path.empty()) {
    err = "`path` arg required\n";
    return false;
  }
  if (uuid.empty()) {
    uuid = FindUUID(name, ctx.layers);
  }
  if (!ctx.layers.count(uuid)) {
    err = "Layer not found: " + uuid;
    return false;
  }

  Path prim_path(path, "");
  if (!prim_path.is_valid()) {
    err = "Invalid prim path: " + path;
    return false;
  }

  const PrimSpec *ps = nullptr;
  std::string find_err;
  const Layer &layer = ctx.layers.at(uuid).layer;
  if (!layer.find_primspec_at(prim_path, &ps, &find_err) || !ps) {
    err = "PrimSpec not found at " + path;
    if (!find_err.empty()) {
      err += ": " + find_err;
    }
    return false;
  }

  nlohmann::json content;
  content["type"] = "text";
  content["mimeType"] = "application/json";
    content["text"] = PrimSpecToMiniJSON(*ps, max_depth).dump(2);

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

  return true;
}

bool ToUSDA(Context &ctx, const nlohmann::json &args, nlohmann::json &result,
            std::string &err) {
  DCOUT("args " << args);
  if (!args.contains("name")) {
    DCOUT("name param not found");
    err = "`name` param not found.";
    return false;
  }

  std::string name = args.at("name");

  std::string uuid = FindUUID(name, ctx.layers);

  if (!ctx.layers.count(uuid)) {
    err = "Internal error. No corresponding Layer found\n";
    return false;
  }

  nlohmann::json content;
  content["type"] = "text";
  content["mimeType"] = "text/plain";

  const Layer &layer = ctx.layers.at(uuid).layer;
  std::string str = to_string(layer);
  content["text"] = str;

  result["content"] = nlohmann::json::array();
  result["content"].push_back(content);

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
  DCOUT("args " << args);
  if (!args.contains("name")) {
    DCOUT("name param not found");
    err = "`name` param not found.";
    return false;
  }
  if (!args.contains("data")) {
    DCOUT("data param not found");
    err = "`data` param not found.";
    return false;
  }
  if (!args.contains("mimeType")) {
    DCOUT("mimeType param not found");
    err = "`mimeType` param not found.";
    return false;
  }

  std::string name = args["name"];
  std::string data = args["data"];
  if (data.size() > security_policy::kMCPMaxBase64InputBytes) {
    err = "`data` payload exceeds size limit.";
    return false;
  }
  std::string mimeType = args["mimeType"];

  Screenshot screenshot;
  screenshot.uuid = generateUUID();
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
  DCOUT("args " << args);
  if (!args.contains("name")) {
    DCOUT("name param not found");
    err = "`name` param not found.";
    return false;
  }

  std::string name = args["name"];

  if (!ctx.screenshots.count(name)) {
    DCOUT("Screenshot not found: " << name);
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
  DCOUT("args " << args);
  if (!args.contains("name")) {
    DCOUT("name param not found");
    err = "`name` param not found.";
    return false;
  }

  std::string name = args.at("name");

  std::string uuid = FindUUID(name, ctx.layers);

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
    DCOUT("assets param not found");
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
  DCOUT("args " << args);

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

} // namespace

// ===========================================================================
// GetToolsList
// ===========================================================================
bool GetToolsList(Context &ctx, nlohmann::json &result) {
  (void)ctx;
  result["tools"] = nlohmann::json::array();

  // Helper to add a tool definition
  auto add_tool = [&](const std::string &name, const std::string &desc,
                       nlohmann::json schema) {
    nlohmann::json j;
    j["name"] = name;
    j["description"] = desc;
    j["inputSchema"] = schema;
    result["tools"].push_back(j);
  };

  auto str_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "string"}, {"description", desc}};
  };
  auto num_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "number"}, {"description", desc}};
  };
  auto bool_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "boolean"}, {"description", desc}};
  };
  auto int_prop = [](const std::string &desc) -> nlohmann::json {
    return {{"type", "integer"}, {"description", desc}};
  };

  // =========================================================================
  // Utility
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("get_version", "Get LightUSD MCP server version", schema);
  }

  // =========================================================================
  // Stage tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    schema["properties"]["upAxis"] = str_prop("Up axis (X, Y, or Z)");
    schema["properties"]["defaultPrim"] = str_prop("Default prim name");
    schema["properties"]["metersPerUnit"] = num_prop("Meters per unit");
    add_tool("stage_new", "Create a new empty USD stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("USD file path to load");
    schema["properties"]["options"] = {{"type", "object"},
                                        {"description", "Load options"}};
    schema["required"] = nlohmann::json::array({"uri"});
    add_tool("stage_load", "Load a USD file into the session stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded USD data");
    schema["properties"]["format"] = str_prop("Format hint: auto, usda, usdc");
    schema["required"] = nlohmann::json::array({"data"});
    add_tool("stage_load_data", "Load USD from base64 data", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["format"] =
        str_prop("Export format: usda (default), usdc, usdz");
    add_tool("stage_to_string",
             "Export the current stage to a USDA/USDC string", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("stage_info", "Get stage metadata (upAxis, prims, etc.)", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("Output file path");
    schema["properties"]["format"] = str_prop("Export format: usda, usdc, usdz");
    schema["required"] = nlohmann::json::array({"uri"});
    add_tool("stage_export", "Export stage to a file", schema);
  }

  // =========================================================================
  // Scene graph tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path (default: /)");
    schema["properties"]["max_depth"] =
        int_prop("Max recursion depth (-1 = unlimited)");
    schema["properties"]["include_attributes"] =
        bool_prop("Include attribute definitions");
    add_tool("prim_list",
             "List prims at or under a path in the scene graph", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path (e.g. /World/mesh0)");
    schema["properties"]["include_attributes"] =
        bool_prop("Include attribute details");
    schema["properties"]["include_metadata"] =
        bool_prop("Include prim metadata");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("prim_get", "Get full details of a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Path for the new prim");
    schema["properties"]["type_name"] = str_prop("USD prim type (e.g. Xform, Mesh)");
    schema["properties"]["specifier"] =
        str_prop("Specifier: def (default), over, or class");
    schema["required"] = nlohmann::json::array({"path", "type_name"});
    add_tool("prim_create", "Create a new prim in the stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Path of prim to remove");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("prim_remove", "Remove a prim from the stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Path of prim to rename");
    schema["properties"]["new_name"] = str_prop("New element name");
    schema["required"] = nlohmann::json::array({"path", "new_name"});
    add_tool("prim_rename", "Rename a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("prim_get_metadata", "Get prim metadata (kind, active, etc.)",
             schema);
  }

  // =========================================================================
  // Attribute tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("attr_list", "List attributes on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["attr_name"] = str_prop("Attribute name");
    schema["properties"]["time"] = num_prop("Optional time code for sampled attributes");
    schema["required"] = nlohmann::json::array({"path", "attr_name"});
    add_tool("attr_get", "Get an attribute value (with optional time code)",
             schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["attr_name"] = str_prop("Attribute name");
    schema["properties"]["value"] = {
        {"type", "object"},
        {"description",
         "Value as {type: string, value: any} (see structured JSON format)"}};
    schema["required"] = nlohmann::json::array({"path", "attr_name", "value"});
    add_tool("attr_set", "Set an attribute value", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["attr_name"] = str_prop("Attribute name to block");
    schema["required"] = nlohmann::json::array({"path", "attr_name"});
    add_tool("attr_block", "Block (set to None) an attribute", schema);
  }

  // =========================================================================
  // Composition tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["asset_path"] = str_prop("Referenced USD file path");
    schema["properties"]["prim_path"] =
        str_prop("Prim path within the referenced file");
    schema["properties"]["offset"] = num_prop("Layer offset");
    schema["properties"]["scale"] = num_prop("Layer time scale");
    schema["required"] =
        nlohmann::json::array({"path", "asset_path"});
    add_tool("reference_add", "Add a reference arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("reference_list", "List references on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("reference_clear", "Clear all references on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["asset_path"] = str_prop("Payload USD file path");
    schema["required"] =
        nlohmann::json::array({"path", "asset_path"});
    add_tool("payload_add", "Add a payload arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("payload_list", "List payload arcs on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["prim_path"] = str_prop("Prim path to inherit from");
    schema["required"] = nlohmann::json::array({"path", "prim_path"});
    add_tool("inherit_add", "Add an inherit arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["prim_path"] = str_prop("Prim path to specialize from");
    schema["required"] = nlohmann::json::array({"path", "prim_path"});
    add_tool("specialize_add", "Add a specialize arc to a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("variant_list_sets",
             "List variant sets and their selections on a prim", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["variant_set"] = str_prop("Variant set name");
    schema["required"] =
        nlohmann::json::array({"path", "variant_set"});
    add_tool("variant_get_selection",
             "Get the current variant selection for a variant set", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["variant_set"] = str_prop("Variant set name");
    schema["properties"]["variant"] = str_prop("Variant name to select");
    schema["required"] =
        nlohmann::json::array({"path", "variant_set", "variant"});
    add_tool("variant_set_selection",
             "Set the variant selection for a variant set", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path");
    schema["properties"]["variant_set"] = str_prop("Variant set name");
    schema["properties"]["variant_name"] = str_prop("Variant name to define");
    schema["required"] =
        nlohmann::json::array({"path", "variant_set", "variant_name"});
    add_tool("variant_define", "Define a variant in a variant set", schema);
  }

  // =========================================================================
  // Query tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["type_name"] = str_prop("USD prim type to search for");
    schema["required"] = nlohmann::json::array({"type_name"});
    add_tool("query_prims_by_type",
             "Find all prims of a given type in the stage", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("schema_list_types",
             "List all registered USD prim type names", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["type_name"] = str_prop("USD prim type name");
    schema["required"] = nlohmann::json::array({"type_name"});
    add_tool("schema_get_type",
             "Get the schema definition for a prim type", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["query"] = str_prop("Search query string");
    schema["properties"]["scope"] =
        str_prop("Search scope: names, all (default)");
    schema["required"] = nlohmann::json::array({"query"});
    add_tool("search", "Search prim names across the stage", schema);
  }

  // =========================================================================
  // Validation
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] =
        str_prop("Base64-encoded USD to validate (usda/usdc/usdz)");
    schema["properties"]["uri"] =
        str_prop("USD file path to validate (native builds only)");
    schema["properties"]["layer_uuid"] =
        str_prop("UUID of a previously-loaded session layer to validate");
    schema["properties"]["name"] =
        str_prop("Filename hint when validating `data`");
    schema["properties"]["groups"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description",
         "Rule groups to run: any of \"core\", \"geom\", \"shade\", "
         "\"lux\", \"physics\", \"crate\", or \"all\". Default [\"core\"]."}};
    add_tool("usd_validate",
             "Validate USD against AOUSD Core semantic rules. Validates "
             "`data`/`uri`/`layer_uuid` if given, else the current session "
             "stage. Returns structured issues (severity, rule_id, location, "
             "message).",
             schema);
  }

  // =========================================================================
  // Scripting
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["script"] =
        str_prop("JavaScript code to execute (uses lightusd.* API)");
    schema["required"] = nlohmann::json::array({"script"});
    add_tool("run_script",
             "Execute JavaScript code against the session stage. "
             "Use lightusd.stage, lightusd.prim, lightusd.query etc. "
             "When a diff is loaded, lightusd.diff.{summary,paths,prim} are also "
             "available for efficient ad-hoc diff queries.",
             schema);
  }

  // =========================================================================
  // Diff tools (value-level, ULP-tolerant USD layer diff)
  // =========================================================================
  {
    nlohmann::json side;
    side["type"] = "object";
    side["properties"]["path"] = str_prop("Filesystem path to a USD file");
    side["properties"]["data"] =
        str_prop("base64-encoded USD bytes (alternative to path)");
    side["properties"]["uuid"] =
        str_prop("UUID of an already-loaded layer (alternative to path)");
    side["properties"]["name"] = str_prop("Display name (optional)");

    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["left"] = side;
    schema["properties"]["right"] = side;
    schema["properties"]["ulps"] =
        int_prop("Float ULP tolerance (default 1; 0 = bitwise-exact)");
    schema["properties"]["eps"] =
        num_prop("Absolute float epsilon (optional; OR'd with ULP)");
    schema["properties"]["compareMetadata"] =
        bool_prop("Compare attribute/prim/layer metadata (default true)");
    schema["properties"]["flatten"] = bool_prop(
        "Flatten (compose sublayers/refs/payload/inherits/variants) before "
        "diff (default false)");
    schema["required"] = nlohmann::json::array({"left", "right"});
    add_tool("diff_open",
             "Diff two USD layers (each given by path/data/uuid), optionally "
             "flattened, with ULP-tolerant float compare. Caches the result in "
             "the session and returns a summary (counts + reason tally). Drill "
             "down with diff_paths / diff_prim, or query via run_script "
             "(lightusd.diff.*).",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("diff_summary",
             "Summary (counts + reason tally) of the cached diff.", schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["reason"] = str_prop(
        "Keep only paths whose reasons contain this substring "
        "(e.g. 'value', 'type', 'meta:kind')");
    schema["properties"]["path_substr"] =
        str_prop("Keep only paths containing this substring");
    schema["properties"]["kind"] = str_prop(
        "Filter by change-kind substring: added|deleted|modified|prim|prop");
    schema["properties"]["group_by"] = str_prop(
        "If set, return aggregated {key,count} groups instead of a path list: "
        "reason | property | prim | kind");
    schema["properties"]["offset"] = int_prop("Pagination offset (default 0)");
    schema["properties"]["limit"] =
        int_prop("Max paths to return (default 200)");
    add_tool("diff_paths",
             "Filtered, paginated list of changed paths from the cached diff. "
             "Pass group_by to get aggregated counts instead.",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] =
        str_prop("Subtree root (default \"/\")");
    schema["properties"]["depth"] =
        int_prop("Levels below root to roll up (default 2)");
    add_tool("diff_tree",
             "Subtree rollup of change counts under a path (navigable "
             "overview: which subtrees changed most).",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["path"] = str_prop("Prim path, e.g. /Root/Foo");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("diff_prim",
             "Full per-prim diff detail (reasons + old/new values) from the "
             "cached diff.",
             schema);
  }
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("diff_text",
             "Full rendered text diff (token-heavy escape hatch).", schema);
    add_tool("diff_json",
             "Full structured JSON diff (token-heavy escape hatch).", schema);
  }

  // =========================================================================
  // Existing: USD Layer tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("get_all_usd_descriptions",
             "Get description of all loaded USD Layers", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("get_usd_description",
             "Get description of a loaded USD Layer", schema);
  }

#if !defined(__EMSCRIPTEN__)
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("USD file path");
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"uri", "name"});
    add_tool("load_usd_layer_from_file",
             "Load USD as Layer from file (C++ native only)", schema);
  }
#endif

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded USD data");
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"data", "name"});
    add_tool("load_usd_layer_from_data",
             "Load USD as Layer from base64 data", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name to load as layer");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("load_usd_layer_from_asset",
             "Load USD as Layer from a stored asset", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Layer name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("to_usda", "Convert a loaded USD Layer to USDA text", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uuid"] = str_prop("Layer UUID");
    schema["properties"]["name"] = str_prop("Layer name");
    add_tool("list_primspecs",
             "List root PrimSpecs in a loaded USD Layer", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uuid"] = str_prop("Layer UUID");
    schema["properties"]["name"] = str_prop("Layer name");
    schema["properties"]["path"] = str_prop("Absolute PrimSpec path");
    schema["properties"]["max_depth"] =
        int_prop("Maximum child depth to include");
    schema["required"] = nlohmann::json::array({"path"});
    add_tool("debug_primspec_dump",
             "Dump a loaded Layer PrimSpec as deterministic JSON for debugging",
             schema);
  }

  // =========================================================================
  // Existing: Asset management tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded asset data");
    schema["properties"]["name"] = str_prop("Asset name");
    schema["properties"]["description"] = str_prop("Optional description");
    schema["required"] = nlohmann::json::array({"data", "name"});
    add_tool("store_asset",
             "Store an asset (USD, texture) with optional preview", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("read_asset",
             "Read asset data, transform, and metadata", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("read_asset_preview",
             "Read preview image from an asset", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["include_preview"] =
        bool_prop("Include preview image data");
    add_tool("get_all_asset_descriptions",
             "Get descriptions of all assets", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Asset name");
    schema["properties"]["include_preview"] =
        bool_prop("Include preview image data");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("get_asset_description",
             "Get description of a specific asset", schema);
  }

  // =========================================================================
  // Existing: Scene arrangement tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["assets"] = {
        {"type", "array"},
        {"description",
         "Array of asset objects with name and optional transforms"}};
    schema["required"] = nlohmann::json::array({"assets"});
    add_tool("select_assets",
             "Select and arrange assets with transforms", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("get_selected_assets",
             "Get currently selected assets", schema);
  }

  // =========================================================================
  // Existing: Screenshot tools (kept from original)
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64 encoded image data");
    schema["properties"]["name"] = str_prop("Screenshot name");
    schema["properties"]["mimeType"] = str_prop("MIME type (image/png, image/jpeg)");
    schema["required"] = nlohmann::json::array({"data", "name", "mimeType"});
    add_tool("save_screenshot", "Save a screenshot image", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    add_tool("list_screenshots", "List screenshot names", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["name"] = str_prop("Screenshot name");
    schema["required"] = nlohmann::json::array({"name"});
    add_tool("read_screenshot", "Read a screenshot image", schema);
  }

  // =========================================================================
  // USDZ conversion / texture tools
  // =========================================================================
  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["input"] = str_prop("Input USD(A/C/Z) file path");
    schema["properties"]["output"] = str_prop("Output .usdz file path");
    schema["properties"]["flatten"] = bool_prop("Compose/flatten before writing (default true)");
    schema["properties"]["arkitCompatible"] = bool_prop("Apply ARKit-friendly metadata");
    schema["properties"]["metersPerUnit"] = num_prop("Override metersPerUnit");
    schema["properties"]["upAxis"] = str_prop("Up axis X/Y/Z");
    schema["properties"]["resizeTextures"] = int_prop("Cap texture longest edge (pixels)");
    schema["properties"]["textureFormat"] = str_prop("keep | png | jpeg");
    schema["properties"]["pngEncoder"] = str_prop("fpnge | fpng");
    schema["properties"]["jpegQuality"] = int_prop("JPEG quality 1-100");
    schema["properties"]["reencode"] = bool_prop("Re-encode unmodified textures (default true)");
    schema["properties"]["targetTextureBytes"] = num_prop("Total texture byte budget; shrink all textures to fit");
    schema["properties"]["fitStrategy"] = str_prop("Fit lever: size | quality");
    schema["properties"]["fitMinTextureSize"] = int_prop("Min longest edge for size-fit (default 64)");
    schema["properties"]["fitMinQuality"] = int_prop("Min JPEG quality for quality-fit (default 30)");
    schema["required"] = nlohmann::json::array({"input", "output"});
    add_tool("usdz_convert",
             "Convert a USD file to an ARKit-friendly USDZ (flatten + texture "
             "resize/re-encode with fpnge)",
             schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["uri"] = str_prop("Output .usdz path. If omitted, returns base64 data.");
    schema["properties"]["arkitCompatible"] = bool_prop("Apply ARKit-friendly metadata");
    add_tool("usdz_pack",
             "Pack the current session stage into a USDZ (file or base64)",
             schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["data"] = str_prop("Base64-encoded input image");
    schema["properties"]["max_size"] = int_prop("Cap the longest edge (pixels)");
    schema["properties"]["width"] = int_prop("Target width (with height)");
    schema["properties"]["height"] = int_prop("Target height (with width)");
    schema["properties"]["format"] = str_prop("Output format: png (default) or jpeg");
    schema["properties"]["pngEncoder"] = str_prop("fpnge | fpng");
    schema["required"] = nlohmann::json::array({"data"});
    add_tool("texture_resize",
             "Resize a base64 image and re-encode (PNG via fpnge)", schema);
  }

  {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"]["channels"] = int_prop("Output channel count 1-4");
    schema["properties"]["width"] = int_prop("Output width (default: max of inputs)");
    schema["properties"]["height"] = int_prop("Output height (default: max of inputs)");
    schema["properties"]["format"] = str_prop("Output format: png (default) or jpeg");
    schema["properties"]["pngEncoder"] = str_prop("fpnge | fpng");
    nlohmann::json chan = {
        {"type", "object"},
        {"description",
         "Channel source: { data: base64, channel: int } or { const: int }"}};
    schema["properties"]["r"] = chan;
    schema["properties"]["g"] = chan;
    schema["properties"]["b"] = chan;
    schema["properties"]["a"] = chan;
    add_tool("texture_repack",
             "Merge channels from base64 images into one image (e.g. R=gloss, "
             "G=roughness)",
             schema);
  }

  return true;
}

// ===========================================================================
// CallTool
// ===========================================================================
bool CallTool(Context &ctx, const std::string &tool_name,
              const nlohmann::json &args, nlohmann::json &result,
              std::string &err) {
  (void)args;

  // ---- Utility ----
  if (tool_name == "get_version") {
    return GetVersion(result);
  }

  // ---- Stage tools ----
  if (tool_name == "stage_new") return StageNew(ctx, args, result, err);
  if (tool_name == "stage_load") return StageLoad(ctx, args, result, err);
  if (tool_name == "stage_load_data") return StageLoadData(ctx, args, result, err);
  if (tool_name == "stage_export") return StageExport(ctx, args, result, err);
  if (tool_name == "stage_to_string") return StageToString(ctx, args, result, err);
  if (tool_name == "stage_info") return StageInfo(ctx, args, result, err);

  // ---- Scene graph ----
  if (tool_name == "prim_list") return PrimList(ctx, args, result, err);
  if (tool_name == "prim_get") return PrimGet(ctx, args, result, err);
  if (tool_name == "prim_create") return PrimCreate(ctx, args, result, err);
  if (tool_name == "prim_remove") return PrimRemove(ctx, args, result, err);
  if (tool_name == "prim_rename") return PrimRename(ctx, args, result, err);
  if (tool_name == "prim_get_metadata") return PrimGetMetadata(ctx, args, result, err);

  // ---- Attributes ----
  if (tool_name == "attr_list") return AttrList(ctx, args, result, err);
  if (tool_name == "attr_get") return AttrGet(ctx, args, result, err);
  if (tool_name == "attr_set") return AttrSet(ctx, args, result, err);
  if (tool_name == "attr_block") return AttrBlock(ctx, args, result, err);
  if (tool_name == "attr_connections") return AttrConnections(ctx, args, result, err);

  // ---- Composition ----
  if (tool_name == "reference_add") return ReferenceAdd(ctx, args, result, err);
  if (tool_name == "reference_list") return ReferenceList(ctx, args, result, err);
  if (tool_name == "reference_clear") return ReferenceClear(ctx, args, result, err);
  if (tool_name == "payload_add") return PayloadAdd(ctx, args, result, err);
  if (tool_name == "payload_list") return PayloadList(ctx, args, result, err);
  if (tool_name == "inherit_add") return InheritAdd(ctx, args, result, err);
  if (tool_name == "specialize_add") return SpecializeAdd(ctx, args, result, err);
  if (tool_name == "variant_list_sets") return VariantListSets(ctx, args, result, err);
  if (tool_name == "variant_get_selection") return VariantGetSelection(ctx, args, result, err);
  if (tool_name == "variant_set_selection") return VariantSetSelection(ctx, args, result, err);
  if (tool_name == "variant_define") return VariantDefine(ctx, args, result, err);

  // ---- Query ----
  if (tool_name == "query_prims_by_type") return QueryPrimsByType(ctx, args, result, err);
  if (tool_name == "schema_list_types") return SchemaListTypes(ctx, args, result, err);
  if (tool_name == "schema_get_type") return SchemaGetType(ctx, args, result, err);
  if (tool_name == "search") return Search(ctx, args, result, err);

  // ---- Validation ----
  if (tool_name == "usd_validate") return UsdValidate(ctx, args, result, err);

  // ---- USDZ conversion / textures ----
  if (tool_name == "usdz_convert") return USDZConvert(ctx, args, result, err);
  if (tool_name == "usdz_pack") return USDZPack(ctx, args, result, err);
  if (tool_name == "texture_resize") return TextureResize(ctx, args, result, err);
  if (tool_name == "texture_repack") return TextureRepack(ctx, args, result, err);

  // ---- Diff ----
  if (tool_name == "diff_open") return DiffOpen(ctx, args, result, err);
  if (tool_name == "diff_summary") return DiffSummary(ctx, args, result, err);
  if (tool_name == "diff_paths") return DiffPaths(ctx, args, result, err);
  if (tool_name == "diff_prim") return DiffPrim(ctx, args, result, err);
  if (tool_name == "diff_tree") return DiffTree(ctx, args, result, err);
  if (tool_name == "diff_text") return DiffText(ctx, args, result, err);
  if (tool_name == "diff_json") return DiffJson(ctx, args, result, err);

  // ---- Scripting ----
  if (tool_name == "run_script") return RunScript(ctx, args, result, err);

  // ---- Legacy USD Layer tools ----
  if (tool_name == "get_all_usd_descriptions") {
    return GetAllUSDDescriptions(ctx, args, result, err);
  }
  if (tool_name == "get_usd_description") {
    return GetUSDDescription(ctx, args, result, err);
  }
#if !defined(__EMSCRIPTEN__)
  if (tool_name == "load_usd_layer_from_file") {
    return LoadUSDLayerFromFile(ctx, args, result, err);
  }
#endif
  if (tool_name == "to_usda") {
    return ToUSDA(ctx, args, result, err);
  }
  if (tool_name == "load_usd_layer_from_data") {
    return LoadUSDLayerFromData(ctx, args, result, err);
  }
  if (tool_name == "list_primspecs") {
    return ListPrimSpecs(ctx, args, result, err);
  }
  if (tool_name == "debug_primspec_dump") {
    return DebugPrimSpecDump(ctx, args, result, err);
  }
  if (tool_name == "load_usd_layer_from_asset") {
    // Not implemented yet
    err = "Not implemented";
    return false;
  }

  // ---- Legacy viewer tools ----
  if (tool_name == "list_screenshots") return ListScreenshots(ctx, args, result, err);
  if (tool_name == "save_screenshot") return SaveScreenshot(ctx, args, result, err);
  if (tool_name == "read_screenshot") return ReadScreenshot(ctx, args, result, err);
  if (tool_name == "read_asset") return ReadAsset(ctx, args, result, err);
  if (tool_name == "read_asset_preview") return ReadAssetPreview(ctx, args, result, err);
  if (tool_name == "store_asset") return StoreAsset(ctx, args, result, err);
  if (tool_name == "get_all_asset_descriptions") {
    return GetAllAssetDescriptions(ctx, args, result, err);
  }
  if (tool_name == "get_asset_description") {
    return GetAssetDescription(ctx, args, result, err);
  }
  if (tool_name == "select_assets") return SelectAssets(ctx, args, result, err);
  if (tool_name == "get_selected_assets") {
    return GetSelectedAssets(ctx, args, result, err);
  }

  // Tool not found
  return false;
}

} // namespace mcp
} // namespace tydra
} // namespace lightusd
