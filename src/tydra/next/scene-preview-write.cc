// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cmath>
#include <sstream>

#include "next/crate/crate-writer.hh"
#include "next/layer/prim-spec.hh"
#include "scene-preview-internal.hh"

namespace lightusd {
namespace tydra {
namespace next {
namespace preview_detail {
namespace {
bool Save(const fs::path& file, const usd::Layer& layer, std::string* error) {
  usd::CrateWriteOptions options;
  options.num_threads = 1;
  usd::CrateWriter writer(options);
  const auto result = writer.WriteLayerToFile(file.string().c_str(), layer);
  if (!result.success) {
    if (error) *error = result.error;
    return false;
  }
  return true;
}
}  // namespace
void MakePreviewParticles(usd::Layer& layer, const std::string& name,
                          const Sample* samples, size_t count, double cell,
                          ScenePreviewKind kind) {
  usd::LayerBuilder builder(layer);
  builder.begin_prim(name, kind == ScenePreviewKind::Gaussian
                               ? "ParticleField3DGaussianSplat"
                               : "ParticleField");
  if (kind == ScenePreviewKind::Points) {
    builder.current()->meta().apiSchemas() = {
        "ParticleFieldPositionAttributeAPI",
        "ParticleFieldOrientationAttributeAPI",
        "ParticleFieldScaleAttributeAPI",
        "ParticleFieldOpacityAttributeAPI",
        "ParticleFieldKernelConstantSurfletAPI",
        "ParticleFieldSphericalHarmonicsAttributeAPI"};
    builder.current()->meta().setApiSchemasAuthored();
  }
  std::vector<float> positions, scales, rotations, colors, opacities;
  positions.reserve(count * 3);
  scales.reserve(count * 3);
  rotations.reserve(count * 4);
  colors.reserve(count * 3);
  opacities.reserve(count);
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i < count; ++i) {
    const Sample& s = samples[i];
    const float radius = std::max(static_cast<float>(cell * .75), s.radius);
    for (int k = 0; k < 3; ++k) {
      positions.push_back(static_cast<float>(s.p[k]));
      lo[k] = std::min(lo[k], static_cast<float>(s.p[k]) - radius);
      hi[k] = std::max(hi[k], static_cast<float>(s.p[k]) + radius);
      colors.push_back((s.color[k] - .5f) / .2820947918f);
    }
    const float sigma =
        kind == ScenePreviewKind::Gaussian ? radius * .5f : radius;
    scales.insert(scales.end(), {sigma, sigma, std::max(sigma * .05f, 1e-6f)});
    // Schema quaternion lanes are real,x,y,z, rotating the surflet +Z normal.
    if (s.n[2] < -.99999f)
      rotations.insert(rotations.end(), {0, 1, 0, 0});
    else {
      const float w = std::sqrt(std::max(0.f, (1 + s.n[2]) * .5f));
      rotations.insert(rotations.end(),
                       {w, -s.n[1] / (2 * w), s.n[0] / (2 * w), 0});
    }
    opacities.push_back(s.opacity);
  }
  builder.add_property("positions",
                       usd::Value::MakeArrayFromRaw(usd::TypeId::Point3f,
                                                    positions.data(), count));
  builder.add_property("scales", usd::Value::MakeFloat3Array(scales));
  builder.add_property("orientations",
                       usd::Value::MakeArrayFromRaw(usd::TypeId::Quatf,
                                                    rotations.data(), count));
  builder.add_property("opacities", usd::Value::MakeFloatArray(opacities));
  builder.add_property("radiance:sphericalHarmonicsDegree",
                       usd::Value(int32_t(0)));
  builder.add_property("radiance:sphericalHarmonicsCoefficients",
                       usd::Value::MakeFloat3Array(colors));
  builder.add_property(
      "extent",
      usd::Value::MakeFloat3Array({lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]}));
  builder.add_property("doubleSided", usd::Value(true));
  builder.end_prim();
  builder.finalize();
}
void AppendPreviewExtras(const Build& build, usd::Layer& layer) {
  usd::LayerBuilder builder(layer);
  for (const auto& extra : build.extras) {
    const std::string path = extra.path().str();
    size_t first = 1, depth = 0;
    while (first < path.size()) {
      const size_t end = path.find('/', first);
      const auto name =
          path.substr(first, end == std::string::npos ? end : end - first);
      builder.begin_prim(name,
                         end == std::string::npos ? extra.type_name() : "");
      ++depth;
      if (end == std::string::npos) {
        auto copy = extra.Clone();
        copy.clear_child_indices();
        *builder.current() = std::move(copy);
        break;
      }
      first = end + 1;
    }
    while (depth--) builder.end_prim();
  }
  builder.finalize();
}
bool WriteProxy(Build& build) {
  if (build.cells.empty()) {
    build.result.error = "scene contains no visible preview surfaces";
    return false;
  }
  std::vector<std::pair<Key, Accum>> ordered;
  ordered.reserve(build.cells.size());
  for (const auto& item : build.cells) ordered.push_back(item);
  build.cells.clear();
  build.cells.rehash(0);
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<Sample> samples;
  samples.reserve(ordered.size());
  for (const auto& item : ordered)
    samples.push_back(item.second.representative);
  ordered.clear();
  ordered.shrink_to_fit();
  build.result.samples = samples.size();
  build.result.voxel_size = build.cell_size;
  for (int k = 0; k < 3; ++k) {
    build.result.bounds_min[k] = 1e300;
    build.result.bounds_max[k] = -1e300;
  }
  for (const auto& s : samples)
    for (int k = 0; k < 3; ++k) {
      build.result.bounds_min[k] = std::min(build.result.bounds_min[k], s.p[k]);
      build.result.bounds_max[k] = std::max(build.result.bounds_max[k], s.p[k]);
    }
  usd::Layer root;
  root.meta().upAxis = build.meta.upAxis;
  root.meta().upAxis_set = true;
  root.meta().metersPerUnit = build.meta.metersPerUnit;
  root.meta().metersPerUnit_set = true;
  root.meta().defaultPrim = "ScenePreview";
  root.meta().defaultPrim_set = true;
  if (build.opts.kind == ScenePreviewKind::Volume) {
    if (!WriteVolume(build, root, samples)) return false;
  } else {
    usd::LayerBuilder builder(root);
    builder.begin_prim("ScenePreview", "Xform");
    const size_t chunk = 65536;
    for (size_t first = 0; first < samples.size(); first += chunk) {
      const std::string name = "Tile_" + std::to_string(first / chunk),
                        file = "particles_" + std::to_string(first / chunk) +
                               ".usdc";
      usd::Layer tile;
      MakePreviewParticles(tile, name, samples.data() + first,
                           std::min(chunk, samples.size() - first),
                           build.cell_size, build.opts.kind);
      if (!Save(build.directory / file, tile, &build.result.error) ||
          !build.check("write", first))
        return false;
      builder.begin_prim(name, "");
      builder.current()->meta().references.push_back("@" + file + "@</" + name +
                                                     ">");
      builder.end_prim();
    }
    builder.end_prim();
    builder.finalize();
    build.result.estimated_gpu_bytes =
        std::min(build.opts.gpu_budget_bytes / 2, uint64_t(512) << 20) +
        samples.size() * 1024;
  }
  AppendPreviewExtras(build, root);
  return Save(build.directory / "root.usdc", root, &build.result.error);
}
}  // namespace preview_detail
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
