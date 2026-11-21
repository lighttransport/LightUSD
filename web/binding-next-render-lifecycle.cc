// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
#include "next/schema/usd-shade.hh"
#include "binding-next-assets.hh"
#include "binding-next-layer.hh"
#include "c-api/lightusd-render-c.h"
#include "next/load-usd.hh"
#include "next/composition/composition.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/writer/usdc-writer.hh"
#include "image-loader.hh"
#include "core/safe-arithmetic.hh"
#include "next/c/storage.h"
#include <cmath>
namespace lightusd {
namespace web_next {
static_assert(LIGHTUSD_RENDER_NODE == 0 && LIGHTUSD_RENDER_MESH == 1 &&
              LIGHTUSD_RENDER_MATERIAL == 2 && LIGHTUSD_RENDER_TEXTURE == 3 &&
              LIGHTUSD_RENDER_IMAGE == 4 && LIGHTUSD_RENDER_LIGHT == 5 &&
              LIGHTUSD_RENDER_CAMERA == 6 && LIGHTUSD_RENDER_SKELETON == 7 &&
              LIGHTUSD_RENDER_ANIMATION == 8 && LIGHTUSD_RENDER_UNSUPPORTED == 9 &&
              LIGHTUSD_RENDER_INSTANCER == 10 && LIGHTUSD_RENDER_ROOT_NODE == 11 &&
              LIGHTUSD_RENDER_POINTS == 12 && LIGHTUSD_RENDER_CURVES == 13 &&
              LIGHTUSD_RENDER_POINT_INSTANCE_DRAW == 14,
              "web resource kinds must match the native C render enum");
RenderStream::RenderStream() {
    // Route resolved memory identifiers through ReadAsset rather than a
    // filesystem open. Missing keys still fail at the byte-reader boundary.
    asset_resolver_.SetAssetReader(
        [](const std::string&, std::vector<uint8_t>*, std::string* error) {
          if (error) *error = "asset is not present in the imported memory store";
          return false;
        });
}
RenderStream::~RenderStream() = default;


const tr::RenderMesh* RenderStream::sourceRenderMesh_(int mesh_id) const {
    if (!loaded_ || mesh_id < 0 || mesh_id >= meshCount()) return nullptr;
    int source_index = -1;
    if (mesh_merge_) {
      if (static_cast<size_t>(mesh_id) < outputs_.size())
        source_index = outputs_[static_cast<size_t>(mesh_id)].source_index;
    } else if (static_cast<size_t>(mesh_id) < meshes_.size()) {
      source_index = mesh_id;
    }
    if (source_index < 0 || !render_scene_valid_ ||
        static_cast<size_t>(source_index) >= meshes_.size()) return nullptr;
    const std::string path = meshes_[static_cast<size_t>(source_index)]
                                 .GetPrim().GetPath().str();
    const auto it = render_scene_.mesh_by_path.find(path);
    if (it == render_scene_.mesh_by_path.end() || it->second < 0 ||
        static_cast<size_t>(it->second) >= render_scene_.meshes.size()) return nullptr;
    return &render_scene_.meshes[static_cast<size_t>(it->second)];
}





namespace {
template <typename Visitor>
bool VisitLayerAssetPaths(const lightusd::next::Stage& stage, uint8_t kind,
                          Visitor&& visitor) {
    const lightusd::next::Layer* root = stage.GetRootLayer();
    if (!root || kind > 2) return true;
    if (kind == 0) {
      for (const std::string& path : root->meta().subLayers)
        if (!visitor(path)) return false;
      return true;
    }
    for (size_t i = 0; i < root->prim_count(); ++i) {
      const lightusd::next::PrimSpec* prim =
          root->prim(static_cast<uint32_t>(i));
      if (!prim) continue;
      const auto& arcs = kind == 1 ? prim->meta().references
                                   : prim->meta().payloads;
      for (const std::string& encoded : arcs) {
        const auto arc = kind == 1
            ? lightusd::next::Compositor::ParseReference(encoded)
            : lightusd::next::Compositor::ParsePayload(encoded);
        if (kind == 1 && arc.is_internal) continue;
        if (arc.asset_path.empty()) continue;
        if (!visitor(arc.asset_path)) return false;
      }
    }
    return true;
}
}  // namespace


void RenderStream::setTangentMethod(const std::string &method) {
    tangent_method_ = method;
    std::transform(tangent_method_.begin(), tangent_method_.end(),
                   tangent_method_.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
  }

bool RenderStream::beginOwned(std::string &&crate) {
    // The next-only WASM surface accepts bytes, not filesystem paths. USDC
    // lazy arrays retain this owned input buffer rather than an mmap.
    end();
    error_.clear();
    warning_.clear();
    composition_report_ = lightusd::next::pcp::CompositionReport{};
    stats_ = Stats{};
    stats_.input_copy_ms = pending_input_copy_ms_;
    stats_.input_bytes = crate.size();
    pending_input_copy_ms_ = 0.0;
    pending_input_bytes_ = 0;
    if (crate.size() > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return false;
    }
    const size_t memory_limit = maxMemoryLimitBytes();
    const size_t asset_bytes = providedAssetMemoryBytes();
    if (asset_bytes >= memory_limit ||
        crate.size() >= memory_limit - asset_bytes) {
      error_ = "Input and provided assets leave no resident memory for the Stage";
      return false;
    }
    const size_t load_resident_limit =
        memory_limit - asset_bytes - crate.size();
    const double stage_load_start_ms = emscripten_get_now();
    if (IsUSDCBytes(crate)) {
      lightusd::next::USDCLoadOptions opts;
      opts.crate_options.max_memory = std::min(
          opts.crate_options.max_memory,
          std::min(static_cast<size_t>(max_input_bytes_),
                   load_resident_limit));
      opts.crate_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        return reportNextLoadProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
      };
      lightusd::next::USDCLoadResult res =
          lightusd::next::LoadUSDCFromMemoryOwned(std::move(crate), opts);
      if (!res.success) {
        error_ = res.error_summary.empty() ? std::string("USDC load failed")
                                           : res.error_summary;
        return false;
      }
      stage_ = std::move(res.stage);
    } else {
      lightusd::next::LoadUSDOptions opts;
      opts.limits.max_input_bytes = max_input_bytes_;
      opts.limits.max_resident_bytes = load_resident_limit;
      opts.usda_options.parse_options.enable_usda_lazy_arrays = true;
      opts.usda_options.parse_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        return reportNextLoadProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
      };
      opts.usdc_options.crate_options.progress_callback =
          [](const char *phase, size_t current, size_t total) -> bool {
        return reportNextLoadProgress(
            phase, static_cast<double>(current), static_cast<double>(total));
      };
      std::string warn;
      std::string err;
      const bool ok = lightusd::next::LoadUSDFromMemoryOwned(
          std::move(crate), &stage_, opts, &warn, &err);
      if (!ok) {
        error_ = err.empty() ? std::string("USD memory load failed") : err;
        return false;
      }
      warning_ = std::move(warn);
    }
    stats_.stage_load_ms = emscripten_get_now() - stage_load_start_ms;
    bool authored_has_inherits = false;
    bool authored_has_specializes = false;
    if (const lightusd::next::Layer* root = stage_.GetRootLayer()) {
      for (size_t i = 0; i < root->prim_count(); ++i) {
        const lightusd::next::PrimSpec* prim =
            root->prim(static_cast<uint32_t>(i));
        if (!prim) continue;
        authored_has_inherits = authored_has_inherits ||
                                !prim->meta().inherits.empty();
        authored_has_specializes = authored_has_specializes ||
                                   !prim->meta().specializes.empty();
        if (authored_has_inherits && authored_has_specializes) break;
      }
    }
    // Record authored variant sets (consumed by composition below), then
    // compose in place when the layer carries composition arcs — variants,
    // internal references, inherits/specializes. External arcs cannot anchor
    // for memory roots and surface as warnings (multi-layer scenes go through
    // NextFlattenSession instead).
    const double composition_start_ms = emscripten_get_now();
    collectVariantSets_();
    // In-place composition consumes the root layer's arcs; snapshot the
    // authored sublayer/reference/payload asset paths for the arc queries.
    for (uint8_t kind = 0; kind < 3; ++kind) {
      authored_arc_paths_[kind].clear();
      VisitLayerAssetPaths(stage_, kind, [&](const std::string& value) {
        authored_arc_paths_[kind].push_back(value);
        return true;
      });
    }
    authored_arc_present_[0] = [&] {
      const lightusd::next::Layer* root = stage_.GetRootLayer();
      return root && !root->meta().subLayers.empty();
    }();
    for (uint8_t kind = 1; kind < 3; ++kind) {
      authored_arc_present_[kind] = false;
      if (const lightusd::next::Layer* root = stage_.GetRootLayer()) {
        for (size_t i = 0; i < root->prim_count() && !authored_arc_present_[kind]; ++i) {
          const lightusd::next::PrimSpec* prim = root->prim(static_cast<uint32_t>(i));
          if (prim) authored_arc_present_[kind] = !(kind == 1 ? prim->meta().references
                                                            : prim->meta().payloads).empty();
        }
      }
    }
    if (enable_composition_ && lightusd::next::StageNeedsComposition(stage_)) {
      lightusd::next::pcp::CompositionOptions copts;
      copts.variant_overrides = variant_overrides_;
      std::string cwarn, cerr;
      lightusd::next::LoadUSDOptions composition_options;
      composition_options.limits.max_input_bytes = max_input_bytes_;
      composition_options.limits.max_resident_bytes = load_resident_limit;
      if (!lightusd::next::ComposeLoadedStage(
              &stage_, asset_resolver_, "memory-root", composition_options,
              &cwarn, &cerr, &copts, &composition_report_)) {
        error_ = cerr.empty() ? std::string("in-memory composition failed")
                              : cerr;
        return false;
      }
      if (!cwarn.empty()) {
        if (!warning_.empty()) warning_ += "\n";
        warning_ += cwarn;
      }
    }
    if (stage_.GetMemoryUsage() >= load_resident_limit) {
      error_ = "Stage exceeds configured resident memory limit";
      stage_ = lightusd::next::Stage();
      return false;
    }
    stats_.composition_ms = emscripten_get_now() - composition_start_ms;
    const double mesh_discovery_start_ms = emscripten_get_now();
    meshes_ = lightusd::next::GetAllMeshes(stage_);
    tangent_requests_.assign(meshes_.size(), 0);
    stats_.mesh_discovery_ms =
        emscripten_get_now() - mesh_discovery_start_ms;
    stats_.source_mesh_count = meshes_.size();
    // GetAllMeshes() exposes composed instance children whose GetParent()
    // chain does not include the instance root. Seed transform caches from
    // the render traversal before mesh-only optimization so baking sees the
    // same composed world transforms as the node hierarchy. A full
    // RenderScene is intentionally not built for mesh-only workers.
    buildMeshTransformCaches_();
    if (!mesh_only_) {
      // Public material ids index materials_. Register every bound material
      // in mesh order now so ids are deterministic (like legacy) instead of
      // depending on which mesh a caller views first. Mesh-only workers
      // convert only the materials of the meshes they are asked for.
      // The fallback record for unbound meshes is registered last so it
      // never shifts the ids of bound materials.
      const double material_start_ms = emscripten_get_now();
      bool has_unbound_mesh = false;
      for (const auto& mesh : meshes_) {
        const lightusd::next::UsdPrim& prim = mesh.GetPrim();
        const lightusd::next::UsdPrim bound = lightusd::next::GetBoundMaterial(stage_, prim);
        if (bound.IsValid()) (void)registerMaterial_(bound);
        else has_unbound_mesh = true;
        for (const lightusd::next::UsdPrim& child : prim.GetChildren()) {
          if (!child.IsValid() || child.GetTypeName() != "GeomSubset") continue;
          const lightusd::next::UsdPrim subset_bound =
              lightusd::next::GetBoundMaterial(stage_, child);
          if (subset_bound.IsValid()) (void)registerMaterial_(subset_bound);
        }
      }
      if (has_unbound_mesh) (void)registerMaterial_(lightusd::next::UsdPrim());
      stats_.material_ms += emscripten_get_now() - material_start_ms;
    }
    if (mesh_merge_) {
      const double optimize_start_ms = emscripten_get_now();
      buildOptimizedOutputs_();
      stats_.optimize_ms = emscripten_get_now() - optimize_start_ms;
    }
    if (!mesh_only_) {
      buildRenderScene_();
      source_primvars_.clear();
      buildAnalyticOutputs_();
      remapNodeMeshIds_();
    }
    loaded_ = true;
    authored_has_inherits_ = authored_has_inherits;
    authored_has_specializes_ = authored_has_specializes;
    return true;
}

int RenderStream::preflightBeginInput(uint32_t size) {
    if (size > max_input_bytes_) {
      error_ = "Input exceeds configured byte limit";
      return 0;
    }
    if (size > (uint32_t{1} << 30)) {
      error_ = "Input exceeds 1 GiB limit";
      return 0;
    }
    const size_t memory_limit = maxMemoryLimitBytes();
    const size_t asset_bytes = providedAssetMemoryBytes();
    if (asset_bytes >= memory_limit ||
        size >= memory_limit - asset_bytes) {
      error_ = "Input and provided assets leave no resident memory for the Stage";
      return 0;
    }
    if (size > remainingMemoryLimitBytes()) {
      error_ = "Input exceeds remaining resident memory allowance";
      return 0;
    }
    error_.clear();
    return 1;
}

int RenderStream::beginBytes(const uint8_t* bytes, uint32_t size) {
    const double input_copy_start_ms = emscripten_get_now();
    if (!bytes && size) {
      error_ = "Invalid input buffer";
      return 0;
    }
    if (!preflightBeginInput(size)) return 0;
    std::string s;
    if (size) s.assign(reinterpret_cast<const char*>(bytes), size);
    pending_input_copy_ms_ = emscripten_get_now() - input_copy_start_ms;
    pending_input_bytes_ = size;
    return beginOwned(std::move(s)) ? 1 : 0;
}

int RenderStream::beginFromLayerDocument(NextLayerDocument& document) {
    const int32_t size = document.exportUsdaSize();
    if (size < 0) {
      error_ = "Layer document could not export USDA";
      return 0;
    }
    const uintptr_t data = document.exportUsdaData();
    if (!data && size) {
      error_ = "Layer document USDA output is missing";
      return 0;
    }
    return beginBytes(reinterpret_cast<const uint8_t*>(data),
                      static_cast<uint32_t>(size));
}

int RenderStream::setMaxInputBytes(uint32_t limit) {
    if (limit == 0 || limit > (uint32_t{1} << 30)) return -1;
    max_input_bytes_ = limit;
    return 0;
}

int RenderStream::setMaxMemoryLimitMB(int32_t limit_mb) {
    if (limit_mb < 1 ||
        static_cast<uint64_t>(limit_mb) >
            (static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) >> 20)) {
      return -1;
    }
    const size_t new_limit = static_cast<size_t>(limit_mb) * (size_t{1} << 20);
    const size_t current_limit = maxMemoryLimitBytes();
    const size_t remaining = remainingMemoryLimitBytes();
    const size_t used = remaining >= current_limit ? 0 : current_limit - remaining;
    if (used >= new_limit) return -1;
    if (loaded_) {
      const size_t input_bytes = stats_.input_bytes;
      const size_t stage_bytes = stageMemoryForBudget_();
      if (input_bytes >= new_limit - used ||
          stage_bytes >= new_limit - used - input_bytes) return -1;
    }
    max_memory_limit_mb_ = limit_mb;
    return 0;
}


size_t RenderStream::stageMemoryForBudget_() const {
    if (mesh_copy_scope_id_ < 0) return stage_.GetMemoryUsage();
    // On-demand opinion fillers can edit arbitrary C++ metadata as well as
    // values. Keep their accounting uncached rather than infer completeness.
    if (stage_.GetRootLayer() && stage_.GetRootLayer()->prim_filler())
      return stage_.GetMemoryUsage();
    for (size_t i = 0; i < stage_.GetSubLayerCount(); ++i)
      if (stage_.GetSubLayer(i) && stage_.GetSubLayer(i)->prim_filler())
        return stage_.GetMemoryUsage();
    const size_t epoch = lu_storage_materialization_epoch();
    if (epoch == 0) return stage_.GetMemoryUsage();
    if (!stage_budget_cache_valid_ || stage_budget_cache_epoch_ != epoch) {
      stage_budget_cache_bytes_ = stage_.GetMemoryUsage();
      stage_budget_cache_epoch_ = epoch;
      stage_budget_cache_valid_ = lu_storage_materialization_epoch() == epoch;
    }
    return stage_budget_cache_bytes_;
}

size_t RenderStream::remainingMemoryLimitBytes() const {
    const size_t limit = maxMemoryLimitBytes();
    size_t used = providedAssetMemoryBytes();
    if (stats_.input_bytes > std::numeric_limits<size_t>::max() - used) return 0;
    used += stats_.input_bytes;
    const size_t stage_bytes = stageMemoryForBudget_();
    if (stage_bytes > std::numeric_limits<size_t>::max() - used) return 0;
    used += stage_bytes;
    if (formatted_material_text_.size() >
        std::numeric_limits<size_t>::max() - used) return 0;
    used += formatted_material_text_.size();
    if (stage_usdc_export_.size() > std::numeric_limits<size_t>::max() - used) return 0;
    used += stage_usdc_export_.size();
    const size_t range_bytes = streamingRangeBookkeepingBytes_();
    if (range_bytes > std::numeric_limits<size_t>::max() - used) return 0;
    used += range_bytes;
    if (used >= limit) return 0;
    return limit - used;
}

int RenderStream::setSphereSubdivisions(int value) {
    if (value < 0 || value > 6) return -1;
    sphere_subdivisions_ = value;
    return 0;
}

int RenderStream::setTargetBoneCount(uint32_t value) {
    if (value == 0 || value > 128) return -1;
    target_bone_count_ = value;
    return 0;
}

int RenderStream::setValueClipSetting(uint8_t field, double value) {
    if (!std::isfinite(value)) return -1;
    switch (field) {
      case 0:
        if (value < 0.0 || value > 10000.0) return -1;
        value_clip_sample_rate_ = static_cast<float>(value);
        return 0;
      case 1:
        if (value != 0.0 && value != 1.0) return -1;
        value_clip_use_time_range_ = value != 0.0;
        return 0;
      case 2: value_clip_start_time_ = value; return 0;
      case 3: value_clip_end_time_ = value; return 0;
      default: return -1;
    }
}

double RenderStream::valueClipSetting(uint8_t field) const {
    switch (field) {
      case 0: return value_clip_sample_rate_;
      case 1: return value_clip_use_time_range_ ? 1.0 : 0.0;
      case 2: return value_clip_start_time_;
      case 3: return value_clip_end_time_;
      default: return std::numeric_limits<double>::quiet_NaN();
    }
}

void RenderStream::end() {
    mesh_copy_scope_id_ = -1;
    invalidateCopyPreparation_();
    loaded_ = false;
    warning_.clear();
    composition_report_ = lightusd::next::pcp::CompositionReport{};
    authored_has_inherits_ = false;
    authored_has_specializes_ = false;
    for (auto& paths : authored_arc_paths_) paths.clear();
    for (bool& present : authored_arc_present_) present = false;
    source_primvars_.clear();
    mesh_view_error_.clear();
    mesh_view_source_id_ = -1;
    render_scene_valid_ = false;
    formatted_material_id_ = -1;
    formatted_material_format_ = 0xff;
    formatted_material_status_ = 0;
    formatted_material_text_.clear();
    render_scene_ = tr::RenderScene();
    render_scene_warnings_.clear();
    stage_usdc_export_.clear();
    meshes_.clear();
    meshes_.shrink_to_fit();
    tangent_requests_.clear();
    tangent_requests_.shrink_to_fit();
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
  }

void RenderStream::reset() {
    end();
    clearVariantOverrides();
    variant_sets_.clear();
    clip_assets_.clear();
    clip_asset_budget_reservations_.clear();
    streaming_assets_.clear();
    asset_resolver_.ClearMemoryAssets();
    imported_asset_bytes_ = 0;
    payload_budget_.reset();
    stats_ = Stats{};
    pending_input_copy_ms_ = 0.0;
    pending_input_bytes_ = 0;
    formatted_light_id_ = -1;
    formatted_light_format_ = 0xff;
    formatted_light_status_ = 0;
    formatted_light_cached_ = false;
    formatted_light_text_.clear();
    error_.clear();
    warning_.clear();
  }

int RenderStream::exportStageUSDC() {
    stage_usdc_export_.clear();
    if (!loaded_) {
      error_ = "No stage loaded";
      return 0;
    }
    const size_t budget = remainingMemoryLimitBytes();
    if (!budget) {
      error_ = "USDC export exceeds remaining resident memory limit";
      return 0;
    }
    lightusd::next::USDCWriteOptions options;
    options.crate_options.max_file_size_bytes = budget;
    options.crate_options.max_memory_bytes = budget;
    const lightusd::next::USDCWriteResult result =
        lightusd::next::WriteUSDCToMemory(stage_usdc_export_, stage_, options);
    if (!result.success) {
      stage_usdc_export_.clear();
      error_ = result.error.empty() ? "USDC export failed" : result.error;
      return 0;
    }
    error_.clear();
    return 1;
}

int RenderStream::stageUSDCSize() const {
    return stage_usdc_export_.size() <=
            static_cast<size_t>(std::numeric_limits<int>::max())
        ? static_cast<int>(stage_usdc_export_.size()) : -1;
}

uintptr_t RenderStream::stageUSDCData() const {
    return stage_usdc_export_.empty()
        ? uintptr_t{0}
        : reinterpret_cast<uintptr_t>(stage_usdc_export_.data());
}

tr::AnimationConfig::ClipStageLoader RenderStream::valueClipLoader_() {
    if (!enable_value_clips_) return {};
    return [this](const std::string& asset_path, tn::Stage* stage,
               std::string* warn, std::string* err) {
          std::string key = NormalizeRenderAssetKey(asset_path);
          auto it = clip_assets_.find(key);
          if (it == clip_assets_.end()) {
            for (const std::string& candidate :
                 tn::AssetResolver::SuffixCandidates(key)) {
              it = clip_assets_.find(candidate);
              if (it != clip_assets_.end()) break;
            }
          }
          if (it == clip_assets_.end()) {
            const tn::ResolvedAsset resolved =
                asset_resolver_.Resolve(key, "memory-root");
            std::vector<uint8_t> bytes;
            std::string read_error;
            if (!resolved.exists || !asset_resolver_.ReadAsset(
                    resolved.resolved_path, &bytes, &read_error)) {
              if (err) *err = "asset was not supplied to RenderStream";
              return false;
            }
            tn::LoadUSDOptions options;
            options.usda_options.parse_options.enable_usda_lazy_arrays = true;
            std::string owned;
            if (!bytes.empty())
              owned.assign(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
            return tn::LoadUSDFromMemoryOwned(std::move(owned), stage, options,
                                              warn, err);
          }
          tn::LoadUSDOptions options;
          options.usda_options.parse_options.enable_usda_lazy_arrays = true;
          std::string bytes = it->second;
          return tn::LoadUSDFromMemoryOwned(std::move(bytes), stage, options,
                                            warn, err);
        };
}

void RenderStream::buildRenderScene_() {
    tr::ConverterConfig cfg;
    cfg.time_code = 0.0;
    cfg.limits.max_resident_bytes = remainingMemoryLimitBytes();
    cfg.mesh.compute_tangents = compute_tangents_;
    cfg.mesh.sphere_subdivisions = sphere_subdivisions_;
    cfg.mesh.tangent_method = tangentMethod_();
    cfg.mesh.enable_bone_reduction = enable_bone_reduction_;
    cfg.mesh.target_bone_count = target_bone_count_;
    cfg.mesh.round_bone_count = round_bone_count_;
    // RenderStream builds one browser-facing mesh lazily from Stage. Keeping a
    // second, complete geometry copy in RenderScene only inflates the wasm
    // heap; retain its material/skinning/animation metadata instead. Keep the
    // compact earcut result used by the lazy mesh builder, plus generated
    // analytic geometry which has no authored Mesh payload to rebuild from.
    cfg.mesh.retain_geometry = false;
    cfg.mesh.retain_custom_primvars = true;
    cfg.mesh.retain_triangulation = true;
    cfg.mesh.retain_analytic_geometry = true;
    cfg.material.load_textures = load_texture_in_native_;
    cfg.material.combine_udim_tiles = combine_udim_tiles_;
    cfg.material.allow_missing_textures = true;
    size_t texture_reservation_bytes = 0;
    if (load_texture_in_native_) {
      cfg.material.custom_texture_loader = [this, &texture_reservation_bytes](
          const std::string& asset_path, tr::TextureImage* out) {
        if (!out || asset_path.empty()) return false;
        std::shared_ptr<const std::vector<uint8_t>> memory_view;
        const auto resolved = asset_resolver_.Resolve(asset_path, "memory-root");
        if (resolved.exists && !resolved.resolved_path.empty()) {
          memory_view = asset_resolver_.GetMemoryAssetView(resolved.resolved_path);
        }
        if (!memory_view) memory_view = asset_resolver_.GetMemoryAssetView(asset_path);
        if (!memory_view) return false;
        const uint8_t* encoded = memory_view->data();
        const size_t encoded_size = memory_view->size();
        if (!encoded || encoded_size == 0) return false;
        const auto info = lightusd::image::GetImageInfoFromMemory(
            encoded, encoded_size, asset_path);
        if (!info) return false;
        size_t pixels = 0;
        size_t estimate = 0;
        if (!lightusd::safe::mul(static_cast<size_t>(info.value().width),
                                 static_cast<size_t>(info.value().height),
                                 &pixels) ||
            !lightusd::safe::mul(pixels, size_t{16}, &estimate)) return false;
        const size_t remaining = remainingMemoryLimitBytes();
        if (texture_reservation_bytes > remaining ||
            estimate > remaining - texture_reservation_bytes) return false;
        auto decoded = lightusd::image::LoadImageFromMemory(
            encoded, encoded_size, asset_path);
        if (!decoded || decoded.value().image.width <= 0 ||
            decoded.value().image.height <= 0 ||
            decoded.value().image.channels <= 0 ||
            decoded.value().image.data.empty()) return false;
        const auto& image = decoded.value().image;
        if (image.data.size() > remaining - texture_reservation_bytes) return false;
        texture_reservation_bytes += estimate;
        out->width = static_cast<uint32_t>(image.width);
        out->height = static_cast<uint32_t>(image.height);
        out->channels = static_cast<uint8_t>(image.channels);
        if (image.format == lightusd::Image::PixelFormat::Float) {
          out->component_type = image.bpp == 16 ? tr::ComponentType::Float16
              : image.bpp == 64 ? tr::ComponentType::Float64
                                : tr::ComponentType::Float32;
        } else if (image.format == lightusd::Image::PixelFormat::Int) {
          out->component_type = image.bpp == 16 ? tr::ComponentType::Int16
              : image.bpp == 32 ? tr::ComponentType::Int32
                                : tr::ComponentType::Int8;
        } else {
          out->component_type = image.bpp == 16 ? tr::ComponentType::UInt16
              : image.bpp == 32 ? tr::ComponentType::UInt32
                                : tr::ComponentType::UInt8;
        }
        if (!out->data.append(image.data.data(), image.data.size())) return false;
        return true;
      };
    }
    cfg.material.render_settings_path = render_settings_path_;
    cfg.asset_resolver = &asset_resolver_;
    cfg.point_instancer.duplicate_meshes = false;
    cfg.animation.clip_stage_loader = valueClipLoader_();
    cfg.animation.bake_value_clips = enable_value_clips_;
    cfg.animation.value_clip_sample_rate = value_clip_sample_rate_;
    cfg.animation.value_clip_use_time_range = value_clip_use_time_range_;
    cfg.animation.value_clip_start_time = value_clip_start_time_;
    cfg.animation.value_clip_end_time = value_clip_end_time_;
    tr::RenderSceneConverter converter(cfg);
    tr::ConvertResult result = converter.Convert(stage_);
    render_scene_ = std::move(result.scene);
    render_scene_warnings_ = result.warnings;
    render_scene_valid_ = result.success;
    if (!result.success && !result.error.empty()) {
      error_ = result.error;
    }
    if (render_scene_valid_ && flatten_render_tree_) flattenRenderTree_();
  }

// Legacy setNativeFlattenRenderTree: replace the hierarchy with one
// "OptimizedRenderRoot" whose direct children are the renderable nodes (in
// depth-first order) carrying their world transform as local transform.
// Node ids stay stable; bypassed Xform nodes remain addressable by id but are
// no longer reachable from the root, and animation channels targeting them
// are detached as in legacy.
void RenderStream::flattenRenderTree_() {
    auto& nodes = render_scene_.nodes;
    const auto keep = [](const tr::SceneNode& node) {
      return node.data_id >= 0 && node.type != tr::NodeType::Xform;
    };
    std::vector<int32_t> kept;
    std::vector<uint8_t> is_kept(nodes.size(), 0);
    std::vector<std::pair<int32_t, size_t>> stack;
    for (auto it = render_scene_.root_nodes.rbegin();
         it != render_scene_.root_nodes.rend(); ++it) stack.emplace_back(*it, 0);
    constexpr size_t kMaxDepth = 1u << 16;
    while (!stack.empty()) {
      const auto [id, depth] = stack.back();
      stack.pop_back();
      if (id < 0 || static_cast<size_t>(id) >= nodes.size() || depth >= kMaxDepth) continue;
      const tr::SceneNode& node = nodes[static_cast<size_t>(id)];
      if (keep(node) && !is_kept[static_cast<size_t>(id)]) {
        is_kept[static_cast<size_t>(id)] = 1;
        kept.push_back(id);
      }
      for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
        stack.emplace_back(*child, depth + 1);
    }
    const int32_t root_id = static_cast<int32_t>(nodes.size());
    for (const int32_t id : kept) {
      tr::SceneNode& node = nodes[static_cast<size_t>(id)];
      node.local_transform = node.world_transform;
      node.children.clear();
      node.parent_id = root_id;
    }
    tr::SceneNode flat_root;
    flat_root.name = "OptimizedRenderRoot";
    flat_root.prim_path = "/OptimizedRenderRoot";
    flat_root.type = tr::NodeType::Xform;
    flat_root.children = std::move(kept);
    nodes.push_back(std::move(flat_root));
    render_scene_.node_by_path["/OptimizedRenderRoot"] = root_id;
    render_scene_.root_nodes.assign(1, root_id);
    for (tr::AnimationClip& clip : render_scene_.animations) {
      for (tr::AnimationChannel& channel : clip.channels) {
        if (channel.target_node >= 0 &&
            (static_cast<size_t>(channel.target_node) >= is_kept.size() ||
             !is_kept[static_cast<size_t>(channel.target_node)]))
          channel.target_node = -1;
      }
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
        // PrimVariantSetData itself.
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
