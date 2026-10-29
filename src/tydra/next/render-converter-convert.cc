// SPDX-License-Identifier: Apache-2.0
// Retained (Convert) scene conversion driver. Split from render-converter.cc.
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
std::string LeafNameFromJointPath(const std::string& path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos) return path;
  if (pos + 1 >= path.size()) return "";
  return path.substr(pos + 1);
}

bool JointTokenMatches(const SkeletonJoint& joint, const std::string& token) {
  if (token.empty()) return false;
  if (joint.path == token || joint.name == token) return true;
  if (LeafNameFromJointPath(joint.path) == token) return true;
  if (joint.path.size() > token.size() &&
      joint.path.compare(joint.path.size() - token.size(), token.size(),
                         token) == 0) {
    const size_t sep = joint.path.size() - token.size();
    return sep == 0 || joint.path[sep - 1] == '/';
  }
  return false;
}


std::vector<std::string> ReadRelationshipTargets(const UsdPrim& prim,
                                                 const std::string& name) {
  std::vector<std::string> out;
  const std::vector<::lightusd::next::Path>* targets =
      prim.GetRelationship(name);
  if (!targets) return out;
  out.reserve(targets->size());
  for (const ::lightusd::next::Path& target : *targets) {
    out.push_back(target.str());
  }
  return out;
}

void ReleaseMeshGeometry(RenderMesh* mesh, bool keep_binding_inputs,
                         bool keep_triangulation = false,
                         bool keep_custom_primvars = false) {
  if (!mesh) return;

  mesh->face_vertex_indices = UInt32Chunked();
  mesh->points = FloatChunked();
  mesh->normals = FloatChunked();
  mesh->tangents = FloatChunked();
  mesh->colors = FloatChunked();
  mesh->opacities = FloatChunked();
  if (!keep_triangulation) {
    mesh->triangulated_indices = UInt32Chunked();
    mesh->triangulated_face_vertex_indices = UInt32Chunked();
  }

  if (keep_binding_inputs) return;

  mesh->face_vertex_counts = UInt32Chunked();
  mesh->texcoords_0 = FloatChunked();
  mesh->texcoords_1 = FloatChunked();
  if (!keep_custom_primvars) std::vector<VertexAttribute>().swap(mesh->primvars);
  std::vector<uint32_t>().swap(mesh->face_triangle_offsets);
  std::vector<int32_t>().swap(mesh->sanitize_face_remap);
  std::vector<uint32_t>().swap(mesh->hole_faces);
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
}  // namespace

ConvertResult RenderSceneConverter::Impl::Convert(const Stage& stage) {
  ConvertResult result;
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

  // Built with -fno-exceptions: the conversion helpers report failures via
  // return codes / the warnings_ list rather than throwing, so no try/catch.
  {
    // Report progress
    if (config_.progress_callback) {
      config_.progress_callback(0.0f, "Starting conversion...");
    }

    // Set scene metadata
    auto meta = stage.GetMeta();
    result.scene.name = meta.defaultPrim;
    result.scene.default_prim = meta.defaultPrim;
    result.scene.meters_per_unit = static_cast<float>(
        meta.metersPerUnit_set ? meta.metersPerUnit : 1.0);
    result.scene.up_axis = (meta.upAxis == "Z") ?
                           RenderScene::UpAxis::Z : RenderScene::UpAxis::Y;
    result.scene.start_time = meta.startTimeCode;
    result.scene.end_time = meta.endTimeCode;
    result.scene.frames_per_second = meta.timeCodesPerSecond;
    ::lightusd::next::color_management::RenderingColorConfig color_config;
    std::string color_warning;
    (void)::lightusd::next::color_management::ResolveRenderingColorConfig(
        stage, config_.material.render_settings_path, &color_config,
        &color_warning);
    result.scene.render_settings_path = color_config.render_settings_path;
    result.scene.working_color_space = color_config.working_space;
    ::lightusd::color::ColorSpaceDesc display_linear;
    ::lightusd::color::ColorTransform working_to_display;
    if (::lightusd::color::GetBuiltinColorSpace("lin_rec709_scene",
                                                &display_linear) &&
        ::lightusd::color::BuildColorTransform(
            color_config.working_definition, display_linear,
            &working_to_display)) {
      for (size_t i = 0; i < 9; ++i) {
        result.scene.working_to_display_linear[i] =
            working_to_display.matrix[i];
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

    // Build node hierarchy first
    if (config_.progress_callback) {
      config_.progress_callback(0.1f, "Building node hierarchy...");
    }
    BuildNodeHierarchy(extracted, &result.scene);
    ExtractPhysicsAnnotations(stage, &result.scene);
    result.scene.unsupported_renderables.reserve(
        std::min<size_t>(extracted.records.size(), 128));
    const bool emit_animations =
        config_.animation.enabled &&
        (stage.HasTimeSamples() || stage.HasValueClips());
    if (emit_animations) {
      result.scene.animations.reserve(extracted.records.size());
    }
    const size_t point_prim_count = extracted.points.size();
    result.scene.meshes.reserve(extracted.meshes.size());
    result.scene.points.reserve(point_prim_count);
    result.scene.curves.reserve(extracted.curves.size());
    result.scene.point_instancers.reserve(extracted.point_instancers.size());
    result.scene.lights.reserve(extracted.lights.size());
    result.scene.cameras.reserve(extracted.cameras.size());
    result.scene.materials.reserve(extracted.materials.size());
    result.scene.skeletons.reserve(extracted.skeletons.size());
    result.scene.mesh_by_path.reserve(extracted.meshes.size());
    result.scene.points_by_path.reserve(point_prim_count);
    result.scene.curves_by_path.reserve(extracted.curves.size());
    result.scene.point_instancer_by_path.reserve(extracted.point_instancers.size());
    result.scene.material_by_path.reserve(extracted.materials.size());

    const bool has_time_samples = emit_animations;
    // UsdVol Volumes (field resolution only; voxels decode on demand).
    result.scene.volumes.reserve(extracted.volumes.size());
    for (const RenderPrimRecord& vrec : extracted.volumes) {
      RenderVolume volume;
      if (ConvertVolume(stage, vrec.prim, &result.scene, &volume)) {
        for (const std::string& w : volume.warnings) AddWarning(w);
        result.scene.volume_by_path[volume.prim_path] =
            static_cast<int32_t>(result.scene.volumes.size());
        result.scene.volumes.push_back(std::move(volume));
      }
    }
    for (const RenderPrimRecord& rec : extracted.records) {
      if (rec.type_name.compare(0, 13, "ParticleField") == 0) {
        RenderParticleField pf;
        if (ConvertParticleField(stage, rec.prim, config_.time_code, &pf)) {
          for (const std::string& w : pf.warnings) AddWarning(w);
          result.scene.particle_fields.push_back(std::move(pf));
        }
      }
    }
    for (const RenderPrimRecord& rec : extracted.records) {
      if (rec.type_name != "Points" &&
          rec.type_name != "ParticleField3DGaussianSplat" &&
          IsUnsupportedRenderableTypeName(rec.type_name)) {
        UnsupportedRenderable unsupported;
        unsupported.prim_path = rec.path;
        unsupported.type_name = rec.type_name;
        unsupported.reason =
            "recognized by extraction but not converted to render geometry";
        result.scene.unsupported_renderables.push_back(unsupported);
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
        const auto node_it = result.scene.node_by_path.find(rec.path);
        if (node_it != result.scene.node_by_path.end()) {
          for (AnimationChannel& channel : clip.channels) {
            channel.target_node = node_it->second;
          }
        }
        result.scene.animations.push_back(std::move(clip));
      }
    }

    // Category lists retain the records needed by conversion workers. Drop
    // the traversal-order pointer index before materializing geometry.
    extracted.release_records();

    // Convert meshes
    //
    // Each mesh converts independently of every other (ConvertRenderableMesh
    // reads the composed stage and writes only into its own RenderMesh out-
    // param -- verified no other prim's data or scene-wide converter state is
    // touched); the only shared state it reaches is warnings_/last_error_/the
    // BudgetWouldExceed counters, all funneled through AddWarning()/
    // SetLastError()/BudgetWouldExceed() under state_mu_ above. So the actual
    // per-mesh conversion runs on a worker pool, in batches sized to the
    // hardware; scene bookkeeping (id assignment, mesh_by_path, node linking,
    // geometry-retention trimming) stays serial and in original prim order so
    // output stays byte-identical to the single-threaded path.
    //
    // The budget check is still made on the main thread, one mesh at a time,
    // before it is added to a batch -- same "decide with real RSS, then do
    // the work" ordering as the serial loop had, just batched at worker-pool
    // granularity instead of per-mesh.
    float mesh_progress_start = 0.2f;
    float mesh_progress_end = 0.5f;

    const size_t mesh_count = extracted.meshes.size();
    size_t mesh_workers = 1;
#if defined(LIGHTUSD_ENABLE_THREAD)
    if (config_.execution.max_threads == 0) {
      const unsigned hw_threads = std::thread::hardware_concurrency();
      mesh_workers = std::max<size_t>(
          1, std::min<size_t>(hw_threads ? hw_threads : 4, 16));
    } else {
      mesh_workers = std::min<size_t>(
          static_cast<size_t>(config_.execution.max_threads),
          static_cast<size_t>(::lightusd::next::kMaxExecutionThreads));
    }
#endif
    ::lightusd::next::TaskArena task_arena(mesh_workers);

    // Dynamic work distribution: instead of fixed-size batch waves (where one
    // oversized mesh idled the other workers until the wave barrier), all
    // mesh indices are pushed through a single arena.Run() pass and claimed
    // via an atomic cursor. Each output writes into its own slot, and the
    // serial merge below walks slots in prim order -- so results stay
    // byte-identical to the previous batched scheduler regardless of which
    // worker ran what.
    //
    // Per-item geometry size estimates are computed up front (one serial
    // pass) so the budget gate inside each task no longer re-walks the prim's
    // authored arrays on the hot path.
    std::vector<size_t> mesh_estimates(mesh_count);
    for (size_t i = 0; i < mesh_count; ++i) {
      mesh_estimates[i] =
          BuildGeometryInfo(extracted.meshes[i].prim, GeometryKind::Mesh, -1)
              .estimated_resident_bytes;
    }

    std::vector<RenderMesh> mesh_out(mesh_count);
    std::vector<uint8_t> mesh_ok(mesh_count, 0);
    std::atomic<bool> mesh_budget_hit{false};

    task_arena.Run(mesh_count, [&](size_t mi) {
      ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
      if (mesh_budget_hit.load(std::memory_order_relaxed)) {
        return;
      }
      // Cumulative budget guard: skip the rest of the geometry rather than
      // let a scene of many individually-small meshes OOM the process. Same
      // "decide with real accounting, then do the work" ordering as before,
      // just executed by whichever worker claims the item (BudgetWouldExceed
      // is state_mu_-guarded and latches).
      if (BudgetWouldExceed(mesh_estimates[mi], "mesh conversion")) {
        mesh_budget_hit.store(true, std::memory_order_relaxed);
        return;
      }
      mesh_ok[mi] =
          ConvertRenderableMesh(stage, extracted.meshes[mi].prim,
                                &mesh_out[mi]) ? 1 : 0;
    });

    const bool budget_latched = mesh_budget_hit.load(std::memory_order_relaxed);

    // Serial merge in original prim order. Progress is reported here on the
    // calling thread as items complete (the previous scheduler reported
    // dispatch progress from the batch-fill loop; both are advisory).
    for (size_t mi = 0; mi < mesh_count; ++mi) {
      if (!mesh_ok[mi]) {
        if (budget_latched) {
          // Remaining geometry was skipped by the memory budget.
          break;
        }
        AddWarning("Failed to convert renderable mesh prim: " +
                            extracted.meshes[mi].prim.GetPath().str());
        result.scene.conversion_diagnostics.push_back({ConversionDisposition::Failed,
            extracted.meshes[mi].prim.GetPath().str(), "mesh conversion failed"});
        continue;
      }
      if (config_.progress_callback) {
        float p = mesh_progress_start +
                  (mesh_progress_end - mesh_progress_start) * mi /
                      std::max<size_t>(mesh_count, 1);
        config_.progress_callback(p,
                                  "Converting mesh: " +
                                      extracted.meshes[mi].prim.GetName());
      }

      const UsdPrim& mesh_prim = extracted.meshes[mi].prim;
      RenderMesh& mesh = mesh_out[mi];
      if (mesh.has_alloc_failure()) {
        // ConvertGeomPrimitive does not run ConvertMesh's alloc check.
        AddConversionWarning(mesh_prim.GetPath().str(), "Out of memory converting prim '" +
                            mesh_prim.GetPath().str() +
                            "'; the prim was skipped", ConversionDisposition::Failed);
        continue;
      }
      const bool analytic = mesh_prim.GetTypeName() != "Mesh";
      if (!config_.mesh.retain_geometry &&
          !(analytic && config_.mesh.retain_analytic_geometry)) {
        // Retain only the small inputs still required by the later material
        // binding and UV-selection passes. This bounds conversion memory by
        // one source mesh instead of accumulating the whole render scene.
        ReleaseMeshGeometry(&mesh, true,
                            config_.mesh.retain_triangulation);
      }
      // Release chunk-allocation slack before retaining: thousands of small
      // meshes each holding 64KB-minimum chunks otherwise OOM wasm32.
      mesh.compact();
      int32_t mesh_id = static_cast<int32_t>(result.scene.meshes.size());
      result.scene.mesh_by_path[mesh.prim_path] = mesh_id;
      result.scene.meshes.push_back(std::move(mesh));
      AssignNodeDataId(&result.scene, mesh_prim.GetPath().str(), mesh_id);
    }

    // Points and Curves conversion is as independent per-prim as mesh
    // conversion (verified: ConvertPoints/ConvertCurves touch no converter
    // state beyond warnings_/last_error_, already funneled through
    // AddWarning()/SetLastError() under state_mu_), so run them across the
    // same worker pool with dynamic index distribution (single Run pass; no
    // fixed-size batch waves). Bookkeeping (id assignment, *_by_path, node
    // linking, compact/release) stays serial and in original record order.
    {
      const size_t n_points = extracted.points.size();
      std::vector<RenderPoints> points_out(n_points);
      std::vector<uint8_t> points_ok(n_points, 0);
      task_arena.Run(n_points, [&](size_t bi) {
        ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
        points_ok[bi] = ConvertPoints(stage,
                                      extracted.points[bi].prim,
                                      &points_out[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n_points; ++bi) {
        const auto& rec = extracted.points[bi];
        RenderPoints& points = points_out[bi];
        if (points_ok[bi]) {
          if (points.has_alloc_failure()) {
            AddConversionWarning(rec.path, "Out of memory converting Points '" + rec.path +
                                "'; the prim was skipped", ConversionDisposition::Failed);
            continue;
          }
          int32_t points_id = static_cast<int32_t>(result.scene.points.size());
          points.compact();
          result.scene.points_by_path[points.prim_path] = points_id;
          result.scene.points.push_back(std::move(points));
          AssignNodeDataId(&result.scene, rec.path, points_id);
        } else {
          AddConversionWarning(rec.path, "Failed to convert Points: " + rec.path,
                               ConversionDisposition::Failed);
        }
      }
    }

    {
      const size_t n_curves = extracted.curves.size();
      std::vector<RenderCurves> curves_out(n_curves);
      std::vector<uint8_t> curves_ok(n_curves, 0);
      task_arena.Run(n_curves, [&](size_t bi) {
        ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
        curves_ok[bi] =
            ConvertCurves(extracted.curves[bi].prim, &curves_out[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n_curves; ++bi) {
        const auto& rec = extracted.curves[bi];
        RenderCurves& curves = curves_out[bi];
        if (curves_ok[bi]) {
          if (curves.has_alloc_failure()) {
            AddConversionWarning(rec.path, "Out of memory converting curves '" + rec.path +
                                "'; the prim was skipped", ConversionDisposition::Failed);
            continue;
          }
          int32_t curves_id = static_cast<int32_t>(result.scene.curves.size());
          curves.compact();
          result.scene.curves_by_path[curves.prim_path] = curves_id;
          result.scene.curves.push_back(std::move(curves));
          AssignNodeDataId(&result.scene, rec.path, curves_id);
        } else {
          AddConversionWarning(rec.path, "Failed to convert curves prim: " + rec.path,
                               ConversionDisposition::Failed);
        }
      }
    }

    // ConvertPointInstancer itself (reading positions/orientations/scales/
    // velocities/ids arrays) touches no shared converter or scene state, so
    // it runs in the worker batch; prototype-binding resolution, instance
    // draw expansion and id/scene bookkeeping all mutate result.scene and
    // stay serial in original record order.
    for (size_t batch_start = 0; batch_start < extracted.point_instancers.size();
         batch_start += mesh_workers) {
      const size_t batch_end = std::min(batch_start + mesh_workers,
                                        extracted.point_instancers.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderPointInstancer> batch_inst(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
        batch_ok[bi] = ConvertPointInstancer(
                           extracted.point_instancers[batch_start + bi].prim,
                           &batch_inst[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        const auto& rec = extracted.point_instancers[batch_start + bi];
        RenderPointInstancer& instancer = batch_inst[bi];
        if (!batch_ok[bi]) {
          AddConversionWarning(rec.path, "Failed to convert PointInstancer: " + rec.path,
                               ConversionDisposition::Failed);
          continue;
        }
        int32_t instancer_id =
            static_cast<int32_t>(result.scene.point_instancers.size());
        result.scene.point_instancer_by_path[instancer.prim_path] = instancer_id;
        if (!instancer.valid) {
          AddWarning("Invalid PointInstancer data at " +
                              instancer.prim_path + ": " +
                              instancer.validation_error);
        }
        ResolvePointInstancerPrototypeBindings(&result.scene, &instancer);
        for (size_t proto_i = 0; proto_i < instancer.prototype_paths.size();
             ++proto_i) {
          if (proto_i >= instancer.prototype_node_ids.size() ||
              instancer.prototype_node_ids[proto_i] < 0) {
            AddWarning("Unresolved PointInstancer prototype at " +
                                instancer.prim_path + ": " +
                                instancer.prototype_paths[proto_i]);
          } else if (instancer.prototype_mesh_count(proto_i) == 0) {
            AddWarning("PointInstancer prototype has no meshes at " +
                                instancer.prim_path + ": " +
                                instancer.prototype_paths[proto_i]);
          }
        }
        if (config_.point_instancer.build_instance_draws ||
            config_.point_instancer.duplicate_meshes) {
          AppendPointInstanceDraws(instancer_id, &instancer, &result.scene);
        }
        result.scene.point_instancers.push_back(std::move(instancer));
        AssignNodeDataId(&result.scene, rec.path, instancer_id);
      }
    }

    // Convert materials. Each ConvertMaterial call is given its own
    // per-worker LOCAL scratch RenderScene (tl_material_local_scope_ set for
    // the duration -- see MaterialLocalScope below), so the graph-walk/
    // texture-registration work runs in parallel; a serial merge afterward
    // dedups/appends each worker's local images/textures into the shared
    // scene (in original material order, replicating exactly what the
    // fully-serial dedup would have done) and remaps every
    // ShaderParam::texture_id from the local scratch's numbering to the
    // final shared numbering.
    float mat_progress_start = 0.5f;
    float mat_progress_end = 0.7f;

    for (size_t batch_start = 0; batch_start < extracted.materials.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.materials.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderMaterial> batch_material(n);
      std::vector<RenderScene> batch_local_scene(n);
      std::vector<uint8_t> batch_ok(n, 0);
      for (size_t bi = 0; bi < n; ++bi) {
        // Only the color-managed fields ConvertMaterial/ExtractShaderParam
        // read are seeded; everything else on the scratch scene starts
        // empty and is discarded after merge.
        batch_local_scene[bi].working_color_space =
            result.scene.working_color_space;
        if (config_.progress_callback) {
          const size_t i = batch_start + bi;
          float p = mat_progress_start +
                    (mat_progress_end - mat_progress_start) * i /
                        std::max<size_t>(extracted.materials.size(), 1);
          config_.progress_callback(
              p, "Converting material: " +
                     extracted.materials[i].prim.GetName());
        }
      }
      auto convert_one = [&](size_t bi) {
        ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
        MaterialLocalScope scope;
        const UsdPrim& mat_prim = extracted.materials[batch_start + bi].prim;
        batch_ok[bi] = ConvertMaterial(stage, mat_prim, &batch_material[bi],
                                       &batch_local_scene[bi]) ? 1 : 0;
      };
#if defined(LIGHTUSD_ENABLE_THREAD)
      const bool callbacks_may_run_concurrently =
          config_.execution.callback_concurrency ==
          ::lightusd::next::CallbackConcurrency::Concurrent;
      const bool resolver_has_callbacks =
          config_.asset_resolver && config_.asset_resolver->HasUserCallbacks();
      const bool material_parallel_safe =
          callbacks_may_run_concurrently ||
          (!config_.material.custom_texture_loader &&
           !resolver_has_callbacks);
      if (n == 1 || !material_parallel_safe) {
#else
      if (true) {
#endif
        for (size_t bi = 0; bi < n; ++bi) convert_one(bi);
      } else {
#if defined(LIGHTUSD_ENABLE_THREAD)
        task_arena.Run(n, convert_one);
#endif
      }

      for (size_t bi = 0; bi < n; ++bi) {
        const UsdPrim& mat_prim = extracted.materials[batch_start + bi].prim;
        RenderMaterial& material = batch_material[bi];
        RenderScene& local = batch_local_scene[bi];

        // Dedup images against the shared scene (same key ConvertMaterial
        // itself uses), in the local scratch's own append order -- this
        // reproduces the exact sequence of "check cache, maybe append"
        // decisions the fully-serial converter would have made.
        std::vector<int32_t> image_remap(local.images.size(), -1);
        for (size_t ii = 0; ii < local.images.size(); ++ii) {
          TextureImage& img = local.images[ii];
          const std::string resolved_path = img.resolved_path;
          const ColorSpace cs = img.color_space;
          int32_t id = FindCachedImageId(&result.scene, resolved_path, cs);
          if (id < 0) {
            id = static_cast<int32_t>(result.scene.images.size());
            result.scene.images.push_back(std::move(img));
            RememberImageId(&result.scene, resolved_path, cs, id);
          }
          image_remap[ii] = id;
        }
        std::vector<int32_t> udim_remap(local.udim_textures.size(), -1);
        for (size_t ui = 0; ui < local.udim_textures.size(); ++ui) {
          RenderUDIMTexture& udim = local.udim_textures[ui];
          for (RenderUDIMTexture::Tile& tile : udim.tiles) {
            if (tile.image_id >= 0 &&
                static_cast<size_t>(tile.image_id) < image_remap.size()) {
              tile.image_id = image_remap[static_cast<size_t>(tile.image_id)];
            }
          }
          udim_remap[ui] = static_cast<int32_t>(result.scene.udim_textures.size());
          result.scene.udim_textures.push_back(std::move(udim));
        }
        // Textures are never deduped against each other (matches the
        // original: every ExtractShaderParam call appends a fresh
        // RenderTexture), just index-remapped and appended.
        std::vector<int32_t> texture_remap(local.textures.size(), -1);
        for (size_t ti = 0; ti < local.textures.size(); ++ti) {
          RenderTexture& tex = local.textures[ti];
          if (tex.image_id >= 0 &&
              static_cast<size_t>(tex.image_id) < image_remap.size()) {
            tex.image_id = image_remap[static_cast<size_t>(tex.image_id)];
          }
          if (tex.udim_texture_id >= 0 &&
              static_cast<size_t>(tex.udim_texture_id) < udim_remap.size()) {
            tex.udim_texture_id =
                udim_remap[static_cast<size_t>(tex.udim_texture_id)];
          }
          texture_remap[ti] = static_cast<int32_t>(result.scene.textures.size());
          result.scene.textures.push_back(std::move(tex));
        }
        RemapMaterialTextureIds(&material, texture_remap);

        if (batch_ok[bi]) {
          int32_t mat_id = static_cast<int32_t>(result.scene.materials.size());
          result.scene.material_by_path[material.prim_path] = mat_id;
          result.scene.materials.push_back(std::move(material));
        } else {
          AddWarning("Failed to convert material: " + mat_prim.GetPath().str());
          result.scene.conversion_diagnostics.push_back({ConversionDisposition::Failed,
              mat_prim.GetPath().str(), "material conversion failed"});
        }
      }
    }

    AssignMaterialBindings(stage, &result.scene);
    // 2-arg on purpose: promotion compares against the mesh's ACTUAL
    // texcoords_0/1 names now (53415635e), which supersedes the old
    // `default_uv` / `default_uv + "1"` heuristic and its config field.
    PromoteMaterialUVPrimvars(&result.scene, &warnings_);
    if (!config_.mesh.retain_geometry) {
      for (RenderMesh& mesh : result.scene.meshes) {
        const UsdPrim source = stage.GetPrimAtPath(mesh.prim_path);
        const bool analytic = source.IsValid() && source.GetTypeName() != "Mesh";
        if (analytic && config_.mesh.retain_analytic_geometry) continue;
        ReleaseMeshGeometry(&mesh, false,
                            config_.mesh.retain_triangulation,
                            config_.mesh.retain_custom_primvars);
      }
    }
    AssignPointInstanceDrawMaterials(&result.scene);
    ResolveVolumeMaterials(&result.scene);
    if (config_.point_instancer.duplicate_meshes) {
      DuplicatePointInstanceMeshes(&result.scene);
    }

    // Convert lights. ConvertLight itself touches no shared converter/scene
    // state (verified), so it runs in the worker batch; the DomeLight
    // environment-texture lookup calls ResolveImageId, which mutates the
    // scene-wide image dedup cache (image_id_by_key_/image_id_cache_) and
    // result.scene.images -- that stays serial in the merge step, same as
    // id assignment.
    for (size_t batch_start = 0; batch_start < extracted.lights.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.lights.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderLight> batch_light(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
        batch_ok[bi] = ConvertLight(stage,
                           extracted.lights[batch_start + bi].prim,
                           &batch_light[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        if (!batch_ok[bi]) {
          const auto& failed = extracted.lights[batch_start + bi];
          AddWarning("Failed to convert light: " + failed.path);
          result.scene.conversion_diagnostics.push_back({ConversionDisposition::Failed,
                                                        failed.path, "light conversion failed"});
          continue;
        }
        const auto& rec = extracted.lights[batch_start + bi];
        RenderLight& light = batch_light[bi];
        for (int i = 0; i < 16; ++i) {
          light.transform.m[i] = static_cast<float>(rec.world[i]);
        }
        // DomeLight environment texture -> image, id stored in params.dome.
        if (light.type == LightType::Dome) {
          light.params.dome.texture_id = -1;
          std::string tex;
          const Value* fv = rec.prim.GetPropertyValue(kIdInputsTextureFile());
          if (fv) {
            if (auto ap = fv->as_asset_path()) tex = *ap;
            else if (auto s = fv->as_string()) tex = *s;
            else if (auto t = fv->as_token()) tex = *t;
          }
          if (!tex.empty()) {
            light.params.dome.texture_id = ResolveImageId(
                &result.scene, tex, ColorSpace::Linear, AssetAnchorOf(rec.prim));
          }
        }
        int32_t light_id = static_cast<int32_t>(result.scene.lights.size());
        result.scene.lights.push_back(std::move(light));
        AssignNodeDataId(&result.scene, rec.path, light_id);
      }
    }

    // Convert cameras. ConvertCamera touches no converter state beyond
    // warnings_/last_error_ (already mutex-guarded), so batch it the same
    // way as mesh/points/curves conversion.
    for (size_t batch_start = 0; batch_start < extracted.cameras.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.cameras.size());
      const size_t n = batch_end - batch_start;
      std::vector<RenderCamera> batch_camera(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
        batch_ok[bi] = ConvertCamera(
                           stage, extracted.cameras[batch_start + bi].prim,
                           &batch_camera[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        if (!batch_ok[bi]) continue;
        const auto& rec = extracted.cameras[batch_start + bi];
        RenderCamera& camera = batch_camera[bi];
        for (int i = 0; i < 16; ++i) {
          camera.transform.m[i] = static_cast<float>(rec.world[i]);
        }
        int32_t camera_id = static_cast<int32_t>(result.scene.cameras.size());
        result.scene.cameras.push_back(std::move(camera));
        AssignNodeDataId(&result.scene, rec.path, camera_id);
      }
    }

    // Convert skeletons. ConvertSkeleton itself (joint hierarchy + bind/rest
    // transform computation) is the expensive part and touches no shared
    // state, so it runs in the worker batch; the skel:animationSource
    // ancestor walk and id/scene bookkeeping stay serial in original order.
    // The path->id map is populated only for skeletons that were actually
    // appended: a failed conversion must not shift later ids.
    std::unordered_map<std::string, int32_t> skeleton_id_by_path;
    skeleton_id_by_path.reserve(extracted.skeletons.size());
    for (size_t batch_start = 0; batch_start < extracted.skeletons.size();
         batch_start += mesh_workers) {
      const size_t batch_end =
          std::min(batch_start + mesh_workers, extracted.skeletons.size());
      const size_t n = batch_end - batch_start;
      std::vector<Skeleton> batch_skel(n);
      std::vector<uint8_t> batch_ok(n, 0);
      task_arena.Run(n, [&](size_t bi) {
        ScopedChunkAllocationBudget worker_budget_scope(chunk_budget);
        batch_ok[bi] = ConvertSkeleton(
                           extracted.skeletons[batch_start + bi].prim,
                           &batch_skel[bi]) ? 1 : 0;
      });
      for (size_t bi = 0; bi < n; ++bi) {
        if (!batch_ok[bi]) {
          AddWarning("Failed to convert skeleton: " +
                     extracted.skeletons[batch_start + bi].path);
          continue;
        }
        const auto& rec = extracted.skeletons[batch_start + bi];
        Skeleton& skeleton = batch_skel[bi];
        // skel:animationSource may be authored on the SkelRoot (or another
        // ancestor) instead of the Skeleton itself; every descendant
        // Skeleton inherits it (UsdSkel binding inheritance).
        if (skeleton.animation_source_path.empty()) {
          UsdPrim anc = GetParent(stage, rec.prim);
          while (anc.IsValid()) {
            const std::vector<std::string> sources =
                ReadRelationshipTargets(anc, "skel:animationSource");
            if (!sources.empty()) {
              skeleton.animation_source_path = sources[0];
              break;
            }
            if (::lightusd::tydra::next::IsSkelRoot(anc)) break;
            anc = GetParent(stage, anc);
          }
        }
        int32_t skeleton_id = static_cast<int32_t>(result.scene.skeletons.size());
        result.scene.skeletons.push_back(std::move(skeleton));
        skeleton_id_by_path[rec.path] = skeleton_id;
        AssignNodeDataId(&result.scene, rec.path, skeleton_id);
      }
    }

    // Resolve mesh skin bindings to skeleton ids (successfully converted ones).
    std::vector<std::unordered_map<std::string, int32_t>> skeleton_joint_lookup;
    if (!result.scene.skeletons.empty()) {
      skeleton_joint_lookup.resize(result.scene.skeletons.size());
      for (size_t si = 0; si < result.scene.skeletons.size(); ++si) {
        const auto& skel = result.scene.skeletons[si];
        if (skel.joints.empty()) continue;
        auto& lookup = skeleton_joint_lookup[si];
        lookup.reserve(skel.joints.size());
        lookup.max_load_factor(0.7f);
        for (size_t ji = 0; ji < skel.joints.size(); ++ji) {
          const SkeletonJoint& joint = skel.joints[ji];
          const std::string& key = joint.path.empty() ? joint.name : joint.path;
          if (!key.empty()) {
            lookup.emplace(key, static_cast<int32_t>(ji));
          }
        }
      }
    }
    for (RenderMesh& mesh : result.scene.meshes) {
      if (!mesh.skin || mesh.skin->skeleton_path.empty()) continue;
      const auto sit = skeleton_id_by_path.find(mesh.skin->skeleton_path);
      if (sit != skeleton_id_by_path.end()) {
        mesh.skin->skeleton_id = sit->second;
      }

      // Mesh-local `skel:joints`: jointIndices index into the mesh's own
      // (subset/permuted) joint list — remap them onto the skeleton's joint
      // order. Unmatched tokens zero the influence weight rather than
      // silently deforming by joint 0.
      if (!mesh.skin->mesh_joint_order.empty() &&
          mesh.skin->skeleton_id >= 0) {
        const Skeleton& skel =
            result.scene.skeletons[static_cast<size_t>(mesh.skin->skeleton_id)];
        const auto& joint_lookup =
            skeleton_joint_lookup[static_cast<size_t>(mesh.skin->skeleton_id)];
        std::vector<int32_t> remap(mesh.skin->mesh_joint_order.size(), -1);
        bool identity = true;
        for (size_t k = 0; k < mesh.skin->mesh_joint_order.size(); ++k) {
          const std::string& token = mesh.skin->mesh_joint_order[k];
          auto it = joint_lookup.find(token);
          if (it != joint_lookup.end()) {
            remap[k] = it->second;
          } else {
            for (size_t ji = 0; ji < skel.joints.size(); ++ji) {
              if (JointTokenMatches(skel.joints[ji], token)) {
                remap[k] = static_cast<int32_t>(ji);
                break;
              }
            }
          }
          if (remap[k] != static_cast<int32_t>(k)) identity = false;
          if (remap[k] < 0) {
            AddWarning("Mesh " + mesh.prim_path +
                                " skel:joints token '" + token +
                                "' not found in skeleton " + skel.prim_path);
          }
        }
        if (!identity) {
          const size_t n = mesh.skin->joint_indices.size();
          for (size_t k = 0; k < n; ++k) {
            const uint16_t local = mesh.skin->joint_indices[k];
            const int32_t target =
                local < remap.size() ? remap[local] : -1;
            if (target >= 0 && target <= 65535) {
              mesh.skin->joint_indices.mutable_at(k) = static_cast<uint16_t>(target);
            } else {
              mesh.skin->joint_indices.mutable_at(k) = 0;
              if (k < mesh.skin->joint_weights.size()) {
                mesh.skin->joint_weights.mutable_at(k) = 0.0f;
              }
            }
          }
        }
      }
    }

    ResolveSkeletalAnimationTargets(&result.scene);

    ResolveLightLinking(stage, &result.scene);
    extracted.release_storage();

    AppendDegradationDiagnostics(&result.scene);
    result.scene.conversion_diagnostics.insert(result.scene.conversion_diagnostics.end(),
        conversion_diagnostics_.begin(), conversion_diagnostics_.end());
    NormalizeConversionDiagnostics(&result.scene.conversion_diagnostics);
    if (config_.strict_conversion && !warnings_.empty()) {
      result.status = ::lightusd::next::OperationStatus::InvalidData;
      result.error = "strict render conversion rejected warnings: " + warnings_.front();
      result.warnings = std::move(warnings_);
      return result;
    }
    if (result.scene.memory_usage() > config_.limits.max_resident_bytes) {
      result.status = ::lightusd::next::OperationStatus::ResourceLimit;
      result.error = "converted render scene exceeds resident memory limit";
      result.warnings = std::move(warnings_);
      return result;
    }

    if (config_.progress_callback) {
      config_.progress_callback(1.0f, "Conversion complete");
    }

    result.success = true;
    result.status = ::lightusd::next::OperationStatus::Ok;
    result.warnings = std::move(warnings_);
  }

  return result;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
