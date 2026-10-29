#pragma once
// UsdVol render data for tydra/next: a Volume prim's bound fields, resolved
// against their files by tydra/vol (FieldCache), and the constants of its
// volume material. Voxels stay in the FieldCache and are produced on demand
// (dense with a budget, or the sparse grid), so extraction does not decode
// voxel data.
#include <string>
#include <vector>

#include "tydra/vol/volume-field.hh"

namespace lightusd { namespace tydra { namespace next {

struct RenderVolumeField {
  std::string binding;                 // "density", "temperature", ...
  vol::ResolvedField field;            // grid, class, type, world placement
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

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
