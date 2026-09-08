// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Render-node hierarchy construction and node-attached camera/light handling.

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "lightusd.hh"
#include "common-utils.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/render-data-material-internal.hh"
#include "tydra/scene-access.hh"
#include "tydra/materialx-to-json.hh"
#include "usdGeom.hh"
#include "usdLux.hh"
#include "usdVol.hh"
#include "value-types.hh"

namespace lightusd {
namespace tydra {

// Helper to get NodeCategory from NodeType
static NodeCategory GetNodeCategoryFromType(NodeType nodeType) {
  switch (nodeType) {
    case NodeType::Xform:
      return NodeCategory::Group;
    case NodeType::Volume:
      return NodeCategory::Geom;
    case NodeType::Mesh:
      return NodeCategory::Geom;
    case NodeType::Camera:
      return NodeCategory::Camera;
    case NodeType::SkelRoot:
    case NodeType::Skeleton:
      return NodeCategory::Skeleton;
    case NodeType::PointLight:
    case NodeType::DirectionalLight:
    case NodeType::EnvmapLight:
    case NodeType::RectLight:
    case NodeType::DiskLight:
    case NodeType::CylinderLight:
    case NodeType::GeometryLight:
      return NodeCategory::Light;
  }
  return NodeCategory::Group;  // Default
}

// Extract the common FieldAsset attributes (filePath / fieldName) from a
// field-asset prim (OpenVDBAsset / Field3DAsset / FieldAsset). Returns false if
// the prim is not a field-asset type.
static bool GetFieldAssetInfo(const lightusd::Prim &prim,
                              value::AssetPath *filePath,
                              std::string *fieldName) {
  const FieldAsset *fa = nullptr;
  if (const auto *openvdb_asset = prim.as<OpenVDBAsset>()) {
    fa = openvdb_asset;
  } else if (const auto *field3d_asset = prim.as<Field3DAsset>()) {
    fa = field3d_asset;
  } else if (const auto *field_asset = prim.as<FieldAsset>()) {
    fa = field_asset;
  }
  if (!fa) return false;

  if (auto fpv = fa->filePath.get_value()) {
    fpv.value().get_scalar(filePath);
  }
  if (auto fnv = fa->fieldName.get_value()) {
    value::token tk;
    if (fnv.value().get_scalar(&tk)) {
      *fieldName = tk.str();
    }
  }
  return true;
}

static const Attribute *FindShadeInput(const UsdShadePrim &shader,
                                       const char *name) {
  const auto it = shader.props.find(name);
  if (it == shader.props.end() || !it->second.is_attribute()) return nullptr;
  return it->second.get_attribute_or_null();
}

static const UsdShadePrim *GetShadeNodeData(const Prim *prim) {
  if (!prim) return nullptr;
  if (const Shader *shader = prim->as<Shader>()) {
    return shader->value.as<ShaderNode>();
  }
  if (const NodeGraph *graph = prim->as<NodeGraph>()) return graph;
  if (const Material *material = prim->as<Material>()) return material;
  return nullptr;
}

static const Attribute *ResolveShadeInput(const Stage &stage,
                                          const UsdShadePrim &node,
                                          const char *name, int depth = 0) {
  if (depth > 16) return nullptr;
  const Attribute *attr = FindShadeInput(node, name);
  if (!attr) return nullptr;
  if (!attr->has_connections()) return attr;
  const auto &connections = attr->connections();
  if (connections.size() != 1) return nullptr;
  const Path &target = connections[0];
  const Prim *target_prim = nullptr;
  if (!stage.find_prim_at_path(Path(target.prim_part(), ""), target_prim) ||
      !target_prim) return nullptr;
  const UsdShadePrim *target_node = GetShadeNodeData(target_prim);
  if (!target_node) return nullptr;
  const std::string property = target.prop_part();
  if (!property.empty()) {
    if (const Attribute *resolved = ResolveShadeInput(
            stage, *target_node, property.c_str(), depth + 1)) {
      return resolved;
    }
  }
  // Generic MaterialX constant nodes declare outputs:out without a value.
  if (const Shader *shader = target_prim->as<Shader>()) {
    if (shader->info_id.find("constant") != std::string::npos ||
        shader->info_id.find("Constant") != std::string::npos) {
      return ResolveShadeInput(stage, *target_node, "inputs:value", depth + 1);
    }
  }
  return nullptr;
}

static void ApplyVolumeShaderConstants(const Stage &stage,
                                       const UsdShadePrim &shader,
                                       float *density_scale,
                                       float albedo[3],
                                       float emission_color[3],
                                       float *emission_scale) {
  auto scalar = [&](const char *name, float *out) {
    if (const Attribute *source = FindShadeInput(shader, name)) {
      if (source->connections().size() == 1) {
        auto evaluated = EvaluateMtlxNodeGraphAsConstant(
            stage, source->connections()[0], "lin_rec709");
        if (evaluated && evaluated->n >= 1) {
          *out = evaluated->v[0];
          return;
        }
      }
    }
    const Attribute *attr = ResolveShadeInput(stage, shader, name);
    if (!attr || attr->has_timesamples()) return;
    if (auto v = attr->get_value<float>()) *out = v.value();
    else if (auto d = attr->get_value<double>()) *out = float(d.value());
  };
  auto color = [&](const char *name, float out[3]) {
    if (const Attribute *source = FindShadeInput(shader, name)) {
      if (source->connections().size() == 1) {
        auto evaluated = EvaluateMtlxNodeGraphAsConstant(
            stage, source->connections()[0], "lin_rec709");
        if (evaluated && evaluated->n >= 3) {
          out[0] = evaluated->v[0]; out[1] = evaluated->v[1];
          out[2] = evaluated->v[2];
          return;
        }
      }
    }
    const Attribute *attr = ResolveShadeInput(stage, shader, name);
    if (!attr || attr->has_timesamples()) return;
    if (auto color_value = attr->get_value<value::color3f>()) {
      out[0] = (*color_value)[0]; out[1] = (*color_value)[1];
      out[2] = (*color_value)[2];
    } else if (auto float_value = attr->get_value<value::float3>()) {
      out[0] = (*float_value)[0]; out[1] = (*float_value)[1];
      out[2] = (*float_value)[2];
    }
  };
  scalar("inputs:density", density_scale);
  color("inputs:scattering_color", albedo);
  color("inputs:scatter_color", albedo);
  color("inputs:emission_color", emission_color);
  color("inputs:emissionColor", emission_color);
  scalar("inputs:emission", emission_scale);
  scalar("inputs:emission_intensity", emission_scale);
  scalar("inputs:emissionIntensity", emission_scale);
  *density_scale = std::max(0.0f, *density_scale);
  *emission_scale = std::max(0.0f, *emission_scale);
}

bool RenderSceneConverter::ConvertVolume(
    const RenderSceneConverterEnv &env, const std::string &volume_abs_path,
    const Volume &volume, RenderVolume *dst) {
  if (!dst) return false;

  dst->abs_path = volume_abs_path;

  Path material_path;
  const Material *material = nullptr;
  std::string material_err;
  GetBoundMaterialCached(env.stage, Path(volume_abs_path, ""), "",
                         &material_path, &material, &material_err);
  // Dynamic UsdVol reconstruction predates MaterialBindingAPI support. Keep a
  // relationship fallback so a directly authored binding is not lost even
  // when the applied API instance was not reconstructed on this schema.
  if (!material) {
    Relationship direct_binding;
    bool has_direct_binding =
        volume.get_materialBinding(value::token(""), &direct_binding);
    if (has_direct_binding) {
      const Relationship &rel = direct_binding;
      Path target;
      if (!rel.targetPathVector.empty()) target = rel.targetPathVector[0];
      else target = rel.targetPath;
      const Prim *mat_prim = nullptr;
      if (target.is_valid() &&
          env.stage.find_prim_at_path(Path(target.prim_part(), ""), mat_prim) &&
          mat_prim) {
        material = mat_prim->as<Material>();
      }
    }
  }
  if (material) {
    std::vector<Path> connections;
    if (material->volume.authored()) {
      connections = material->volume.get_connections();
    } else {
      const auto output = material->props.find("outputs:volume");
      if (output != material->props.end() && output->second.is_attribute()) {
        if (const Attribute *attr = output->second.get_attribute_or_null()) {
          connections = attr->connections();
        }
      }
    }
    if (connections.size() == 1) {
      const Prim *shader_prim = nullptr;
      std::string lookup_err;
      if (env.stage.find_prim_at_path(
              Path(connections[0].prim_part(), ""), shader_prim,
              &lookup_err) && shader_prim) {
        if (const Shader *shader = shader_prim->as<Shader>()) {
          if (const ShaderNode *node = shader->value.as<ShaderNode>()) {
            ApplyVolumeShaderConstants(env.stage, *node,
                                        &dst->density_scale, dst->albedo,
                                        dst->emission_color,
                                        &dst->emission_scale);
          }
        }
      }
    }
  }

  const AssetResolutionResolver &assetResolver = env.asset_resolver;

  for (const auto &item : volume.fieldRelationships) {
    const std::string &field_name = item.first;
    const Relationship &rel = item.second;

    // Resolve the field-asset prim path from the relationship target.
    Path target_path;
    if (!rel.targetPathVector.empty()) {
      target_path = rel.targetPathVector[0];
    } else {
      target_path = rel.targetPath;
    }
    const std::string target_prim = target_path.prim_part();
    if (target_prim.empty()) {
      DCOUT("field:" << field_name << " relationship has no target; skip.");
      continue;
    }

    const Prim *fieldPrim = nullptr;
    {
      auto pv = env.stage.GetPrimAtPath(Path(target_prim, /* prop */ ""));
      if (pv) {
        fieldPrim = pv.value();
      }
    }
    if (!fieldPrim) {
      DCOUT("field-asset prim not found: " << target_prim);
      continue;
    }

    value::AssetPath filePath;
    std::string vdb_field_name = field_name;  // default to the rel name
    if (!GetFieldAssetInfo(*fieldPrim, &filePath, &vdb_field_name)) {
      DCOUT("target prim is not a field-asset: " << target_prim);
      continue;
    }
    if (filePath.GetAssetPath().empty()) {
      DCOUT("field-asset has empty filePath: " << target_prim);
      continue;
    }

    // Resolve + open the .vdb asset.
    std::string sanitized = utils::SanitizeAssetPath(
        filePath.GetAssetPath(), assetResolver.get_allow_parent_relative_paths());
    if (sanitized.empty()) {
      DCOUT("Unsafe vdb asset path: " << filePath.GetAssetPath());
      continue;
    }
    std::string resolvedPath = assetResolver.resolve(sanitized);
    if (resolvedPath.empty()) {
      DCOUT("Failed to resolve vdb asset path: " << filePath.GetAssetPath());
      continue;
    }

    Asset asset;
    std::string aerr, awarn;
    if (!assetResolver.open_asset(resolvedPath, sanitized, &asset, &awarn,
                                  &aerr)) {
      DCOUT("Failed to open vdb asset: " << resolvedPath << " : " << aerr);
      continue;
    }

    // Decode the .vdb into dense float grids.
    std::vector<usdVol::VDBGrid> grids;
    std::string vwarn, verr;
    if (!usdVol::ReadVDBFromMemory(asset.data(), asset.size(), resolvedPath,
                                   &grids, &vwarn, &verr)) {
      DCOUT("Failed to decode vdb: " << resolvedPath << " : " << verr);
      continue;
    }
    if (grids.empty()) continue;

    // Pick the grid whose name matches the requested field, else the first.
    const usdVol::VDBGrid *g = nullptr;
    for (const auto &gg : grids) {
      if (gg.name == vdb_field_name) {
        g = &gg;
        break;
      }
    }
    if (!g) g = &grids[0];
    if (g->data.empty() || g->dim[0] <= 0 || g->dim[1] <= 0 || g->dim[2] <= 0) {
      continue;
    }

    RenderVolumeField f;
    f.field_name = field_name;
    f.field_data_type = g->value_type;
    f.background = g->background;
    for (int a = 0; a < 3; a++) {
      f.dim[a] = g->dim[a];
      f.origin[a] = g->origin[a];
      f.voxel_size[a] = float(g->voxel_size[a]);
      f.world_translation[a] = float(g->world_translation[a]);
      // Object-space AABB of the grid.
      f.bounds_min[a] =
          float(g->origin[a]) * f.voxel_size[a] + f.world_translation[a];
      f.bounds_max[a] = float(g->origin[a] + g->dim[a]) * f.voxel_size[a] +
                        f.world_translation[a];
    }

    // Store the dense float voxels in a BufferData.
    BufferData buf;
    buf.componentType = ComponentType::Float;
    size_t byte_size;
    if (!safe::mul(g->data.size(), sizeof(float), &byte_size)) {
      DCOUT("VDB grid data size overflow.");
      continue;
    }
    std::vector<uint8_t> bytes(byte_size);
    std::memcpy(bytes.data(), g->data.data(), bytes.size());
    SetBufferDataBytes(buf, std::move(bytes));
    f.buffer_id = int64_t(buffers.size());
    buffers.push_back(std::move(buf));

    dst->fields.push_back(std::move(f));
  }

  return true;
}

bool RenderSceneConverter::BuildSingleNode(
    const RenderSceneConverterEnv &env, const std::string &primPath,
    const XformNode &node, Node &out_rnode) {
  Node rnode;

  const lightusd::Prim *prim = node.prim;
  if (prim) {
    rnode.prim_name = prim->element_name();
    rnode.abs_path = primPath;
    rnode.display_name = prim->metas().has_displayName() ? prim->metas().get_displayName() : "";

    DCOUT("rnode.prim_name " << rnode.prim_name);
    DCOUT("node.local_mat " << node.get_local_matrix());
    DCOUT("node.has_resetXform " << node.has_resetXformStack());
    DCOUT("prim.type_name " << prim->type_name());
    DCOUT("prim.type_id " << prim->type_id());
    DCOUT("xform " << value::TYPE_ID_GEOM_XFORM);

    // NOTE: this ~13-branch else-if chain on prim->type_id() was converted
    // to standalone ifs -- same MSVC C1061 ("blocks nested too deeply")
    // risk class already fixed for the same reason elsewhere in this
    // codebase. Unlike some sibling fixes, EVERY condition here (not just
    // the final fallback) needs an explicit `!matched &&` prefix: the range
    // check below (prim->type_id() > TYPE_ID_MODEL_BEGIN && < GEOM_END) can
    // structurally overlap several of the specific-type branches above it
    // (Mesh/Volume/Camera/Xform/Scope/Model/parametric prims are all
    // plausibly within that range), and -- unlike a loop with break/continue
    // -- nothing else here would stop a naive flatten from double-executing
    // node setup for a type_id() that satisfies both. The `!matched &&`
    // guard reproduces the original chain's exact "first match wins"
    // semantics regardless of any such overlap.
    bool matched = false;

    if (prim->type_id() == value::TYPE_ID_GEOM_MESH) {
      matched = true;
      // GeomMesh(GPrim) also has xform.
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.nodeType = NodeType::Mesh;
      rnode.has_resetXform = node.has_resetXformStack();

      if (auto mesh_it = meshMap.find(primPath); mesh_it != meshMap.s_end()) {
        rnode.id = int32_t(mesh_it->second);
      } else {
        rnode.id = -1;
      }

      // Note: MeshLightAPI is now handled in ConvertMesh, which sets
      // mesh.is_area_light = true and stores light properties directly in RenderMesh
    }
    if (!matched && prim->type_id() == value::TYPE_ID_VOLUME) {
      matched = true;
      // UsdVol Volume: decode referenced .vdb field(s) into a RenderVolume.
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Volume;

      const Volume *vol = prim->as<Volume>();
      if (vol) {
        RenderVolume rvol;
        rvol.prim_name = prim->element_name();
        rvol.abs_path = primPath;
        rvol.display_name = prim->metas().has_displayName()
                                ? prim->metas().get_displayName()
                                : "";
        rvol.world_matrix = node.get_world_matrix();
        // Best-effort: keep the node even if some/all fields fail to decode.
        ConvertVolume(env, primPath, *vol, &rvol);

        size_t vol_id = volumes.size();
        volumeMap.add(primPath, vol_id);
        volumes.push_back(std::move(rvol));
        rnode.id = int32_t(vol_id);
      } else {
        rnode.id = -1;
      }
    }
    if (!matched && prim->type_id() == value::TYPE_ID_GEOM_CAMERA) {
      matched = true;
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Camera;

      const GeomCamera *geomCamera = prim->as<GeomCamera>();
      if (geomCamera) {
        RenderCamera rcam;
        rcam.name = prim->element_name();
        rcam.abs_path = primPath;
        rcam.display_name = prim->metas().has_displayName() ? prim->metas().get_displayName() : "";

        // Extract lens properties
        float val_f;
        if (geomCamera->focalLength.get_value().get_scalar(&val_f)) {
          rcam.focalLength = val_f;
        }
        if (geomCamera->verticalAperture.get_value().get_scalar(&val_f)) {
          rcam.verticalAperture = val_f;
        }
        if (geomCamera->horizontalAperture.get_value().get_scalar(&val_f)) {
          rcam.horizontalAperture = val_f;
        }
        if (geomCamera->horizontalApertureOffset.get_value().get_scalar(&val_f)) {
          rcam.horizontalApertureOffset = val_f;
        }
        if (geomCamera->verticalApertureOffset.get_value().get_scalar(&val_f)) {
          rcam.verticalApertureOffset = val_f;
        }
        if (geomCamera->exposure.get_value().get_scalar(&val_f)) {
          rcam.exposure = val_f;
        }
        if (geomCamera->focusDistance.get_value().get_scalar(&val_f)) {
          rcam.focusDistance = val_f;
        }
        if (geomCamera->fStop.get_value().get_scalar(&val_f)) {
          rcam.fStop = val_f;
        }

        value::float2 range_val;
        if (geomCamera->clippingRange.get_value().get_scalar(&range_val)) {
          rcam.znear = range_val[0];
          rcam.zfar = range_val[1];
        }

        GeomCamera::Projection proj_val;
        if (geomCamera->projection.get_value().get_scalar(&proj_val)) {
          rcam.projection = proj_val;
        }
        rcam.stereoRole = geomCamera->stereoRole.get_value();
        geomCamera->shutterOpen.get_value().get_scalar(&rcam.shutterOpen);
        geomCamera->shutterClose.get_value().get_scalar(&rcam.shutterClose);
        if (geomCamera->clippingPlanes.authored()) {
          auto planes = geomCamera->clippingPlanes.get_value();
          if (planes.has_value()) {
            planes->get_default(&rcam.clippingPlanes);
          }
        }

        size_t cam_id = cameras.size();
        cameraMap.add(primPath, cam_id);
        cameras.push_back(std::move(rcam));
        rnode.id = int32_t(cam_id);
      } else {
        rnode.id = -1;
      }
    }
    if (!matched && prim->type_id() == value::TYPE_ID_GEOM_XFORM) {
      matched = true;
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      DCOUT("rnode.local_matrix " << rnode.local_matrix);
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Xform;
    }
    if (!matched && prim->type_id() == value::TYPE_ID_SCOPE) {
      matched = true;
      // NOTE: get_local_matrix() should return identity matrix.
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Xform;
    }
    if (!matched && prim->type_id() == value::TYPE_ID_MODEL) {
      matched = true;
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Xform;
    }
    if (!matched &&
        (prim->type_id() == value::TYPE_ID_GEOM_CUBE || prim->type_id() == value::TYPE_ID_GEOM_SPHERE ||
         prim->type_id() == value::TYPE_ID_GEOM_CYLINDER || prim->type_id() == value::TYPE_ID_GEOM_CONE ||
         prim->type_id() == value::TYPE_ID_GEOM_CAPSULE || prim->type_id() == value::TYPE_ID_GEOM_PLANE)) {
      matched = true;
      // Parametric primitives are converted to meshes
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.nodeType = NodeType::Mesh;
      rnode.has_resetXform = node.has_resetXformStack();

      if (auto mesh_it = meshMap.find(primPath); mesh_it != meshMap.s_end()) {
        rnode.id = int32_t(mesh_it->second);
      } else {
        rnode.id = -1;
      }
    }
    if (!matched && (prim->type_id() > value::TYPE_ID_MODEL_BEGIN) && (prim->type_id() < value::TYPE_ID_GEOM_END)) {
      matched = true;
      // Other Geom prims (e.g. GeomCone, GeomCylinder) - not yet converted to meshes
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Xform;
    }
    if (!matched && IsLightPrim(*prim)) {
      matched = true;
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();

      // Convert USD light to RenderLight and add to scene
      RenderLight rlight;
      bool light_converted = false;
      std::string light_abs_path = primPath;
      Path lightPath(light_abs_path, /* prop_part */ "");

      if (prim->type_id() == value::TYPE_ID_LUX_SPHERE) {
        const SphereLight *sphereLight = prim->as<SphereLight>();
        if (sphereLight) {
          if (!ConvertSphereLight(env, lightPath, *sphereLight, &rlight)) {
            return false;
          }
          rnode.nodeType = NodeType::PointLight;
          light_converted = true;
        }
      } else if (prim->type_id() == value::TYPE_ID_LUX_DISTANT) {
        const DistantLight *distantLight = prim->as<DistantLight>();
        if (distantLight) {
          if (!ConvertDistantLight(env, lightPath, *distantLight, &rlight)) {
            return false;
          }
          rnode.nodeType = NodeType::DirectionalLight;
          light_converted = true;
        }
      } else if (prim->type_id() == value::TYPE_ID_LUX_DOME) {
        const DomeLight *domeLight = prim->as<DomeLight>();
        if (domeLight) {
          if (!ConvertDomeLight(env, lightPath, *domeLight, &rlight)) {
            return false;
          }
          rnode.nodeType = NodeType::EnvmapLight;
          light_converted = true;
        }
      } else if (prim->type_id() == value::TYPE_ID_LUX_RECT) {
        const RectLight *rectLight = prim->as<RectLight>();
        if (rectLight) {
          if (!ConvertRectLight(env, lightPath, *rectLight, &rlight)) {
            return false;
          }
          rnode.nodeType = NodeType::RectLight;
          light_converted = true;
        }
      } else if (prim->type_id() == value::TYPE_ID_LUX_DISK) {
        const DiskLight *diskLight = prim->as<DiskLight>();
        if (diskLight) {
          if (!ConvertDiskLight(env, lightPath, *diskLight, &rlight)) {
            return false;
          }
          rnode.nodeType = NodeType::DiskLight;
          light_converted = true;
        }
      } else if (prim->type_id() == value::TYPE_ID_LUX_CYLINDER) {
        const CylinderLight *cylinderLight = prim->as<CylinderLight>();
        if (cylinderLight) {
          if (!ConvertCylinderLight(env, lightPath, *cylinderLight, &rlight)) {
            return false;
          }
          rnode.nodeType = NodeType::CylinderLight;
          light_converted = true;
        }
      } else if (prim->type_id() == value::TYPE_ID_LUX_GEOMETRY) {
        const GeometryLight *geometryLight = prim->as<GeometryLight>();
        if (geometryLight) {
          if (!ConvertGeometryLight(env, lightPath, *geometryLight, &rlight)) {
            return false;
          }
          rnode.nodeType = NodeType::GeometryLight;
          light_converted = true;
        }
      } else {
        // Unsupported light type
        DCOUT("Unsupported light type: " << prim->type_name());
        rnode.nodeType = NodeType::Xform;
      }

      if (light_converted) {
        // Copy world transform to the light
        // rnode.global_matrix is a matrix4d, rlight.transform is mat4 (float)
        const auto &m = rnode.global_matrix;
        rlight.transform.m[0][0] = float(m.m[0][0]);
        rlight.transform.m[0][1] = float(m.m[0][1]);
        rlight.transform.m[0][2] = float(m.m[0][2]);
        rlight.transform.m[0][3] = float(m.m[0][3]);
        rlight.transform.m[1][0] = float(m.m[1][0]);
        rlight.transform.m[1][1] = float(m.m[1][1]);
        rlight.transform.m[1][2] = float(m.m[1][2]);
        rlight.transform.m[1][3] = float(m.m[1][3]);
        rlight.transform.m[2][0] = float(m.m[2][0]);
        rlight.transform.m[2][1] = float(m.m[2][1]);
        rlight.transform.m[2][2] = float(m.m[2][2]);
        rlight.transform.m[2][3] = float(m.m[2][3]);
        rlight.transform.m[3][0] = float(m.m[3][0]);
        rlight.transform.m[3][1] = float(m.m[3][1]);
        rlight.transform.m[3][2] = float(m.m[3][2]);
        rlight.transform.m[3][3] = float(m.m[3][3]);

        // Extract position from transform (translation column)
        rlight.position[0] = float(m.m[3][0]);
        rlight.position[1] = float(m.m[3][1]);
        rlight.position[2] = float(m.m[3][2]);

        // Extract direction from transform (light faces -Z in local space)
        // Direction is the negative of the Z column (third column) of the rotation part
        rlight.direction[0] = -float(m.m[2][0]);
        rlight.direction[1] = -float(m.m[2][1]);
        rlight.direction[2] = -float(m.m[2][2]);

        // Add light to the lights array
        size_t light_id = lights.size();
        lightMap.add(light_abs_path, light_id);
        lights.push_back(std::move(rlight));
        rnode.id = int32_t(light_id);
      } else {
        rnode.id = -1;
      }
    }
    if (!matched && prim->type_id() == value::TYPE_ID_SKEL_ROOT) {
      matched = true;
      // UsdSkelRoot: encapsulation prim for skinned subtree.
      // SkelRoot is Xformable and its world transform (skelLocalToWorld)
      // positions the skinned result in world space.
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::SkelRoot;
    }
    if (!matched && prim->type_id() == value::TYPE_ID_SKELETON) {
      matched = true;
      // UsdSkeleton: joint hierarchy with bindTransforms and restTransforms.
      // Skeleton is Xformable; its world transform contributes to
      // skelLocalToWorld for positioning skinned results.
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Skeleton;
    }
    if (!matched && prim->type_id() == value::TYPE_ID_GEOM_POINT_INSTANCER) {
      matched = true;
      // UsdGeomPointInstancer: the instancer prim itself is an Xform node with
      // no directly-attached geometry (id == -1). Its instances are expanded
      // into RenderScene::instances (see ExpandPointInstancer); the instancer's
      // world transform recorded here is used as the instance space origin.
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Xform;
    }
    if (!matched) {
      // ignore other node types.
      DCOUT("Unknown/Unsupported prim. " << prim->type_name());

      // Setup as xform for now.
      rnode.local_matrix = node.get_local_matrix();
      rnode.global_matrix = node.get_world_matrix();
      rnode.has_resetXform = node.has_resetXformStack();
      rnode.nodeType = NodeType::Xform;
    }

    // Set category based on nodeType
    rnode.category = GetNodeCategoryFromType(rnode.nodeType);

    // AOUSD Spec 11.3.3: Mark instance prims
    if (prim->IsInstance() && prim->HasCompositionArcs()) {
      rnode.is_instance = true;
      int proto_idx = env.stage.GetPrototypeIndex(
          Path(primPath, /* prop_part */ ""));
      rnode.prototype_index = proto_idx;
    }
  }

  out_rnode = std::move(rnode);

  return true;
}

bool RenderSceneConverter::BuildNodeHierarchyIterative(
    const RenderSceneConverterEnv &env, const std::string &parentPrimPath,
    const XformNode &root_node, Node &out_rnode) {

  // Use a post-order iterative approach:
  // 1. Process nodes in DFS order, building a flat list of (node_data, child_count)
  // 2. Then assemble the tree from leaves up using a result stack.

  struct FlatEntry {
    Node node;           // node data (without children)
    size_t child_count;  // number of direct children
  };

  // Phase 1: DFS to build flat entries in pre-order
  struct WorkItem {
    const XformNode* xform_node;
    std::string parent_path;
  };

  std::vector<FlatEntry> flat;
  std::vector<WorkItem> stack;
  stack.push_back({&root_node, parentPrimPath});

  constexpr size_t kMaxIter = 1024 * 1024;
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxIter) {
    WorkItem item = std::move(stack.back());
    stack.pop_back();

    std::string primPath;
    if (item.parent_path.empty()) {
      primPath = "/" + item.xform_node->element_name;
    } else {
      primPath = item.parent_path + "/" + item.xform_node->element_name;
    }

    Node rnode;
    if (!BuildSingleNode(env, primPath, *item.xform_node, rnode)) {
      return false;
    }

    flat.push_back({std::move(rnode), item.xform_node->children.size()});

    // Push children in reverse order so first child is processed first
    const auto& children = item.xform_node->children;
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      stack.push_back({&(*it), primPath});
    }
  }

  // Phase 2: Assemble tree from flat list using a result stack.
  // flat is in pre-order. We process from the end (leaves first).
  // Each entry knows its child_count; we pop that many from the result stack.
  std::vector<Node> result_stack;
  for (size_t i = flat.size(); i > 0; --i) {
    auto& entry = flat[i - 1];
    // The last child_count items on result_stack are this node's children
    // (in reverse order because we process right-to-left)
    entry.node.children.resize(entry.child_count);
    for (size_t c = 0; c < entry.child_count; c++) {
      entry.node.children[c] = std::move(result_stack.back());
      result_stack.pop_back();
    }
    result_stack.push_back(std::move(entry.node));
  }

  if (!result_stack.empty()) {
    out_rnode = std::move(result_stack.back());
  }

  return true;
}

//

bool RenderSceneConverter::BuildNodeHierarchy(
    const RenderSceneConverterEnv &env, const XformNode &root) {
  std::string defaultRootNode = env.stage.metas().defaultPrim.str();

  default_node = -1;

  for (const auto &rootNode : root.children) {
    Node root_node;
    if (!BuildNodeHierarchyIterative(env, /* root */ "", rootNode, root_node)) {
      return false;
    }

    if (defaultRootNode == rootNode.element_name) {
      default_node = int(root_nodes.size());
    }

    root_nodeMap.add("/" + rootNode.element_name, root_nodes.size());
    root_nodes.push_back(root_node);
  }

  return true;
}


}  // namespace tydra
}  // namespace lightusd
