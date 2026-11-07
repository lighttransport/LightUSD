// ld3d12view - forward mesh pass. Per frame: an optional directional shadow
// pass into SceneGPU's shadow map, then the lit pass (authored lights or the
// studio rig, SH9 irradiance + environment reflection, GGX specular, Khronos
// PBR Neutral tonemap). Material/light data are structured buffers; the
// per-frame constants live in a small upload ring (one slot per swapchain
// buffer), the per-draw data are 20 root constants (world + material index).
#pragma once

#include "scene_gpu.hh"

#include <vector>

namespace ld3d12 {

struct FrameParams {
  float view_proj[16];  // column-major proj * view
  float eye[3];
  float exposure_stops = 0.0f;
  bool wireframe = false;
  bool shadows = true;
};

class MeshRenderer {
 public:
  bool Init(ID3D12Device* device, std::string* err);
  // Records the shadow pass (when enabled and the scene has a caster) and the
  // lit mesh pass. The main render targets must be bound on entry.
  void Draw(Device& device, ID3D12GraphicsCommandList* list,
            const FrameParams& frame, const lmv_scene* scene,
            const SceneGPU& gpu);

 private:
  enum class Pass { Shadow, Main, Wire };
  // One drawable index range with its material (a mesh subset or a whole mesh).
  struct DrawItem {
    int32_t mesh_id = 0;
    const float* world = nullptr;  // borrowed from the scene for this frame
    uint32_t first = 0, count = 0, material = 0;
    float depth = 0.0f;  // squared distance from the eye (blended sort key)
  };
  void DrawMeshes(ID3D12GraphicsCommandList* list, const lmv_scene* scene,
                  const SceneGPU& gpu, UINT slot, Pass pass, const float eye[3]);
  void Emit(ID3D12GraphicsCommandList* list, const SceneGPU& gpu, UINT slot,
            const DrawItem& item, int32_t* bound_mesh);

  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_solid_;
  ComPtr<ID3D12PipelineState> pso_wire_;
  ComPtr<ID3D12PipelineState> pso_blend_;  // alpha blend, depth test only
  std::vector<DrawItem> opaque_items_, mask_items_, blend_items_;  // reused each frame
  ComPtr<ID3D12PipelineState> pso_shadow_;
  ComPtr<ID3D12PipelineState> pso_shadow_mask_;  // alpha-tested casters
  ComPtr<ID3D12Resource> dummy_;  // valid VA for unused vertex-stage SRVs
  ComPtr<ID3D12Resource> frame_cb_;  // kFrameCount slots of kFrameCbStride
  uint8_t* frame_cb_mapped_ = nullptr;
};

}  // namespace ld3d12
