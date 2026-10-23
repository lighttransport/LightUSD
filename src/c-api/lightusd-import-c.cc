// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include "lightusd-import-c.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <new>

#include "c-internal.hh"
#include "external/filesystem/include/ghc/filesystem.hpp"
#include "next/resolver/asset-resolver.hh"
#ifdef LIGHTUSD_WITH_USDOBJ
#include "next/reader/obj-reader.hh"
#endif
#include "tydra/asset-import/import.hh"
#include "tydra/next/urdf-to-usd.hh"
namespace ia = lightusd::tydra::import_asset;
using lightusd_internal::Fail;
namespace {
thread_local std::string last_import_report = "{}";
thread_local bool import_cancelled = false;  // set by the progress trampoline
lightusd_status Options(const lightusd_import_options* in, ia::Options* out) {
  // Finish() hands Result::meshes to the converter.
  out->typed_meshes = true;
  if (!in) return LIGHTUSD_OK;
  // Callers built before the progress fields pass the shorter struct.
  constexpr size_t kBaseSize = offsetof(lightusd_import_options, progress);
  if (in->struct_size < kBaseSize || in->format > LIGHTUSD_IMPORT_MJCF ||
      in->max_memory_bytes > SIZE_MAX || in->max_input_bytes > SIZE_MAX ||
      !in->max_depth || in->max_depth > 1024)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "Invalid import options");
  if (in->struct_size >= sizeof(*in) && in->progress) {
    auto fn = in->progress;
    void* user = in->progress_user_data;
    // Nested readers account separately; report a monotonic high-water mark.
    auto high = std::make_shared<uint64_t>(0);
    out->progress = [fn, user, high](const char* phase, uint64_t done, uint64_t total) {
      *high = std::max(*high, done);
      if (fn(user, phase, *high, total) == 0) return true;
      import_cancelled = true;
      return false;
    };
  }
  const char* formats[] = {"auto", "obj", "urdf", "mjcf"};
  out->format = formats[in->format];
  if (in->max_memory_bytes)
    out->max_memory = static_cast<size_t>(in->max_memory_bytes);
  if (in->max_input_bytes)
    out->max_input = static_cast<size_t>(in->max_input_bytes);
  out->max_depth = in->max_depth;
  if (in->settings_json) {
    ia::Json settings;
    lightusd::minijson::Error error;
    if (!lightusd::minijson::Parse(in->settings_json, &settings, &error) ||
        !settings.is_object())
      return Fail(LIGHTUSD_ERR_INVALID_ARG,
                  "Import settings must be a JSON object");
    for (const auto& member : *settings.object_items()) {
      const auto& key = member.key;
      const auto& v = member.value();
      if (key == "up_axis" && v.is_string())
        out->up_axis = v.get<std::string>();
      else if (key == "profile" && v.is_string()) {
        out->profile = v.get<std::string>();
        if (out->profile != "newton" && out->profile != "openusd" &&
            out->profile != "legacy")
          return Fail(LIGHTUSD_ERR_INVALID_ARG,
                      "profile must be newton, openusd or legacy");
      }
      else if (key == "layout" && v.is_string()) {
        out->layout = v.get<std::string>();
        if (out->layout != "flat" && out->layout != "nested")
          return Fail(LIGHTUSD_ERR_INVALID_ARG, "layout must be flat or nested");
      }
      else if (key == "obj_topology" && v.is_string()) {
        out->obj_topology = v.get<std::string>();
        if (out->obj_topology != "expanded" && out->obj_topology != "indexed")
          return Fail(LIGHTUSD_ERR_INVALID_ARG, "obj_topology must be expanded or indexed");
      }
      else if (key == "allow_missing" && v.is_boolean())
        out->allow_missing = v.get<bool>();
      else if (key == "tessellate_collision_shapes" && v.is_boolean())
        out->tessellate_collision_shapes = v.get<bool>();
      else if (key == "unsupported_policy" && v.is_string()) {
        const auto policy = v.get<std::string>();
        if (policy == "warn") out->unsupported_policy = ia::UnsupportedPolicy::Warn;
        else if (policy == "error") out->unsupported_policy = ia::UnsupportedPolicy::Error;
        else if (policy == "preserve") out->unsupported_policy = ia::UnsupportedPolicy::Preserve;
        else return Fail(LIGHTUSD_ERR_INVALID_ARG, "unsupported_policy must be warn, error or preserve");
      }
      else if (key == "asset_dirs" && v.is_array()) {
        for (const auto& dir : v) {
          if (!dir.is_string())
            return Fail(LIGHTUSD_ERR_INVALID_ARG,
                        "asset_dirs must contain strings");
          out->asset_dirs.push_back(dir.get<std::string>());
        }
      } else if (key == "packages" && v.is_object()) {
        for (const auto& package : *v.object_items()) {
          if (!package.value().is_string())
            return Fail(LIGHTUSD_ERR_INVALID_ARG,
                        "package roots must be strings");
          out->packages[package.key] = package.value().get<std::string>();
        }
      } else
        return Fail(LIGHTUSD_ERR_INVALID_ARG,
                    "Unknown or invalid import setting: " + key);
    }
  }
  if (in->resolver) {
    auto* resolver = lightusd_internal::AssetResolverForLoad(in->resolver);
    out->open = [resolver](const std::string& name, const std::string& anchor,
                           ia::AssetSource* source, std::string* err) {
      auto asset = resolver->Resolve(name, anchor, false);
      if (!asset.exists) {
        if (err) *err = "Unresolved import asset: " + name;
        return false;
      }
      source->identifier = asset.resolved_path;
      source->canonical = asset.canonical_path;
      source->read = [resolver, asset](std::vector<uint8_t>* data, std::string* why) {
        return resolver->ReadAsset(asset, data, why);
      };
      return true;
    };
    out->read = [resolver](const std::string& name, const std::string& anchor,
                           std::vector<uint8_t>* bytes, std::string* resolved,
                           std::string* err) {
      auto asset = resolver->Resolve(name, anchor, false);
      if (!asset.exists) {
        if (err) *err = "Unresolved import asset: " + name;
        return false;
      }
      *resolved = asset.resolved_path;
      return resolver->ReadAsset(asset, bytes, err);
    };
  }
  return LIGHTUSD_OK;
}
// Phase-boundary progress report; false (with *err) when cancelled.
bool Proceed(const ia::Options& options, const char* phase, std::string* err) {
  if (!options.progress || options.progress(phase, 0, 0)) return true;
  *err = "Import cancelled";
  return false;
}
lightusd_status Failure(const std::string& error) {
  if (import_cancelled) return Fail(LIGHTUSD_ERR_CANCELLED, error);
  if (error.find("budget") != std::string::npos ||
      error.find("limit") != std::string::npos)
    return Fail(LIGHTUSD_ERR_RESOURCE_LIMIT, error);
  return Fail(LIGHTUSD_ERR_PARSE, error);
}
#ifdef LIGHTUSD_WITH_USDOBJ
bool IsOBJ(const std::string& source, const ia::Options& options) {
  std::string ext = ghc::filesystem::path(source).extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return options.format == "obj" || (options.format == "auto" && ext == ".obj");
}
lightusd_status FinishOBJ(std::shared_ptr<lightusd::next::Layer> layer,
                         const std::string& source, std::string warnings,
                         lightusd_stage** out) {
  auto* handle = new (std::nothrow) lightusd_stage;
  if (!handle) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate imported stage");
  auto assets = layer->meta().imported_assets;
  if (assets) {
    lightusd_internal::RetainImportedAssets(handle, *assets);
    layer->meta().imported_assets.reset();
  }
  handle->stage.SetRootLayer(std::move(*layer));
  handle->warnings = std::move(warnings);
  handle->source_dir = ghc::filesystem::path(source).parent_path().string();
  *out = handle;
  return LIGHTUSD_OK;
}
#endif
lightusd_status Finish(ia::Result& result, const std::string& source,
                       lightusd_stage** out) {
  auto* handle = new (std::nothrow) lightusd_stage;
  if (!handle)
    return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate imported stage");
  std::string warn, err;
  std::map<std::string, lightusd::tydra::next::URDFMeshBuffer> meshes;
  for (auto& entry : result.meshes) {
    auto& buffer = meshes[entry.first];
    buffer.positions = std::move(entry.second.positions);
    buffer.normals = std::move(entry.second.normals);
    buffer.uvs = std::move(entry.second.uvs);
    buffer.indices = std::move(entry.second.indices);
  }
  result.meshes.clear();
  if (!lightusd::tydra::next::ConvertURDFPayloadToUSDStage(
          result.payload, &meshes, &handle->stage, &warn, &err)) {
    delete handle;
    return Failure(err);
  }
  handle->warnings = result.warnings + warn;
  if (!warn.empty() && result.report)
    result.report->Diagnostic("warning", "import.conversion_warning", warn, source);
  handle->source_dir = ghc::filesystem::path(source).parent_path().string();
  // source_filename is intentionally empty: a USD provenance query must not
  // try to reload a robot or OBJ file through the USD-only core loader.
  handle->import_assets = std::move(result.assets);
  // Collect source -> archive names from the material import contract.
  for (const auto& mat : result.payload["materials"]) {
    if (mat.contains("texturePackage")) {
      const std::string texture = mat["texture"].get<std::string>();
      const std::string package = mat["texturePackage"].get<std::string>();
      handle->import_asset_paths[texture] = package;
      if (mat.contains("texture_generated")) {
        auto bytes = handle->import_assets.find(package);
        if (bytes != handle->import_assets.end())
          handle->generated_assets[texture] = bytes->second;
      }
    }
    for (const auto& map : mat["maps"])
      if (map.contains("package"))
        handle->import_asset_paths[map["file"].get<std::string>()] =
            map["package"].get<std::string>();
  }
  *out = handle;
  return LIGHTUSD_OK;
}
template <class Import>
lightusd_status ReportedImport(ia::Options& options, const std::string& source,
                               lightusd_stage** out, Import import) {
  std::string error;
  lightusd_status status;
  {
    ia::ImportSession session(&options, source, options.format, &error);
    status = import(&error);
    if (status == LIGHTUSD_OK) session.Succeed();
    else if (error.empty()) error = lightusd_last_error();
  }
  last_import_report = options.report->ToJSON().dump();
  if (status == LIGHTUSD_OK && *out) (*out)->import_report = last_import_report;
  return status;
}
lightusd_status CopyReport(const std::string& report, lightusd_string** out) {
  if (!out) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Report output is null");
  *out = new (std::nothrow) lightusd_string;
  if (!*out) return Fail(LIGHTUSD_ERR_OUT_OF_MEMORY, "Cannot allocate import report");
  (*out)->s = report;
  return LIGHTUSD_OK;
}
}  // namespace
extern "C" {
void lightusd_import_options_init(lightusd_import_options* o) {
  if (!o) return;
  *o = {};
  o->struct_size = sizeof(*o);
  o->max_memory_bytes = 1024ull * 1024 * 1024;
  o->max_input_bytes = 512ull * 1024 * 1024;
  o->max_depth = 64;
}
lightusd_status lightusd_import_file(const char* filename,
                                     const lightusd_import_options* options,
                                     lightusd_stage** out) {
  last_import_report = "{}";
  import_cancelled = false;
  if (out) *out = nullptr;
  if (!filename || !out)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "Import filename/output is null");
  ia::Options opts;
  auto status = Options(options, &opts);
  if (status != LIGHTUSD_OK) return status;
  return ReportedImport(opts, filename, out, [&](std::string* err) {
    ia::Result result;
#ifdef LIGHTUSD_WITH_USDOBJ
    if (IsOBJ(filename, opts)) {
      std::string warn;
      auto layer = lightusd::next::LoadOBJLayerFromFile(filename, opts, &warn, err);
      if (!layer) return Failure(*err);
      return FinishOBJ(std::move(layer), filename, std::move(warn), out);
    }
#endif
    if (!ia::BuildPayloadFile(filename, opts, &result, err)) return Failure(*err);
    if (!Proceed(opts, "convert", err)) return Failure(*err);
    return Finish(result, filename, out);
  });
}
lightusd_status lightusd_import_memory(const uint8_t* data, size_t size,
                                       const char* source,
                                       const lightusd_import_options* options,
                                       lightusd_stage** out) {
  last_import_report = "{}";
  import_cancelled = false;
  if (out) *out = nullptr;
  if ((!data && size) || !out)
    return Fail(LIGHTUSD_ERR_INVALID_ARG, "Import data/output is null");
  ia::Options opts;
  auto status = Options(options, &opts);
  if (status != LIGHTUSD_OK) return status;
  return ReportedImport(opts, source ? source : "", out, [&](std::string* err) {
    opts.report->input_bytes = size;
    if (size > opts.max_input || size > opts.max_memory) {
      *err = "Import input limit exceeded";
      opts.report->Diagnostic("error", "import.input_limit", *err);
      return Fail(LIGHTUSD_ERR_RESOURCE_LIMIT, *err);
    }
    ia::Result result;
#ifdef LIGHTUSD_WITH_USDOBJ
    if (IsOBJ(source ? source : "", opts)) {
      std::string warn;
      auto layer = lightusd::next::LoadOBJLayerFromMemory(source ? source : "",
          data, size, opts, &warn, err);
      if (!layer) return Failure(*err);
      return FinishOBJ(std::move(layer), source ? source : "", std::move(warn), out);
    }
#endif
    std::string text(size ? reinterpret_cast<const char*>(data) : "", size);
    if (!ia::BuildPayload(text, source ? source : "", opts, &result, err))
      return Failure(*err);
    if (!Proceed(opts, "convert", err)) return Failure(*err);
    return Finish(result, source ? source : "", out);
  });
}
lightusd_status lightusd_stage_import_report(const lightusd_stage* stage,
                                             lightusd_string** out) {
  if (out) *out = nullptr;
  if (!stage) return Fail(LIGHTUSD_ERR_INVALID_ARG, "Stage is null");
  return CopyReport(stage->import_report, out);
}
lightusd_status lightusd_import_last_report(lightusd_string** out) {
  return CopyReport(last_import_report, out);
}
}
