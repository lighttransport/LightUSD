// SPDX-License-Identifier: Apache-2.0
#include "core/native-io.hh"
#include <chrono>

#include "crate/crate-writer.hh"
#include "../pcp/cache.hh"
#include "resolver/asset-resolver.hh"
#include "scene-preview-internal.hh"
namespace lightusd {
namespace tydra {
namespace next {
ScenePreviewResult OpenScenePreviewVolume(const std::string& source,
                                          const ScenePreviewOptions& options) {
  if (!::lightusd::NativeIOEnabled()) { ScenePreviewResult r; r.error = ::lightusd::NativeIOUnavailableMessage(); return r; }

  namespace detail = preview_detail;
  namespace fs = std::filesystem;
  ScenePreviewResult result;
  result.usd_path = source;
  PreviewVolumeRenderer renderer;
  if (!renderer.load(
          result, PreviewComputeBackend::Cpu,
          std::min(options.gpu_budget_bytes, options.host_budget_bytes / 3),
          &result.error, options.compute_options))
    return result;
  renderer.bounds(result.bounds_min, result.bounds_max, &result.voxel_size);
  ::lightusd::next::Stage stage;
  ::lightusd::next::AssetResolver resolver;
  if (!::lightusd::next::pcp::ComposeStageFromFile(
          source, resolver, &stage, options.composition, &result.warning,
          &result.error))
    return result;
  std::error_code ec;
  const auto absolute = fs::absolute(source, ec);
  if (ec) {
    result.error = "cannot resolve native volume path";
    return result;
  }
  std::string key = absolute.lexically_normal().string();
  for (size_t k = 0; k < 3; ++k)
    key += ":" + std::to_string(result.bounds_min[k]) + ":" +
           std::to_string(result.bounds_max[k]);
  const auto cache = options.cache_directory.empty()
                         ? DefaultScenePreviewCacheDirectory()
                         : options.cache_directory;
  const auto dir =
      fs::path(cache) / ("native-volume-" + std::to_string(detail::Hash(key)));
  fs::create_directories(dir, ec);
  if (ec) {
    result.error = "cannot create native volume metadata cache";
    return result;
  }
  detail::Build build(options, result, dir);
  ::lightusd::next::pcp::ComposedBatch batch;
  batch.layer = stage.ReleaseRootLayer();
  if (!build.plan(batch)) return result;
  ::lightusd::next::Layer layer;
  layer.meta() = build.meta;
  layer.meta().defaultPrim = "ScenePreview";
  layer.meta().defaultPrim_set = true;
  ::lightusd::next::LayerBuilder b(layer);
  b.begin_prim("ScenePreview", "Xform");
  b.begin_prim("Bounds", "Cube");
  b.add_property("size", ::lightusd::next::Value(2.));
  std::vector<double> center(3), scale(3);
  for (size_t k = 0; k < 3; ++k) {
    center[k] = (result.bounds_min[k] + result.bounds_max[k]) * .5;
    scale[k] = std::max(result.voxel_size,
                        (result.bounds_max[k] - result.bounds_min[k]) * .5);
  }
  b.add_property("xformOp:translate", ::lightusd::next::Value::MakeDouble3(
                                          center[0], center[1], center[2]));
  b.add_property("xformOp:scale", ::lightusd::next::Value::MakeDouble3(
                                      scale[0], scale[1], scale[2]));
  b.add_property(
      "xformOpOrder",
      ::lightusd::next::Value::MakeTokenArray(
          std::vector<std::string>{"xformOp:translate", "xformOp:scale"}));
  b.end_prim();
  b.end_prim();
  b.finalize();
  detail::AppendPreviewExtras(build, layer);
  const auto generation = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto target = dir / ("render-" + generation + ".usdc");
  const auto temporary = dir / ("render-" + generation + ".tmp");
  ::lightusd::next::CrateWriteOptions write;
  write.num_threads = 1;
  auto written = ::lightusd::next::CrateWriter(write).WriteLayerToFile(
      temporary.string().c_str(), layer);
  if (!written.success) {
    result.error = written.error;
    fs::remove(temporary, ec);
    return result;
  }
  fs::rename(temporary, target, ec);
  if (ec) {
    result.error = "cannot publish native volume metadata";
    fs::remove(temporary, ec);
    return result;
  }
  result.render_usd_path = target.string();
  result.success = true;
  return result;
}
}  // namespace next
}  // namespace tydra
}  // namespace lightusd
