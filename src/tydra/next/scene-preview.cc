// SPDX-License-Identifier: Apache-2.0
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
#if defined(__linux__)
#include <unistd.h>
#endif

namespace lightusd {
namespace tydra {
namespace next {
namespace preview_impl = preview_detail;
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
constexpr int kVersion = 3;
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
  if (o.kind == ScenePreviewKind::Volume) s << ":adaptive-bricks-1";
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
void ReadStatistics(const json& j, ScenePreviewResult& r) {
  r.source_prims = j.value("source_prims", uint64_t(0));
  r.source_meshes = j.value("source_meshes", uint64_t(0));
  r.source_instances = j.value("source_instances", uint64_t(0));
  r.samples = j.value("samples", uint64_t(0));
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
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg && *xdg)
    return (fs::path(xdg) / "lightusd" / "scene-preview").string();
  const char* home = std::getenv("HOME");
  return ((home && *home ? fs::path(home) / ".cache" : fs::path(".cache")) /
          "lightusd" / "scene-preview")
      .string();
}
ScenePreviewResult ConvertScenePreview(
    const std::string& source, ::lightusd::next::AssetResolver& resolver,
    const ScenePreviewOptions& options) {
  const auto start = std::chrono::steady_clock::now();
  ScenePreviewResult result;
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
  if (!options.refresh) {
    const auto manifest = ReadJson(directory / "manifest.json");
    bool valid = ValidManifest(manifest);
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
        if (!build.insert(sample)) {
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
  } else {
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
            "preview caching requires file-backed source dependencies: " + file;
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
  }
  if (!build.finish_placements() || !build.check("write", build.cells.size()) ||
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
bool ExportScenePreview(const ScenePreviewResult& result,
                        const std::string& directory, std::string* error) {
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
  // Conservative common cost: records + both upload copies + BVH build and
  // traversal/sort scratch. Keep half a GiB for context, shaders and targets.
  const uint64_t reserve =
      std::min(o.gpu_budget_bytes / 2, uint64_t(512) << 20);
  max_cells = static_cast<size_t>(std::min(
      (o.gpu_budget_bytes - reserve) / 1024, o.host_budget_bytes / 1024));
  max_cells = std::max(size_t(1), max_cells);
  std::error_code ec;
  fs::create_directories(directory / "prototypes", ec);
}
bool Build::check(const std::string& phase, uint64_t n) {
  if (opts.progress && !opts.progress(phase, n)) {
    result.error = "preview conversion cancelled";
    return false;
  }
#if defined(__linux__)
  uint64_t pages = 0, resident = 0;
  std::ifstream stat("/proc/self/statm");
  stat >> pages >> resident;
  const long page_size = sysconf(_SC_PAGESIZE);
  if (stat && page_size > 0 &&
      resident > opts.host_budget_bytes / static_cast<uint64_t>(page_size)) {
    result.error =
        "preview host RSS budget exceeded; reduce batch size or resolution";
    return false;
  }
  std::ifstream mem("/proc/meminfo");
  std::string key, line;
  uint64_t kib = 0;
  while (mem >> key) {
    if (key == "MemAvailable:") {
      mem >> kib;
      break;
    }
    std::getline(mem, line);
  }
  if (kib && kib < (uint64_t(4) << 20)) {
    result.error = "preview stopped to preserve 4 GiB of host memory";
    return false;
  }
#endif
  return true;
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
