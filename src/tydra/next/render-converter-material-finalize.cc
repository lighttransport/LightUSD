// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Material shader-parameter iteration and post-binding UV finalization.

#include "render-converter-material-finalize.hh"
#include "render-data.hh"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace lightusd { namespace tydra { namespace next {
namespace {
// The single authoritative walk over every ShaderParam a RenderMaterial owns.
// Both parallel texture-id remapping and material-driven UV promotion use this
// list so adding a new lobe cannot silently leave worker-local ids behind.
template <typename Fn>
void ForEachMaterialShaderParam(RenderMaterial* mat, Fn&& fn) {
  if (!mat) return;
  if (mat->preview_surface) {
    PreviewSurfaceShader& ps = *mat->preview_surface;
    for (ShaderParam* p :
         {&ps.diffuse_color, &ps.emissive_color, &ps.specular_color,
          &ps.metallic, &ps.roughness, &ps.clearcoat,
          &ps.clearcoat_roughness, &ps.opacity, &ps.opacity_threshold,
          &ps.ior, &ps.normal, &ps.displacement, &ps.occlusion}) {
      fn(*p);
    }
  }
  if (mat->openpbr) {
    OpenPBRSurfaceShader& o = *mat->openpbr;
    for (ShaderParam* p :
         {&o.base_weight, &o.base_color, &o.base_roughness,
          &o.base_metalness, &o.specular_weight, &o.specular_color,
          &o.specular_roughness, &o.specular_ior, &o.specular_anisotropy,
          &o.specular_roughness_anisotropy, &o.specular_rotation,
          &o.transmission_weight, &o.transmission_color,
          &o.transmission_depth,
          &o.transmission_dispersion, &o.transmission_dispersion_scale,
          &o.subsurface_weight, &o.subsurface_color, &o.subsurface_radius,
          &o.subsurface_scale, &o.coat_weight, &o.coat_color, &o.coat_roughness,
          &o.coat_ior, &o.coat_anisotropy, &o.coat_roughness_anisotropy,
          &o.coat_normal, &o.sheen_weight, &o.sheen_color,
          &o.sheen_roughness,
          &o.thin_film_weight, &o.thin_film_thickness, &o.thin_film_ior,
          &o.emission_luminance, &o.emission_color, &o.normal, &o.opacity,
          &o.tangent, &o.displacement}) {
      fn(*p);
    }
  }
  for (RetainedMaterialParam& retained : mat->retained_params) {
    fn(retained.value);
  }
}

template <typename Fn>
void ForEachMaterialShaderParam(const RenderMaterial& mat, Fn&& fn) {
  // The visitor does not mutate while gathering UV names. Reuse the canonical
  // mutable field list rather than maintaining a second error-prone list.
  ForEachMaterialShaderParam(
      const_cast<RenderMaterial*>(&mat),
      [&fn](ShaderParam& param) { fn(static_cast<const ShaderParam&>(param)); });
}

// Material-driven UV primvar promotion: a UsdPrimvarReader varname that the
// mesh's own UV-set selection (MeshConfig::uv_primvar_names) did not pick only
// survives as a generic mesh.primvars entry, which no texture consumer samples.
// After materials are bound, promote the primvar each bound material's textures
// actually reference into texcoords_0/1 (legacy selects UV sets from the shader
// network the same way).
void PromoteMaterialUVPrimvarsImpl(RenderScene* scene,
                               std::vector<std::string>* warnings) {
  if (!scene) return;

  auto texture_uv_names = [scene](const RenderMaterial& mat,
                                  std::vector<std::string>* names) {
    auto add = [scene, names](const ShaderParam& p) {
      if (p.texture_id < 0 ||
          static_cast<size_t>(p.texture_id) >= scene->textures.size()) {
        return;
      }
      const std::string& uv =
          scene->textures[static_cast<size_t>(p.texture_id)].uv_primvar;
      if (uv.empty()) return;
      if (std::find(names->begin(), names->end(), uv) == names->end()) {
        names->push_back(uv);
      }
    };
    ForEachMaterialShaderParam(mat, add);
  };

  for (RenderMesh& mesh : scene->meshes) {
    // Gather UV names referenced by every material bound to this mesh
    // (direct binding + subsets).
    std::vector<int32_t> material_ids;
    if (mesh.material_id >= 0) material_ids.push_back(mesh.material_id);
    for (const RenderMesh::MaterialSubset& subset : mesh.material_subsets) {
      if (subset.material_id >= 0) material_ids.push_back(subset.material_id);
    }
    std::vector<std::string> wanted;
    for (int32_t mid : material_ids) {
      if (static_cast<size_t>(mid) >= scene->materials.size()) continue;
      texture_uv_names(scene->materials[static_cast<size_t>(mid)], &wanted);
    }
    if (wanted.empty()) continue;

    auto take_primvar = [&mesh, warnings](const std::string& name,
                                         FloatChunked* value,
                                         Interpolation* interp) -> bool {
      for (size_t ai = 0; ai < mesh.primvars.size(); ++ai) {
        VertexAttribute& attr = mesh.primvars[ai];
        if (attr.name != name || attr.format != VertexFormat::Vec2) continue;
        FloatChunked promoted;
        if (attr.has_indices()) {
          const size_t elems = attr.float_data.size() / 2;
          // Both counts are known up front; without reserve() a 1M-vertex UV
          // set grows 122 chunks one push_back at a time.
          promoted.reserve(attr.indices.size() * 2);
          for (size_t k = 0; k < attr.indices.size(); ++k) {
            const uint32_t idx = attr.indices[k];
            if (idx >= elems) {
              warnings->push_back("Mesh '" + mesh.prim_path + "': UV primvar '" +
                                  name + "' has out-of-range indices; not promoted");
              return false;
            }
            promoted.push_back(attr.float_data[idx * 2 + 0]);
            promoted.push_back(attr.float_data[idx * 2 + 1]);
          }
        } else {
          // Straight copy: go chunk-at-a-time rather than element-at-a-time.
          promoted.reserve(attr.float_data.size());
          for (size_t c = 0; c < attr.float_data.chunk_count(); ++c) {
            const size_t n = attr.float_data.chunk_size(c);
            if (n == 0) break;
            promoted.append(attr.float_data.chunk_data(c), n);
          }
        }
        if (promoted.alloc_failed()) {
          warnings->push_back("Mesh '" + mesh.prim_path + "': UV primvar '" +
                              name + "' allocation failed; not promoted");
          return false;
        }
        *value = std::move(promoted);
        *interp = attr.interpolation;
        mesh.primvars.erase(mesh.primvars.begin() +
                            static_cast<std::ptrdiff_t>(ai));
        return true;
      }
      return false;
    };

    auto swap_uv_slots = [&mesh]() {
      std::swap(mesh.texcoords_0, mesh.texcoords_1);
      std::swap(mesh.texcoords_0_interp, mesh.texcoords_1_interp);
      std::swap(mesh.texcoords_0_name, mesh.texcoords_1_name);
    };

    // The first material-referenced UV set must occupy the primary slot
    // (matching legacy's shader-network-driven selection). Mesh extraction may
    // already have selected it as texcoords_1; normalize that case instead of
    // treating either occupied slot as equivalent. If a second referenced set
    // currently occupies slot 0, preserve it by swapping before slot 0 is
    // overwritten by a promoted generic primvar.
    bool primary_ready = false;
    if (mesh.texcoords_0_name == wanted[0]) {
      primary_ready = true;
    } else if (mesh.texcoords_1_name == wanted[0]) {
      swap_uv_slots();
      primary_ready = true;
    } else {
      FloatChunked primary;
      Interpolation primary_interp = Interpolation::Vertex;
      if (take_primvar(wanted[0], &primary, &primary_interp)) {
        if (wanted.size() > 1 && mesh.texcoords_0_name == wanted[1]) {
          swap_uv_slots();
        }
        mesh.texcoords_0 = std::move(primary);
        mesh.texcoords_0_interp = primary_interp;
        mesh.texcoords_0_name = wanted[0];
        primary_ready = true;
      }
    }

    // Keep the second distinct material-referenced UV set in slot 1. It may
    // already be there after the normalization above or may still be a generic
    // primvar that needs promotion.
    if (primary_ready && wanted.size() > 1 &&
        mesh.texcoords_1_name != wanted[1]) {
      FloatChunked secondary;
      Interpolation secondary_interp = Interpolation::Vertex;
      if (take_primvar(wanted[1], &secondary, &secondary_interp)) {
        mesh.texcoords_1 = std::move(secondary);
        mesh.texcoords_1_interp = secondary_interp;
        mesh.texcoords_1_name = wanted[1];
      }
    }
  }
}
}  // namespace

void RemapMaterialTextureIds(RenderMaterial* material,
                             const std::vector<int32_t>& texture_remap) {
  ForEachMaterialShaderParam(material, [&texture_remap](ShaderParam& param) {
    if (param.texture_id >= 0 &&
        static_cast<size_t>(param.texture_id) < texture_remap.size()) {
      param.texture_id = texture_remap[static_cast<size_t>(param.texture_id)];
    }
  });
}

void PromoteMaterialUVPrimvars(RenderScene* scene,
                               std::vector<std::string>* warnings) {
  PromoteMaterialUVPrimvarsImpl(scene, warnings);
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
