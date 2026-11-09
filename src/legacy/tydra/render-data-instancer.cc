// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file render-data-instancer.cc
/// @brief UsdGeomPointInstancer expansion for RenderSceneConverter.
///
/// Preliminary instancing support: expands each PointInstancer into
/// RenderScene::instances (one RenderInstance per visible instance). Geometry
/// is shared — each RenderInstance references a prototype RenderMesh by
/// `mesh_id` rather than duplicating mesh data.
///
/// Known limitations (preliminary):
///  - velocities/accelerations/angularVelocities are ignored (no motion blur).
///  - orientations sampled with Held interpolation (no slerp).
///  - prototype prims under the instancer are also emitted as ordinary meshes
///    (drawn once at their authored location in addition to the instances).
///  - nested instancing (a prototype that itself contains a PointInstancer or
///    an instanceable prim) is not recursively expanded.
///

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common-utils.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "usdGeom.hh"
#include "value-types.hh"
#include "xform.hh"

#include "common-macros.inc"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"

namespace lightusd {
namespace tydra {

namespace {

std::vector<float> ReadFloatPrimvarArray(const GPrim &prim,
                                         const std::string &name,
                                         double time) {
  GeomPrimvar pv;
  if (!prim.get_primvar(name, &pv)) return {};
  value::Value value;
  std::string err;
  if (!pv.flatten_with_indices(time, &value,
                               value::TimeSampleInterpolationType::Linear,
                               &err)) {
    return {};
  }
  if (const auto *floats = value.as<std::vector<float>>()) {
    return *floats;
  }
  return {};
}

}  // namespace

bool RenderSceneConverter::ExpandPointInstancer(
    const RenderSceneConverterEnv &env, const std::string &instancer_abs_path,
    const GeomPointInstancer &pi, const value::matrix4d &instancer_world,
    const std::unordered_map<std::string, value::matrix4d> &path_to_global) {
  //
  // 1. Resolve the `prototypes` relationship into ordered target paths.
  //    protoIndices[i] indexes into this ordered list.
  //
  std::vector<std::string> proto_paths;
  if (pi.prototypes.has_value()) {
    const Relationship &rel = pi.prototypes.value();
    if (rel.is_path()) {
      proto_paths.push_back(rel.targetPath.prim_part());
    } else if (rel.is_pathvector()) {
      for (const auto &p : rel.targetPathVector) {
        proto_paths.push_back(p.prim_part());
      }
    }
  }

  if (proto_paths.empty()) {
    PushWarn("PointInstancer <" + instancer_abs_path +
             "> has no `prototypes` targets; skipping expansion.");
    return true;
  }

  //
  // 2. For each prototype, gather its prototype-root RenderMesh(es) and each
  //    mesh's transform relative to the prototype root.
  //    A mesh belongs to prototype `p` if its path equals the prototype path or
  //    is a descendant of it.
  //
  struct ProtoMesh {
    int32_t mesh_id{-1};
    value::matrix4d proto_rel{value::matrix4d::identity()};  // mesh-local -> proto-root
  };
  std::vector<std::vector<ProtoMesh>> proto_meshes(proto_paths.size());

  for (size_t p = 0; p < proto_paths.size(); p++) {
    const std::string &proto_path = proto_paths[p];

    // Prototype-root world transform (identity if not in the node tree).
    value::matrix4d proto_root_world = value::matrix4d::identity();
    {
      auto it = path_to_global.find(proto_path);
      if (it != path_to_global.end()) proto_root_world = it->second;
    }
    value::matrix4d inv_proto_root = value::matrix4d::identity();
    const bool invertible = inverse(proto_root_world, inv_proto_root);

    for (auto it = meshMap.s_begin(); it != meshMap.s_end(); ++it) {
      const std::string &mesh_path = it->first;
      const bool is_exact = (mesh_path == proto_path);
      const bool is_descendant =
          (mesh_path.size() > proto_path.size() &&
           mesh_path.compare(0, proto_path.size(), proto_path) == 0 &&
           mesh_path[proto_path.size()] == '/');
      if (!is_exact && !is_descendant) continue;

      ProtoMesh pm;
      pm.mesh_id = static_cast<int32_t>(it->second);
      if (is_descendant && invertible) {
        // mesh-local -> proto-root = mesh_world * inverse(proto_root_world)
        value::matrix4d mesh_world = value::matrix4d::identity();
        auto mit = path_to_global.find(mesh_path);
        if (mit != path_to_global.end()) mesh_world = mit->second;
        pm.proto_rel = value::Mult(mesh_world, inv_proto_root);
      }
      proto_meshes[p].push_back(pm);
    }

    if (proto_meshes[p].empty()) {
      PushWarn("PointInstancer <" + instancer_abs_path + "> prototype <" +
               proto_path + "> resolved to no RenderMesh; its instances are skipped.");
    }
  }

  //
  // 3. Compute per-instance transforms and visibility mask at env.timecode.
  //
  std::vector<value::matrix4d> inst_xforms;
  std::string err;
  if (!ComputeInstanceTransformsAtTime(
          pi, env.timecode, value::TimeSampleInterpolationType::Linear,
          &inst_xforms, &err, /* proto_xforms */ nullptr)) {
    PushWarn("Failed to compute instance transforms for PointInstancer <" +
             instancer_abs_path + ">: " + err);
    return false;
  }

  std::vector<bool> mask;
  ComputeMaskAtTime(pi, env.timecode, &mask, &err);

  const std::vector<int32_t> protoIndices = pi.get_protoIndices(env.timecode);
  const size_t n = inst_xforms.size();
  const std::vector<value::color3f> display_colors =
      pi.get_displayColors(env.timecode);
  const std::vector<float> display_opacities =
      ReadFloatPrimvarArray(pi, "displayOpacity", env.timecode);

  int instancer_material_id = -1;
  {
    Path bound_material_path;
    const Material *bound_material{nullptr};
    std::string material_err;
    if (GetBoundMaterialCached(env.stage, Path(instancer_abs_path, ""),
                               /* purpose */ "", &bound_material_path,
                               &bound_material, &material_err)) {
      const std::string material_path = bound_material_path.full_path_name();
      if (auto mat_it = materialMap.find(material_path);
          mat_it != materialMap.s_end()) {
        instancer_material_id = int(mat_it->second);
      }
    }
  }

  // Instancer prim name (basename) for labeling.
  std::string instancer_name = instancer_abs_path;
  {
    size_t s = instancer_abs_path.rfind('/');
    if (s != std::string::npos) instancer_name = instancer_abs_path.substr(s + 1);
  }

  //
  // 4. Emit one RenderInstance per (visible instance x prototype mesh).
  //
  size_t emitted = 0;
  for (size_t i = 0; i < n; i++) {
    if (i < mask.size() && !mask[i]) continue;  // masked / invisible
    if (i >= protoIndices.size()) break;

    const int32_t pidx = protoIndices[i];
    if (pidx < 0 || static_cast<size_t>(pidx) >= proto_meshes.size()) {
      PushWarn(fmt::format(
          "PointInstancer <{}> instance {} has protoIndex {} out of range "
          "[0, {}); skipped.",
          instancer_abs_path, i, pidx, proto_meshes.size()));
      continue;
    }

    for (const ProtoMesh &pm : proto_meshes[static_cast<size_t>(pidx)]) {
      RenderInstance rinst;
      rinst.prim_name = instancer_name + "[" + std::to_string(i) + "]";
      rinst.abs_path = instancer_abs_path + "/instance_" + std::to_string(i);
      rinst.prototype_index = pidx;
      rinst.mesh_id = pm.mesh_id;
      rinst.material_id = instancer_material_id;  // -1: inherit prototype mesh material
      if (display_colors.size() == n) {
        rinst.has_display_color = true;
        rinst.display_color = {{display_colors[i][0], display_colors[i][1],
                                display_colors[i][2]}};
      } else if (display_colors.size() == 1) {
        rinst.has_display_color = true;
        rinst.display_color = {{display_colors[0][0], display_colors[0][1],
                                display_colors[0][2]}};
      }
      if (display_opacities.size() == n) {
        rinst.has_display_opacity = true;
        rinst.display_opacity = display_opacities[i];
      } else if (display_opacities.size() == 1) {
        rinst.has_display_opacity = true;
        rinst.display_opacity = display_opacities[0];
      }

      // instance-space local = proto_rel * instance_SRT  (row-vector: local-first)
      const value::matrix4d local = value::Mult(pm.proto_rel, inst_xforms[i]);
      rinst.local_matrix = local;
      rinst.global_matrix = value::Mult(local, instancer_world);
      rinst.visible = true;

      instances.emplace_back(std::move(rinst));
      emitted++;
    }
  }

  DCOUT("[Tydra] PointInstancer <" << instancer_abs_path << "> expanded "
        << n << " instances into " << emitted << " RenderInstance(s).");
  (void)emitted;

  return true;
}

}  // namespace tydra
}  // namespace lightusd
