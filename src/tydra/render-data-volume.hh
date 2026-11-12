#pragma once
// UsdVol render data for Tydra: a Volume prim's bound fields, resolved
// against their files by tydra/vol (FieldCache), and the constants of its
// volume material. Voxels stay in the FieldCache and are produced on demand
// (dense with a budget, or the sparse grid), so extraction does not decode
// voxel data.
#include <string>
#include <memory>
#include <utility>
#include <vector>

#include "tydra/vol/volume-field.hh"

namespace lightusd { namespace tydra {

struct RenderVolumeField {
  RenderVolumeField() = default;
  RenderVolumeField(std::string name, vol::ResolvedField resolved,
                    std::string error = {})
      : binding(std::move(name)), field(std::move(resolved)),
        resolution_error(std::move(error)) {}
  std::string binding;                 // "density", "temperature", ...
  vol::ResolvedField field;            // grid, class, type, world placement
  std::string resolution_error;        // nonempty if an authored field failed resolution
};

// A resolved MaterialX document and selected volume material. Multiple
// volumes may share one immutable binding; texture paths use anchor_path.
struct RenderMaterialXVolumeBinding {
  std::shared_ptr<const std::string> resolved_xml;
  std::string material_name;
  std::string anchor_path;
};

struct RenderVolume {
  std::string prim_path;
  double world_matrix[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  // Authored extent (local space) when present; otherwise the union of the
  // fields' world boxes (world space, extent_is_world).
  bool has_extent = false;
  bool extent_is_world = false;
  double extent_min[3] = {0, 0, 0}, extent_max[3] = {0, 0, 0};
  std::vector<RenderVolumeField> fields;
  std::string material_path;           // bound Material ("" when unbound)
  int material_id = -1;                // index into RenderScene::materials
  std::shared_ptr<const RenderMaterialXVolumeBinding> materialx_binding;
  // Constants of a ND_standard_volume / UsdPreviewSurface-style volume
  // shader (legacy ApplyVolumeShaderConstants semantics).
  float density_scale = 1.0f;
  float albedo[3] = {1.0f, 1.0f, 1.0f};
  float emission[3] = {0.0f, 0.0f, 0.0f};
  float emission_intensity = 0.0f;
  std::vector<std::string> warnings;   // unresolved / mismatched fields

  const RenderVolumeField *Field(const std::string &binding_name) const {
    for (const RenderVolumeField &f : fields)
      if (f.binding == binding_name) return &f;
    return nullptr;
  }
};

}  // namespace tydra
}  // namespace lightusd
