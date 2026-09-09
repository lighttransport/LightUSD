// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// TODO:
//   - [ ] Subdivision surface to polygon mesh conversion.
//     - [ ] Correctly handle primvar with 'vertex' interpolation(Use the basis
//     function of subd surface)
//   - [ ] Support material binding collection(Collection API)
//   - [ ] Support multiple skel animation
//   https://github.com/PixarAnimationStudios/OpenUSD/issues/2246
//   - [ ] Adjust normal vector computation with handness?
//   - [ ] Node xform animation
//   - [ ] Better build of index buffer
//     - [ ] Preserve the order of 'points' variable(mesh.points, Skin
//     indices/weights, BlendShape points, ...) as much as possible.
//     - Implement spatial hash
//
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "color-management.hh"
#include "common-macros.inc"
#include "lightusd.hh"
#include "pprint-enum.hh"
#include "tiny-format.hh"
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/render-data-material-internal.hh"
#include "tydra/scene-access.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "value-types.hh"

namespace lightusd {

namespace tydra {

namespace {

using TydraPerfClock = std::chrono::steady_clock;

static double ElapsedMs(const TydraPerfClock::time_point &start) {
  return double(std::chrono::duration_cast<std::chrono::microseconds>(
                    TydraPerfClock::now() - start)
                    .count()) *
         0.001;
}

static double NsToMs(const uint64_t ns) {
  return double(ns) * 0.000001;
}


}  // namespace

namespace {
// Clears the converter's streaming-sink pointer on every exit path of
// ConvertToRenderSceneImpl (works without exceptions). Declared at the top of
// the function so all `return`s (incl. PUSH_ERROR_AND_RETURN) run the dtor.
struct SinkScopeGuard {
  const RenderSceneSink **slot;
  ~SinkScopeGuard() { *slot = nullptr; }
};
}  // namespace

bool RenderSceneConverter::ConvertToRenderScene(
    const RenderSceneConverterEnv &env, RenderScene *scene) {
  return ConvertToRenderSceneImpl(env, scene, /* sink */ nullptr);
}


bool RenderSceneConverter::ConvertToRenderSceneStreaming(
    const RenderSceneConverterEnv &env, const RenderSceneSink &sink,
    RenderScene *scene) {
  return ConvertToRenderSceneImpl(env, scene, &sink);
}

bool RenderSceneConverter::ConvertToRenderSceneImpl(
    const RenderSceneConverterEnv &env, RenderScene *scene,
    const RenderSceneSink *sink) {
  _sink = sink;
  SinkScopeGuard _sink_guard{&_sink};

  if (!scene) {
    PUSH_ERROR_AND_RETURN("nullptr for RenderScene argument.");
  }

  color_management::RenderingColorConfig rendering_color;
  std::string color_warning;
  if (!color_management::ResolveRenderingColorConfig(
          env.stage, env.material_config.render_settings_path,
          &rendering_color, &color_warning)) {
    PUSH_ERROR_AND_RETURN("Failed to resolve rendering color configuration.");
  }
  _working_color_space = rendering_color.working_space;
  if (!color_warning.empty()) PushWarn(color_warning);

  const auto total_start = TydraPerfClock::now();
  double count_ms = 0.0;
  double skel_map_ms = 0.0;
  double xform_ms = 0.0;
  double visit_prims_ms = 0.0;
  double standalone_skel_ms = 0.0;
  double skel_anim_ms = 0.0;
  double hierarchy_ms = 0.0;
  double xform_anim_ms = 0.0;
  double merge_ms = 0.0;
  double instance_map_ms = 0.0;
  double stage_meta_ms = 0.0;

  // Reset progress state
  _progress_info = DetailedProgressInfo{};
  _timing_info.clear();

  // Clear lookup caches from previous conversion
  _skelPathToIndex.clear();
  _animPathToIndex.clear();
  _skelNameToIndexCache.clear();
  _skelRootToSkeleton.clear();
  _uvNameCache.clear();
  _materialBindingCache.clear();
  _materialSourceSignatureCache.clear();
  _value_clip_layer_cache.clear();
  _value_clip_stage_cache.clear();
  ResetConnectionResolveCache(env.stage);

  // Report initial progress
  if (!CallProgressCallback(0.0f)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  // Count meshes and materials before conversion for accurate progress reporting
  // Single-pass traversal: walk the stage tree once and classify prims by type_id
  DCOUT("[Tydra] Counting primitives...");
  PathPrimMap<GeomMesh> meshPrimMap;
  PathPrimMap<GeomCube> cubePrimMap;
  PathPrimMap<GeomSphere> spherePrimMap;
  PathPrimMap<GeomCylinder> cylinderPrimMap;
  PathPrimMap<GeomCone> conePrimMap;
  PathPrimMap<GeomCapsule> capsulePrimMap;
  PathPrimMap<GeomPlane> planePrimMap;
  PathPrimMap<Material> materialPrimMap;
  PathPrimMap<Skeleton> allSkeletons;
  PathPrimMap<SkelRoot> allSkelRoots;
  PathPrimMap<SkelAnimation> allAnimations;
  PathPrimMap<GeomPointInstancer> pointInstancerPrimMap;

  {
    const auto phase_start = TydraPerfClock::now();
    // Iterative stack-based traversal visiting each prim exactly once
    struct StackEntry {
      const Prim *parent;
      size_t child_idx;
      size_t parent_path_len;
    };
    std::vector<StackEntry> stack;
    stack.reserve(64);
    std::string path_buf;
    path_buf.reserve(256);

    auto classifyPrim = [&](const Prim &prim) {
      switch (prim.type_id()) {
        case value::TYPE_ID_GEOM_MESH:
          if (const auto *p = prim.as<GeomMesh>()) meshPrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_GEOM_CUBE:
          if (const auto *p = prim.as<GeomCube>()) cubePrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_GEOM_SPHERE:
          if (const auto *p = prim.as<GeomSphere>()) spherePrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_GEOM_CYLINDER:
          if (const auto *p = prim.as<GeomCylinder>()) cylinderPrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_GEOM_CONE:
          if (const auto *p = prim.as<GeomCone>()) conePrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_GEOM_CAPSULE:
          if (const auto *p = prim.as<GeomCapsule>()) capsulePrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_GEOM_PLANE:
          if (const auto *p = prim.as<GeomPlane>()) planePrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_MATERIAL:
          if (const auto *p = prim.as<Material>()) materialPrimMap[path_buf] = p;
          break;
        case value::TYPE_ID_SKELETON:
          if (const auto *p = prim.as<Skeleton>()) allSkeletons[path_buf] = p;
          break;
        case value::TYPE_ID_SKEL_ROOT:
          if (const auto *p = prim.as<SkelRoot>()) allSkelRoots[path_buf] = p;
          break;
        case value::TYPE_ID_SKELANIMATION:
          if (const auto *p = prim.as<SkelAnimation>()) allAnimations[path_buf] = p;
          break;
        case value::TYPE_ID_GEOM_POINT_INSTANCER:
          if (const auto *p = prim.as<GeomPointInstancer>())
            pointInstancerPrimMap[path_buf] = p;
          break;
        default:
          break;
      }
    };

    for (const auto &root_prim : env.stage.root_prims()) {
      path_buf = "/" + root_prim.local_path().full_path_name();
      classifyPrim(root_prim);

      if (!root_prim.children().empty()) {
        stack.push_back({&root_prim, 0, 0});
      }

      size_t iter = 0;
      while (!stack.empty()) {
        if (iter++ >= kMaxDefaultTraversalLimit) {
          PUSH_WARN("Prim traversal exceeded max iteration limit during pre-processing.");
          break;
        }
        auto &top = stack.back();
        if (top.child_idx >= top.parent->children().size()) {
          path_buf.resize(top.parent_path_len);
          stack.pop_back();
          continue;
        }

        const Prim &child = top.parent->children()[top.child_idx];
        ++top.child_idx;

        size_t cur_len = path_buf.size();
        path_buf += "/";
        path_buf += child.local_path().full_path_name();

        classifyPrim(child);

        if (!child.children().empty()) {
          stack.push_back({&child, 0, cur_len});
        } else {
          path_buf.resize(cur_len);
        }
      }
    }
    count_ms = ElapsedMs(phase_start);
  }
  DCOUT("[Tydra] Pre-discovered " << allSkeletons.size() << " skeletons, "
        << allSkelRoots.size() << " skelroots, " << allAnimations.size() << " animations");

  {
    const auto phase_start = TydraPerfClock::now();
    SkelRootSkeletonResolver::BuildMap(allSkeletons, allSkelRoots,
                                       &_skelRootToSkeleton);
    skel_map_ms = ElapsedMs(phase_start);
  }
  DCOUT("Precomputed SkelRoot->Skeleton entries: " << _skelRootToSkeleton.size());

  // Total meshes includes GeomMesh and all parametric primitives (all converted to meshes)
  const size_t total_meshes = meshPrimMap.size() + cubePrimMap.size() + spherePrimMap.size() +
                              cylinderPrimMap.size() + conePrimMap.size() +
                              capsulePrimMap.size() + planePrimMap.size();
  const size_t total_materials = materialPrimMap.size();
  DCOUT("[Tydra] Found " << total_meshes << " meshes ("
        << meshPrimMap.size() << " mesh, " << cubePrimMap.size() << " cube, "
        << spherePrimMap.size() << " sphere, "
        << cylinderPrimMap.size() << " cylinder, " << conePrimMap.size() << " cone, "
        << capsulePrimMap.size() << " capsule, " << planePrimMap.size() << " plane), "
        << total_materials << " materials");

  // Report counting complete via detailed progress
  _progress_info.stage = DetailedProgressInfo::Stage::CountingPrims;
  _progress_info.meshes_total = total_meshes;
  _progress_info.materials_total = total_materials;
  _progress_info.message = "Counted " + std::to_string(total_meshes) + " meshes, " +
                           std::to_string(total_materials) + " materials";
  if (!CallDetailedProgressCallback(_progress_info)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  // 1. Convert Xform
  // 2. Convert Material/Texture
  // 3. Convert Mesh/SkinWeights/BlendShapes
  // 4. Convert Skeleton(bones)
  // 5. Build node hierarchy (includes lights and cameras)

  //
  // 1. Build Xform at specified time.
  //    Each Prim in Stage is converted to XformNode.
  //
  _progress_info.stage = DetailedProgressInfo::Stage::ConvertingXforms;
  _progress_info.progress = 0.1f;
  _progress_info.message = "Building xform hierarchy";
  if (!CallDetailedProgressCallback(_progress_info)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  XformNode xform_node;
  {
    const auto phase_start = TydraPerfClock::now();
    if (!BuildXformNodeFromStage(env.stage, &xform_node, env.timecode)) {
      PUSH_ERROR_AND_RETURN("Failed to build Xform node hierarchy.\n");
    }
    xform_ms = ElapsedMs(phase_start);
  }

  // Report progress after xform building (20%)
  if (!CallProgressCallback(0.2f)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  std::string err;

  //
  // 2. Convert Material/Texture
  // 3. Convert Mesh/SkinWeights/BlendShapes
  // 4. Convert Skeleton(bones) and SkelAnimation
  //
  // Material conversion will be done in MeshVisitor.
  //
  _progress_info.stage = DetailedProgressInfo::Stage::ConvertingMeshes;
  _progress_info.progress = 0.2f;
  _progress_info.message = "Converting meshes and materials";
  if (!CallDetailedProgressCallback(_progress_info)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }
  if (!EmitPhase(StreamPhase::MaterialsAndMeshes)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  MeshVisitorEnv menv;
  menv.env = &env;
  menv.converter = this;
  menv.meshes_total = total_meshes;
  menv.materials_total = total_materials;
  menv.allSkeletons = &allSkeletons;
  menv.allSkelRoots = &allSkelRoots;
  menv.allAnimations = &allAnimations;

  // Store pre-discovered maps in converter for use by ConvertMesh
  _allSkeletons = &allSkeletons;
  _allSkelRoots = &allSkelRoots;
  _allAnimations = &allAnimations;

  // Parallel per-mesh geometry conversion (non-streaming only: the streaming
  // sink contract requires interleaved material/mesh emission in traversal
  // order). When enabled, materials convert inline during a collection pass
  // and geometry runs on a worker pool; meshes are merged back in original
  // traversal order, so output is identical to the serial path.
  const size_t geom_workers = ResolveGeometryWorkerCount(env.scene_config.num_threads);
  const bool use_deferred_geometry =
      (geom_workers > 1) && !HasStreamingSink();

  std::vector<MeshWorkItem> work_items;

  {
    const auto phase_start = TydraPerfClock::now();
    bool ret = false;
    if (use_deferred_geometry) {
      menv.work_items = &work_items;
      ret = tydra::VisitPrims(env.stage, MeshVisitor, &menv, &err);
      if (ret) {
        ret = ConvertDeferredMeshes(env, work_items, geom_workers,
                                    &menv, &err);
      }
    } else {
      ret = tydra::VisitPrims(env.stage, MeshVisitor, &menv, &err);
    }

    visit_prims_ms = ElapsedMs(phase_start);
    if (!ret) {
      PUSH_ERROR_AND_RETURN(err);
    }
  }

  // Add standalone skeletons (not referenced by any mesh) to the render scene.
  // This ensures skeletons with SkelAnimations but no bound meshes are still
  // available for visualization (e.g. bone hierarchy display).
  {
    const auto phase_start = TydraPerfClock::now();
    for (const auto &skelEntry : allSkeletons) {
      const std::string &skelPathStr = skelEntry.first;
      if (_skelPathToIndex.find(skelPathStr) != _skelPathToIndex.end()) {
        continue;  // Already added by a mesh binding
      }
      const Skeleton *skelPtr = skelEntry.second;
      if (!skelPtr) continue;

      int32_t skel_id = int32_t(skeletons.size());
      SkelHierarchy skel;

      std::string primName = skelPathStr;
      size_t lastSlash = primName.rfind('/');
      if (lastSlash != std::string::npos) {
        primName = primName.substr(lastSlash + 1);
      }
      if (!ConvertSkeletonFromPtr(env, Path(skelPathStr, ""), *skelPtr, primName, &skel)) {
        PushWarn(fmt::format(
            "Skipping invalid standalone skeleton {}: {}\n",
            skelPathStr, GetError()));
        _err.clear();
        continue;
      }

      _skelPathToIndex[skelPathStr] = skel_id;
      skeletons.emplace_back(std::move(skel));
      DCOUT("Added standalone skeleton: " << skelPathStr);
    }
    standalone_skel_ms = ElapsedMs(phase_start);
  }

  // Convert all SkelAnimation prims now that all skeletons have been discovered.
  // This supports multiple animations per skeleton (when animationSource is a pathvector).
  DCOUT("Converting all SkelAnimation prims...");
  {
    const auto phase_start = TydraPerfClock::now();
    if (!ConvertAllSkelAnimations(env)) {
      PUSH_ERROR_AND_RETURN("Failed to convert SkelAnimation prims");
    }
    skel_anim_ms = ElapsedMs(phase_start);
  }
  DCOUT("SkelAnimation conversion complete");

  // Clear temporary pointers
  _allSkeletons = nullptr;
  _allSkelRoots = nullptr;
  _allAnimations = nullptr;
  _skelRootToSkeleton.clear();
  _materialBindingCache.clear();
  _materialSourceSignatureCache.clear();

  // Report progress after mesh/material conversion (70%)
  _progress_info.stage = DetailedProgressInfo::Stage::BuildingHierarchy;
  _progress_info.progress = 0.7f;
  _progress_info.meshes_processed = menv.meshes_processed;
  _progress_info.message = "Mesh conversion complete (" +
      std::to_string(menv.meshes_processed) + " meshes)";
  if (!CallDetailedProgressCallback(_progress_info)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  if (!CallProgressCallback(0.7f)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  //
  // 5. Build node hierarchy from XformNode and meshes, materials, skeletons,
  // etc.
  //
  _progress_info.message = "Building node hierarchy";
  if (!CallDetailedProgressCallback(_progress_info)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }
  if (!EmitPhase(StreamPhase::Hierarchy)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  {
    const auto phase_start = TydraPerfClock::now();
    if (!BuildNodeHierarchy(env, xform_node)) {
      return false;
    }
    if (!ResolveBlendShapeAnimationTargets()) {
      return false;
    }
    hierarchy_ms = ElapsedMs(phase_start);
  }

  // Stream cameras, lights and the node tree now that world matrices are known.
  // (Skipped entirely without a streaming sink.)
  if (_sink) {
    for (size_t i = 0; i < cameras.size(); i++) {
      if (!EmitCamera(i, cameras[i].abs_path)) {
        PushError("Conversion cancelled by user.\n");
        return false;
      }
    }
    for (size_t i = 0; i < lights.size(); i++) {
      if (!EmitLight(i, lights[i].abs_path)) {
        PushError("Conversion cancelled by user.\n");
        return false;
      }
    }
    for (size_t i = 0; i < root_nodes.size(); i++) {
      if (!EmitRootNode(i)) {
        PushError("Conversion cancelled by user.\n");
        return false;
      }
    }
  }

  // Report progress after node hierarchy building (85%)
  _progress_info.stage = DetailedProgressInfo::Stage::ExtractingAnimations;
  _progress_info.progress = 0.85f;
  _progress_info.message = "Hierarchy complete, extracting animations";
  if (!CallDetailedProgressCallback(_progress_info)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  if (!CallProgressCallback(0.85f)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  //
  // 6. Extract xformOp animations from nodes with time-sampled transforms
  //
  {
    const auto phase_start = TydraPerfClock::now();
    ExtractXformAnimations(env, xform_node);
    xform_anim_ms = ElapsedMs(phase_start);
  }

  // Report progress after animation extraction (90%)
  if (!CallProgressCallback(0.9f)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  //
  // 7. Merge meshes with same material (optional optimization)
  //
  if (env.scene_config.dedup_materials_by_texture_identity) {
    const size_t before_textures = textures.size();
    const size_t removed_textures = DeduplicateTexturesByIdentityImpl();
    if (removed_textures > 0) {
      PushInfo("Texture deduplication before material deduplication: " +
               std::to_string(before_textures) + " -> " +
               std::to_string(textures.size()) + ".");
    }

    const size_t before_materials = materials.size();
    const size_t removed = DeduplicateMaterialsByTextureIdentityImpl();
    if (removed > 0) {
      PushInfo("Material deduplication before merge: " +
               std::to_string(before_materials) + " -> " +
               std::to_string(materials.size()) + ".");
    }
  }

  if (env.scene_config.merge_meshes) {
    const auto phase_start = TydraPerfClock::now();
    if (!MergeMeshesImpl(env)) {
      PushWarn("Mesh merging encountered issues, but conversion continues.\n");
    }
    merge_ms = ElapsedMs(phase_start);
  }

  // Report progress after mesh merging (95%)
  if (!CallProgressCallback(0.95f)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  instance_map_ms = BuildStageInstances(env, pointInstancerPrimMap);

  if (env.scene_config.flatten_optimized_render_tree) {
    const size_t before_nodes = root_nodes.size();
    const size_t kept_nodes = FlattenOptimizedRenderTreeImpl();
    PushInfo("Flattened optimized render tree: roots " +
             std::to_string(before_nodes) + " -> 1, render nodes " +
             std::to_string(kept_nodes) + ".");
  }

  // render_scene.meshMap = std::move(meshMap);
  // render_scene.materialMap = std::move(materialMap);
  // render_scene.textureMap = std::move(textureMap);
  // render_scene.imageMap = std::move(imageMap);
  // render_scene.bufferMap = std::move(bufferMap);

  // Stream skeletons/animations then instances (fast tail phases) before the
  // member arrays are moved into the RenderScene. Skipped without a sink.
  if (_sink) {
    if (!EmitPhase(StreamPhase::Animations)) {
      PushError("Conversion cancelled by user.\n");
      return false;
    }
    for (size_t i = 0; i < skeletons.size(); i++) {
      if (!EmitSkeleton(i, skeletons[i].abs_path)) {
        PushError("Conversion cancelled by user.\n");
        return false;
      }
    }
    for (size_t i = 0; i < animations.size(); i++) {
      if (!EmitAnimation(i, animations[i].abs_path)) {
        PushError("Conversion cancelled by user.\n");
        return false;
      }
    }
    if (!EmitPhase(StreamPhase::Instances)) {
      PushError("Conversion cancelled by user.\n");
      return false;
    }
    for (size_t i = 0; i < instances.size(); i++) {
      if (!EmitInstance(i, instances[i].abs_path)) {
        PushError("Conversion cancelled by user.\n");
        return false;
      }
    }
  }

  RenderScene render_scene;
  render_scene.usd_filename = env.usd_filename;
  render_scene.default_root_node = 0;
  if (default_node > -1) {
    if (size_t(default_node) >= root_nodes.size()) {
      PushWarn("Invalid default_node id. Use 0 for default_node id.");
    } else {
      render_scene.default_root_node = uint32_t(default_node);
    }
  }

  render_scene.nodes = std::move(root_nodes);
  render_scene.meshes = std::move(meshes);
  render_scene.textures = std::move(textures);
  render_scene.udim_textures = std::move(udim_textures);
  render_scene.images = std::move(images);
  render_scene.buffers = std::move(buffers);
  render_scene.materials = std::move(materials);
  render_scene.cameras = std::move(cameras);
  render_scene.lights = std::move(lights);
  render_scene.skeletons = std::move(skeletons);
  render_scene.animations = std::move(animations);
  render_scene.instances = std::move(instances);
  render_scene.volumes = std::move(volumes);

  // Populate scene metadata from Stage
  {
    const auto phase_start = TydraPerfClock::now();
    const auto &stage_metas = env.stage.metas();

    render_scene.meta.renderSettingsPrimPath =
        rendering_color.render_settings_path;
    render_scene.meta.workingColorSpace = rendering_color.working_space;
    color::ColorSpaceDesc display_linear;
    color::ColorTransform display_transform;
    if (color::GetBuiltinColorSpace("lin_rec709_scene", &display_linear) &&
        color::BuildColorTransform(rendering_color.working_definition,
                                   display_linear, &display_transform)) {
      std::copy(display_transform.matrix, display_transform.matrix + 9,
                render_scene.meta.workingToDisplayLinear.begin());
    }

    // upAxis
    if (stage_metas.upAxis.authored()) {
      render_scene.meta.upAxis = to_string(stage_metas.upAxis.get_value());
    }

    // metersPerUnit
    if (stage_metas.metersPerUnit.authored()) {
      render_scene.meta.metersPerUnit = stage_metas.metersPerUnit.get_value();
    }

    // framesPerSecond
    if (stage_metas.framesPerSecond.authored()) {
      render_scene.meta.framesPerSecond = stage_metas.framesPerSecond.get_value();
    }

    // timeCodesPerSecond
    if (stage_metas.timeCodesPerSecond.authored()) {
      render_scene.meta.timeCodesPerSecond = stage_metas.timeCodesPerSecond.get_value();
    }

    // startTimeCode
    if (stage_metas.startTimeCode.authored()) {
      render_scene.meta.startTimeCode = stage_metas.startTimeCode.get_value();
    }

    // endTimeCode
    if (stage_metas.endTimeCode.authored()) {
      render_scene.meta.endTimeCode = stage_metas.endTimeCode.get_value();
    }

    // autoPlay
    if (stage_metas.autoPlay.authored()) {
      render_scene.meta.autoPlay = stage_metas.autoPlay.get_value();
    }

    // comment
    if (!stage_metas.comment.value.empty()) {
      render_scene.meta.comment = stage_metas.comment.value;
    }

    // copyright - Check if customLayerData contains copyright info
    auto it = stage_metas.customLayerData.find("copyright");
    if (it != stage_metas.customLayerData.end()) {
      // Try to extract string value from MetaVariable
      auto copyright_val = it->second.get_value<std::string>();
      if (copyright_val) {
        render_scene.meta.copyright = copyright_val.value();
      }
    }
    stage_meta_ms = ElapsedMs(phase_start);
  }

  (*scene) = std::move(render_scene);

  {
    std::ostringstream ss;
    ss << "total=" << ElapsedMs(total_start)
       << " count=" << count_ms
       << " skelMap=" << skel_map_ms
       << " xform=" << xform_ms
       << " visitPrims=" << visit_prims_ms
       << " materialResolve=" << NsToMs(menv.resolve_material_ns)
       << "/" << menv.material_resolve_calls
       << " materialFound=" << menv.material_resolve_found
       << " materialConvert=" << NsToMs(menv.convert_material_ns)
       << " materialCache=" << menv.material_cache_hits << "/"
       << menv.material_cache_misses
       << " meshConvert=" << NsToMs(menv.convert_mesh_ns)
       << " meshProgress=" << NsToMs(menv.progress_ns)
       << " standaloneSkel=" << standalone_skel_ms
       << " skelAnim=" << skel_anim_ms
       << " hierarchy=" << hierarchy_ms
       << " xformAnim=" << xform_anim_ms
       << " merge=" << merge_ms
       << " instanceMap=" << instance_map_ms
       << " metadata=" << stage_meta_ms
       << " meshes=" << scene->meshes.size()
       << " materials=" << scene->materials.size()
       << " textures=" << scene->textures.size()
       << " images=" << scene->images.size()
       << " instances=" << scene->instances.size();
    _timing_info = ss.str();
  }

  // Report completion (100%)
  _progress_info.stage = DetailedProgressInfo::Stage::Complete;
  _progress_info.progress = 1.0f;
  _progress_info.message = "Conversion complete";
  if (!CallDetailedProgressCallback(_progress_info)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }
  CallProgressCallback(1.0f);

  // Light-link collections depend on the final RenderScene mesh table. Resolve
  // them after conversion/merge and before the streaming completion callback,
  // so both monolithic consumers and sinks observe identical records.
  ResolveLightLinking(env.stage, scene);

  if (!EmitPhase(StreamPhase::Complete)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }
  if (!EmitComplete(*scene)) {
    PushError("Conversion cancelled by user.\n");
    return false;
  }

  DCOUT("[Tydra] Conversion complete: " << scene->meshes.size() << " meshes, "
        << scene->materials.size() << " materials, " << scene->textures.size() << " textures");

  return true;
}


}  // namespace tydra
}  // namespace lightusd
