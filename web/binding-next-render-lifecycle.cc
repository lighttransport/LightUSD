// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
RenderStream::RenderStream() = default;
RenderStream::~RenderStream() = default;

void RenderStream::provideAsset(const std::string& name, const emscripten::val& bytes) {
    std::string copy_error;
    std::string data = CopyUint8ArrayToString(bytes, &copy_error);
    if (!copy_error.empty()) {
      error_ = copy_error;
      return;
    }
    std::string key = tn::AssetResolver::NormalizePath(name);
    while (key.rfind("./", 0) == 0) key.erase(0, 2);
    clip_assets_[key] = std::move(data);
  }

emscripten::val RenderStream::listVariants() const {
    emscripten::val out = emscripten::val::array();
    for (const VariantSetInfo &info : variant_sets_) {
      emscripten::val item = emscripten::val::object();
      item.set("primPath", info.prim_path);
      item.set("setName", info.set_name);
      item.set("selected", info.selected);
      emscripten::val names = emscripten::val::array();
      for (const std::string &name : info.variant_names) {
        names.call<void>("push", name);
      }
      item.set("variants", names);
      out.call<void>("push", item);
    }
    return out;
  }

void RenderStream::setTangentMethod(const std::string &method) {
    tangent_method_ = method;
    std::transform(tangent_method_.begin(), tangent_method_.end(),
                   tangent_method_.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
  }

emscripten::val RenderStream::beginOwned(std::string &&crate) {
    emscripten::val r = emscripten::val::object();
    end();
    error_.clear();
    stats_ = Stats{};
    stats_.input_copy_ms = pending_input_copy_ms_;
    stats_.input_bytes = pending_input_bytes_;
    pending_input_copy_ms_ = 0.0;
    pending_input_bytes_ = 0;
    const double stage_load_start_ms = emscripten_get_now();
    if (IsUSDCBytes(crate)) {
      lightusd::next::USDCLoadOptions opts;
      opts.crate_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        reportNextCrateProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
        return true;
      };
      lightusd::next::USDCLoadResult res =
          lightusd::next::LoadUSDCFromMemoryOwned(std::move(crate), opts);
      if (!res.success) {
        error_ = res.error_summary.empty() ? std::string("USDC load failed")
                                           : res.error_summary;
        r.set("success", false);
        r.set("error", error_);
        return r;
      }
      stage_ = std::move(res.stage);
    } else {
      lightusd::next::LoadUSDOptions opts;
      opts.usda_options.parse_options.enable_usda_lazy_arrays = true;
      opts.usdc_options.crate_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        reportNextCrateProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
        return true;
      };
      std::string warn;
      std::string err;
      const bool ok = lightusd::next::LoadUSDFromMemoryOwned(
          std::move(crate), &stage_, opts, &warn, &err);
      if (!ok) {
        error_ = err.empty() ? std::string("USD memory load failed") : err;
        r.set("success", false);
        r.set("error", error_);
        return r;
      }
    }
    stats_.stage_load_ms = emscripten_get_now() - stage_load_start_ms;
    // Record authored variant sets (consumed by composition below), then
    // compose in place when the layer carries composition arcs — variants,
    // internal references, inherits/specializes. External arcs cannot anchor
    // for memory roots and surface as warnings (multi-layer scenes go through
    // NextFlattenSession instead).
    const double composition_start_ms = emscripten_get_now();
    collectVariantSets_();
    if (lightusd::next::StageNeedsComposition(stage_)) {
      lightusd::next::pcp::CompositionOptions copts;
      copts.variant_overrides = variant_overrides_;
      std::string cwarn, cerr;
      if (!lightusd::next::ComposeLoadedStage(&stage_, &cwarn, &cerr, &copts,
                                              "memory-root")) {
        error_ = cerr.empty() ? std::string("in-memory composition failed")
                              : cerr;
        r.set("success", false);
        r.set("error", error_);
        return r;
      }
    }
    stats_.composition_ms = emscripten_get_now() - composition_start_ms;
    const double mesh_discovery_start_ms = emscripten_get_now();
    meshes_ = lightusd::next::GetAllMeshes(stage_);
    stats_.mesh_discovery_ms =
        emscripten_get_now() - mesh_discovery_start_ms;
    stats_.source_mesh_count = meshes_.size();
    // GetAllMeshes() exposes composed instance children whose GetParent()
    // chain does not include the instance root. Seed transform caches from
    // the render traversal before mesh-only optimization so baking sees the
    // same composed world transforms as the node hierarchy. A full
    // RenderScene is intentionally not built for mesh-only workers.
    buildMeshTransformCaches_();
    if (mesh_merge_) {
      const double optimize_start_ms = emscripten_get_now();
      buildOptimizedOutputs_();
      stats_.optimize_ms = emscripten_get_now() - optimize_start_ms;
    }
    if (!mesh_only_) {
      buildRenderScene_();
      buildAnalyticOutputs_();
    }
    loaded_ = true;
    r.set("success", true);
    r.set("meshCount", meshCount());
    r.set("points", static_cast<int>(pointsCount()));
    r.set("pointsCount", static_cast<int>(pointsCount()));
    r.set("curves", static_cast<int>(curvesCount()));
    r.set("curvesCount", static_cast<int>(curvesCount()));
    r.set("nodes", static_cast<int>(nodeCount()));
    r.set("nodeCount", static_cast<int>(nodeCount()));
    r.set("lights", static_cast<int>(lightCount()));
    r.set("lightCount", static_cast<int>(lightCount()));
    r.set("cameras", static_cast<int>(cameraCount()));
    r.set("cameraCount", static_cast<int>(cameraCount()));
    r.set("pointInstancers", static_cast<int>(pointInstancerCount()));
    r.set("pointInstancerCount", static_cast<int>(pointInstancerCount()));
    r.set("skeletons", static_cast<int>(skeletonCount()));
    r.set("skeletonCount", static_cast<int>(skeletonCount()));
    r.set("unsupportedRenderables", static_cast<int>(unsupportedRenderableCount()));
    r.set("unsupportedRenderableCount", static_cast<int>(unsupportedRenderableCount()));
    r.set("animations", static_cast<int>(animationCount()));
    r.set("animationCount", static_cast<int>(animationCount()));
    r.set("pointInstanceDraws", static_cast<int>(pointInstanceDrawCount()));
    r.set("pointInstanceDrawCount", static_cast<int>(pointInstanceDrawCount()));
    return r;
  }

emscripten::val RenderStream::begin(emscripten::val bytes) {
    const double input_copy_start_ms = emscripten_get_now();
    const size_t size = bytes["byteLength"].as<size_t>();
    constexpr size_t kMaxLayerBytes = size_t(1) << 30;  // 1 GiB
    if (size > kMaxLayerBytes) {
      emscripten::val r = emscripten::val::object();
      error_ = "Input exceeds 1 GiB limit";
      r.set("success", false);
      r.set("error", error_);
      return r;
    }
    std::string s;
    s.resize(size);
    if (size > 0) {
      emscripten::val view = emscripten::val::global("Uint8Array").new_(
          bytes["buffer"], bytes["byteOffset"],
          emscripten::val(static_cast<double>(size)));
      emscripten::val heapView = emscripten::val(emscripten::typed_memory_view(
          size, reinterpret_cast<uint8_t *>(&s[0])));
      heapView.call<void>("set", view);
    }
    pending_input_copy_ms_ = emscripten_get_now() - input_copy_start_ms;
    pending_input_bytes_ = size;
    return beginOwned(std::move(s));
  }

emscripten::val RenderStream::getStats() const {
    emscripten::val s = emscripten::val::object();
    s.set("sourceMeshes", static_cast<int>(stats_.source_mesh_count));
    const size_t source_material_count =
        std::max(stats_.source_material_count, source_material_keys_.size());
    const size_t source_texture_count =
        std::max(stats_.source_texture_count, source_texture_keys_.size());
    s.set("sourceMaterials", static_cast<int>(source_material_count));
    s.set("sourceTextures", static_cast<int>(source_texture_count));
    s.set("optimizedMeshes", static_cast<int>(meshCount()));
    s.set("optimizedMaterials", static_cast<int>(materials_.size()));
    s.set("optimizedTextures", static_cast<int>(texture_keys_.size()));
    s.set("mergedMeshes", static_cast<int>(stats_.merged_mesh_count));
    s.set("mergeGroups", static_cast<int>(stats_.merge_group_count));
    s.set("skippedMergeMeshes", static_cast<int>(stats_.skipped_merge_count));
    s.set("materialDedup", material_dedup_);
    s.set("meshMerge", mesh_merge_);
    s.set("meshMergeBakeTransform", mesh_merge_bake_transform_);
    s.set("flattenRenderTree", flatten_render_tree_);
    s.set("nativeStageLoadMs", stats_.stage_load_ms);
    s.set("nativeInputCopyMs", stats_.input_copy_ms);
    s.set("nativeInputBytes", static_cast<double>(stats_.input_bytes));
    s.set("nativeCompositionMs", stats_.composition_ms);
    s.set("nativeMeshDiscoveryMs", stats_.mesh_discovery_ms);
    s.set("nativeOptimizeMs", stats_.optimize_ms);
    s.set("nativeMaterialMs", stats_.material_ms);
    s.set("nativeMaterialIdentityMs", stats_.material_identity_ms);
    s.set("nativeMaterialConversionMs", stats_.material_conversion_ms);
    s.set("nativeGeometryBuildMs", stats_.geometry_build_ms);
    s.set("nativeMergeAppendMs", stats_.merge_append_ms);
    s.set("materialIdentityHits",
          static_cast<int>(stats_.material_identity_hits));
    s.set("materialIdentityMisses",
          static_cast<int>(stats_.material_identity_misses));
    s.set("materialGraphCacheHits",
          static_cast<int>(stats_.material_graph_cache_hits));
    s.set("materialGraphCacheMisses",
          static_cast<int>(stats_.material_graph_cache_misses));
    s.set("geometryBorrowedBytes",
          static_cast<double>(stats_.geometry_borrowed_bytes));
    s.set("geometryMaterializedBytes",
          static_cast<double>(stats_.geometry_materialized_bytes));
    size_t provided_asset_bytes = 0;
    for (const auto& asset : clip_assets_) {
      provided_asset_bytes += asset.second.size();
    }
    s.set("providedAssetBytes", static_cast<double>(provided_asset_bytes));
    s.set("stageMemoryBytes", static_cast<double>(stage_.GetMemoryUsage()));
    if (render_scene_valid_) {
      s.set("renderSceneMemoryBytes",
            static_cast<double>(render_scene_.memory_usage()));
      size_t mesh_points_bytes = 0;
      size_t mesh_normals_bytes = 0;
      size_t mesh_uv_bytes = 0;
      size_t mesh_topology_bytes = 0;
      size_t mesh_triangulation_bytes = 0;
      for (const tr::RenderMesh &mesh : render_scene_.meshes) {
        mesh_points_bytes += mesh.points.memory_usage();
        mesh_normals_bytes += mesh.normals.memory_usage();
        mesh_uv_bytes += mesh.texcoords_0.memory_usage() +
                         mesh.texcoords_1.memory_usage();
        mesh_topology_bytes += mesh.face_vertex_counts.memory_usage() +
                               mesh.face_vertex_indices.memory_usage();
        mesh_triangulation_bytes +=
            mesh.triangulated_indices.memory_usage() +
            mesh.triangulated_face_vertex_indices.memory_usage();
      }
      s.set("renderMeshPointsBytes", static_cast<double>(mesh_points_bytes));
      s.set("renderMeshNormalsBytes", static_cast<double>(mesh_normals_bytes));
      s.set("renderMeshUvBytes", static_cast<double>(mesh_uv_bytes));
      s.set("renderMeshTopologyBytes", static_cast<double>(mesh_topology_bytes));
      s.set("renderMeshTriangulationBytes",
            static_cast<double>(mesh_triangulation_bytes));
      s.set("renderSceneNodes", static_cast<int>(render_scene_.nodes.size()));
      s.set("renderSceneMeshes", static_cast<int>(render_scene_.meshes.size()));
      s.set("renderScenePoints", static_cast<int>(render_scene_.points.size()));
      s.set("renderSceneCurves", static_cast<int>(render_scene_.curves.size()));
      s.set("renderScenePointInstancers",
            static_cast<int>(render_scene_.point_instancers.size()));
      s.set("renderScenePointInstanceDraws",
            static_cast<int>(render_scene_.point_instance_draws.size()));
      s.set("renderSceneMaterials",
            static_cast<int>(render_scene_.materials.size()));
      s.set("renderSceneTextures",
            static_cast<int>(render_scene_.textures.size()));
      s.set("renderSceneImages", static_cast<int>(render_scene_.images.size()));
      s.set("renderSceneLights", static_cast<int>(render_scene_.lights.size()));
      s.set("renderSceneCameras",
            static_cast<int>(render_scene_.cameras.size()));
      s.set("renderSceneAnimations",
            static_cast<int>(render_scene_.animations.size()));
      s.set("renderSceneSkeletons",
            static_cast<int>(render_scene_.skeletons.size()));
      s.set("renderSceneUnsupportedRenderables",
            static_cast<int>(render_scene_.unsupported_renderables.size()));
      s.set("renderSceneWarnings", static_cast<int>(render_scene_warnings_.size()));
    } else {
      s.set("renderSceneNodes", 0);
      s.set("renderSceneMeshes", 0);
      s.set("renderScenePoints", 0);
      s.set("renderSceneCurves", 0);
      s.set("renderScenePointInstancers", 0);
      s.set("renderScenePointInstanceDraws", 0);
      s.set("renderSceneMaterials", 0);
      s.set("renderSceneTextures", 0);
      s.set("renderSceneImages", 0);
      s.set("renderSceneLights", 0);
      s.set("renderSceneCameras", 0);
      s.set("renderSceneAnimations", 0);
      s.set("renderSceneSkeletons", 0);
      s.set("renderSceneUnsupportedRenderables", 0);
      s.set("renderSceneWarnings", 0);
    }
    return s;
  }

emscripten::val RenderStream::getSceneMetadata() const {
    emscripten::val metadata = emscripten::val::object();
    if (!loaded_) return metadata;
    const lightusd::next::StageMeta &meta = stage_.GetMeta();
    metadata.set("upAxis", meta.upAxis);
    metadata.set("metersPerUnit", meta.metersPerUnit);
    // MassAPI SI conversion on the web/sim side (parity with the legacy
    // binding's kilogramsPerUnit export).
    metadata.set("kilogramsPerUnit", meta.kilogramsPerUnit);
    metadata.set("framesPerSecond", meta.framesPerSecond);
    metadata.set("timeCodesPerSecond", meta.timeCodesPerSecond);
    metadata.set("startTimeCode", meta.startTimeCode);
    metadata.set("endTimeCode", meta.endTimeCode);
    metadata.set("renderSettingsPrimPath", render_scene_.render_settings_path);
    metadata.set("workingColorSpace", render_scene_.working_color_space);
    metadata.set("workingToDisplayLinear",
                 Matrix3Value(render_scene_.working_to_display_linear));
    return metadata;
  }

emscripten::val RenderStream::getMesh(int i) {
    emscripten::val out = emscripten::val::object();
    if (!loaded_ || i < 0 || i >= meshCount()) {
      out.set("error", std::string("invalid mesh index"));
      return out;
    }
    if (mesh_merge_) {
      if (static_cast<size_t>(i) < outputs_.size()) {
        const OutputMesh &record = outputs_[static_cast<size_t>(i)];
        if (record.merged) return outputMergedMesh_(record);
        return outputSourceMesh_(record.source_index);
      }
      return outputMergedMesh_(
          analytic_outputs_[static_cast<size_t>(i) - outputs_.size()]);
    }
    if (static_cast<size_t>(i) < meshes_.size()) return outputSourceMesh_(i);
    return outputMergedMesh_(
        analytic_outputs_[static_cast<size_t>(i) - meshes_.size()]);
  }

void RenderStream::end() {
    loaded_ = false;
    render_scene_valid_ = false;
    render_scene_ = tr::RenderScene();
    render_scene_warnings_.clear();
    meshes_.clear();
    meshes_.shrink_to_fit();
    outputs_.clear();
    outputs_.shrink_to_fit();
    analytic_outputs_.clear();
    analytic_outputs_.shrink_to_fit();
    materials_.clear();
    material_key_to_id_.clear();
    material_path_to_id_.clear();
    material_identity_to_id_.clear();
    local_matrix_cache_.clear();
    world_matrix_cache_.clear();
    source_material_keys_.clear();
    source_texture_keys_.clear();
    texture_keys_.clear();
    stage_ = lightusd::next::Stage();
    freeVec_(s_points_);
    freeVec_(s_normals_);
    freeVec_(s_uv_);
    freeVec_(s_tangents_);
    freeVec_(s_indices_);
    freeVec_(s_joint_indices_);
    freeVec_(s_joint_weights_);
    freeVec_(s_point_source_indices_);
    freeVec_(s_points_cloud_points_);
    freeVec_(s_points_cloud_widths_);
    freeVec_(s_points_cloud_colors_);
    freeVec_(s_curve_points_);
    freeVec_(s_curve_widths_);
    freeVec_(s_curve_colors_);
    freeVec_(s_curve_tessellated_points_);
    freeVec_(s_curve_tessellated_widths_);
    freeVec_(s_curve_tessellated_colors_);
  }

void RenderStream::buildRenderScene_() {
    tr::ConverterConfig cfg;
    cfg.time_code = 0.0;
    cfg.mesh.compute_tangents = compute_tangents_;
    cfg.mesh.tangent_method = tangentMethod_();
    cfg.mesh.enable_bone_reduction = true;
    cfg.mesh.target_bone_count = 4;
    // RenderStream builds one browser-facing mesh lazily from Stage. Keeping a
    // second, complete geometry copy in RenderScene only inflates the wasm
    // heap; retain its material/skinning/animation metadata instead. Keep the
    // compact earcut result used by the lazy mesh builder, plus generated
    // analytic geometry which has no authored Mesh payload to rebuild from.
    cfg.mesh.retain_geometry = false;
    cfg.mesh.retain_triangulation = true;
    cfg.mesh.retain_analytic_geometry = true;
    cfg.material.load_textures = false;
    cfg.material.allow_missing_textures = true;
    cfg.material.render_settings_path = render_settings_path_;
    cfg.point_instancer.duplicate_meshes = false;
    cfg.animation.clip_stage_loader =
        [this](const std::string& asset_path, tn::Stage* stage,
               std::string* warn, std::string* err) {
          std::string key = tn::AssetResolver::NormalizePath(asset_path);
          while (key.rfind("./", 0) == 0) key.erase(0, 2);
          auto it = clip_assets_.find(key);
          if (it == clip_assets_.end()) {
            for (const std::string& candidate :
                 tn::AssetResolver::SuffixCandidates(key)) {
              it = clip_assets_.find(candidate);
              if (it != clip_assets_.end()) break;
            }
          }
          if (it == clip_assets_.end()) {
            if (err) *err = "asset was not supplied to RenderStream";
            return false;
          }
          tn::LoadUSDOptions options;
          options.usda_options.parse_options.enable_usda_lazy_arrays = true;
          std::string bytes = it->second;
          return tn::LoadUSDFromMemoryOwned(std::move(bytes), stage, options,
                                            warn, err);
        };
    tr::RenderSceneConverter converter(cfg);
    tr::ConvertResult result = converter.Convert(stage_);
    render_scene_ = std::move(result.scene);
    render_scene_warnings_ = result.warnings;
    render_scene_valid_ = result.success;
    if (!result.success && !result.error.empty()) {
      error_ = result.error;
    }
  }

void RenderStream::collectVariantSets_() {
    variant_sets_.clear();
    const lightusd::next::Layer *root = stage_.GetRootLayer();
    if (!root) return;
    for (const auto &prim : root->prims()) {
      const auto &meta = prim.meta();
      for (const auto &vs : meta.variantSets()) {
        VariantSetInfo info;
        info.prim_path = prim.path().str();
        info.set_name = vs.name;
        // Authored selection lives in the prim's `variants = {...}` metadata
        // (variantSelections / legacy single variantSelection), not on the
        // VariantSetData itself.
        info.selected = vs.selected;
        for (const auto &sel : meta.variantSelections()) {
          if (sel.first == vs.name) {
            info.selected = sel.second;
            break;
          }
        }
        if (info.selected.empty() && !meta.variantSelection.empty()) {
          const std::string &legacy = meta.variantSelection;
          const size_t eq = legacy.find('=');
          if (eq != std::string::npos && legacy.substr(0, eq) == vs.name) {
            info.selected = legacy.substr(eq + 1);
          }
        }
        for (const auto &variant : vs.variants) {
          info.variant_names.push_back(variant.name);
        }
        variant_sets_.push_back(std::move(info));
      }
    }
  }
}  // namespace web_next
}  // namespace lightusd
