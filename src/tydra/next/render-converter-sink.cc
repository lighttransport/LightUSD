// SPDX-License-Identifier: Apache-2.0
// Streaming (ConvertToSink) scene conversion driver. Split from
// render-converter.cc for translation-unit size.
#include "safe-arithmetic.hh"
#include "../../next/layer/prim-spec.hh"
#include "next/core/path-expression-eval.hh"
#include "next/schema/usd-vol.hh"
#include "next/schema/usd-geom-camera.hh"
#include "render-converter-internal.hh"
#include "render-converter-prop-ids.hh"
#include "render-converter-assembly.hh"
#include "render-converter-material-finalize.hh"
#include "render-converter-light-linking.hh"
#include "next/schema/color-space.hh"
#include "next/eval/value-clip.hh"
#include "next/resolver/asset-resolver.hh"
#include "materialx.hh"
#include "next/layer/asset-anchor.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/schema/usdPhysics.hh"
#include "next/schema/usd-shade.hh"
#include "next/schema/usd-skel.hh"
#include "next/types/type-info.hh"
#include "tydra/fast-mikktspace.hh"
#include "tydra/mikktspace-tangent.hh"
#include "tydra/shape-to-mesh.hh"
#include "tydra/tangent-quantize.hh"
#include "tsd/tinysubdiv.hh"
#include "external/mapbox/earcut/earcut.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <deque>
#include <memory>
#include <unordered_map>
#include <optional>
#include <set>
#include <sstream>
#include <iomanip>
#include <atomic>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif
#include <unordered_set>

namespace lightusd {
namespace tydra {
namespace next {

namespace {

std::string FindInheritedMaterialBinding(const Stage& stage,
                                         const std::string& prim_path,
                                         const std::string& purpose) {
  // Core UsdShade resolution now validates targets at every purpose/ancestor
  // step and associates bindMaterialAs with the relationship that actually
  // won. Keep one implementation shared by the converter and applications.
  return ::lightusd::next::GetInheritedBoundMaterialPathForPurpose(
      stage, prim_path, purpose.empty() ? "preview" : purpose);
}

void NormalizeConversionDiagnostics(std::vector<ConversionDiagnostic>* diagnostics) {
  std::sort(diagnostics->begin(), diagnostics->end(),
            [](const ConversionDiagnostic& a, const ConversionDiagnostic& b) {
    if (a.prim_path != b.prim_path) return a.prim_path < b.prim_path;
    if (a.disposition != b.disposition) return a.disposition < b.disposition;
    return a.message < b.message;
  });
}

void AppendDegradationDiagnostics(RenderScene* scene) {
  for (const auto& unsupported : scene->unsupported_renderables) {
    scene->conversion_diagnostics.push_back({
        ConversionDisposition::PreservedUnsupported, unsupported.prim_path,
        unsupported.reason});
  }
  for (const auto& material : scene->materials) {
    if (material.default_fallback && material.diagnostics.empty()) {
      scene->conversion_diagnostics.push_back({
          ConversionDisposition::Approximated, material.prim_path,
          "material uses a fallback surface"});
    }
    for (const auto& diagnostic : material.diagnostics) {
      scene->conversion_diagnostics.push_back({
          material.default_fallback ? ConversionDisposition::Approximated
                                    : ConversionDisposition::PreservedUnsupported,
          material.prim_path, diagnostic.message});
    }
  }
}

void ReleaseSourceMeshStaticArrays(Stage& stage, const UsdPrim& mesh_prim) {
  constexpr size_t kLowmemStaticReleaseThreshold = 1;
  stage.ReleaseStaticGeometryArraysForPrim(mesh_prim,
                                          kLowmemStaticReleaseThreshold);
}
}  // namespace

StreamConvertResult RenderSceneConverter::Impl::ConvertToSink(Stage& stage,
                                                        SceneSink* sink) {
  StreamConvertResult result;
  ResetOperationState();
  ResetImageIdCache();
  if (!config_.limits.valid() || config_.execution.max_threads < 0) {
    result.status = ::lightusd::next::OperationStatus::InvalidArgument;
    result.error = "invalid converter limits or execution policy";
    return result;
  }
  size_t chunk_limit = config_.limits.max_resident_bytes;
  if (config_.execution.max_in_flight_bytes) {
    chunk_limit = std::min(chunk_limit,
                           config_.execution.max_in_flight_bytes);
  }
  auto chunk_budget = std::make_shared<ChunkAllocationBudget>(chunk_limit);
  ScopedChunkAllocationBudget chunk_budget_scope(chunk_budget);
  if (!sink) {
    result.status = ::lightusd::next::OperationStatus::InvalidArgument;
    result.error = "ConvertToSink: null scene sink";
    return result;
  }
  const auto cancellation_requested = [&]() {
    return config_.cancel_callback && config_.cancel_callback();
  };
  if (cancellation_requested()) {
    result.status = ::lightusd::next::OperationStatus::Cancelled;
    result.cancelled = true;
    result.error = "conversion cancelled";
    return result;
  }

  RenderScene catalog;
  const auto meta = stage.GetMeta();
  catalog.name = meta.defaultPrim;
  catalog.default_prim = meta.defaultPrim;
  // The render product retains its 1-meter fallback for unauthored layers;
  // the next Stage keeps its independent 0.01 USD metadata fallback.
  catalog.meters_per_unit = static_cast<float>(
      meta.metersPerUnit_set ? meta.metersPerUnit : 1.0);
  catalog.up_axis = meta.upAxis == "Z" ? RenderScene::UpAxis::Z
                                        : RenderScene::UpAxis::Y;
  catalog.start_time = meta.startTimeCode;
  catalog.end_time = meta.endTimeCode;
  catalog.frames_per_second = meta.timeCodesPerSecond;
  ::lightusd::next::color_management::RenderingColorConfig color_config;
  std::string color_warning;
  (void)::lightusd::next::color_management::ResolveRenderingColorConfig(
      stage, config_.material.render_settings_path, &color_config,
      &color_warning);
  catalog.render_settings_path = color_config.render_settings_path;
  catalog.working_color_space = color_config.working_space;
  ::lightusd::color::ColorSpaceDesc display_linear;
  ::lightusd::color::ColorTransform working_to_display;
  if (::lightusd::color::GetBuiltinColorSpace("lin_rec709_scene",
                                              &display_linear) &&
      ::lightusd::color::BuildColorTransform(
          color_config.working_definition, display_linear,
          &working_to_display)) {
    for (size_t i = 0; i < 9; ++i) {
      catalog.working_to_display_linear[i] = working_to_display.matrix[i];
    }
  }
  if (!color_warning.empty()) AddWarning(color_warning);

  RenderExtractOptions xopts;
  xopts.time_code = config_.time_code;
  xopts.max_depth = config_.limits.max_namespace_depth;
  xopts.max_records = config_.limits.max_render_records;
  xopts.collect_other = true;
  RenderExtractResult extracted;
  CollectRenderPrims(stage, xopts, &extracted);
  if (extracted.limit_exceeded) {
    result.status = ::lightusd::next::OperationStatus::ResourceLimit;
    result.error = "render extraction limit exceeded";
    return result;
  }
  const bool emit_animations =
      config_.animation.enabled &&
      (stage.HasTimeSamples() || stage.HasValueClips());
  const bool has_time_samples = emit_animations;
  if (cancellation_requested()) {
    result.status = ::lightusd::next::OperationStatus::Cancelled;
    result.cancelled = true;
    result.error = "conversion cancelled";
    return result;
  }
  BuildNodeHierarchy(extracted, &catalog);
  ExtractPhysicsAnnotations(stage, &catalog);
  catalog.meshes.reserve(extracted.meshes.size());
  const size_t point_record_count = extracted.points.size();
  catalog.points.reserve(point_record_count);
  catalog.curves.reserve(extracted.curves.size());
  catalog.point_instancers.reserve(extracted.point_instancers.size());
  catalog.lights.reserve(extracted.lights.size());
  catalog.cameras.reserve(extracted.cameras.size());
  catalog.skeletons.reserve(extracted.skeletons.size());
  catalog.materials.reserve(extracted.materials.size());
  catalog.mesh_by_path.reserve(extracted.meshes.size());
  catalog.point_instancer_by_path.reserve(extracted.point_instancers.size());
  catalog.points_by_path.reserve(point_record_count);
  catalog.curves_by_path.reserve(extracted.curves.size());
  catalog.material_by_path.reserve(extracted.materials.size());
  catalog.unsupported_renderables.reserve(
      std::min<size_t>(extracted.records.size(), 128));
  if (emit_animations) {
    catalog.animations.reserve(extracted.records.size());
  }
  for (const RenderPrimRecord& rec : extracted.records) {
    if (rec.type_name != "Points" &&
        rec.type_name != "ParticleField3DGaussianSplat" &&
        rec.type_name != "Volume" &&
        IsUnsupportedRenderableTypeName(rec.type_name)) {
      UnsupportedRenderable unsupported;
      unsupported.prim_path = rec.path;
      unsupported.type_name = rec.type_name;
      unsupported.reason = "recognized but not converted to render geometry";
      catalog.unsupported_renderables.push_back(std::move(unsupported));
      AddWarning("Unsupported renderable prim '" + rec.path +
                          "' of type '" + rec.type_name + "'");
    }

    if (!has_time_samples ||
        (!stage.HasValueClips() &&
         (!rec.prim.GetPrimSpec() ||
          !rec.prim.GetPrimSpec()->has_any_time_samples()))) {
      continue;
    }
    AnimationClip clip;
    if (ConvertAnimation(stage, rec.prim, &clip)) {
      const auto node = catalog.node_by_path.find(rec.path);
      if (node != catalog.node_by_path.end()) {
        for (AnimationChannel& channel : clip.channels) {
          channel.target_node = node->second;
        }
      }
      catalog.animations.push_back(std::move(clip));
    }
  }

  // Materials and their image/texture descriptors are a small catalog and
  // must be assigned before geometry is emitted.
  for (const RenderPrimRecord& rec : extracted.materials) {
    RenderMaterial material;
    if (ConvertMaterial(stage, rec.prim, &material, &catalog)) {
      const int32_t id = static_cast<int32_t>(catalog.materials.size());
      catalog.material_by_path[material.prim_path] = id;
      catalog.materials.push_back(std::move(material));
    } else {
      AddWarning("Failed to convert material: " + rec.path);
      catalog.conversion_diagnostics.push_back({ConversionDisposition::Failed,
                                               rec.path, "material conversion failed"});
    }
  }

  for (const RenderPrimRecord& rec : extracted.skeletons) {
    Skeleton skeleton;
    if (ConvertSkeleton(rec.prim, &skeleton)) {
      const int32_t id = static_cast<int32_t>(catalog.skeletons.size());
      catalog.skeletons.push_back(std::move(skeleton));
      AssignNodeDataId(&catalog, rec.path, id);
    }
  }
  ResolveSkeletalAnimationTargets(&catalog);

  // UsdVol Volumes and ParticleFields are small records (voxels and particle
  // payloads decode on demand / once), so they ride on the catalog.
  for (const RenderPrimRecord& rec : extracted.volumes) {
    RenderVolume volume;
    if (ConvertVolume(stage, rec.prim, &catalog, &volume)) {
      for (const std::string& w : volume.warnings) AddWarning(w);
      catalog.volume_by_path[volume.prim_path] = static_cast<int32_t>(catalog.volumes.size());
      catalog.volumes.push_back(std::move(volume));
    }
  }
  ResolveVolumeMaterials(&catalog);
  for (const RenderPrimRecord& rec : extracted.records) {
    if (rec.type_name.compare(0, 13, "ParticleField") != 0) continue;
    RenderParticleField pf;
    if (ConvertParticleField(stage, rec.prim, config_.time_code, &pf)) {
      for (const std::string& w : pf.warnings) AddWarning(w);
      catalog.particle_fields.push_back(std::move(pf));
    }
  }

  for (const RenderPrimRecord& rec : extracted.lights) {
    RenderLight light;
    if (!ConvertLight(stage, rec.prim, &light)) {
      AddWarning("Failed to convert light: " + rec.path);
      catalog.conversion_diagnostics.push_back({ConversionDisposition::Failed,
                                               rec.path, "light conversion failed"});
      continue;
    }
    for (int i = 0; i < 16; ++i) light.transform.m[i] = float(rec.world[i]);
    if (light.type == LightType::Dome) {
      std::string texture;
      if (const Value* value = rec.prim.GetPropertyValue(kIdInputsTextureFile())) {
        if (auto p = value->as_asset_path()) texture = *p;
        else if (auto p = value->as_string()) texture = *p;
        else if (auto p = value->as_token()) texture = *p;
      }
      light.params.dome.texture_id =
          texture.empty() ? -1
                          : ResolveImageId(&catalog, texture, ColorSpace::Linear,
                                           AssetAnchorOf(rec.prim));
    }
    const int32_t id = static_cast<int32_t>(catalog.lights.size());
    catalog.lights.push_back(std::move(light));
    AssignNodeDataId(&catalog, rec.path, id);
  }
  for (const RenderPrimRecord& rec : extracted.cameras) {
    RenderCamera camera;
    if (!ConvertCamera(stage, rec.prim, &camera)) continue;
    for (int i = 0; i < 16; ++i) camera.transform.m[i] = float(rec.world[i]);
    const int32_t id = static_cast<int32_t>(catalog.cameras.size());
    catalog.cameras.push_back(std::move(camera));
    AssignNodeDataId(&catalog, rec.path, id);
  }

  // Reserve stable geometry IDs with lightweight placeholders. This is enough
  // for native/PointInstancer prototype binding without retaining geometry.
  catalog.meshes.resize(extracted.meshes.size());
  for (size_t i = 0; i < extracted.meshes.size(); ++i) {
    RenderMesh& placeholder = catalog.meshes[i];
    placeholder.name = extracted.meshes[i].prim.GetName();
    placeholder.prim_path = extracted.meshes[i].path;
    catalog.mesh_by_path[placeholder.prim_path] = static_cast<int32_t>(i);
    AssignMeshMaterialBinding(stage, catalog, &placeholder);
    AssignNodeDataId(&catalog, placeholder.prim_path, static_cast<int32_t>(i));
  }
  for (const RenderPrimRecord& rec : extracted.points) {
    const int32_t id = static_cast<int32_t>(catalog.points.size());
    RenderPoints placeholder;
    placeholder.name = rec.prim.GetName();
    placeholder.prim_path = rec.path;
    catalog.points_by_path[rec.path] = id;
    catalog.points.push_back(std::move(placeholder));
    AssignNodeDataId(&catalog, rec.path, id);
  }
  catalog.curves.resize(extracted.curves.size());
  for (size_t i = 0; i < extracted.curves.size(); ++i) {
    RenderCurves& placeholder = catalog.curves[i];
    placeholder.name = extracted.curves[i].prim.GetName();
    placeholder.prim_path = extracted.curves[i].path;
    const std::string material =
        FindInheritedMaterialBinding(stage, placeholder.prim_path,
                                     config_.material.binding_purpose);
    const auto found = catalog.material_by_path.find(material);
    if (found != catalog.material_by_path.end()) {
      placeholder.material_id = found->second;
    }
    catalog.curves_by_path[placeholder.prim_path] = static_cast<int32_t>(i);
    AssignNodeDataId(&catalog, placeholder.prim_path, static_cast<int32_t>(i));
  }

  for (const RenderPrimRecord& rec : extracted.point_instancers) {
    RenderPointInstancer instancer;
    if (!ConvertPointInstancer(rec.prim, &instancer)) {
      AddConversionWarning(rec.path, "Failed to convert PointInstancer: " + rec.path,
                           ConversionDisposition::Failed);
      continue;
    }
    const int32_t id = static_cast<int32_t>(catalog.point_instancers.size());
    catalog.point_instancer_by_path[instancer.prim_path] = id;
    ResolvePointInstancerPrototypeBindings(&catalog, &instancer);
    if (config_.point_instancer.build_instance_draws ||
        config_.point_instancer.duplicate_meshes) {
      AppendPointInstanceDraws(id, &instancer, &catalog);
    }
    catalog.point_instancers.push_back(std::move(instancer));
    AssignNodeDataId(&catalog, rec.path, id);
  }
  AssignPointInstanceDrawMaterials(&catalog);

  RenderScene binding_catalog;
  binding_catalog.material_by_path = catalog.material_by_path;
  std::unordered_map<std::string, int32_t> skeleton_by_path;
  skeleton_by_path.reserve(extracted.skeletons.size());
  skeleton_by_path.max_load_factor(0.7f);
  for (size_t i = 0; i < catalog.skeletons.size(); ++i) {
    skeleton_by_path[catalog.skeletons[i].prim_path] = static_cast<int32_t>(i);
  }

  if (catalog.memory_usage() > config_.limits.max_resident_bytes) {
    result.status = ::lightusd::next::OperationStatus::ResourceLimit;
    result.error = "stream render catalog exceeds resident memory limit";
    result.warnings = std::move(warnings_);
    return result;
  }
  AppendDegradationDiagnostics(&catalog);
  catalog.conversion_diagnostics.insert(catalog.conversion_diagnostics.end(),
      conversion_diagnostics_.begin(), conversion_diagnostics_.end());
  conversion_diagnostics_.clear();
  NormalizeConversionDiagnostics(&catalog.conversion_diagnostics);
  result.conversion_diagnostics = catalog.conversion_diagnostics;
  if (config_.strict_conversion && !warnings_.empty()) {
    result.error = "strict render conversion rejected warnings: " + warnings_.front();
    result.warnings = std::move(warnings_);
    return result;
  }
  if (!sink->BeginScene(std::move(catalog))) {
    result.status = ::lightusd::next::OperationStatus::SinkRejected;
    result.error = "scene sink rejected catalog";
    return result;
  }
  // The catalog and all non-geometry metadata are now owned by the sink.
  // Drop the traversal-order pointer index before the first large geometry
  // payload is decoded; category lists reference the single owned records.
  extracted.release_records();

  const auto capture_diagnostics = [&]() {
    result.conversion_diagnostics.insert(result.conversion_diagnostics.end(),
        conversion_diagnostics_.begin(), conversion_diagnostics_.end());
    conversion_diagnostics_.clear();
    NormalizeConversionDiagnostics(&result.conversion_diagnostics);
  };
  const auto abort = [&](const std::string& error, bool cancelled) {
    capture_diagnostics();
    sink->AbortScene();
    result.status = cancelled
                        ? ::lightusd::next::OperationStatus::Cancelled
                        : ::lightusd::next::OperationStatus::SinkRejected;
    result.cancelled = cancelled;
    result.error = error;
    result.warnings = std::move(warnings_);
  };

  for (size_t i = 0; i < extracted.meshes.size(); ++i) {
    const UsdPrim& prim = extracted.meshes[i].prim;
    if (cancellation_requested()) {
      abort("conversion cancelled", true);
      return result;
    }
    const GeometryInfo info =
        GetGeometryInfo(prim, GeometryKind::Mesh, static_cast<int32_t>(i));
    const GeometryDisposition disposition = sink->SelectGeometry(info);
    if (disposition == GeometryDisposition::Cancel) {
      abort("conversion cancelled by scene sink", true);
      return result;
    }
    if (disposition == GeometryDisposition::Skip) continue;

    RenderMesh mesh;
    bool converted = false;
    if (disposition == GeometryDisposition::Proxy) {
      converted = ConvertExtentProxy(prim, &mesh);
      if (!converted) {
        AddWarning("Skipping proxy without a valid extent: " +
                            prim.GetPath().str());
        continue;
      }
    } else {
      converted = ConvertRenderableMesh(stage, prim, &mesh);
    }
    if (!converted || mesh.has_alloc_failure()) {
      AddConversionWarning(prim.GetPath().str(),
          "Failed to convert renderable mesh prim: " + prim.GetPath().str(),
          ConversionDisposition::Failed);
      continue;
    }
    if (!config_.mesh.retain_geometry) {
      ReleaseSourceMeshStaticArrays(stage, prim);
    }
    AssignMeshMaterialBinding(stage, binding_catalog, &mesh);
    if (mesh.skin) {
      const auto skeleton = skeleton_by_path.find(mesh.skin->skeleton_path);
      if (skeleton != skeleton_by_path.end()) {
        mesh.skin->skeleton_id = skeleton->second;
      }
    }
    mesh.compact();
    if (!sink->AddMesh(static_cast<int32_t>(i), std::move(mesh))) {
      abort("scene sink rejected mesh", false);
      return result;
    }
    ++result.mesh_count;
  }
  RenderExtractResult::release_list(&extracted.meshes);

  int32_t points_id = 0;
  for (const RenderPrimRecord& rec : extracted.points) {
    if (cancellation_requested()) {
      abort("conversion cancelled", true);
      return result;
    }
    const GeometryInfo info =
        GetGeometryInfo(rec.prim, GeometryKind::Points, points_id);
    const GeometryDisposition disposition = sink->SelectGeometry(info);
    if (disposition == GeometryDisposition::Cancel) {
      abort("conversion cancelled by scene sink", true);
      return result;
    }
    if (disposition != GeometryDisposition::Full) {
      if (disposition == GeometryDisposition::Proxy) {
        AddWarning("Skipping Points proxy without mesh expansion: " +
                            rec.path);
      }
      ++points_id;
      continue;
    }
    RenderPoints points;
    if (ConvertPoints(stage, rec.prim, &points)) {
      if (points.has_alloc_failure()) {
        AddConversionWarning(rec.path, "Out of memory converting Points '" + rec.path +
                            "'; the prim was skipped", ConversionDisposition::Failed);
        ++points_id;
        continue;
      }
      if (!config_.mesh.retain_geometry) {
        ReleaseSourceMeshStaticArrays(stage, rec.prim);
      }
      points.compact();
      if (!sink->AddPoints(points_id, std::move(points))) {
        abort("scene sink rejected points", false);
        return result;
      }
      ++result.point_count;
    } else {
      AddConversionWarning(rec.path, "Failed to convert Points: " + rec.path,
                           ConversionDisposition::Failed);
    }
    ++points_id;
  }
  RenderExtractResult::release_list(&extracted.points);
  for (size_t i = 0; i < extracted.curves.size(); ++i) {
    if (cancellation_requested()) {
      abort("conversion cancelled", true);
      return result;
    }
    const GeometryInfo info = GetGeometryInfo(
        extracted.curves[i].prim, GeometryKind::Curves,
        static_cast<int32_t>(i));
    const GeometryDisposition disposition = sink->SelectGeometry(info);
    if (disposition == GeometryDisposition::Cancel) {
      abort("conversion cancelled by scene sink", true);
      return result;
    }
    if (disposition != GeometryDisposition::Full) {
      if (disposition == GeometryDisposition::Proxy) {
        AddWarning("Skipping Curves proxy without mesh expansion: " +
                            extracted.curves[i].path);
      }
      continue;
    }
    RenderCurves curves;
    if (!ConvertCurves(extracted.curves[i].prim, &curves)) {
      AddConversionWarning(extracted.curves[i].path,
          "Failed to convert curves prim: " + extracted.curves[i].path,
          ConversionDisposition::Failed);
      continue;
    }
    if (curves.has_alloc_failure()) {
      AddConversionWarning(extracted.curves[i].path, "Out of memory converting curves '" +
                          extracted.curves[i].path + "'; the prim was skipped",
                          ConversionDisposition::Failed);
      continue;
    }
    if (!config_.mesh.retain_geometry) {
      ReleaseSourceMeshStaticArrays(stage, extracted.curves[i].prim);
    }
    const std::string material =
        FindInheritedMaterialBinding(stage, curves.prim_path,
                                     config_.material.binding_purpose);
    const auto found = binding_catalog.material_by_path.find(material);
    if (found != binding_catalog.material_by_path.end()) {
      curves.material_id = found->second;
    }
    curves.compact();
    if (!sink->AddCurves(static_cast<int32_t>(i), std::move(curves))) {
      abort("scene sink rejected curves", false);
      return result;
    }
    ++result.curve_count;
  }
  RenderExtractResult::release_list(&extracted.curves);
  RenderExtractResult::release_list(&extracted.point_instancers);
  RenderExtractResult::release_list(&extracted.materials);
  RenderExtractResult::release_list(&extracted.skeletons);
  RenderExtractResult::release_list(&extracted.lights);
  RenderExtractResult::release_list(&extracted.cameras);
  extracted.release_storage();

  if (config_.strict_conversion && !warnings_.empty()) {
    sink->AbortScene();
    capture_diagnostics();
    result.status = ::lightusd::next::OperationStatus::InvalidData;
    result.error = "strict render conversion rejected warnings: " + warnings_.front();
    result.warnings = std::move(warnings_);
    return result;
  }
  capture_diagnostics();
  if (!sink->EndScene()) {
    abort("scene sink failed to finalize", false);
    return result;
  }
  result.success = true;
  result.status = ::lightusd::next::OperationStatus::Ok;
  result.warnings = std::move(warnings_);
  return result;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
