// SPDX-License-Identifier: Apache-2.0
#include "scene_loader.hh"
#include "scene_optimize.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

#include "composition.hh"
#include "mesh_build.hh"
#include "skinning.hh"
#include "lightusd.hh"
#include "tydra/render-data-converter.hh"

namespace lusdview {

namespace {

std::string DirName(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return path.substr(0, 1);
  }
  return path.substr(0, slash);
}

bool HasUsdzExtension(const std::string& path) {
  if (path.size() < 5) return false;
  std::string ext = path.substr(path.size() - 5);
  for (char& c : ext) c = static_cast<char>(std::tolower(c));
  return ext == ".usdz";
}

bool Cancelled(LoadControl* ctrl) { return ctrl && ctrl->cancel.load(); }

// True when the layer carries no composition arcs, so plain LayerToStage (or
// the legacy direct Stage load) yields the same result as full composition.
bool LayerHasCompositionArcs(const lightusd::Layer& layer) {
  return !layer.metas().subLayers.empty() ||
         layer.check_unresolved_references() ||
         layer.check_unresolved_payload() || layer.check_unresolved_inherits() ||
         layer.check_unresolved_variant() ||
         layer.check_unresolved_specializes();
}

// Compose `src` (post-sublayer) to a fixed point in LIVRPS order, honoring the
// payload policy in `opts`. Deferred payloads are appended to `deferred`.
// Pattern follows ComposeLayerToFixedPoint() in src/usdz-convert.cc, plus the
// PayloadCompositionOptions::load_policy hook for lazy payloads.
bool ComposeToFixedPoint(lightusd::AssetResolutionResolver& resolver,
                         lightusd::Layer&& src, const LoadOptions& opts,
                         lightusd::Layer* composed,
                         std::vector<DeferredArc>* deferred,
                         std::string* warn, std::string* err,
                         LoadControl* ctrl) {
  // One whitelist drives both arc types: a whitelisted prim path loads its
  // payloads and (when deferred) its references.
  const std::set<std::string>* whitelist =
      (opts.payloadPolicy == PayloadPolicy::Whitelist) ? &opts.payloadWhitelist
                                                       : nullptr;

  lightusd::PayloadCompositionOptions pl_opts;
  pl_opts.allow_parent_relative_paths = opts.allowParentRelativePaths;
  if (opts.payloadPolicy != PayloadPolicy::LoadAll) {
    pl_opts.load_policy = [whitelist, deferred](
                              const lightusd::Path& prim_path,
                              const lightusd::Payload& payload) -> bool {
      const std::string p = prim_path.full_path_name();
      if (whitelist && whitelist->count(p)) {
        return true;
      }
      deferred->push_back({p, payload.asset_path.GetAssetPath(), "payload"});
      return false;
    };
  }

  lightusd::ReferencesCompositionOptions ref_opts;
  ref_opts.allow_parent_relative_paths = opts.allowParentRelativePaths;
  if (opts.deferReferences) {
    ref_opts.load_policy = [whitelist, deferred](
                               const lightusd::Path& prim_path,
                               const lightusd::Reference& reference) -> bool {
      const std::string p = prim_path.full_path_name();
      if (whitelist && whitelist->count(p)) {
        return true;
      }
      deferred->push_back({p, reference.asset_path.GetAssetPath(), "reference"});
      return false;
    };
  }

  lightusd::Layer work = std::move(src);

  constexpr int kMaxIteration = 64;
  for (int i = 0; i < kMaxIteration; i++) {
    if (Cancelled(ctrl)) {
      if (err) *err = "Load cancelled.";
      return false;
    }

    bool has_unresolved = false;

    if (work.check_unresolved_references()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositeReferences(resolver, work, &tmp, warn, err,
                                         ref_opts)) {
        return false;
      }
      work = std::move(tmp);
    }

    if (work.check_unresolved_payload()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositePayload(resolver, work, &tmp, warn, err,
                                      pl_opts)) {
        return false;
      }
      work = std::move(tmp);
    }

    if (work.check_unresolved_inherits()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositeInherits(work, &tmp, warn, err)) {
        return false;
      }
      work = std::move(tmp);
    }

    if (work.check_unresolved_variant()) {
      has_unresolved = true;  // not done yet either way
      // Defer variant resolution until references & payloads have settled
      // (AOUSD Core Spec 10.3.2.5). A variant's CONTENT often arrives through a
      // reference/payload (e.g. ALab: a component references a geo fragment
      // whose variant gates the mesh payload); resolving the variant before
      // those arcs compose selects an empty/stale option and the geometry is
      // lost. Mirrors the lusdcat / feat-variant-payload-chain flatten driver.
      const bool arcs_settled = !work.check_unresolved_references() &&
                                !work.check_unresolved_payload();
      if (arcs_settled) {
        lightusd::Layer tmp;
        if (!lightusd::CompositeVariant(work, &tmp, warn, err)) {
          return false;
        }
        work = std::move(tmp);
      }
      // else: loop again to settle refs/payloads first.
    }

    if (work.check_unresolved_specializes()) {
      has_unresolved = true;
      lightusd::Layer tmp;
      if (!lightusd::CompositeSpecializes(work, &tmp, warn, err)) {
        return false;
      }
      work = std::move(tmp);
    }

    if (!has_unresolved) {
      *composed = std::move(work);
      return true;
    }
  }

  if (err) {
    (*err) += "Composition did not converge before the iteration limit.";
  }
  return false;
}

// Build the Stage from `out->comp.rootLayer` (post-sublayer snapshot) with the
// given payload policy, refreshing comp.deferred/loadedPayloads.
bool ComposeStage(const LoadOptions& opts, LoadedScene* out,
                  LoadControl* ctrl) {
  lightusd::AssetResolutionResolver resolver;
  resolver.set_search_paths(out->comp.searchPaths);
  if (out->comp.usdzAsset &&
      !lightusd::SetupUSDZAssetResolution(resolver, out->comp.usdzAsset.get())) {
    out->err += "Failed to configure USDZ composition asset resolution.\n";
    return false;
  }

  lightusd::Layer work = *out->comp.rootLayer;  // compose from a copy

  // Apply variant selection overrides before composition.
  if (!opts.variantOverrides.empty()) {
    lightusd::VariantSelectorMap vsmap;
    for (const auto& [primPath, selections] : opts.variantOverrides) {
      lightusd::Path path(primPath, "");
      lightusd::VariantSelector sel;
      sel.selection = selections.empty() ? "" : selections.begin()->second;
      sel.vsmap = selections;
      vsmap[path] = sel;
    }
    lightusd::Layer variantLayer;
    std::string vwarn, verr;
    if (lightusd::ApplyVariantSelector(work, vsmap, &variantLayer, &vwarn, &verr)) {
      work = std::move(variantLayer);
      if (!vwarn.empty()) out->warn += vwarn;
    } else {
      if (!verr.empty()) out->warn += "Variant override failed: " + verr + "\n";
    }
  }

  out->comp.deferred.clear();
  lightusd::Layer composed;
  if (!ComposeToFixedPoint(resolver, std::move(work), opts, &composed,
                           &out->comp.deferred, &out->warn, &out->err, ctrl)) {
    return false;
  }
  out->comp.loadedPayloads = (opts.payloadPolicy == PayloadPolicy::Whitelist)
                                 ? opts.payloadWhitelist
                                 : std::set<std::string>{};
  out->comp.composed = true;

  return lightusd::LayerToStage(std::move(composed), &out->stage, &out->warn,
                                &out->err);
}

// Acquire `out->stage` without composition: mmap zero-copy load of the root
// layer only (legacy path; also used for .usdz).
bool LoadStageDirect(const std::string& path, LoadedScene* out) {
  lightusd::USDLoadOptions opts;
  opts.mmap_zero_copy = true;

  auto mh = std::shared_ptr<lightusd::io::MMapFileHandle>(
      new lightusd::io::MMapFileHandle(),
      [](lightusd::io::MMapFileHandle* h) {
        std::string e;
        lightusd::io::UnmapFile(*h, &e);
        delete h;
      });
  std::string merr;
  const bool mapped = lightusd::io::MMapFile(path, mh.get(), /*writable=*/false, &merr) &&
                      mh->addr && mh->size > 0;

  bool ok = false;
  if (mapped) {
    out->mmap = mh;  // keep the mapping alive alongside the Stage
    ok = lightusd::LoadUSDFromMemory(mh->addr, static_cast<size_t>(mh->size), path,
                                     &out->stage, &out->warn, &out->err, opts);
  } else {
    // Fall back to a regular (copying) file load. Disable zero-copy: it requires
    // a persistent mapping which we don't have on this path.
    opts.mmap_zero_copy = false;
    ok = lightusd::LoadUSDFromFile(path, &out->stage, &out->warn, &out->err, opts);
  }
  if (!ok && out->err.empty()) {
    out->err = "Failed to load USD file: " + path;
  }
  return ok;
}

// Acquire `out->stage` with composition. USDZ packages retain their archive
// backing so internal layers remain resolvable during deferred recomposition.
// Falls back to plain LayerToStage when the file has no composition arcs and
// retains the post-sublayer layer in out->comp for later payload recompose.
bool LoadStageComposed(const std::string& path, const LoadOptions& opts,
                       LoadedScene* out, LoadControl* ctrl) {
  lightusd::Layer root;
  bool rootOk = false;
  if (HasUsdzExtension(path)) {
    out->comp.usdzAsset = std::make_shared<lightusd::USDZAsset>();
    constexpr size_t kMiB = 1024u * 1024u;
    const size_t maxArchiveMb = opts.maxMemoryBytes == 0
                                    ? 65536u
                                    : opts.maxMemoryBytes / kMiB +
                                          (opts.maxMemoryBytes % kMiB != 0);
    rootOk = lightusd::ReadUSDZAssetInfoFromFile(
        path, out->comp.usdzAsset.get(), &out->warn, &out->err,
        std::max<size_t>(maxArchiveMb, 1u));
    if (rootOk) {
      const auto it = out->comp.usdzAsset->asset_map.find(
          out->comp.usdzAsset->root_asset_name);
      if (it == out->comp.usdzAsset->asset_map.end() ||
          it->second.second < it->second.first ||
          out->comp.usdzAsset->data.empty()) {
        out->err += "USDZ root entry is missing or invalid.\n";
        rootOk = false;
      } else {
        const uint8_t* bytes = out->comp.usdzAsset->data.data();
        rootOk = lightusd::LoadLayerFromMemory(
            bytes + it->second.first, it->second.second - it->second.first,
            out->comp.usdzAsset->root_asset_name, &root, &out->warn, &out->err);
      }
    }
  } else {
    rootOk = lightusd::LoadLayerFromFile(path, &root, &out->warn, &out->err);
  }
  if (!rootOk) {
    if (out->err.empty()) {
      out->err = "Failed to load USD layer: " + path;
    }
    return false;
  }

  if (Cancelled(ctrl)) {
    out->err = "Load cancelled.";
    return false;
  }

  if (!LayerHasCompositionArcs(root)) {
    // For files without arcs, keep the direct parser path. LayerToStage currently
    // drops some less-common concrete schemas (e.g. NurbsPatch), while direct
    // Stage loading preserves the full hierarchy and also retains zero-copy USDC
    // storage through out->mmap.
    out->warn.clear();
    out->err.clear();
    // The direct path owns an mmap and can resolve embedded assets from it;
    // release the copied archive retained only for composition inspection.
    out->comp.usdzAsset.reset();
    return LoadStageDirect(path, out);
  }

  out->comp.searchPaths = {DirName(path)};

  lightusd::AssetResolutionResolver resolver;
  resolver.set_search_paths(out->comp.searchPaths);
  if (out->comp.usdzAsset) {
    lightusd::SetupUSDZAssetResolution(resolver, out->comp.usdzAsset.get());
  }

  // Flatten sublayers first, then snapshot: payload recompose restarts from
  // this layer (CompositePayload strips payload metadata even for deferred
  // arcs, so the composed result alone cannot load payloads later).
  if (!root.metas().subLayers.empty()) {
    lightusd::SublayersCompositionOptions sl_opts;
    sl_opts.allow_parent_relative_paths = opts.allowParentRelativePaths;
    lightusd::Layer tmp;
    if (!lightusd::CompositeSublayers(resolver, root, &tmp, &out->warn,
                                      &out->err, sl_opts)) {
      return false;
    }
    root = std::move(tmp);
  }
  out->comp.rootLayer = std::make_shared<lightusd::Layer>(std::move(root));

  return ComposeStage(opts, out, ctrl);
}

// Convert out->stage to RenderScene + DrawScene (shared by load and
// recompose). Returns false with *err set on failure. Reads `stage` (const) +
// `mmap`/`path` for USDZ asset resolution; writes geometry to `draw` and the
// full RenderScene to `render`. `timecode` selects the evaluation time (NaN =
// static default). `loadTextures=false` skips texture image decode (used by
// playback re-evaluation, which keeps the initial load's textures).
bool ConvertStageToSceneImpl(const lightusd::Stage& stage,
                             const std::string& path,
                             const std::shared_ptr<lightusd::io::MMapFileHandle>& mmap,
                             const std::shared_ptr<lightusd::USDZAsset>& retainedUsdzAsset,
                             double timecode, bool rtPath, bool loadTextures,
                             const TextureRuntimeOptions& textureOptions,
                             int subdivisionLevel,
                             const std::map<std::string, int>& subdivisionPrimLevels,
                             bool allowParentRelativePaths,
                             lightusd::tydra::RenderScene* render, DrawScene* draw,
                             std::string* warn, std::string* err,
                             LoadControl* ctrl) {
  lightusd::tydra::RenderSceneConverterEnv env(stage);
  env.usd_filename = path;
  env.set_search_paths({DirName(path)});
  // Honor the caller's parent-relative-path policy for texture/light asset
  // resolution (the tydra asset resolver rejects '..' paths by default).
  env.asset_resolver.set_allow_parent_relative_paths(allowParentRelativePaths);
  env.timecode = timecode;
  env.scene_config.load_texture_assets = loadTextures;
  env.scene_config.keep_compressed_textures = textureOptions.keepCompressed;
  // Collapse materials that differ only by authored path/name before they are
  // converted into DrawMaterialCPU records. Mesh merging remains disabled here:
  // it bakes away the per-prim identity used by picking and hide/isolate.
  env.scene_config.dedup_materials_by_texture_identity = true;

  // USDZ assets (textures, audio, ...) live *inside* the .usdz archive. Register
  // the archive's internal asset map with the resolver so embedded textures
  // resolve; without this only assets that happen to exist on disk next to the
  // .usdz would load. Composed packages reuse their retained shared backing;
  // direct loads use a temporary map backed by the live mmap or a local copy.
  // In every case the asset outlives the synchronous conversion below.
  lightusd::USDZAsset usdzAsset;
  if (loadTextures && HasUsdzExtension(path)) {
    std::string uwarn, uerr;
    const lightusd::USDZAsset* resolverAsset = retainedUsdzAsset.get();
    bool gotInfo = resolverAsset != nullptr;
    if (!gotInfo && mmap) {
      // Zero-copy: reference the mmap (kept alive by the caller through the
      // conversion below).
      gotInfo = lightusd::ReadUSDZAssetInfoFromMemory(
          mmap->addr, static_cast<size_t>(mmap->size),
          /*asset_on_memory=*/true, &usdzAsset, &uwarn, &uerr);
      resolverAsset = gotInfo ? &usdzAsset : nullptr;
    } else if (!gotInfo) {
      gotInfo = lightusd::ReadUSDZAssetInfoFromFile(path, &usdzAsset, &uwarn, &uerr);
      resolverAsset = gotInfo ? &usdzAsset : nullptr;
    }
    if (gotInfo) {
      if (!lightusd::SetupUSDZAssetResolution(env.asset_resolver, resolverAsset)) {
        *warn += "Failed to set up USDZ asset resolution; embedded textures "
                 "may not load.\n";
      }
    } else {
      *warn += "Failed to read USDZ asset info" +
               (uerr.empty() ? std::string() : ": " + uerr) + "\n";
    }
  }

  // Configure for a single-index OpenGL/Vulkan renderer.
  auto& mc = env.mesh_config;
  mc.triangulate = true;
  // Ray tracing only needs triangulated positions + a uint32 index buffer for the
  // BLAS, so skip the rasterization-only single-index dedup on the RT path.
  mc.build_vertex_indices = !rtPath;
  mc.compute_normals = true;
  // Keep normals/tangents as plain float3. Current shaders only consume normals,
  // but tangents/binormals are preserved in DrawMeshCPU for the later material
  // evaluator (normal maps, anisotropy, MaterialX tangent inputs).
  mc.normal_storage =
      lightusd::tydra::MeshConverterConfig::NormalStorageFormat::Float3;
  mc.tangent_storage =
      lightusd::tydra::MeshConverterConfig::TangentStorageFormat::Float3;
  mc.compute_tangents_and_binormals = true;
  mc.compute_tangents_only_with_normal_map = true;
  // Expose secondary UV sets (e.g. primvars:st1) for the multi-UV debug AOV even
  // when no material shader references them.
  mc.extract_all_texcoords = true;
  // Keep per-face triangle counts so each triangle can be mapped back to its
  // source USD face (SourceFaceId debug AOV).
  mc.keep_triangulation_intermediates = true;
  mc.subdivision_level = std::max(0, subdivisionLevel);
  for (const auto& kv : subdivisionPrimLevels) {
    if (!kv.first.empty()) {
      mc.subdivision_prim_levels[kv.first] = std::max(0, kv.second);
    }
  }

  // Keep texels 8-bit (avoids float-image conversion). Sparse UDIM is the
  // default for large scenes; atlas mode remains available for older backends.
  auto& matc = env.material_config;
  matc.preserve_texel_bitdepth = true;
  matc.linearize_color_space = false;
  matc.combine_udim_tiles =
      textureOptions.udimMode == UdimMode::Atlas;
  if (textureOptions.maxTextureSize > 0) {
    matc.udim_max_atlas_size = textureOptions.maxTextureSize;
  }
  // Graceful skip for missing/failed textures (these are already defaults).
  matc.allow_texture_load_failure = true;
  matc.allow_missing_asset = true;
  // Renderer-parity policy: a material that fails to convert (unknown shader,
  // unresolvable network) should NOT sink the whole load. Substitute the default
  // material so the geometry still renders; tydra records a "using default
  // material" warning that the app's load summary reports as a degraded_material,
  // which the smoke harness fails on. Keeps degraded scenes loadable while still
  // flagging the regression.
  matc.assign_default_material = true;

  // RenderSceneConverter is non-copyable / non-movable: keep it local.
  lightusd::tydra::RenderSceneConverter converter;

  // Cancellation + progress + conversion time budget (worker-thread friendly).
  bool budgetHit = false;
  if (ctrl) {
    const auto start = std::chrono::steady_clock::now();
    converter.SetDetailedProgressCallback(
        [ctrl, start, &budgetHit](const lightusd::tydra::DetailedProgressInfo& info,
                                   void*) -> bool {
          ctrl->stage.store(static_cast<int>(info.stage));
          ctrl->meshesDone.store(static_cast<long long>(info.meshes_processed));
          ctrl->meshesTotal.store(static_cast<long long>(info.meshes_total));
          if (ctrl->cancel.load()) return false;
          if (ctrl->convertTimeBudgetSec > 0.0) {
            const double el =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                    .count();
            if (el > ctrl->convertTimeBudgetSec) {
              budgetHit = true;
              return false;
            }
          }
          return true;  // continue
        },
        nullptr);
  }

  // Streaming convert: builds `draw` incrementally as meshes/materials/textures
  // are produced (and fully populates *render). Falls back to the monolithic
  // path if no DrawScene sink was requested.
  const bool converted =
      draw ? BuildDrawSceneStreaming(converter, env, render, draw, ctrl,
                                     textureOptions)
           : converter.ConvertToRenderScene(env, render);
  if (!converted) {
    if (ctrl && ctrl->cancel.load()) {
      *err = "Load cancelled.";
    } else if (budgetHit) {
      *err = "Aborted: conversion exceeded the time budget (scene too large).";
    } else {
      *err = converter.GetError();
    }
    if (!converter.GetWarning().empty()) {
      *warn += converter.GetWarning();
    }
    if (err->empty()) {
      *err = "RenderScene conversion failed for: " + path;
    }
    return false;
  }
  if (!converter.GetWarning().empty()) {
    *warn += converter.GetWarning();
  }
  return true;
}

// Convert out->stage to RenderScene + DrawScene at `timecode` (out->stage,
// out->mmap, out->render, out->warn/err are the load targets).
bool ConvertStageToScene(const std::string& path, double timecode,
                         const TextureRuntimeOptions& textureOptions,
                         int subdivisionLevel,
                         const std::map<std::string, int>& subdivisionPrimLevels,
                         LoadedScene* out, DrawScene* draw, bool rtPath,
                         LoadControl* ctrl) {
  // When loading at a concrete time code (e.g. --time for a headless screenshot
  // of an animated frame), use the two-phase convert→deform→pack path so
  // skeletal/blendshape geometry is posed. The default (NaN) interactive load
  // keeps the streaming path for progressive UI (rest pose; playback deforms
  // via RenderSceneAtTime).
  if (draw && std::isfinite(timecode)) {
    if (!ConvertStageToSceneImpl(out->stage, path, out->mmap,
                                 out->comp.usdzAsset, timecode, rtPath,
                                 /*loadTextures=*/true, textureOptions,
                                 subdivisionLevel, subdivisionPrimLevels,
                                 out->comp.allowParentRelativePaths,
                                 &out->render,
                                 /*draw=*/nullptr, &out->warn, &out->err,
                                 ctrl)) {
      return false;
    }
    DeformSkinnedMeshes(out->stage, out->render, timecode);
    BuildDrawScene(out->render, draw, ctrl, &out->stage, textureOptions);
    ApplyMeshPurposes(out->stage, draw);
    if (draw) CanonicalizeDrawMaterials(draw);
    out->ok = true;
    return true;
  }
  if (!ConvertStageToSceneImpl(out->stage, path, out->mmap,
                               out->comp.usdzAsset, timecode, rtPath,
                               /*loadTextures=*/true, textureOptions,
                               subdivisionLevel, subdivisionPrimLevels,
                               out->comp.allowParentRelativePaths,
                               &out->render, draw,
                               &out->warn, &out->err, ctrl)) {
    return false;
  }
  if (draw) {
    ApplyMeshPurposes(out->stage, draw);
    CanonicalizeDrawMaterials(draw);
  }
  out->ok = true;
  return true;
}

}  // namespace

bool LoadUSD(const std::string& path, const LoadOptions& opts, LoadedScene* out,
             DrawScene* draw, bool rtPath, LoadControl* ctrl) {
  out->filepath = path;
  out->ok = false;
  out->warn.clear();
  out->err.clear();
  if (draw) *draw = DrawScene{};

  if (Cancelled(ctrl)) {
    out->err = "Load cancelled.";
    return false;
  }

  const bool compose = opts.composition;

  bool ok = false;
  if (compose) {
    ok = LoadStageComposed(path, opts, out, ctrl);
  } else {
    ok = LoadStageDirect(path, out);
  }
  if (!ok) {
    return false;
  }

  // Retain the '..' policy for the RenderScene conversion (texture/light asset
  // resolution). Set here so BOTH the composed and direct (single-file, .usdz)
  // load branches carry it — not just files that happen to have composition arcs.
  out->comp.allowParentRelativePaths = opts.allowParentRelativePaths;
  out->subdivisionLevel = std::max(0, opts.subdivisionLevel);
  out->subdivisionPrimLevels = opts.subdivisionPrimLevels;
  return ConvertStageToScene(path, opts.timecode, opts.textureOptions,
                             out->subdivisionLevel, out->subdivisionPrimLevels,
                             out, draw, rtPath, ctrl);
}

bool RecomposeWithPayloads(const std::string& path, const CompositionInfo& prev,
                           const LoadOptions& opts, LoadedScene* out,
                           DrawScene* draw, bool rtPath, LoadControl* ctrl) {
  out->filepath = path;
  out->ok = false;
  out->warn.clear();
  out->err.clear();
  if (draw) *draw = DrawScene{};

  if (!prev.rootLayer) {
    out->err = "No retained composition layer to recompose from.";
    return false;
  }

  if (Cancelled(ctrl)) {
    out->err = "Load cancelled.";
    return false;
  }

  out->comp.rootLayer = prev.rootLayer;
  out->comp.searchPaths = prev.searchPaths;
  out->comp.usdzAsset = prev.usdzAsset;
  out->comp.allowParentRelativePaths = opts.allowParentRelativePaths;

  if (!ComposeStage(opts, out, ctrl)) {
    return false;
  }

  out->subdivisionLevel = std::max(0, opts.subdivisionLevel);
  out->subdivisionPrimLevels = opts.subdivisionPrimLevels;
  return ConvertStageToScene(path, opts.timecode, opts.textureOptions,
                             out->subdivisionLevel, out->subdivisionPrimLevels,
                             out, draw, rtPath, ctrl);
}

bool RenderSceneAtTime(const LoadedScene& src, double timecode, bool rtPath,
                       DrawScene* draw, std::string* warn, std::string* err,
                       LoadControl* ctrl,
                       const std::unordered_map<std::string, float>* blendOverride,
                       RestSceneCache* restCache) {
  if (draw) *draw = DrawScene{};
  // Convert the scene at `timecode` WITHOUT packing (draw=nullptr): node
  // transforms / time-sampled points / value clips are resolved by Tydra, but
  // skeletal skinning and blendshapes are not. Deform the rest-pose meshes on
  // the CPU, then pack the posed geometry. Textures are not re-decoded
  // (loadTextures=false), so the caller keeps the initial load's textures.
  //
  // `scratch` ends up holding the DEFORMED geometry (DeformSkinnedMeshes mutates
  // points in place). The rest cache stores a pre-deform copy keyed by timecode:
  // a same-timecode hit (e.g. dragging a blendshape weight while paused) reuses it
  // and skips the heavy ConvertStageToSceneImpl.
  lightusd::tydra::RenderScene scratch;
  const bool cacheHit =
      restCache && restCache->valid && restCache->timecode == timecode;
  if (cacheHit) {
    scratch = restCache->scene;  // copy rest (cheap vs re-converting the stage)
  } else {
    if (!ConvertStageToSceneImpl(src.stage, src.filepath, src.mmap,
                                 src.comp.usdzAsset, timecode, rtPath,
                                 /*loadTextures=*/false,
                                 TextureRuntimeOptions{}, src.subdivisionLevel,
                                 src.subdivisionPrimLevels,
                                 src.comp.allowParentRelativePaths, &scratch,
                                 /*draw=*/nullptr, warn, err, ctrl)) {
      return false;
    }
    if (restCache) {  // store the rest scene (before deform) for later reuse
      restCache->scene = scratch;
      restCache->timecode = timecode;
      restCache->valid = true;
    }
  }
  DeformSkinnedMeshes(src.stage, scratch, timecode, blendOverride);
  BuildDrawScene(scratch, draw, ctrl, &src.stage);
  ApplyMeshPurposes(src.stage, draw);
  CanonicalizeDrawMaterials(draw);
  return true;
}

}  // namespace lusdview
