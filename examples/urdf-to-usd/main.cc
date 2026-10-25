// SPDX-License-Identifier: Apache-2.0
// Shared native OBJ/URDF/MJCF frontends; no Python or Node dependency.
#include "tydra/asset-import/import.hh"
#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
#include "next/load-usd.hh"
#include "next/stage/stage.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"
#include "tydra/next/urdf-to-usd.hh"
using OutputStage = lightusd::next::Stage;
#else
#include "lightusd.hh"
#include "tydra/urdf-to-usd.hh"
#include "usda-writer.hh"
#include "usdc-writer.hh"
using OutputStage = lightusd::Stage;
#endif
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "external/filesystem/include/ghc/filesystem.hpp"
namespace ia = lightusd::tydra::import_asset;
namespace fs = ghc::filesystem;
namespace {
void Help() {
  std::cout
      << "Native OBJ/URDF/MJCF -> USD Physics + MuJoCo + Newton exporter\n"
         "Usage: urdf-to-usd INPUT [--input-format auto|obj|urdf|mjcf] "
         "--format usda|usdc|usdz|all -o OUTPUT\n"
         "Options: --up-axis Z|Y --asset-dir DIR --package NAME=DIR "
         "--allow-missing\n"
         "         --tessellate-collision-shapes --dump-json FILE --max-memory "
         "BYTES\n"
         "         --max-input BYTES --max-depth 1..1024 "
         "--unsupported-policy warn|error|preserve\n"
         "         --layout flat|nested\n"
         "         --sdf auto|off --sdf-resolution N --sdf-band VOXELS (MJCF sdf\n"
         "           geoms: level-set .vdb sidecars, newton profile)\n"
         "         --flex-tetrahedralize mujoco|stuffing --flex-tet-spacing H\n"
         "         --volumetric-device cpu|cuda|hip|auto (level sets and tets;\n"
         "           CUDA/HIP need a LIGHTUSD_TINYVDB_GPU build)\n"
         "         --profile newton|openusd|legacy (next-core writer; the "
         "legacy writer always emits legacy)\n";
}
#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
using MeshBuffers = std::map<std::string, lightusd::tydra::next::URDFMeshBuffer>;
#else
using MeshBuffers = std::map<std::string, int>;  // the legacy converter takes JSON meshes
#endif

bool Convert(const ia::Json& payload, const MeshBuffers& meshes, OutputStage* stage,
             std::string* warn, std::string* err) {
#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
  return lightusd::tydra::next::ConvertURDFPayloadToUSDStage(payload, &meshes, stage,
                                                          warn, err);
#else
  (void)meshes;
  return lightusd::tydra::ConvertURDFPayloadToUSDStage(payload, stage, warn,
                                                    err);
#endif
}
bool Save(const OutputStage& stage, const std::string& path,
          const std::string& format,
          const ia::AssetMap& assets,
          std::string* err) {
#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
  if (format == "usda") return lightusd::next::WriteUSDA(stage, path, err);
  if (format == "usdc") return lightusd::next::WriteUSDC(stage, path, err);
  std::vector<uint8_t> crate, zip;
  auto cr = lightusd::next::WriteUSDCToMemory(crate, stage);
  if (!cr.success) {
    *err = cr.error;
    return false;
  }
  lightusd::next::USDZAssetViews views;
  for (const auto& asset : assets)
    if (asset.second) views[asset.first] = {asset.second->data(), asset.second->size()};
  auto z = lightusd::next::WriteUSDZFromUSDCAndAssetViewsToMemory(
      zip, crate.data(), crate.size(), views);
  if (!z.success) {
    *err = z.error;
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(zip.data()),
            static_cast<std::streamsize>(zip.size()));
  if (!out) {
    *err = "Cannot write USDZ: " + path;
    return false;
  }
  return true;
#else
  std::string warn;
  if (format == "usda")
    return lightusd::usda::SaveAsUSDA(path, stage, &warn, err);
  if (format == "usdc")
    return lightusd::usdc::SaveAsUSDCToFile(path, stage, &warn, err);
  std::map<std::string, std::vector<uint8_t>> owned;
  for (const auto& asset : assets)
    if (asset.second) owned[asset.first] = *asset.second;
  return lightusd::SaveAsUSDZToFile(path, stage, owned, &warn, err);
#endif
}
}  // namespace
int main(int argc, char** argv) {
  ia::Options opts;
  std::string input, output, format = "usda", dump, err;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    auto value = [&]() {
      if (i + 1 >= argc) {
        err = arg + " needs a value";
        return std::string();
      }
      return std::string(argv[++i]);
    };
    if (arg == "-h" || arg == "--help") {
      Help();
      return 0;
    }
    if (arg == "--input-format")
      opts.format = value();
    else if (arg == "--format")
      format = value();
    else if (arg == "--up-axis")
      opts.up_axis = value();
    else if (arg == "--profile") {
      opts.profile = value();
      if (opts.profile != "newton" && opts.profile != "openusd" &&
          opts.profile != "legacy")
        err = "--profile expects newton, openusd or legacy";
    }
    else if (arg == "--layout") {
      opts.layout = value();
      if (opts.layout != "flat" && opts.layout != "nested")
        err = "--layout expects flat or nested";
    }
    else if (arg == "--sdf") {
      opts.sdf = value();
      if (opts.sdf != "auto" && opts.sdf != "off") err = "--sdf expects auto or off";
    } else if (arg == "--volumetric-device") {
      opts.volumetric_device = value();
      if (opts.volumetric_device != "cpu" && opts.volumetric_device != "cuda" &&
          opts.volumetric_device != "hip" && opts.volumetric_device != "auto")
        err = "--volumetric-device expects cpu, cuda, hip or auto";
    } else if (arg == "--flex-tetrahedralize") {
      opts.flex_tetrahedralize = value();
      if (opts.flex_tetrahedralize != "mujoco" && opts.flex_tetrahedralize != "stuffing")
        err = "--flex-tetrahedralize expects mujoco or stuffing";
    } else if (arg == "--sdf-resolution" || arg == "--sdf-band" || arg == "--flex-tet-spacing") {
      const std::string str = value();
      char* end = nullptr;
      const double v = std::strtod(str.c_str(), &end);
      if (str.empty() || !end || *end || !std::isfinite(v))
        err = "Invalid number for " + arg;
      else if (arg == "--sdf-resolution") {
        if (v < 8 || v > 4096 || v != std::floor(v)) err = "--sdf-resolution expects 8..4096";
        else opts.sdf_resolution = int(v);
      } else if (arg == "--sdf-band") {
        if (v < 1 || v > 64) err = "--sdf-band expects 1..64 voxels";
        else opts.sdf_band = v;
      } else {
        if (v < 0) err = "--flex-tet-spacing expects a nonnegative length";
        else opts.flex_tet_spacing = v;
      }
    }
    else if (arg == "-o" || arg == "--output")
      output = value();
    else if (arg == "--dump-json")
      dump = value();
    else if (arg == "--asset-dir")
      opts.asset_dirs.push_back(value());
    else if (arg == "--package") {
      auto p = value();
      auto eq = p.find('=');
      if (eq == std::string::npos)
        err = "--package expects NAME=DIR";
      else
        opts.packages[p.substr(0, eq)] = p.substr(eq + 1);
    } else if (arg == "--max-memory" || arg == "--max-input" || arg == "--max-depth") {
      auto str = value();
      char* end = nullptr;
      errno = 0;
      auto amount = std::strtoull(str.c_str(), &end, 10);
      if (str.empty() || str.find_first_not_of("0123456789") != std::string::npos ||
          !end || *end || errno == ERANGE || !amount || amount > SIZE_MAX ||
          (arg == "--max-depth" && amount > 1024))
        err = "Invalid limit for " + arg;
      else if (arg == "--max-memory")
        opts.max_memory = static_cast<size_t>(amount);
      else if (arg == "--max-input") opts.max_input = static_cast<size_t>(amount);
      else opts.max_depth = static_cast<size_t>(amount);
    } else if (arg == "--unsupported-policy") {
      auto policy = value();
      if (policy == "warn") opts.unsupported_policy = ia::UnsupportedPolicy::Warn;
      else if (policy == "error") opts.unsupported_policy = ia::UnsupportedPolicy::Error;
      else if (policy == "preserve") opts.unsupported_policy = ia::UnsupportedPolicy::Preserve;
      else err = "--unsupported-policy expects warn, error or preserve";
    } else if (arg == "--allow-missing")
      opts.allow_missing = true;
    else if (arg == "--tessellate-collision-shapes")
      opts.tessellate_collision_shapes = true;
    else if (arg == "-v" || arg == "--verbose") {
    } else if (input.empty() && !arg.empty() && arg[0] != '-')
      input = arg;
    else
      err = "Unknown argument: " + arg;
    if (!err.empty()) break;
  }
  if (input.empty()) err = "Input file is required";
  if (format != "usda" && format != "usdc" && format != "usdz" &&
      format != "all")
    err = "Unsupported output format";
  if (!err.empty()) {
    std::cerr << "urdf-to-usd: " << err << "\n";
    return 1;
  }
#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
  // Large mesh arrays travel as typed buffers (as through the C API) unless
  // the JSON dump needs them inline: one value per scalar costs far more.
  opts.typed_meshes = dump.empty();
#endif
  ia::Result result;
  if (!ia::BuildPayloadFile(input, opts, &result, &err)) {
    std::cerr << "urdf-to-usd: " << err << "\n";
    return 1;
  }
  if (!dump.empty()) {
    std::ofstream file(dump);
    file << result.payload.dump(2) << "\n";
    if (!file) {
      std::cerr << "Cannot write JSON\n";
      return 1;
    }
  }
  if (!result.warnings.empty()) std::cerr << result.warnings;
  MeshBuffers meshes;
#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
  for (auto& entry : result.meshes) {
    auto& buffer = meshes[entry.first];
    buffer.positions = std::move(entry.second.positions);
    buffer.normals = std::move(entry.second.normals);
    buffer.uvs = std::move(entry.second.uvs);
    buffer.indices = std::move(entry.second.indices);
  }
  result.meshes.clear();
#endif
  for (const char* kind : {"usda", "usdc", "usdz"}) {
    if (format != "all" && format != kind) continue;
    fs::path path = output.empty() ? fs::path(input) : fs::path(output);
    if (output.empty() || format == "all" || path.extension().empty())
      path.replace_extension(std::string(".") + kind);
    // Only a USDZ rewrites texture paths, so only it needs its own payload.
    const ia::Json* payload = &result.payload;
    ia::Json usdz_payload;
    if (std::string(kind) == "usdz") {
      usdz_payload = result.payload;
      for (auto& mat : usdz_payload["materials"]) {
        if (mat.contains("texturePackage"))
          mat["texture"] = mat["texturePackage"];
        for (auto& map : mat["maps"])
          if (map.contains("package")) map["file"] = map["package"];
      }
      payload = &usdz_payload;
    }
    OutputStage stage;
    std::string warn;
    if (!Convert(*payload, meshes, &stage, &warn, &err) ||
        !Save(stage, path.string(), kind, result.assets, &err)) {
      std::cerr << "urdf-to-usd: " << err << "\n";
      return 1;
    }
    if (!warn.empty()) std::cerr << warn;
    // Generated textures (MJCF builtins) have no source file: write them
    // beside a USDA/USDC layer; USDZ already packages them.
    if (std::string(kind) != "usdz")
      for (const auto& mat : result.payload["materials"]) {
        if (!mat.contains("texture_generated") || !mat.contains("texturePackage")) continue;
        auto bytes = result.assets.find(mat["texturePackage"].get<std::string>());
        if (bytes == result.assets.end() || !bytes->second) continue;
        const fs::path target = path.parent_path() / mat["texture"].get<std::string>();
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target.string(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes->second->data()),
                  static_cast<std::streamsize>(bytes->second->size()));
        if (!out) {
          std::cerr << "urdf-to-usd: cannot write " << target.string() << "\n";
          return 1;
        }
      }
    // Generated level sets (MJCF sdf geoms) are referenced by relative path.
    if (std::string(kind) != "usdz")
      for (const auto& name : result.payload["generatedAssets"]) {
        if (!name.is_string()) continue;
        auto bytes = result.assets.find(name.get<std::string>());
        if (bytes == result.assets.end() || !bytes->second) continue;
        const fs::path target = path.parent_path() / name.get<std::string>();
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target.string(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes->second->data()),
                  static_cast<std::streamsize>(bytes->second->size()));
        if (!out) {
          std::cerr << "urdf-to-usd: cannot write " << target.string() << "\n";
          return 1;
        }
      }
    std::cout << "Wrote " << path.string() << "\n";
  }
  size_t visuals = 0, collisions = 0;
  for (auto& link : result.payload["links"]) {
    visuals += link["visuals"].size();
    collisions += link["collisions"].size();
  }
  std::cout << "Converted "
            << result.payload.value("name", std::string("scene")) << ": "
            << result.payload["links"].size() << " links, "
            << result.payload["joints"].size() << " joints, " << visuals
            << " visual meshes, " << collisions << " collisions, "
            << result.payload["actuators"].size() << " actuators.\n";
  return 0;
}
