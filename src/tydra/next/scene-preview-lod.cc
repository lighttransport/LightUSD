// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#include "external/jsonhpp/nlohmann/json.hpp"
#include "next/crate/crate-writer.hh"
#include "next/eval/xform-eval.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "scene-preview-internal.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace {
namespace usd = ::lightusd::next;
namespace fs = std::filesystem;
using json = nlohmann::json;
using preview_detail::Sample;
bool Save(const fs::path& file, const usd::Layer& layer, std::string* error) {
  usd::CrateWriteOptions options;
  options.num_threads = 1;
  const auto saved =
      usd::CrateWriter(options).WriteLayerToFile(file.string().c_str(), layer);
  if (!saved.success && error) *error = saved.error;
  return saved.success;
}
std::vector<float> Floats(const usd::UsdPrim& prim, const char* name) {
  const auto* value = prim.GetPropertyValue(name);
  if (!value) return {};
  const auto values = value->view().as_float_array();
  return {values.begin(), values.end()};
}
void CopyPrim(usd::Layer& layer, const usd::UsdPrim& prim,
              const std::string& name) {
  usd::LayerBuilder builder(layer);
  builder.begin_prim(name, prim.GetTypeName());
  for (const auto& property : prim.GetPropertyNames()) {
    if (const auto* value = prim.GetPropertyValue(property)) {
      builder.add_property(property, *value);
      if (const auto* meta = prim.GetPropertyMeta(property))
        builder.current()->ensure_property_meta(property) = *meta;
    }
  }
  builder.current()->meta().apiSchemas() = prim.GetMeta().apiSchemas();
  builder.current()->meta().setApiSchemasAuthored();
  builder.end_prim();
  builder.finalize();
}
}  // namespace
std::vector<Sample> preview_detail::ReadPreviewSamples(const usd::UsdPrim& prim,
                                                       ScenePreviewKind kind) {
  const auto p = Floats(prim, "positions"), q = Floats(prim, "orientations"),
             scale = Floats(prim, "scales"),
             color = Floats(prim, "radiance:sphericalHarmonicsCoefficients"),
             alpha = Floats(prim, "opacities");
  const size_t count = p.size() / 3;
  std::vector<Sample> samples(count);
  for (size_t i = 0; i < count; ++i) {
    auto& s = samples[i];
    for (size_t k = 0; k < 3; ++k) {
      s.p[k] = p[i * 3 + k];
      if (color.size() >= count * 3)
        s.color[k] = .5f + .2820947918f * color[i * 3 + k];
    }
    if (q.size() >= count * 4) {
      const float w = q[i * 4], x = q[i * 4 + 1], y = q[i * 4 + 2],
                  z = q[i * 4 + 3];
      s.n[0] = 2 * (x * z + w * y);
      s.n[1] = 2 * (y * z - w * x);
      s.n[2] = 1 - 2 * (x * x + y * y);
    }
    if (scale.size() >= count * 3)
      s.radius = std::max(scale[i * 3], scale[i * 3 + 1]) *
                 (kind == ScenePreviewKind::Gaussian ? 2 : 1);
    if (alpha.size() >= count) s.opacity = alpha[i];
  }
  return samples;
}
namespace {
int64_t GridCoordinate(double value, double cell) {
  const double q = std::floor(value / cell);
  return std::isfinite(q)
             ? static_cast<int64_t>(std::max(-9e18, std::min(9e18, q)))
             : 0;
}
std::vector<Sample> CoarseSamples(const std::vector<Sample>& samples,
                                  double cell) {
  std::map<std::array<int64_t, 4>, Sample> cells;
  for (const auto& sample : samples) {
    std::array<int64_t, 4> key{};
    int axis = 0;
    for (int k = 0; k < 3; ++k) {
      key[size_t(k)] = GridCoordinate(sample.p[k], cell);
      if (std::abs(sample.n[k]) > std::abs(sample.n[axis])) axis = k;
    }
    key[3] = axis * 2 + (sample.n[axis] < 0 ? 1 : 0);
    cells.emplace(key, sample);
  }
  std::vector<Sample> out;
  out.reserve(cells.size());
  for (const auto& entry : cells) out.push_back(entry.second);
  return out;
}
uint64_t MeshLevel(usd::Layer& out, const usd::UsdPrim& prim,
                   const std::string& name, double cell) {
  const auto p = Floats(prim, "points"),
             colors = Floats(prim, "primvars:displayColor");
  const auto* indices_value = prim.GetPropertyValue("faceVertexIndices");
  if (!indices_value) return 0;
  const auto indices = indices_value->view().as_int_array();
  std::map<std::array<int64_t, 3>, int32_t> cells;
  std::vector<int32_t> remap(p.size() / 3), faces;
  std::vector<float> points, rgb;
  for (size_t i = 0; i < remap.size(); ++i) {
    std::array<int64_t, 3> key{};
    for (size_t k = 0; k < 3; ++k) key[k] = GridCoordinate(p[i * 3 + k], cell);
    const auto inserted =
        cells.emplace(key, static_cast<int32_t>(points.size() / 3));
    remap[i] = inserted.first->second;
    if (inserted.second) {
      for (size_t k = 0; k < 3; ++k) {
        points.push_back(p[i * 3 + k]);
        rgb.push_back(colors.size() == p.size() ? colors[i * 3 + k] : .5f);
      }
    }
  }
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const int32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
    if (a < 0 || b < 0 || c < 0 || size_t(a) >= remap.size() ||
        size_t(b) >= remap.size() || size_t(c) >= remap.size())
      return 0;
    if (remap[size_t(a)] == remap[size_t(b)] ||
        remap[size_t(a)] == remap[size_t(c)] ||
        remap[size_t(b)] == remap[size_t(c)])
      continue;
    faces.insert(faces.end(),
                 {remap[size_t(a)], remap[size_t(b)], remap[size_t(c)]});
  }
  if (faces.empty()) return 0;
  usd::LayerBuilder builder(out);
  builder.begin_prim(name, "Mesh");
  builder.add_property(
      "points", usd::Value::MakeArrayFromRaw(usd::TypeId::Point3f,
                                             points.data(), points.size() / 3));
  builder.add_property("faceVertexIndices", usd::Value::MakeIntArray(faces));
  builder.add_property(
      "faceVertexCounts",
      usd::Value::MakeIntArray(std::vector<int32_t>(faces.size() / 3, 3)));
  builder.add_property("primvars:displayColor",
                       usd::Value::MakeArrayFromRaw(
                           usd::TypeId::Color3f, rgb.data(), rgb.size() / 3));
  builder.current()
      ->ensure_property_meta("primvars:displayColor")
      .interpolation = "vertex";
  builder.add_property("subdivisionScheme", usd::Value::MakeToken("none"));
  builder.add_property("doubleSided", usd::Value(true));
  builder.end_prim();
  builder.finalize();
  return points.size() * sizeof(float) * 12 +
         faces.size() * sizeof(int32_t) * 4;
}
bool ReadManifest(const fs::path& path, ScenePreviewLods* out) {
  std::error_code ec;
  if (fs::file_size(path / "manifest.json", ec) > (uint64_t(8) << 20) || ec)
    return false;
  std::ifstream input(path / "manifest.json");
  const auto j = json::parse(input, nullptr, false);
  if (!j.is_object() || !j.contains("version") || j["version"] != 3 ||
      !j.contains("tiles") || !j["tiles"].is_array() ||
      !j.contains("outputs") || !j["outputs"].is_object() ||
      !j["outputs"].contains("root.usdc") || !j.contains("cell") ||
      !j["cell"].is_number() || !j.contains("budget") ||
      !j["budget"].is_number_unsigned())
    return false;
  for (auto file = j["outputs"].begin(); file != j["outputs"].end(); ++file) {
    if (file.key().find('/') != std::string::npos ||
        file.key().find("..") != std::string::npos ||
        !file.value().is_number_unsigned() ||
        fs::file_size(path / file.key(), ec) != file.value().get<uint64_t>() ||
        ec)
      return false;
  }
  std::vector<ScenePreviewLodTile> tiles;
  for (const auto& t : j["tiles"]) {
    if (!t.is_object() || !t.contains("path") || !t["path"].is_string())
      return false;
    ScenePreviewLodTile tile;
    tile.path = t["path"].get<std::string>();
    if (!t.contains("cell") || !t["cell"].is_number()) return false;
    tile.cell_size = t["cell"].get<double>();
    if (!std::isfinite(tile.cell_size) || tile.cell_size <= 0) return false;
    for (const char* key : {"min", "max", "bytes"})
      if (!t.contains(key) || !t[key].is_array() ||
          t[key].size() !=
              (std::string(key) == "bytes" ? size_t(4) : size_t(3)))
        return false;
    for (size_t k = 0; k < 3; ++k) {
      if (!t["min"][k].is_number() || !t["max"][k].is_number()) return false;
      tile.minimum[k] = t["min"][k].get<double>();
      tile.maximum[k] = t["max"][k].get<double>();
      if (!std::isfinite(tile.minimum[k]) || !std::isfinite(tile.maximum[k]))
        return false;
    }
    for (size_t k = 0; k < 4; ++k) {
      if (!t["bytes"][k].is_number_unsigned()) return false;
      tile.bytes[k] = t["bytes"][k].get<uint64_t>();
    }
    tiles.push_back(tile);
  }
  out->cell_size = j["cell"].get<double>();
  out->gpu_budget_bytes = j["budget"].get<uint64_t>();
  if (!std::isfinite(out->cell_size) || out->cell_size <= 0 || tiles.empty())
    return false;
  out->tiles = std::move(tiles);
  out->root_path = (path / "root.usdc").string();
  out->success = true;
  return true;
}
}  // namespace

ScenePreviewLods BuildScenePreviewLods(const ScenePreviewResult& result,
                                       const ScenePreviewOptions& options) {
  ScenePreviewLods out;
  if (!result.success || result.voxel_size <= 0) {
    out.error = "LOD construction requires a valid preview";
    return out;
  }
  const fs::path directory =
      fs::path(result.render_usd_path).parent_path() / "lod-v3";
  if (ReadManifest(directory, &out)) {
    if (out.cell_size == result.voxel_size &&
        out.gpu_budget_bytes == options.gpu_budget_bytes)
      return out;
    out = ScenePreviewLods{};
  }
  auto work = directory;
  work += ".building-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  fs::create_directory(work, ec);
  if (ec) {
    out.error = "cannot create preview LOD directory";
    return out;
  }
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(path, e);
    }
  } cleanup{work};
  usd::AssetResolver resolver;
  usd::Stage stage;
  usd::pcp::CompositionOptions composition;
  composition.num_threads = 1;
  std::string warning;
  if (!usd::pcp::ComposeStageFromFile(result.render_usd_path, resolver, &stage,
                                      composition, &warning, &out.error))
    return out;
  usd::Layer root;
  root.meta() = stage.GetRootLayer()->meta();
  usd::LayerBuilder builder(root);
  builder.begin_prim("ScenePreview", "Xform");
  ScenePreviewResult extra_result;
  preview_detail::Build extras(options, extra_result, work);
  size_t index = 0;
  bool ok = true;
  stage.Traverse([&](const usd::UsdPrim& prim) {
    const auto type = prim.GetTypeName();
    if (type != "ParticleField" && type != "ParticleField3DGaussianSplat" &&
        type != "Mesh") {
      if (type == "Camera" || type.find("Light") != std::string::npos)
        extras.extras.push_back(prim.GetPrimSpec()->Clone());
      return true;
    }
    const std::string name = "Tile_" + std::to_string(index++);
    ScenePreviewLodTile tile;
    tile.path = "/ScenePreview/" + name;
    tile.cell_size = result.voxel_size;
    if (const auto* v = prim.GetPropertyValue("lightusd:preview:voxelSize"))
      if (const auto* cell = v->as_double())
        if (std::isfinite(*cell) && *cell > 0) tile.cell_size = *cell;
    const auto samples =
        type == "Mesh" ? std::vector<Sample>{}
                       : preview_detail::ReadPreviewSamples(prim, options.kind);
    const auto positions =
        type == "Mesh" ? Floats(prim, "points") : Floats(prim, "positions");
    for (size_t k = 0; k < 3; ++k) {
      tile.minimum[k] = 1e300;
      tile.maximum[k] = -1e300;
    }
    for (size_t i = 0; i + 2 < positions.size(); i += 3)
      for (size_t k = 0; k < 3; ++k) {
        tile.minimum[k] = std::min(tile.minimum[k], double(positions[i + k]));
        tile.maximum[k] = std::max(tile.maximum[k], double(positions[i + k]));
      }
    if (type != "Mesh")
      for (const auto& sample : samples) {
        const double radius =
            sample.radius *
            (options.kind == ScenePreviewKind::Gaussian ? .5 : 1);
        for (size_t k = 0; k < 3; ++k) {
          tile.minimum[k] = std::min(tile.minimum[k], sample.p[k] - radius);
          tile.maximum[k] = std::max(tile.maximum[k], sample.p[k] + radius);
        }
      }
    usd::PrimVariantSetData variants;
    variants.name = "previewLod";
    variants.selected = "lod3";
    for (int level = 0; level < 4; ++level) {
      usd::Layer layer;
      const double cell = tile.cell_size * double(uint64_t(1) << level);
      uint64_t cost = 0;
      if (level == 0) {
        CopyPrim(layer, prim, name);
        if (type == "Mesh") {
          const auto* indices = prim.GetPropertyValue("faceVertexIndices");
          cost = positions.size() * sizeof(float) * 12 +
                 (indices ? indices->array_size() : 0) * sizeof(int32_t) * 4;
        } else
          cost = samples.size() * 1024;
      } else if (type == "Mesh")
        cost = MeshLevel(layer, prim, name, cell);
      else {
        const auto coarse = CoarseSamples(samples, cell);
        preview_detail::MakePreviewParticles(layer, name, coarse.data(),
                                             coarse.size(), cell, options.kind);
        cost = coarse.size() * 1024;
      }
      const std::string file = "tile_" + std::to_string(index - 1) + "_" +
                               std::to_string(level) + ".usdc";
      std::string reference = file;
      if (!cost) {
        reference = "tile_" + std::to_string(index - 1) + "_0.usdc";
        cost = tile.bytes[0];
      } else if (!Save(work / file, layer, &out.error)) {
        ok = false;
        return false;
      }
      tile.bytes[size_t(level)] = cost;
      usd::PrimVariantData choice;
      choice.name = "lod" + std::to_string(level);
      choice.references.push_back("@" + reference + "@</" + name + ">");
      variants.variants.push_back(std::move(choice));
    }
    builder.begin_prim(name, "");
    builder.current()->meta().variantSets().push_back(std::move(variants));
    builder.current()->meta().variantSelections().emplace_back("previewLod",
                                                               "lod3");
    builder.current()->meta().setVariantSelectionsAuthored();
    builder.end_prim();
    out.tiles.push_back(tile);
    return true;
  });
  if (!ok || out.tiles.empty()) {
    if (out.error.empty()) out.error = "preview has no LOD geometry";
    return out;
  }
  builder.end_prim();
  builder.finalize();
  preview_detail::AppendPreviewExtras(extras, root);
  fs::remove(work / "prototypes", ec);
  if (!Save(work / "root.usdc", root, &out.error)) return out;
  out.cell_size = result.voxel_size;
  out.gpu_budget_bytes = options.gpu_budget_bytes;
  json tiles = json::array(), outputs = json::object();
  for (const auto& tile : out.tiles)
    tiles.push_back({{"path", tile.path},
                     {"cell", tile.cell_size},
                     {"min", tile.minimum},
                     {"max", tile.maximum},
                     {"bytes", tile.bytes}});
  for (const auto& entry : fs::directory_iterator(work, ec))
    if (entry.is_regular_file(ec))
      outputs[entry.path().filename().string()] = entry.file_size(ec);
  if (ec) {
    out.error = "cannot enumerate LOD sidecars";
    return out;
  }
  {
    std::ofstream manifest(work / "manifest.json");
    manifest << json{
        {"version", 3},
        {"cell", out.cell_size},
        {"budget", out.gpu_budget_bytes},
        {"tiles", tiles},
        {"outputs",
         outputs}}.dump(2);
    if (!manifest) {
      out.error = "cannot write LOD manifest";
      return out;
    }
  }
  auto old = directory;
  old += ".old-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count());
  if (fs::exists(directory, ec)) fs::rename(directory, old, ec);
  if (!ec) fs::rename(work, directory, ec);
  if (ec) {
    std::error_code restore;
    if (fs::exists(old, restore)) fs::rename(old, directory, restore);
  } else {
    std::error_code cleanup_error;
    fs::remove_all(old, cleanup_error);
  }
  if (ec) {
    out.error = "cannot publish preview LODs: " + ec.message();
    return out;
  }
  out.root_path = (directory / "root.usdc").string();
  out.success = true;
  return out;
}

ScenePreviewLodSelections SelectScenePreviewLods(
    const ScenePreviewLods& lods, const ScenePreviewLodCamera& camera,
    double pixel_error, const ScenePreviewLodSelections& previous) {
  ScenePreviewLodSelections out;
  if (!lods.success || !std::isfinite(pixel_error) || pixel_error <= 0 ||
      !std::isfinite(camera.focal_pixels) || camera.focal_pixels <= 0)
    return out;
  const auto reserve = std::min(lods.gpu_budget_bytes / 2, uint64_t(512) << 20);
  const uint64_t available = lods.gpu_budget_bytes - reserve;
  uint64_t total = 0;
  std::vector<int> levels(lods.tiles.size());
  std::vector<double> errors(lods.tiles.size());
  for (size_t i = 0; i < lods.tiles.size(); ++i) {
    const auto& tile = lods.tiles[i];
    double depth = 0;
    for (size_t k = 0; k < 3; ++k)
      depth += ((camera.forward[k] >= 0 ? tile.minimum[k] : tile.maximum[k]) -
                camera.eye[k]) *
               camera.forward[k];
    const double projected =
        1.75 * (tile.cell_size > 0 ? tile.cell_size : lods.cell_size) *
        camera.focal_pixels /
        (camera.orthographic
             ? 1
             : std::max(std::max(.000001, camera.near_plane), depth));
    int level = 0;
    while (level < 3 &&
           projected * double(uint64_t(1) << (level + 1)) <= pixel_error)
      ++level;
    const auto old = previous.find(tile.path);
    if (old != previous.end()) {
      const auto choice = old->second.find("previewLod");
      if (choice != old->second.end() && choice->second.size() == 4 &&
          choice->second.substr(0, 3) == "lod" && choice->second[3] >= '0' &&
          choice->second[3] <= '3') {
        const int prior = choice->second[3] - '0';
        const double error = projected * double(uint64_t(1) << prior);
        if (error <= pixel_error * 1.25 &&
            (prior == 3 || error * 2 >= pixel_error * .8))
          level = prior;
      }
    }
    levels[i] = level;
    errors[i] = projected;
    if (tile.bytes[size_t(level)] > UINT64_MAX - total) return {};
    total += tile.bytes[size_t(level)];
  }
  // If conservative aggregate costs need another reduction, give up detail on
  // the least-visible tile first. A finest-only source never bypasses the cap.
  while (total > available) {
    size_t selected = lods.tiles.size();
    int selected_level = 0;
    double importance = 1e300;
    for (size_t i = 0; i < levels.size(); ++i) {
      for (int level = levels[i] + 1; level < 4; ++level) {
        if (lods.tiles[i].bytes[size_t(level)] >=
            lods.tiles[i].bytes[size_t(levels[i])])
          continue;
        const double error = errors[i] * double(uint64_t(1) << level);
        if (error < importance) {
          importance = error;
          selected = i;
          selected_level = level;
        }
        break;
      }
    }
    if (selected == lods.tiles.size()) return {};
    const auto& tile = lods.tiles[selected];
    total -= tile.bytes[size_t(levels[selected])];
    levels[selected] = selected_level;
    total += tile.bytes[size_t(selected_level)];
  }
  for (size_t i = 0; i < levels.size(); ++i)
    out[lods.tiles[i].path]["previewLod"] = "lod" + std::to_string(levels[i]);
  return out;
}
ScenePreviewLodCamera InitialScenePreviewLodCamera(
    const ScenePreviewResult& result, const std::string& root,
    const std::string& camera_name, int height, double fit_scale,
    std::array<double, 3> direction) {
  ScenePreviewLodCamera camera;
  const double pixels = std::max(1, height) * .5;
  camera.focal_pixels = pixels / std::tan(45. * 3.141592653589793 / 360.);
  double diagonal = 0, length = 0;
  for (size_t k = 0; k < 3; ++k) {
    diagonal += std::pow(result.bounds_max[k] - result.bounds_min[k], 2);
    length += direction[k] * direction[k];
  }
  diagonal = std::sqrt(diagonal);
  length = std::sqrt(length);
  if (length <= 0) {
    direction = {1, .6, 1};
    length = std::sqrt(2.36);
  }
  for (size_t k = 0; k < 3; ++k) {
    camera.forward[k] = -direction[k] / length;
    camera.eye[k] =
        (result.bounds_min[k] + result.bounds_max[k]) * .5 -
        camera.forward[k] * std::max(1e-6, diagonal * fit_scale * .5);
  }
  if (camera_name.empty()) return camera;
  usd::AssetResolver resolver;
  usd::Stage stage;
  usd::pcp::CompositionOptions opts;
  opts.num_threads = 1;
  std::string warning, error;
  if (!usd::pcp::ComposeStageFromFile(root, resolver, &stage, opts, &warning,
                                      &error))
    return camera;
  usd::UsdPrim chosen;
  stage.Traverse([&](const usd::UsdPrim& prim) {
    if (prim.GetTypeName() == "Camera" &&
        (prim.GetPath().str() == camera_name ||
         prim.GetName() == camera_name)) {
      chosen = prim;
      return false;
    }
    return true;
  });
  if (!chosen) return camera;
  auto matrix = preview_detail::Identity();
  usd::ComputeWorldTransform(stage, chosen, matrix.data());
  for (size_t k = 0; k < 3; ++k) {
    camera.eye[k] = matrix[12 + k];
    camera.forward[k] = -matrix[8 + k];
  }
  double forward_length = 0;
  for (double value : camera.forward) forward_length += value * value;
  forward_length = std::sqrt(forward_length);
  if (forward_length > 0)
    for (double& value : camera.forward) value /= forward_length;
  auto number = [&](const char* name, double fallback) {
    const auto* v = chosen.GetPropertyValue(name);
    if (!v) return fallback;
    if (const auto* d = v->as_double())
      return std::isfinite(*d) ? *d : fallback;
    if (const auto* f = v->as_float())
      return std::isfinite(*f) ? double(*f) : fallback;
    return fallback;
  };
  const double focal = number("focalLength", 50),
               aperture = number("verticalAperture", 20.955);
  const auto* projection = chosen.GetPropertyValue("projection");
  camera.orthographic = projection && projection->as_token() == "orthographic";
  if (aperture > 0)
    camera.focal_pixels = camera.orthographic
                              ? double(std::max(1, height)) / (aperture * .1)
                              : double(std::max(1, height)) * focal / aperture;
  return camera;
}
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
