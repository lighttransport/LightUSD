// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// USDZ packaging tools of the next MCP server. usdz_convert packages the
// input's relative asset references as they are; texture processing
// (resize / re-encode / budget fit) is not part of the next MCP product.
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "c-stage-bridge.hh"
#include "mcp-tools.hh"
#include "mcp-util.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"
#include "next/writer/usdz-writer.hh"

namespace lightusd {
namespace mcp {

namespace {

constexpr size_t kMaxUSDZArchiveBytes = size_t(256) * 1024 * 1024;

bool IsUnsafePath(const std::string& p) {
  return p.empty() || p.find("..") != std::string::npos ||
         p.find('\0') != std::string::npos || p[0] == '/' || p[0] == '\\' ||
         (p.size() >= 2 && p[1] == ':');
}

std::string DirOf(const std::string& path) {
  const size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? std::string() : path.substr(0, pos + 1);
}

// Relative asset references authored on the stage (defaults only).
std::set<std::string> CollectAssetPaths(const next::Stage& stage) {
  std::set<std::string> out;
  auto add = [&](const std::string& p) {
    if (!p.empty() && p.find("://") == std::string::npos) out.insert(p);
  };
  stage.Traverse([&](const next::UsdPrim& prim) {
    for (const std::string& name : prim.GetPropertyNames()) {
      const next::Value* v = prim.GetPropertyValue(name);
      if (!v || v->type_id() != next::TypeId::AssetPath) continue;
      if (v->is_array()) {
        if (const auto* a = v->as_token_array()) {
          for (const std::string& p : *a) add(p);
        }
      } else if (const std::string* p = v->as_asset_path()) {
        add(*p);
      }
    }
    return true;
  });
  return out;
}

bool SetStageUnits(lightusd_stage* stage, const json& args, std::string* err) {
  if (args.contains("metersPerUnit") && args["metersPerUnit"].is_number()) {
    const double mpu = args["metersPerUnit"].get<double>();
    if (mpu > 0.0 && mpu < 1e6 &&
        lightusd_stage_set_metadata(stage, "metersPerUnit", LIGHTUSD_TYPE_DOUBLE,
                                    &mpu, 1) != LIGHTUSD_OK) {
      *err = std::string("set metersPerUnit: ") + lightusd_last_error();
      return false;
    }
  }
  std::string axis = args.value("upAxis", std::string());
  if (args.value("arkitCompatible", false)) axis = "Y";
  if (!axis.empty()) {
    if (axis == "x") axis = "X";
    if (axis == "y") axis = "Y";
    if (axis == "z") axis = "Z";
    if (axis != "X" && axis != "Y" && axis != "Z") {
      *err = "upAxis must be X, Y, or Z";
      return false;
    }
    const char* v = axis.c_str();
    if (lightusd_stage_set_metadata(stage, "upAxis", LIGHTUSD_TYPE_TOKEN, v, 1) !=
        LIGHTUSD_OK) {
      *err = std::string("set upAxis: ") + lightusd_last_error();
      return false;
    }
  }
  return true;
}

}  // namespace

bool USDZConvert(Context&, const json& args, json& result, std::string& err) {
#if defined(__EMSCRIPTEN__)
  (void)args;
  (void)result;
  err = "usdz_convert works on files and is not supported in this build; use usdz_pack";
  return false;
#else
  if (!args.contains("input") || !args["input"].is_string()) {
    err = "Missing 'input' argument";
    return false;
  }
  if (!args.contains("output") || !args["output"].is_string()) {
    err = "Missing 'output' argument";
    return false;
  }
  const std::string input = args["input"].get<std::string>();
  const std::string output = args["output"].get<std::string>();
  if (IsUnsafePath(input) || IsUnsafePath(output)) {
    err = "Path contains unsafe characters or traversal sequences.";
    return false;
  }
  const bool resize = args.value("resizeTextures", 0) > 0;
  const bool fit = args.value("targetTextureBytes", 0.0) > 0.0;
  const std::string tex_format = args.value("textureFormat", std::string("keep"));
  if (resize || fit || (tex_format != "keep" && !tex_format.empty())) {
    err = "Texture processing (resizeTextures, textureFormat, targetTextureBytes) "
          "is not supported by the next MCP product; textures are packaged as-is.";
    return false;
  }

  lightusd_load_options opts;
  lightusd_load_options_init(&opts);
  opts.composed = 1;
  lightusd_stage* loaded = nullptr;
  if (lightusd_stage_load(input.c_str(), &opts, &loaded) != LIGHTUSD_OK) {
    err = std::string("Failed to load ") + input + ": " + lightusd_last_error();
    return false;
  }
  StageRef stage(loaded);
  if (args.value("flatten", true)) {
    next::Layer flat = NativeStage(stage)->Flatten();
    lightusd_stage* flat_stage = nullptr;
    if (lightusd_stage_create(&flat_stage) != LIGHTUSD_OK ||
        !lightusd_internal::SetNativeRootLayer(flat_stage, std::move(flat))) {
      if (flat_stage) lightusd_stage_destroy(flat_stage);
      err = "flatten failed";
      return false;
    }
    stage.reset(flat_stage);
  }
  if (!SetStageUnits(stage.stage, args, &err)) return false;

  // Package relative asset references found next to the input.
  const std::string base = DirOf(input);
  std::vector<std::string> names;
  std::vector<std::vector<uint8_t>> blobs;
  json warnings = json::array();
  for (const std::string& path : CollectAssetPaths(*NativeStage(stage))) {
    std::string entry = path;
    if (entry.compare(0, 2, "./") == 0) entry = entry.substr(2);
    if (IsUnsafePath(entry)) {
      warnings.push_back("not packaged (absolute or outside the package): " + path);
      continue;
    }
    std::ifstream f(base + entry, std::ios::binary);
    if (!f) {
      warnings.push_back("asset not found: " + path);
      continue;
    }
    names.push_back(entry);
    blobs.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  std::vector<const char*> name_ptrs;
  std::vector<const uint8_t*> data_ptrs;
  std::vector<size_t> sizes;
  for (size_t i = 0; i < names.size(); ++i) {
    name_ptrs.push_back(names[i].c_str());
    data_ptrs.push_back(blobs[i].data());
    sizes.push_back(blobs[i].size());
  }
  if (lightusd_stage_save_usdz_with_assets(stage.stage, output.c_str(),
                                           name_ptrs.data(), data_ptrs.data(),
                                           sizes.data(), names.size()) != LIGHTUSD_OK) {
    err = std::string("USDZ write failed: ") + lightusd_last_error();
    return false;
  }
  std::ifstream written(output, std::ios::binary | std::ios::ate);
  result["success"] = true;
  result["output"] = output;
  result["warn"] = warnings;
  result["stats"] = {{"textures", names.size()},
                     {"resized", 0},
                     {"reencoded", 0},
                     {"passthrough", names.size()},
                     {"output_size", written ? static_cast<size_t>(written.tellg()) : 0}};
  return true;
#endif
}

bool USDZPack(Context& ctx, const json& args, json& result, std::string& err) {
  if (!RequireStage(ctx, &err)) return false;
  if (!SetStageUnits(ctx.stage.stage, args, &err)) return false;
  if (args.contains("uri") && args["uri"].is_string()) {
#if defined(__EMSCRIPTEN__)
    err = "Writing to a file URI is not supported in this build; omit `uri`";
    return false;
#else
    const std::string uri = args["uri"].get<std::string>();
    if (IsUnsafePath(uri)) {
      err = "Path contains unsafe characters or traversal sequences.";
      return false;
    }
    lightusd_save_options opts;
    lightusd_save_options_init(&opts);
    opts.format = LIGHTUSD_FORMAT_USDZ;
    if (lightusd_stage_save(ctx.stage.stage, uri.c_str(), &opts) != LIGHTUSD_OK) {
      err = std::string("USDZ write failed: ") + lightusd_last_error();
      return false;
    }
    result["success"] = true;
    result["uri"] = uri;
    return true;
#endif
  }
  std::vector<uint8_t> out;
  next::USDZWriteOptions wopts;
  wopts.max_file_size_bytes = kMaxUSDZArchiveBytes;
  const next::USDZWriteResult r =
      next::WriteUSDZToMemory(out, *NativeStage(ctx.stage), wopts);
  if (!r.success) {
    err = "USDZ write failed: " + r.error;
    return false;
  }
  result["success"] = true;
  result["data"] = EncodeBase64(out.data(), out.size());
  result["size"] = out.size();
  return true;
}

}  // namespace mcp
}  // namespace lightusd
