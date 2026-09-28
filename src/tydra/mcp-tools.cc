#include "mcp-tools.hh"

#include <limits>
#include <string>

#include "mcp-context.hh"
#include "../mcp/mcp-tool-schemas.hh"
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
  lightusd::mcp_schema::AppendToolSchemas(result);
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
