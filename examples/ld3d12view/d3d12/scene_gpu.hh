// ld3d12view - GPU copy of an lmv_scene: per-mesh vertex/index buffers, a
// structured material buffer, and one shader-visible SRV heap that holds every
// uploaded image (bindless: materials refer to textures by descriptor index).
// All uploads go through staging buffers on a throw-away command list.
#pragma once

#include "device.hh"
#include "lighting.hh"
#include "lmv.h"

#include <vector>

namespace ld3d12 {

struct MeshGPU {
  ComPtr<ID3D12Resource> vertex_buffer;
  ComPtr<ID3D12Resource> index_buffer;
  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  D3D12_INDEX_BUFFER_VIEW ibv = {};
  uint32_t index_count = 0;
  float bbox_min[3] = {0, 0, 0};  // mesh space; used to sort blended draws
  float bbox_max[3] = {0, 0, 0};
  int32_t material_id = -1;
  bool skinned = false;  // GPU skinning buffers present (else bind pose)
  // Skinning (static) and blend-shape deltas (interleaved per vertex/target).
  ComPtr<ID3D12Resource> joint_indices;  // ushort x4 per vertex
  ComPtr<ID3D12Resource> joint_weights;  // float4 per vertex
  ComPtr<ID3D12Resource> morph_deltas;   // float3 per vertex per target
  ComPtr<ID3D12Resource> face_ids;       // uint per vertex (Ptex materials)
  uint32_t morph_targets = 0;
  uint32_t joint_count = 0;
  // Per-frame data (palette, morph weights) lives in SceneGPU::deform_ring at
  // these byte offsets within a slot.
  UINT64 palette_offset = 0;
  UINT64 weights_offset = 0;
  bool deforms() const { return skinned || morph_targets > 0; }
  std::vector<lmv_subset> subsets;
  bool valid() const { return vertex_buffer && index_buffer; }
};

// Slot kind lives in flags bits 16..19.
constexpr int kSlotKindShift = 16;
constexpr int kSlotKind2D = 0;
constexpr int kSlotKindUdim = 1;  // texture array + 10x10 tile LUT
constexpr int kSlotKindPtex = 2;  // atlas + per-face rects, per-vertex face id

constexpr int kSlotCount = 7;  // base, metallic, roughness, normal, emission, opacity, coat normal

// One material texture slot; layout mirrors `Slot` in the mesh shader.
struct SlotGPU {
  int32_t index = -1;  // SRV descriptor index, -1 = no texture
  // wrap_s | wrap_t << 4 | channel << 8 | uv_set << 12
  int32_t flags = 0;
  float rotation = 0.0f;  // radians
  int32_t aux = 0;  // UDIM: tile LUT offset; Ptex: rect table offset
  float uv_os[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // offset xy, scale zw
  float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// Mirrors `Material` in the mesh shader (StructuredBuffer element, 640 bytes):
// the packed OpenPBR parameters the preview shader evaluates, plus the texture
// slots.
struct MaterialGPU {
  float base[4] = {0.8f, 0.8f, 0.8f, 1.0f};      // rgb base colour, weight
  float emission[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // rgb, luminance scale
  float params[4] = {0.0f, 0.6f, 0.0f, 0.5f};    // metalness, roughness,
                                                 // alpha mode, alpha cutoff
  float extra[4] = {1.0f, 1.0f, 1.0f, 1.5f};     // normal scale, opacity,
                                                 // coat normal scale, ior
  float specular[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // rgb colour, weight
  float coat[4] = {1.0f, 1.0f, 1.0f, 0.0f};      // rgb colour, weight
  float sheen[4] = {1.0f, 1.0f, 1.0f, 0.0f};     // rgb colour, weight
  float transmission[4] = {1.0f, 1.0f, 1.0f, 0.0f};  // rgb colour, weight
  float coat_params[4] = {0.1f, 1.5f, 0.3f, 0.0f};   // coat roughness, coat ior,
                                                     // sheen roughness, thin walled
  float subsurface[4] = {1.0f, 1.0f, 1.0f, 0.0f};    // rgb colour, weight
  float misc[4] = {0.0f, 1.0f, 1.0f, 0.0f};  // transmission depth, subsurface
                                             // scale, coat affects colour, -
  float thin[4] = {0.0f, 0.0f, 1.0f, 0.0f};  // thin film weight, thickness nm,
                                             // thin film ior, anisotropy
  SlotGPU slots[kSlotCount];
};
static_assert(sizeof(MaterialGPU) == 640, "MaterialGPU must match the shader");

class SceneGPU {
 public:
  bool Build(Device& device, const lmv_scene* scene, std::string* err);

  std::vector<MeshGPU> meshes;
  // Scene materials in lmv order; the last element is the fallback material.
  std::vector<MaterialGPU> materials;
  uint32_t default_material() const {
    return static_cast<uint32_t>(materials.size()) - 1;
  }
  ComPtr<ID3D12Resource> material_buffer;
  // UDIM tile LUTs (100 ints per UDIM texture, -1 = no tile) and Ptex rect
  // tables (float4 per face, preceded by a header holding the face count).
  ComPtr<ID3D12Resource> udim_lut_buffer;
  ComPtr<ID3D12Resource> ptex_rect_buffer;
  ComPtr<ID3D12DescriptorHeap> srv_heap;

  // Lighting. `lights` is the authored set or the studio rig.
  std::vector<LightGPU> lights;
  bool authored_lights = false;
  // Lights are re-read from the scene every frame (animated lights): one
  // persistently mapped upload slot per swapchain buffer.
  static constexpr UINT kMaxLights = 256;
  ComPtr<ID3D12Resource> light_ring;
  uint8_t* light_ring_mapped = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS light_va(UINT slot) const {
    return light_ring->GetGPUVirtualAddress() +
           UINT64(slot) * kMaxLights * sizeof(LightGPU);
  }
  // Skin palettes and blend-shape weights for the current pose, written into
  // swapchain slot `slot` of the deform ring.
  static constexpr UINT64 kDeformAlign = 256;
  ComPtr<ID3D12Resource> deform_ring;
  uint8_t* deform_mapped = nullptr;
  UINT64 deform_slot_bytes = 0;
  D3D12_GPU_VIRTUAL_ADDRESS deform_va(UINT slot) const {
    return deform_ring->GetGPUVirtualAddress() + UINT64(slot) * deform_slot_bytes;
  }
  void UpdateDeform(const lmv_scene* scene, UINT slot);
  // Rebuilds `lights`/`shadow_light` from the scene and writes slot `slot`.
  void RefreshLights(const lmv_scene* scene, UINT slot);
  int32_t env_index = -1;  // SRV descriptor of the dome environment, or -1
  ID3D12Resource* env_resource() const {
    return env_index >= 0 ? textures_[static_cast<size_t>(env_index)].Get() : nullptr;
  }
  float sh[9][4] = {};     // environment irradiance (display-linear)
  float color_matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // working -> display
  float bounds_min[3] = {-1, -1, -1};
  float bounds_max[3] = {1, 1, 1};

  // Directional shadow map (first directional light casts).
  static constexpr UINT kShadowSize = 2048;
  int32_t shadow_light = -1;
  int32_t shadow_srv_index = -1;
  ComPtr<ID3D12Resource> shadow_map;  // R32_TYPELESS, rests in SRV state
  ComPtr<ID3D12DescriptorHeap> shadow_dsv_heap;
  // UsdGeomPoints splats and curve segments (raw lmv_splat / lmv_line_segment).
  ComPtr<ID3D12Resource> splat_buffer;
  uint32_t splat_count = 0;
  ComPtr<ID3D12Resource> line_buffer;
  uint32_t line_count = 0;
  uint32_t texture_count = 0;       // uploaded images
  uint32_t unsupported_textures = 0;  // UDIM / Ptex / unsupported formats

 private:
  std::vector<ComPtr<ID3D12Resource>> textures_;
};

}  // namespace ld3d12
