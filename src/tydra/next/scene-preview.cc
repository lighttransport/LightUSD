// SPDX-License-Identifier: Apache-2.0
#include "core/native-io.hh"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "external/jsonhpp/nlohmann/json.hpp"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "scene-preview-internal.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace preview_impl = preview_detail;
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
// Invalidate previews sampled before inactiveIds metadata affected visibility.
constexpr int kVersion = 4;
json FileStamp(const std::string& path) {
  // Package dependencies are invalidated by their containing archive.
  const std::string file = path.substr(0, path.find('['));
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  if (ec) {
    std::error_code exists_error;
    if (!file.empty() && !fs::exists(file, exists_error) && !exists_error)
      return json{{"path", file}, {"missing", true}};
    return json();
  }
  const auto time = fs::last_write_time(file, ec);
  if (ec) return json();
  return json{{"path", file},
              {"size", size},
              {"mtime", time.time_since_epoch().count()}};
}
std::string Fingerprint(const std::string& source,
                        const ScenePreviewOptions& o) {
  std::error_code ec;
  const auto path = fs::absolute(source, ec);
  std::ostringstream s;
  s << kVersion << ':' << (ec ? source : path.lexically_normal().string())
    << ':' << ScenePreviewKindName(o.kind) << ':' << o.gpu_budget_bytes << ':'
    << o.resolution << ':' << std::setprecision(17) << o.time_code << ':'
    << o.texture_max_edge << ':' << o.texture_budget_bytes << ':'
    << o.composition.load_payloads << ':'
    << static_cast<int>(o.composition.expression_variable_policy);
  if (o.region_enabled) {
    s << ":region-4:" << o.sampling_cell_size << ":" << o.base_preview_path;
    for (size_t k = 0; k < 3; ++k)
      s << ':' << o.region_min[k] << ':' << o.region_max[k];
  }
  if (o.kind == ScenePreviewKind::Volume)
    s << ":adaptive-bricks-1:compute-1:" << static_cast<int>(o.compute_backend)
      << ":carrier:" << o.volume_surface_carrier;
  if (o.composition.strict_aousd_conformance) s << ":strict";
  if (o.composition.error_when_asset_not_found) s << ":missing-error";
  if (!o.composition.detect_instances) s << ":no-instances";
  for (const auto& p : o.composition.variant_overrides_by_path)
    for (const auto& v : p.second)
      s << ':' << p.first << ':' << v.first << '=' << v.second;
  for (const auto& p : o.composition.variant_overrides)
    s << ':' << p.first << '=' << p.second;
  for (const auto& p : o.composition.variant_fallbacks)
    for (const auto& v : p.second) s << ':' << p.first << '=' << v;
  std::ostringstream hex;
  hex << std::hex << preview_impl::Hash(s.str());
  return hex.str();
}
json ReadJson(const fs::path& path) {
  std::error_code ec;
  if (fs::file_size(path, ec) > (uint64_t(64) << 20) || ec) return json();
  std::ifstream input(path);
  if (!input) return json();
  auto value = json::parse(input, nullptr, false);
  return value.is_discarded() ? json() : value;
}
bool ValidManifest(const json& value) {
  if (!value.is_object() || !value.contains("version") ||
      !value["version"].is_number_integer() || value["version"] != kVersion ||
      !value.contains("dependencies") || !value["dependencies"].is_array() ||
      value["dependencies"].empty() || !value.contains("statistics") ||
      !value["statistics"].is_object())
    return false;
  const auto& stats = value["statistics"];
  for (const char* key : {"source_prims", "source_meshes", "source_instances",
                          "samples", "estimated_gpu_bytes"})
    if (!stats.contains(key) || !stats[key].is_number_unsigned()) return false;
  if (!stats.contains("voxel_size") || !stats["voxel_size"].is_number() ||
      !std::isfinite(stats["voxel_size"].get<double>()) ||
      stats["voxel_size"].get<double>() <= 0)
    return false;
  for (int k = 0; k < 3; ++k)
    for (const char* prefix : {"min", "max"}) {
      const auto key = std::string(prefix) + std::to_string(k);
      if (!stats.contains(key) || !stats[key].is_number() ||
          !std::isfinite(stats[key].get<double>()))
        return false;
    }
  return true;
}
bool ValidSourceIndex(const fs::path& path, const json& j) {
  if (!j.is_object() || !j.contains("dependencies") ||
      !j["dependencies"].is_array() || j["dependencies"].empty() ||
      !j.contains("outputs") || !j["outputs"].is_array())
    return false;
  for (const auto& dep : j["dependencies"])
    if (!dep.is_object() || !dep.contains("path") || !dep["path"].is_string() ||
        FileStamp(dep["path"].get<std::string>()) != dep)
      return false;
  for (const auto& f : j["outputs"]) {
    if (!f.is_object() || !f.contains("path") || !f["path"].is_string() ||
        !f.contains("size") || !f["size"].is_number_unsigned())
      return false;
    const auto name = f["path"].get<std::string>();
    std::error_code ec;
    if (fs::path(name).is_absolute() || name.find("..") != std::string::npos ||
        fs::file_size(path / name, ec) != f["size"].get<uint64_t>() || ec)
      return false;
  }
  std::error_code ec;
  return fs::is_regular_file(path / "geometry.json", ec) && !ec &&
         fs::is_regular_file(path / "catalog.usdc", ec) && !ec;
}
void ReadStatistics(const json& j, ScenePreviewResult& r) {
  r.source_prims = j.value("source_prims", uint64_t(0));
  r.source_meshes = j.value("source_meshes", uint64_t(0));
  r.source_instances = j.value("source_instances", uint64_t(0));
  r.samples = j.value("samples", uint64_t(0));
  if (j.contains("refined_samples") &&
      j["refined_samples"].is_number_unsigned())
    r.refined_samples = j["refined_samples"].get<uint64_t>();
  r.voxel_size = j.value("voxel_size", 0.0);
  r.estimated_gpu_bytes = j.value("estimated_gpu_bytes", uint64_t(0));
  for (int i = 0; i < 3; ++i) {
    r.bounds_min[i] = j.value("min" + std::to_string(i), 0.0);
    r.bounds_max[i] = j.value("max" + std::to_string(i), 0.0);
  }
}
json Statistics(const ScenePreviewResult& r) {
  json j = {{"source_prims", r.source_prims},
            {"source_meshes", r.source_meshes},
            {"source_instances", r.source_instances},
            {"samples", r.samples},
            {"refined_samples", r.refined_samples},
            {"voxel_size", r.voxel_size},
            {"estimated_gpu_bytes", r.estimated_gpu_bytes}};
  for (int i = 0; i < 3; ++i) {
    j["min" + std::to_string(i)] = r.bounds_min[i];
    j["max" + std::to_string(i)] = r.bounds_max[i];
  }
  return j;
}
}  // namespace
const char* ScenePreviewKindName(ScenePreviewKind kind) {
  switch (kind) {
    case ScenePreviewKind::Gaussian:
      return "gaussian";
    case ScenePreviewKind::Points:
      return "points";
    case ScenePreviewKind::Volume:
      return "volume";
  }
  return "invalid";
}
bool ParseScenePreviewKind(const std::string& text, ScenePreviewKind* kind) {
  if (!kind) return false;
  if (text == "gaussian")
    *kind = ScenePreviewKind::Gaussian;
  else if (text == "points")
    *kind = ScenePreviewKind::Points;
  else if (text == "volume")
    *kind = ScenePreviewKind::Volume;
  else
    return false;
  return true;
}
uint64_t ScenePreviewGpuBudget(uint64_t capacity, uint64_t available) {
  uint64_t budget = capacity ? capacity / 4 : uint64_t(2) << 30;
  if (available) {
    const uint64_t reserve = uint64_t(512) << 20;
    budget = std::min(budget, available > reserve ? available - reserve : 0);
  }
  return budget;
}
std::string DefaultScenePreviewCacheDirectory() {
  return (fs::path(".cache") / "lightusd" / "scene-preview").string();
}
ScenePreviewResult ConvertScenePreview(
    const std::string& source, ::lightusd::next::AssetResolver& resolver,
    const ScenePreviewOptions& options) {
  if (!::lightusd::NativeIOEnabled()) { ScenePreviewResult r; r.error = ::lightusd::NativeIOUnavailableMessage(); return r; }

  const auto start = std::chrono::steady_clock::now();
  ScenePreviewResult result;
  result.refinement_region = options.region_enabled;
  result.refinement_min = options.region_min;
  result.refinement_max = options.region_max;
  auto finish = [&]() {
    result.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    return result;
  };
  if (options.composition.payload_policy ||
      options.composition.payload_policy_with_prim) {
    result.error =
        "preview caching requires a file-authored payload selection; use a USD "
        "wrapper layer";
    return finish();
  }
  if (options.region_enabled) {
    bool valid = std::isfinite(options.sampling_cell_size) &&
                 options.sampling_cell_size > 0;
    for (size_t k = 0; k < 3; ++k)
      valid = valid && std::isfinite(options.region_min[k]) &&
              std::isfinite(options.region_max[k]) &&
              options.region_min[k] < options.region_max[k];
    if (!valid) {
      result.error = "invalid source refinement region";
      return finish();
    }
  }
  if (source.empty() || !std::isfinite(options.time_code) ||
      options.batch_prims == 0 || options.resolution > 65536 ||
      options.gpu_budget_bytes < (uint64_t(64) << 20) ||
      options.host_budget_bytes < (uint64_t(256) << 20) ||
      !options.texture_budget_bytes || !options.texture_max_edge) {
    result.error = "invalid preview options or insufficient memory budget";
    return finish();
  }
  const fs::path cache = options.cache_directory.empty()
                             ? DefaultScenePreviewCacheDirectory()
                             : options.cache_directory;
  const fs::path directory = cache / Fingerprint(source, options);
  json legacy_lighting;
  if (!options.refresh) {
    const auto manifest = ReadJson(directory / "manifest.json");
    bool valid = ValidManifest(manifest);
    if (options.kind != ScenePreviewKind::Gaussian) {
      valid = valid && manifest.contains("sample_cache") &&
              manifest["sample_cache"].is_object();
      if (valid) {
        const auto& sampled = manifest["sample_cache"];
        valid = sampled.contains("path") && sampled["path"].is_string() &&
                FileStamp(sampled["path"].get<std::string>()) == sampled;
      }
    }
    std::error_code ec;
    valid = valid && fs::is_regular_file(directory / "root.usdc", ec);
    if (valid)
      for (const auto& dep : manifest["dependencies"]) {
        if (!dep.is_object() || !dep.contains("path") ||
            !dep["path"].is_string() ||
            FileStamp(dep["path"].get<std::string>()) != dep) {
          valid = false;
          break;
        }
      }
    if (valid && manifest.contains("outputs") &&
        manifest["outputs"].is_array()) {
      for (const auto& file : manifest["outputs"]) {
        if (!file.is_object() || !file.contains("path") ||
            !file["path"].is_string() || !file.contains("size") ||
            !file["size"].is_number_unsigned()) {
          valid = false;
          break;
        }
        const fs::path relative = file["path"].get<std::string>();
        if (relative.is_absolute() ||
            relative.string().find("..") != std::string::npos ||
            fs::file_size(directory / relative, ec) !=
                file["size"].get<uint64_t>() ||
            ec) {
          valid = false;
          break;
        }
      }
    } else
      valid = false;
    bool has_emission_sidecar = false;
    if (manifest.contains("outputs") && manifest["outputs"].is_array())
      for (const auto& output : manifest["outputs"])
        if (output.contains("path") && output["path"].is_string() &&
            fs::path(output["path"].get<std::string>()).extension() == ".hdr")
          has_emission_sidecar = true;
    const bool lighting_current =
        manifest.contains("lighting_version") &&
        manifest["lighting_version"].is_number_integer() &&
        (manifest["lighting_version"] == 2 ||
         (manifest["lighting_version"] == 1 && !has_emission_sidecar));
    if (valid && !lighting_current) {
      if (options.kind == ScenePreviewKind::Gaussian && !options.region_enabled)
        legacy_lighting = manifest;
      valid = false;
    }
    if (valid) {
      ReadStatistics(manifest["statistics"], result);
      if (manifest.contains("warning") && manifest["warning"].is_string())
        result.warning = manifest["warning"].get<std::string>();
      result.success = true;
      result.cache_hit = true;
      result.usd_path = (directory / "root.usdc").string();
      result.render_usd_path =
          (directory / (options.kind == ScenePreviewKind::Volume ? "render.usdc"
                                                                 : "root.usdc"))
              .string();
      return finish();
    }
  }
  std::error_code ec;
  fs::create_directories(cache, ec);
  if (ec) {
    result.error = "cannot create preview cache: " + ec.message();
    return finish();
  }
  // A unique sibling keeps an interrupted refresh from destroying a good entry.
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path work = cache / (directory.filename().string() + ".building-" +
                                 std::to_string(nonce));
  const bool created = fs::create_directory(work, ec);
  if (!created || ec) {
    result.error = "cannot create preview working directory: " + ec.message();
    return finish();
  }
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(p, e);
    }
  } cleanup{work};
  preview_impl::Build build(options, result, work);
  json dependencies = json::array();
  json sample_cache;
  if (options.kind != ScenePreviewKind::Gaussian) {
    // Geometry discovery/sampling is representation-independent. Reuse the
    // canonical Gaussian cache rather than expanding large instance sets again.
    auto sampled_options = options;
    sampled_options.kind = ScenePreviewKind::Gaussian;
    const auto sampled = ConvertScenePreview(source, resolver, sampled_options);
    if (!sampled.success) {
      result.error = sampled.error;
      return finish();
    }
    const auto manifest =
        ReadJson(fs::path(sampled.usd_path).parent_path() / "manifest.json");
    if (!ValidManifest(manifest)) {
      result.error = "sample cache changed during conversion";
      return finish();
    }
    dependencies = manifest["dependencies"];
    sample_cache = FileStamp(
        (fs::path(sampled.usd_path).parent_path() / "manifest.json").string());
    if (sample_cache.is_null()) {
      result.error = "sample cache manifest disappeared";
      return finish();
    }
    dependencies.push_back(sample_cache);
    result.refined_samples = sampled.refined_samples;
    result.source_prims = sampled.source_prims;
    result.source_meshes = sampled.source_meshes;
    result.source_instances = sampled.source_instances;
    result.warning = sampled.warning;
    build.cell_size = sampled.voxel_size;
    ::lightusd::next::Stage stage;
    ::lightusd::next::pcp::CompositionOptions read;
    read.num_threads = 1;
    if (!::lightusd::next::pcp::ComposeStageFromFile(
            sampled.usd_path, resolver, &stage, read, &result.warning,
            &result.error))
      return finish();
    ::lightusd::next::pcp::ComposedBatch batch;
    batch.layer = stage.ReleaseRootLayer();
    if (!build.plan(batch) || !build.finish_plan()) return finish();
    stage.SetRootLayer(std::move(*batch.layer));
    bool ok = true;
    stage.Traverse([&](const ::lightusd::next::UsdPrim& prim) {
      if (prim.GetTypeName() != "ParticleField3DGaussianSplat") return true;
      const auto samples =
          preview_impl::ReadPreviewSamples(prim, ScenePreviewKind::Gaussian);
      for (const auto& sample : samples)
        if (!build.insert(sample, false)) {
          ok = false;
          return false;
        }
      if (!build.check("reuse-samples", build.cells.size())) {
        ok = false;
        return false;
      }
      return true;
    });
    if (!ok) return finish();
  } else if (!legacy_lighting.is_null()) {
    // Upgrade only lighting: reuse the valid bounded particle geometry while
    // revisiting the source planning catalog, never its geometry sampling pass.
    ReadStatistics(legacy_lighting["statistics"], result);
    build.cell_size = result.voxel_size;
    dependencies = legacy_lighting["dependencies"];
    auto compose = options.composition;
    compose.flatten_instances = false;
    compose.instance_flatten_mode =
        ::lightusd::next::pcp::InstanceFlattenMode::Native;
    compose.release_layers_during_fill = true;
    ::lightusd::next::pcp::BatchedComposeOptions batches;
    batches.target_batch_prims = options.batch_prims;
    batches.roots_per_batch = options.batch_roots;
    batches.pipeline = false;
    ::lightusd::next::pcp::BatchedComposer composer;
    if (!build.check("light-upgrade", 0) ||
        !composer.Open(source, resolver, compose, batches, &result.warning,
                       &result.error) ||
        !composer.Plan(&result.warning, &result.error,
                       [&](auto& batch) { return build.plan(batch); }))
      return finish();
    if (composer.whole() &&
        !composer.Run([&](auto& batch) { return build.plan(batch); },
                      &result.warning, &result.error))
      return finish();
    build.material_specs.clear();
    ::lightusd::next::Stage old;
    ::lightusd::next::pcp::CompositionOptions read;
    read.num_threads = 1;
    if (!::lightusd::next::pcp::ComposeStageFromFile(
            (directory / "root.usdc").string(), resolver, &old, read,
            &result.warning, &result.error))
      return finish();
    bool ok = true;
    old.Traverse([&](const ::lightusd::next::UsdPrim& prim) {
      if (prim.GetTypeName() == "ParticleField3DGaussianSplat")
        for (const auto& sample : preview_impl::ReadPreviewSamples(
                 prim, ScenePreviewKind::Gaussian)) {
          // The cache already selected its samples. Requantizing float
          // positions onto the source lattice can merge neighbors at cell
          // boundaries.
          if (build.cells.size() >= build.max_cells) {
            result.error = "cached particles exceed the preview budget";
            ok = false;
            return false;
          }
          preview_impl::Key key;
          key.x = static_cast<int64_t>(build.cells.size());
          build.cells.emplace(key, preview_impl::Accum{sample, 0});
        }
      return build.check("reuse-samples", build.cells.size());
    });
    if (!ok || !result.error.empty()) return finish();
  } else {
    auto indexed_options = options;
    indexed_options.region_enabled = false;
    indexed_options.base_preview_path.clear();
    indexed_options.resolution = 0;
    indexed_options.gpu_budget_bytes = uint64_t(2) << 30;
    const fs::path source_index =
        cache / ("source-index-v1-" + Fingerprint(source, indexed_options));
    const auto index_manifest = ReadJson(source_index / "manifest.json");
    const bool use_index = options.region_enabled && !options.refresh &&
                           ValidSourceIndex(source_index, index_manifest);
    if (use_index) {
      dependencies = index_manifest["dependencies"];
      const auto base_manifest = ReadJson(
          fs::path(options.base_preview_path).parent_path() / "manifest.json");
      if (ValidManifest(base_manifest)) {
        const auto& stats = base_manifest["statistics"];
        result.source_prims = stats["source_prims"].get<uint64_t>();
        result.source_meshes = stats["source_meshes"].get<uint64_t>();
        result.source_instances = stats["source_instances"].get<uint64_t>();
      }
      if (!build.source_load_index(source_index)) return finish();
      // Indexed geometry catalogs predate lighting upgrades. The canonical
      // preview owns the current portable light assets and camera catalog.
      if (!options.base_preview_path.empty()) {
        ::lightusd::next::Stage base;
        ::lightusd::next::pcp::CompositionOptions read;
        read.num_threads = 1;
        if (!::lightusd::next::pcp::ComposeStageFromFile(
                options.base_preview_path, resolver, &base, read,
                &result.warning, &result.error))
          return finish();
        build.extras.clear();
        ::lightusd::next::pcp::ComposedBatch batch;
        batch.layer = base.ReleaseRootLayer();
        if (!build.plan(batch)) return finish();
      }

    } else {
      build.source_capture = true;
      build.source_directory = work / "source-index";
      fs::create_directories(build.source_directory, ec);
      if (ec) {
        result.error = "cannot create source geometry index";
        return finish();
      }
      ::lightusd::next::pcp::CompositionOptions compose = options.composition;
      compose.flatten_instances = false;
      compose.instance_flatten_mode =
          ::lightusd::next::pcp::InstanceFlattenMode::Native;
      compose.background_reclaim = true;
      compose.release_layers_during_fill = true;
      ::lightusd::next::pcp::BatchedComposeOptions batches;
      batches.target_batch_prims = options.batch_prims;
      batches.roots_per_batch = options.batch_roots;
      batches.pipeline = false;
      ::lightusd::next::pcp::BatchedComposer composer;
      if (!build.check("open", 0) ||
          !composer.Open(source, resolver, compose, batches, &result.warning,
                         &result.error) ||
          !composer.Plan(&result.warning, &result.error,
                         [&](auto& batch) { return build.plan(batch); }))
        return finish();
      if (!composer.whole() && !build.finish_plan()) return finish();
      for (const auto& file : composer.source_identifiers()) {
        const auto dep = FileStamp(file);
        if (dep.is_null() || dep.contains("missing")) {
          result.error =
              "preview caching requires file-backed source dependencies: " +
              file;
          return finish();
        }
        dependencies.push_back(dep);
        build.dependencies.insert(file);
      }
      // Plan() has no callbacks for plain, already-composed files.
      if (!composer.Run(
              [&](auto& batch) {
                if (composer.whole() &&
                    (!build.plan(batch) || !build.finish_plan()))
                  return false;
                return build.consume(batch);
              },
              &result.warning, &result.error))
        return finish();
      if (!build.flush_geometry() || !build.finish_placements() ||
          !build.flush_geometry() || !build.source_save_index())
        return finish();
      json indexed_outputs = json::array();
      for (fs::directory_iterator it(build.source_directory, ec), end;
           !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec))
          indexed_outputs.push_back({{"path", it->path().filename().string()},
                                     {"size", it->file_size(ec)}});
      }
      for (const auto& file : build.dependencies) {
        const auto dep = FileStamp(file);
        if (!dep.is_null() &&
            std::find(dependencies.begin(), dependencies.end(), dep) ==
                dependencies.end())
          dependencies.push_back(dep);
      }
      for (const auto& dep : dependencies)
        if (FileStamp(dep["path"].get<std::string>()) != dep) {
          result.error = "source changed while indexing";
          return finish();
        }
      {
        std::ofstream f(build.source_directory / "manifest.json");
        f << json{{"dependencies", dependencies}, {"outputs", indexed_outputs}}
                 .dump();
        if (!f || ec) {
          result.error = "cannot publish source index manifest";
          return finish();
        }
      }
      // A valid index is immutable. Concurrent converters can keep the winner.
      if (!ValidSourceIndex(source_index,
                            ReadJson(source_index / "manifest.json"))) {
        fs::remove_all(source_index, ec);
        ec.clear();
        fs::rename(build.source_directory, source_index, ec);
        if (ec) {
          result.error =
              "cannot publish source geometry index: " + ec.message();
          return finish();
        }
      }
      build.source_capture = false;
    }
  }
  if (!build.flush_geometry() || !build.finish_placements() ||
      !build.flush_geometry())
    return finish();
  if (options.region_enabled && options.kind == ScenePreviewKind::Gaussian)
    result.refined_samples = build.cells.size();
  if (!options.base_preview_path.empty() &&
      options.kind == ScenePreviewKind::Gaussian) {
    const uint64_t reserve =
        std::min(options.gpu_budget_bytes / 2, uint64_t(512) << 20);
    build.max_cells = static_cast<size_t>(std::max(
        uint64_t(1), std::min((options.gpu_budget_bytes - reserve) / 1024,
                              options.host_budget_bytes / 1024)));
    ::lightusd::next::Stage base;
    ::lightusd::next::pcp::CompositionOptions read;
    read.num_threads = 1;
    if (!::lightusd::next::pcp::ComposeStageFromFile(
            options.base_preview_path, resolver, &base, read, &result.warning,
            &result.error))
      return finish();
    std::vector<preview_impl::Sample> outside;
    base.Traverse([&](const ::lightusd::next::UsdPrim& prim) {
      if (prim.GetTypeName() != "ParticleField3DGaussianSplat") return true;
      for (const auto& s :
           preview_impl::ReadPreviewSamples(prim, ScenePreviewKind::Gaussian)) {
        bool inside = true;
        for (size_t k = 0; k < 3; ++k)
          inside = inside && s.p[k] >= options.region_min[k] &&
                   s.p[k] <= options.region_max[k];
        if (!inside || !result.refined_samples) outside.push_back(s);
      }
      return build.check("merge-region", build.cells.size());
    });
    if (!result.error.empty()) return finish();
    const size_t capacity =
        std::max(size_t(1), build.max_cells - build.cells.size());
    double background_cell = build.cell_size;
    while (outside.size() > capacity) {
      background_cell *= 2;
      std::map<std::array<int64_t, 4>, preview_impl::Sample> coarse;
      for (auto sample : outside) {
        std::array<int64_t, 4> key{};
        int axis = 0;
        for (size_t k = 0; k < 3; ++k) {
          key[k] = static_cast<int64_t>(std::max(
              -9e18,
              std::min(9e18, std::floor(sample.p[k] / background_cell))));
          if (std::abs(sample.n[k]) > std::abs(sample.n[axis]))
            axis = static_cast<int>(k);
        }
        key[3] = axis * 2 + (sample.n[axis] < 0 ? 1 : 0);
        sample.radius =
            std::max(sample.radius, static_cast<float>(background_cell));
        coarse.emplace(key, sample);
      }
      outside.clear();
      for (const auto& item : coarse) outside.push_back(item.second);
      if (!build.check("coarsen-background", outside.size())) return finish();
    }
    for (const auto& sample : outside)
      if (!build.insert(sample, false)) return finish();
  }
  if (!build.bake_lights()) return finish();
  if (!build.check("write", build.cells.size()) ||
      !preview_impl::WriteProxy(build))
    return finish();
  for (const auto& file : build.dependencies) {
    const auto dep = FileStamp(file);
    if (!dep.is_null() && std::find(dependencies.begin(), dependencies.end(),
                                    dep) == dependencies.end())
      dependencies.push_back(dep);
  }
  for (const auto& dep : dependencies)
    if (FileStamp(dep["path"].get<std::string>()) != dep) {
      result.error = "source dependency changed during preview conversion";
      return finish();
    }
  json outputs = json::array();
  for (fs::directory_iterator it(work, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() != ".samples")
      outputs.push_back({{"path", it->path().filename().string()},
                         {"size", it->file_size(ec)}});
  }
  if (ec) {
    result.error = "cannot enumerate preview sidecars: " + ec.message();
    return finish();
  }
  // Prototype scratch is not part of a published cache entry.
  fs::remove_all(work / "prototypes", ec);
  {
    std::ofstream f(work / "manifest.json");
    f << json{{"version", kVersion},
              {"lighting_version", 2},
              {"sample_cache", sample_cache},
              {"dependencies", dependencies},
              {"warning", result.warning},
              {"outputs", outputs},
              {"statistics", Statistics(result)}}
             .dump(2);
    if (!f) {
      result.error = "cannot write preview manifest";
      return finish();
    }
  }
  // Replace only an entry owned by this cache key; keep old data until ready.
  const fs::path old =
      cache / (directory.filename().string() + ".old-" + std::to_string(nonce));
  if (fs::exists(directory, ec)) {
    fs::rename(directory, old, ec);
    if (ec) {
      result.error = "cannot replace preview cache: " + ec.message();
      return finish();
    }
  }
  fs::rename(work, directory, ec);
  if (ec) {
    std::error_code restore;
    if (fs::exists(old, restore)) fs::rename(old, directory, restore);
    result.error = "cannot publish preview cache: " + ec.message();
    return finish();
  }
  fs::remove_all(old, ec);
  result.success = true;
  result.usd_path = (directory / "root.usdc").string();
  result.render_usd_path =
      (directory /
       (options.kind == ScenePreviewKind::Volume ? "render.usdc" : "root.usdc"))
          .string();
  return finish();
}
ScenePreviewResult RefineScenePreview(const std::string& source,
                                      const ScenePreviewResult& base,
                                      const ScenePreviewOptions& options,
                                      const ScenePreviewLodCamera& camera,
                                      int width, int height,
                                      double pixel_error) {
  if (!::lightusd::NativeIOEnabled()) { ScenePreviewResult r; r.error = ::lightusd::NativeIOUnavailableMessage(); return r; }

  if (!base.success || width < 1 || height < 1 || !std::isfinite(pixel_error) ||
      pixel_error <= 0 || camera.focal_pixels <= 0)
    return base;
  ::lightusd::next::AssetResolver resolver;
  auto canonical_options = options;
  canonical_options.kind = ScenePreviewKind::Gaussian;
  const auto canonical =
      ConvertScenePreview(source, resolver, canonical_options);
  if (!canonical.success) return canonical;
  ::lightusd::next::Stage stage;
  ::lightusd::next::pcp::CompositionOptions read;
  read.num_threads = 1;
  std::string warning, error;
  if (!::lightusd::next::pcp::ComposeStageFromFile(
          canonical.usd_path, resolver, &stage, read, &warning, &error)) {
    auto failure = base;
    failure.success = false;
    failure.error = error;
    return failure;
  }
  double depth = 0, score = 1e300, visible_spacing = 0;
  // Inspect only cached samples to choose a visible source window. This avoids
  // refining around an unrelated distant environment shell or behind the eye.
  stage.Traverse([&](const ::lightusd::next::UsdPrim& prim) {
    if (prim.GetTypeName() != "ParticleField3DGaussianSplat") return true;
    for (const auto& s :
         preview_impl::ReadPreviewSamples(prim, ScenePreviewKind::Gaussian)) {
      double d = 0, length2 = 0;
      for (size_t k = 0; k < 3; ++k) {
        const double q = s.p[k] - camera.eye[k];
        d += q * camera.forward[k];
        length2 += q * q;
      }
      if (d <= camera.near_plane) continue;
      const double transverse = std::max(0., length2 - d * d);
      const double radius = (camera.orthographic ? 1 : d) *
                                std::hypot(double(width), double(height)) /
                                (2 * camera.focal_pixels) +
                            s.radius;
      if (transverse > radius * radius) continue;
      const double candidate = d * (1 + transverse / std::max(1e-20, d * d));
      if (candidate < score) {
        score = candidate;
        depth = d;
        visible_spacing = s.radius;
      }
    }
    return true;
  });
  if (score == 1e300) return base;
  depth = std::max(camera.near_plane * 4, depth);
  bool same_region = base.refinement_region;
  for (size_t k = 0; k < 3; ++k) {
    const double focus = camera.eye[k] + camera.forward[k] * depth;
    same_region = same_region && focus >= base.refinement_min[k] &&
                  focus <= base.refinement_max[k];
  }
  const double local_cell =
      same_region ? base.voxel_size
                  : std::max(canonical.voxel_size, visible_spacing);
  const double desired = pixel_error * (camera.orthographic ? 1 : depth) /
                         (1.75 * camera.focal_pixels);
  if (desired >= local_cell * .8) return base;
  auto fine = options;
  fine.resolution = 0;
  fine.region_enabled = true;
  fine.sampling_cell_size = std::max(1e-9, std::min(local_cell * .5, desired));
  const double radius = std::max(fine.sampling_cell_size * 8,
                                 (camera.orthographic ? 1 : depth) *
                                     std::hypot(double(width), double(height)) /
                                     (2 * camera.focal_pixels));
  for (size_t k = 0; k < 3; ++k) {
    const double focus = camera.eye[k] + camera.forward[k] * depth;
    const double quantum = std::max(fine.sampling_cell_size * 8, radius / 4);
    const double midpoint = std::round(focus / quantum) * quantum;
    fine.region_min[k] = midpoint - radius;
    fine.region_max[k] = midpoint + radius;
  }
  fine.base_preview_path = canonical.usd_path;
  const auto refined = ConvertScenePreview(source, resolver, fine);
  if (refined.success &&
      (!refined.refined_samples || refined.voxel_size >= local_cell * .99))
    return base;
  return refined;
}

bool ExportScenePreview(const ScenePreviewResult& result,
                        const std::string& directory, std::string* error) {
  if (!::lightusd::NativeIOAvailable(error)) return false;

  std::error_code ec;
  if (!result.success || fs::exists(directory, ec)) {
    if (error)
      *error =
          "preview export requires a successful result and a new destination "
          "directory";
    return false;
  }
  const fs::path target(directory);
  fs::path work = target;
  work += ".export-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count());
  fs::copy(fs::path(result.usd_path).parent_path(), work,
           fs::copy_options::recursive, ec);
  if (!ec && fs::exists(target, ec)) {
    if (error) *error = "preview export destination appeared during copy";
    std::error_code cleanup;
    fs::remove_all(work, cleanup);
    return false;
  }
  if (!ec) fs::rename(work, target, ec);
  if (ec) {
    if (error) *error = "cannot export preview: " + ec.message();
    std::error_code cleanup;
    fs::remove_all(work, cleanup);
    return false;
  }
  return true;
}
namespace preview_detail {
uint64_t Hash(const std::string& s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}
Build::Build(const ScenePreviewOptions& o, ScenePreviewResult& r, fs::path d)
    : opts(o), result(r), directory(std::move(d)), converter(), decoder([&] {
        TextureDecodeOptions t;
        t.max_edge = o.texture_max_edge;
        t.budget_bytes = o.texture_budget_bytes;
        t.max_source_bytes = uint64_t(256) << 20;
        return t;
      }()) {
  cell_size = o.sampling_cell_size;
  // Conservative common cost: records + both upload copies + BVH build and
  // traversal/sort scratch. Keep half a GiB for context, shaders and targets.
  const uint64_t reserve =
      std::min(o.gpu_budget_bytes / 2, uint64_t(512) << 20);
  max_cells = static_cast<size_t>(std::min(
      (o.gpu_budget_bytes - reserve) / 1024, o.host_budget_bytes / 1024));
  max_cells = std::max(size_t(1), max_cells);
  if (o.region_enabled && o.kind == ScenePreviewKind::Gaussian &&
      !o.base_preview_path.empty()) {
    const auto manifest =
        ReadJson(fs::path(o.base_preview_path).parent_path() / "manifest.json");
    if (ValidManifest(manifest)) {
      const auto retained = manifest["statistics"]["samples"].get<uint64_t>();
      max_cells = std::max(max_cells / 4,
                           max_cells - static_cast<size_t>(std::min(
                                           uint64_t(max_cells - 1), retained)));
    }
  }
  std::error_code ec;
  fs::create_directories(directory / "prototypes", ec);
}
bool Build::check(const std::string& phase, uint64_t n) {
  if (opts.progress && !opts.progress(phase, n)) {
    result.error = "preview conversion cancelled";
    return false;
  }
  const auto& host = opts.host_services;
  if (host && host->resident_memory_bytes) {
    const uint64_t resident = host->resident_memory_bytes();
    if (resident && resident > opts.host_budget_bytes) {
      result.error = "preview host RSS budget exceeded; reduce batch size or resolution";
      return false;
    }
  }
  if (host && host->available_memory_bytes && opts.min_available_host_bytes) {
    const uint64_t available = host->available_memory_bytes();
    if (available && available < opts.min_available_host_bytes) {
      result.error = "preview stopped to preserve the caller's host memory reserve";
      return false;
    }
  }

  return true;
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
