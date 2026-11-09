// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment, Inc.
//
// Scene traversal and mesh dispatch split from material conversion.

#include "common-macros.inc"
#include "core/prim.hh"
#include "pprinter.hh"
#include "usdGeom.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/scene-access.hh"

#include <chrono>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace tydra {

namespace {

using TydraPerfClock = std::chrono::steady_clock;
static uint64_t ElapsedNs(const TydraPerfClock::time_point &start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      TydraPerfClock::now() - start).count());
}

std::vector<const lightusd::GeomSubset *> GetMaterialBindGeomSubsets(
    const lightusd::Prim &prim) {
  std::vector<const lightusd::GeomSubset *> dst;
  for (const auto &child : prim.children()) {
    const lightusd::GeomSubset *subset = child.as<lightusd::GeomSubset>();
    if (!subset) continue;
    value::token family;
    if (subset->familyName.get_value(&family) && family.str() == "materialBind") {
      dst.push_back(subset);
    }
  }
  return dst;
}

}  // namespace

bool MeshVisitor(const lightusd::Path &abs_path, const lightusd::Prim &prim,
                 const int32_t level, void *userdata, std::string *err) {
  if (!userdata) {
    if (err) {
      (*err) += "userdata pointer must be filled.";
    }
    return false;
  }

  MeshVisitorEnv *visitorEnv = reinterpret_cast<MeshVisitorEnv *>(userdata);

  if (size_t(level) > kMaxDefaultTraversalLimit) {
    if (err) {
      (*err) += "Scene graph is too deep.\n";
    }
    // Too deep
    return false;
  }

  // Lambda to convert and cache bound materials - shared by all geometry types
  auto ConvertBoundMaterial = [&](const Path &bound_material_path,
                                  const lightusd::Material *bound_material,
                                  int64_t &rmaterial_id) -> bool {
    std::vector<RenderMaterial> &rmaterials =
        visitorEnv->converter->materials;

    const auto matIt = visitorEnv->converter->materialMap.find(
        bound_material_path.full_path_name());

    if (matIt != visitorEnv->converter->materialMap.s_end()) {
      // Got material in the cache.
      uint64_t mat_id = matIt->second;
      if (mat_id >= visitorEnv->converter->materials
                        .size()) {  // this should not happen though
        if (err) {
          (*err) += "Material index out-of-range.\n";
        }
        return false;
      }

      if (mat_id >= size_t((std::numeric_limits<int32_t>::max)())) {
        if (err) {
          (*err) += "Material index too large.\n";
        }
        return false;
      }

      rmaterial_id = int64_t(mat_id);

    } else {
      RenderSceneConverter *conv = visitorEnv->converter;

      // Record the current sizes so we can stream the buffers/images/textures
      // this material conversion appends (no-op without a streaming sink).
      const size_t buf_begin = conv->buffers.size();
      const size_t img_begin = conv->images.size();
      const size_t tex_begin = conv->textures.size();
      const size_t udim_begin = conv->udim_textures.size();

      std::string source_signature;
      if (visitorEnv->env->scene_config.dedup_materials_by_texture_identity &&
          conv->BuildMaterialSourceSignature(*visitorEnv->env,
                                             bound_material_path,
                                             *bound_material,
                                             &source_signature)) {
        int64_t cached_material_id = -1;
        if (conv->FindMaterialSourceSignature(source_signature,
                                              &cached_material_id)) {
          if (cached_material_id < 0 ||
              size_t(cached_material_id) >= rmaterials.size()) {
            if (err) {
              (*err) += "Material source signature cache index out-of-range.\n";
            }
            return false;
          }
          rmaterial_id = cached_material_id;
          visitorEnv->converter->materialMap.add(
              bound_material_path.full_path_name(), uint64_t(rmaterial_id));
          visitorEnv->material_cache_hits++;
          return true;
        }
      }

      RenderMaterial rmat;
      const auto material_start = TydraPerfClock::now();
      if (!conv->ConvertMaterial(*visitorEnv->env, bound_material_path,
                                 *bound_material, &rmat)) {
        if (!visitorEnv->env->material_config.assign_default_material ||
            visitorEnv->env->material_config.strict_material_check) {
          if (err) {
            (*err) += fmt::format("Material conversion failed: {}",
                                  bound_material_path);
          }
          return false;
        }
        int default_material_id = -1;
        if (!conv->GetOrCreateDefaultMaterial(*visitorEnv->env,
                                              &default_material_id)) {
          if (err) {
            (*err) += fmt::format("Material conversion failed: {}",
                                  bound_material_path);
          }
          return false;
        }
        if (default_material_id < 0) {
          if (err) {
            (*err) += fmt::format("Material conversion failed: {}",
                                  bound_material_path);
          }
          return false;
        }
        const std::string material_error = conv->GetError();
        conv->AddWarning(fmt::format(
            "Material conversion failed for {}; using default material. {}",
            bound_material_path.full_path_name(), material_error));
        conv->ClearError();
        conv->materialMap.add(bound_material_path.full_path_name(),
                              uint64_t(default_material_id));
        rmaterial_id = int64_t(default_material_id);
        return true;
      }
      visitorEnv->convert_material_ns += ElapsedNs(material_start);
      visitorEnv->material_cache_misses++;

      // Assign new material ID
      uint64_t mat_id = rmaterials.size();

      if (mat_id >= uint64_t((std::numeric_limits<int32_t>::max)())) {
        if (err) {
          (*err) += "Material index too large.\n";
        }
        return false;
      }
      rmaterial_id = int64_t(mat_id);

      visitorEnv->converter->materialMap.add(
          bound_material_path.full_path_name(), uint64_t(rmaterial_id));
      if (!source_signature.empty()) {
        visitorEnv->converter->RememberMaterialSourceSignature(
            source_signature, rmaterial_id);
      }
      // Compute material tag for render pass sorting (opaque/translucent/masked)
      rmat.computeMaterialTag();

      DCOUT("Added renderMaterial: " << mat_id << " " << rmat.abs_path
                                     << " ( " << rmat.name << " ) ");

      rmaterials.push_back(rmat);

      // Stream the newly produced GPU dependencies then the material itself, in
      // dependency order (buffers -> images -> textures -> udim -> material), so
      // a consumer's material references already-delivered textures/images.
      if (conv->HasStreamingSink()) {
        for (size_t i = buf_begin; i < conv->buffers.size(); i++) {
          if (!conv->EmitBuffer(i)) {
            if (err) (*err) += "Conversion cancelled by user.\n";
            return false;
          }
        }
        for (size_t i = img_begin; i < conv->images.size(); i++) {
          if (!conv->EmitImage(i)) {
            if (err) (*err) += "Conversion cancelled by user.\n";
            return false;
          }
        }
        for (size_t i = tex_begin; i < conv->textures.size(); i++) {
          if (!conv->EmitTexture(i, conv->textures[i].abs_path)) {
            if (err) (*err) += "Conversion cancelled by user.\n";
            return false;
          }
        }
        for (size_t i = udim_begin; i < conv->udim_textures.size(); i++) {
          if (!conv->EmitUdimTexture(i)) {
            if (err) (*err) += "Conversion cancelled by user.\n";
            return false;
          }
        }
        if (!conv->EmitMaterial(size_t(rmaterial_id),
                                bound_material_path.full_path_name())) {
          if (err) (*err) += "Conversion cancelled by user.\n";
          return false;
        }
      }
      return true;
    }

    visitorEnv->material_cache_hits++;
    return true;
  };

  auto ResolveBoundMaterial = [&](const Path &query_path,
                                  const std::string &purpose,
                                  Path *bound_material_path,
                                  const Material **bound_material,
                                  bool *found) -> bool {
    if (!found) {
      return false;
    }

    visitorEnv->material_resolve_calls++;
    std::string local_err;
    const auto resolve_start = TydraPerfClock::now();
    bool local_found = visitorEnv->converter->GetBoundMaterialCached(
        visitorEnv->env->stage, query_path, purpose, bound_material_path,
        bound_material, &local_err);
    visitorEnv->resolve_material_ns += ElapsedNs(resolve_start);

    if (!local_err.empty()) {
      if (err) {
        (*err) += local_err;
      }
      return false;
    }

    (*found) = local_found;
    if (local_found) {
      visitorEnv->material_resolve_found++;
    }
    return true;
  };

  if (const lightusd::GeomMesh *pmesh = prim.as<lightusd::GeomMesh>()) {
    // Collect GeomSubsets
    // std::vector<const lightusd::GeomSubset *> subsets = GetGeomSubsets(;

    DCOUT("Mesh: " << abs_path);

    if (!pmesh->points.authored()) {
      // Maybe Collider mesh? Ignore for now.
      DCOUT(fmt::format("Mesh {} does not author `points` attribute(Maybe Collider mesh?). Ignore it for now", abs_path));
      return true;
    }

    //
    // First convert Material assigned to GeomMesh.
    //
    // - If prim has GeomSubset with materialBind, convert it to per-face
    // material.
    // - If prim has materialBind, convert it to RenderMesh's material.
    //

    // Convert bound materials in GeomSubsets
    //
    // key: subset Prim name
    std::map<std::string, MaterialPath> subset_material_path_map;
    std::vector<const GeomSubset *> material_subsets;
    {
      material_subsets = GetMaterialBindGeomSubsets(prim);

      for (const auto &psubset : material_subsets) {
        MaterialPath mpath;
        mpath.default_texcoords_primvar_name =
            visitorEnv->env->mesh_config.default_texcoords_primvar_name;

        Path subset_abs_path = abs_path.AppendElement(psubset->name);

        // front and back
        {
          lightusd::Path bound_material_path;
          const lightusd::Material *bound_material{nullptr};
          bool ret{false};
          if (!ResolveBoundMaterial(
                  /* GeomSubset prim path */ subset_abs_path,
                  /* purpose */ "", &bound_material_path, &bound_material,
                  &ret)) {
            return false;
          }

          if (ret && bound_material) {
            int64_t rmaterial_id = -1;  // not used.

            if (!ConvertBoundMaterial(bound_material_path, bound_material,
                                      rmaterial_id)) {
              if (err) {
                (*err) += "Convert boundMaterial failed: " + bound_material_path.full_path_name();
              }
              return false;
            }

            mpath.material_path = bound_material_path.full_path_name();
            DCOUT("GeomSubset " << subset_abs_path << " : Bound material path: "
                                << mpath.backface_material_path);
          }
        }

        std::string backface_purpose =
            visitorEnv->env->material_config
                .default_backface_material_purpose_name;

        if (!backface_purpose.empty() &&
            psubset->has_materialBinding(value::token(backface_purpose))) {
          DCOUT("backface_material_purpose "
                << visitorEnv->env->material_config
                       .default_backface_material_purpose_name);
          lightusd::Path bound_material_path;
          const lightusd::Material *bound_material{nullptr};
          bool ret{false};
          if (!ResolveBoundMaterial(
                  /* GeomSubset prim path */ subset_abs_path,
                  /* purpose */
                  visitorEnv->env->material_config
                      .default_backface_material_purpose_name,
                  &bound_material_path, &bound_material, &ret)) {
            return false;
          }

          if (ret && bound_material) {
            int64_t rmaterial_id = -1;  // not used

            if (!ConvertBoundMaterial(bound_material_path, bound_material,
                                      rmaterial_id)) {
              if (err) {
                (*err) += "Convert boundMaterial failed: " + bound_material_path.full_path_name();
              }
              return false;
            }

            mpath.backface_material_path = bound_material_path.full_path_name();
            DCOUT("GeomSubset " << subset_abs_path
                                << " : Bound backface material path: "
                                << mpath.backface_material_path);
          }
        }

        subset_material_path_map[psubset->name] = mpath;
      }
    }

    MaterialPath material_path;
    material_path.default_texcoords_primvar_name =
        visitorEnv->env->mesh_config.default_texcoords_primvar_name;

    {
      const std::string mesh_path_str = abs_path.full_path_name();

      // Front and back material.
      {
        lightusd::Path bound_material_path;
        const lightusd::Material *bound_material{nullptr};
        bool ret{false};
        if (!ResolveBoundMaterial(
                /* GeomMesh prim path */ abs_path,
                /* purpose */ "", &bound_material_path, &bound_material,
                &ret)) {
          return false;
        }

        DCOUT("Bound material found: " << ret);
        if (ret && bound_material) {
          int64_t rmaterial_id = -1;  // not used

          if (!ConvertBoundMaterial(bound_material_path, bound_material,
                                    rmaterial_id)) {
            if (err) {
              (*err) += "Convert boundMaterial failed: " + bound_material_path.full_path_name();
            }
            return false;
          }

          material_path.material_path = bound_material_path.full_path_name();
          DCOUT("Bound material path: " << material_path.material_path);
        }
      }

      std::string backface_purpose =
          visitorEnv->env->material_config
              .default_backface_material_purpose_name;

      if (!backface_purpose.empty() &&
          pmesh->has_materialBinding(value::token(backface_purpose))) {
        lightusd::Path bound_material_path;
        const lightusd::Material *bound_material{nullptr};
        bool ret{false};
        if (!ResolveBoundMaterial(
                /* GeomMesh prim path */ abs_path,
                /* purpose */
                visitorEnv->env->material_config
                    .default_backface_material_purpose_name,
                &bound_material_path, &bound_material, &ret)) {
          return false;
        }

        if (ret && bound_material) {
          int64_t rmaterial_id = -1;  // not used

          if (!ConvertBoundMaterial(bound_material_path, bound_material,
                                    rmaterial_id)) {
            if (err) {
              (*err) += "Convert boundMaterial failed: " + bound_material_path.full_path_name();
            }
            return false;
          }

          material_path.backface_material_path =
              bound_material_path.full_path_name();
          DCOUT("Bound backface material path: "
                << material_path.backface_material_path);
        }
      }

      if (material_path.material_path.empty()) {
        if (!visitorEnv->converter->GetOrCreateDefaultMaterial(
                *visitorEnv->env, &material_path.default_material_id)) {
          return false;
        }
      }
      if (material_path.backface_material_path.empty()) {
        material_path.default_backface_material_id =
            material_path.default_material_id;
      }

      // BlendShapes
      std::vector<std::pair<std::string, const BlendShape *>> blendshapes;
      {
        std::string local_err;
        blendshapes = GetBlendShapes(visitorEnv->env->stage, prim, &local_err);
        if (local_err.size()) {
          if (err) {
            (*err) += fmt::format("Failed to get BlendShapes prims. err = {}", local_err);
          }
          return false;
        }
      }
      DCOUT("# of blendshapes : " << blendshapes.size());

      // Skinned meshes must not run on workers (skeleton registration has to
      // stay serial + traversal-ordered), so they are tagged and converted by
      // the main thread inside the ordered merge phase instead.
      const bool skinned = pmesh->skeleton.has_value() ||
                           pmesh->has_primvar("skel:jointIndices") ||
                           pmesh->has_primvar("skel:jointWeights") ||
                           pmesh->has_primvar("skel:joints") ||
                           pmesh->has_primvar("skel:geomBindTransform");

      if (visitorEnv->work_items) {
        // Collect mode: record the fully-resolved inputs for deferred
        // geometry conversion. Materials have already been converted above,
        // so materialMap is complete before any worker runs.
        MeshWorkItem wi;
        wi.kind = MeshWorkItem::Kind::Mesh;
        wi.type_name = "mesh";
        wi.abs_path = abs_path;
        wi.prim = pmesh;
        wi.material_path = material_path;
        wi.subset_material_path_map = subset_material_path_map;
        wi.material_subsets = material_subsets;
        wi.blendshapes = std::move(blendshapes);
        wi.convert_serially = skinned || visitorEnv->force_serial_conversion;
        visitorEnv->work_items->push_back(std::move(wi));
        return true;  // continue traversal
      }

      RenderMesh rmesh;

      const auto mesh_start = TydraPerfClock::now();
      if (!visitorEnv->converter->ConvertMesh(
              *visitorEnv->env, abs_path, *pmesh, material_path,
              subset_material_path_map, visitorEnv->converter->materialMap,
              material_subsets, blendshapes, &rmesh)) {
        if (err) {
          (*err) += fmt::format("Mesh conversion failed: {}",
                                abs_path.full_path_name());
          (*err) += "\n" + visitorEnv->converter->GetError() + "\n";

        }
        return false;
      }
      visitorEnv->convert_mesh_ns += ElapsedNs(mesh_start);

      uint64_t mesh_id = uint64_t(visitorEnv->converter->meshes.size());
      if (mesh_id >= size_t((std::numeric_limits<int32_t>::max)())) {
        if (err) {
          (*err) += "Mesh index too large.\n";
        }
        return false;
      }
      visitorEnv->converter->meshMap.add(abs_path.full_path_name(), mesh_id);

      visitorEnv->converter->meshes.emplace_back(std::move(rmesh));

      // Report mesh progress
      visitorEnv->meshes_processed++;
      std::string msg = "Converting mesh " +
          std::to_string(visitorEnv->meshes_processed) + "/" +
          std::to_string(visitorEnv->meshes_total);
      const auto progress_start = TydraPerfClock::now();
      if (!visitorEnv->converter->ReportMeshProgress(
              visitorEnv->meshes_processed, visitorEnv->meshes_total,
              abs_path.full_path_name(), msg)) {
        if (err) {
          (*err) += "Conversion cancelled by user.\n";
        }
        return false;
      }
      visitorEnv->progress_ns += ElapsedNs(progress_start);

      // Stream the just-converted mesh (LOCAL space; world placement arrives in
      // the Hierarchy phase). No-op without a streaming sink.
      if (!visitorEnv->converter->EmitMesh(size_t(mesh_id),
                                           abs_path.full_path_name())) {
        if (err) {
          (*err) += "Conversion cancelled by user.\n";
        }
        return false;
      }
      DCOUT("[Tydra] Mesh " << visitorEnv->meshes_processed << "/" << visitorEnv->meshes_total
            << ": " << abs_path.full_path_name());
    }
  }

  // Handle GeomCube primitives by converting to mesh
  if (const lightusd::GeomCube *pcube = prim.as<lightusd::GeomCube>()) {
    DCOUT("Cube: " << abs_path);

    // Get material binding (same logic as GeomMesh)
    MaterialPath material_path;
    std::map<std::string, MaterialPath> subset_material_path_map;

    {
      const Material *bound_material{nullptr};
      Path bound_material_path;

      bool ret{false};
      if (!ResolveBoundMaterial(abs_path,
                                /* purpose */ "",
                                &bound_material_path, &bound_material,
                                &ret)) {
        return false;
      }

      if (ret && bound_material) {
        int64_t rmaterial_id = -1;

        if (!ConvertBoundMaterial(
                bound_material_path, bound_material, rmaterial_id)) {
          if (err) {
            (*err) += "Convert boundMaterial failed: " +
                      bound_material_path.full_path_name();
          }
          return false;
        }

        material_path.material_path = bound_material_path.full_path_name();
        DCOUT("Bound material path: " << material_path.material_path);
      }
    }

    if (visitorEnv->work_items) {
      // Collect mode: defer cube geometry conversion (see GeomMesh branch).
      MeshWorkItem wi;
      wi.kind = MeshWorkItem::Kind::Cube;
      wi.type_name = "cube";
      wi.abs_path = abs_path;
      wi.prim = pcube;
      wi.material_path = material_path;
      visitorEnv->work_items->push_back(std::move(wi));
      return true;  // continue traversal
    }

    RenderMesh rmesh;
    std::vector<const lightusd::GeomSubset *> material_subsets;  // Cubes don't have subsets
    std::vector<std::pair<std::string, const lightusd::BlendShape *>> blendshapes;  // Cubes don't have blendshapes

    if (!visitorEnv->converter->ConvertCube(
            *visitorEnv->env, abs_path, *pcube, material_path,
            subset_material_path_map, visitorEnv->converter->materialMap,
            material_subsets, blendshapes, &rmesh)) {
      if (err) {
        (*err) += fmt::format("Cube conversion failed: {}",
                              abs_path.full_path_name());
        (*err) += "\n" + visitorEnv->converter->GetError() + "\n";
      }
      return false;
    }

    uint64_t mesh_id = uint64_t(visitorEnv->converter->meshes.size());
    if (mesh_id >= size_t((std::numeric_limits<int32_t>::max)())) {
      if (err) {
        (*err) += "Mesh index too large.\n";
      }
      return false;
    }
    visitorEnv->converter->meshMap.add(abs_path.full_path_name(), mesh_id);
    visitorEnv->converter->meshes.emplace_back(std::move(rmesh));

    // Report mesh progress (cube)
    visitorEnv->meshes_processed++;
    std::string msg = "Converting cube " +
        std::to_string(visitorEnv->meshes_processed) + "/" +
        std::to_string(visitorEnv->meshes_total);
    if (!visitorEnv->converter->ReportMeshProgress(
            visitorEnv->meshes_processed, visitorEnv->meshes_total,
            abs_path.full_path_name(), msg)) {
      if (err) {
        (*err) += "Conversion cancelled by user.\n";
      }
      return false;
    }
    if (!visitorEnv->converter->EmitMesh(size_t(mesh_id),
                                         abs_path.full_path_name())) {
      if (err) {
        (*err) += "Conversion cancelled by user.\n";
      }
      return false;
    }
    DCOUT("[Tydra] Mesh " << visitorEnv->meshes_processed << "/" << visitorEnv->meshes_total
          << " (cube): " << abs_path.full_path_name());
  }

  // Handle GeomSphere primitives by converting to mesh
  if (const lightusd::GeomSphere *psphere = prim.as<lightusd::GeomSphere>()) {
    DCOUT("Sphere: " << abs_path);

    // Get material binding (same logic as GeomMesh)
    MaterialPath material_path;
    std::map<std::string, MaterialPath> subset_material_path_map;

    {
      const Material *bound_material{nullptr};
      Path bound_material_path;

      bool ret{false};
      if (!ResolveBoundMaterial(abs_path,
                                /* purpose */ "",
                                &bound_material_path, &bound_material,
                                &ret)) {
        return false;
      }

      if (ret && bound_material) {
        int64_t rmaterial_id = -1;

        if (!ConvertBoundMaterial(
                bound_material_path, bound_material, rmaterial_id)) {
          if (err) {
            (*err) += "Convert boundMaterial failed: " +
                      bound_material_path.full_path_name();
          }
          return false;
        }

        material_path.material_path = bound_material_path.full_path_name();
        DCOUT("Bound material path: " << material_path.material_path);
      }
    }

    if (visitorEnv->work_items) {
      // Collect mode: defer sphere geometry conversion (see GeomMesh branch).
      MeshWorkItem wi;
      wi.kind = MeshWorkItem::Kind::Sphere;
      wi.type_name = "sphere";
      wi.abs_path = abs_path;
      wi.prim = psphere;
      wi.material_path = material_path;
      visitorEnv->work_items->push_back(std::move(wi));
      return true;  // continue traversal
    }

    RenderMesh rmesh;
    std::vector<const lightusd::GeomSubset *> material_subsets;  // Spheres don't have subsets
    std::vector<std::pair<std::string, const lightusd::BlendShape *>> blendshapes;  // Spheres don't have blendshapes

    if (!visitorEnv->converter->ConvertSphere(
            *visitorEnv->env, abs_path, *psphere, material_path,
            subset_material_path_map, visitorEnv->converter->materialMap,
            material_subsets, blendshapes, &rmesh)) {
      if (err) {
        (*err) += fmt::format("Sphere conversion failed: {}",
                              abs_path.full_path_name());
        (*err) += "\n" + visitorEnv->converter->GetError() + "\n";
      }
      return false;
    }

    uint64_t mesh_id = uint64_t(visitorEnv->converter->meshes.size());
    if (mesh_id >= size_t((std::numeric_limits<int32_t>::max)())) {
      if (err) {
        (*err) += "Mesh index too large.\n";
      }
      return false;
    }
    visitorEnv->converter->meshMap.add(abs_path.full_path_name(), mesh_id);
    visitorEnv->converter->meshes.emplace_back(std::move(rmesh));

    // Report mesh progress (sphere)
    visitorEnv->meshes_processed++;
    std::string msg = "Converting sphere " +
        std::to_string(visitorEnv->meshes_processed) + "/" +
        std::to_string(visitorEnv->meshes_total);
    if (!visitorEnv->converter->ReportMeshProgress(
            visitorEnv->meshes_processed, visitorEnv->meshes_total,
            abs_path.full_path_name(), msg)) {
      if (err) {
        (*err) += "Conversion cancelled by user.\n";
      }
      return false;
    }
    if (!visitorEnv->converter->EmitMesh(size_t(mesh_id),
                                         abs_path.full_path_name())) {
      if (err) {
        (*err) += "Conversion cancelled by user.\n";
      }
      return false;
    }
    DCOUT("[Tydra] Mesh " << visitorEnv->meshes_processed << "/" << visitorEnv->meshes_total
          << " (sphere): " << abs_path.full_path_name());
  }

  // Helper lambda for parametric primitive conversion (Cylinder, Cone, Capsule, Plane)
  auto convertParamPrim = [&](auto *pprim, const char *primTypeName,
                              MeshWorkItem::Kind wi_kind, auto convertFunc) -> bool {
    DCOUT(primTypeName << ": " << abs_path);

    MaterialPath material_path;
    std::map<std::string, MaterialPath> subset_material_path_map;

    {
      const Material *bound_material{nullptr};
      Path bound_material_path;
      bool ret{false};
      if (!ResolveBoundMaterial(abs_path, "", &bound_material_path, &bound_material, &ret)) {
        return false;
      }
      if (ret && bound_material) {
        int64_t rmaterial_id = -1;
        if (!ConvertBoundMaterial(bound_material_path, bound_material, rmaterial_id)) {
          if (err) {
            (*err) += "Convert boundMaterial failed: " + bound_material_path.full_path_name();
          }
          return false;
        }
        material_path.material_path = bound_material_path.full_path_name();
      }
    }

    if (visitorEnv->work_items) {
      // Collect mode: defer parametric geometry conversion (see GeomMesh
      // branch).
      MeshWorkItem wi;
      wi.kind = wi_kind;
      wi.type_name = primTypeName;
      wi.abs_path = abs_path;
      wi.prim = pprim;
      wi.material_path = material_path;
      visitorEnv->work_items->push_back(std::move(wi));
      return true;  // continue traversal
    }

    RenderMesh rmesh;
    std::vector<const lightusd::GeomSubset *> material_subsets;
    std::vector<std::pair<std::string, const lightusd::BlendShape *>> blendshapes;

    if (!(visitorEnv->converter->*convertFunc)(
            *visitorEnv->env, abs_path, *pprim, material_path,
            subset_material_path_map, visitorEnv->converter->materialMap,
            material_subsets, blendshapes, &rmesh)) {
      if (err) {
        (*err) += fmt::format("{} conversion failed: {}", primTypeName, abs_path.full_path_name());
        (*err) += "\n" + visitorEnv->converter->GetError() + "\n";
      }
      return false;
    }

    uint64_t mesh_id = uint64_t(visitorEnv->converter->meshes.size());
    if (mesh_id >= size_t((std::numeric_limits<int32_t>::max)())) {
      if (err) { (*err) += "Mesh index too large.\n"; }
      return false;
    }
    visitorEnv->converter->meshMap.add(abs_path.full_path_name(), mesh_id);
    visitorEnv->converter->meshes.emplace_back(std::move(rmesh));

    visitorEnv->meshes_processed++;
    std::string msg = std::string("Converting ") + primTypeName + " " +
        std::to_string(visitorEnv->meshes_processed) + "/" +
        std::to_string(visitorEnv->meshes_total);
    if (!visitorEnv->converter->ReportMeshProgress(
            visitorEnv->meshes_processed, visitorEnv->meshes_total,
            abs_path.full_path_name(), msg)) {
      if (err) { (*err) += "Conversion cancelled by user.\n"; }
      return false;
    }
    if (!visitorEnv->converter->EmitMesh(size_t(mesh_id), abs_path.full_path_name())) {
      if (err) { (*err) += "Conversion cancelled by user.\n"; }
      return false;
    }
    DCOUT("[Tydra] Mesh " << visitorEnv->meshes_processed << "/" << visitorEnv->meshes_total
          << " (" << primTypeName << "): " << abs_path.full_path_name());
    return true;
  };

  // Handle GeomCylinder primitives
  if (const lightusd::GeomCylinder *pcyl = prim.as<lightusd::GeomCylinder>()) {
    if (!convertParamPrim(pcyl, "cylinder", MeshWorkItem::Kind::Cylinder,
                          &RenderSceneConverter::ConvertCylinder)) {
      return false;
    }
  }

  // Handle GeomCone primitives
  if (const lightusd::GeomCone *pcone = prim.as<lightusd::GeomCone>()) {
    if (!convertParamPrim(pcone, "cone", MeshWorkItem::Kind::Cone,
                          &RenderSceneConverter::ConvertCone)) {
      return false;
    }
  }

  // Handle GeomCapsule primitives
  if (const lightusd::GeomCapsule *pcap = prim.as<lightusd::GeomCapsule>()) {
    if (!convertParamPrim(pcap, "capsule", MeshWorkItem::Kind::Capsule,
                          &RenderSceneConverter::ConvertCapsule)) {
      return false;
    }
  }

  // Handle GeomPlane primitives
  if (const lightusd::GeomPlane *pplane = prim.as<lightusd::GeomPlane>()) {
    if (!convertParamPrim(pplane, "plane", MeshWorkItem::Kind::Plane,
                          &RenderSceneConverter::ConvertPlane)) {
      return false;
    }
  }

  return true;  // continue traversal
}

}  // namespace tydra
}  // namespace lightusd
