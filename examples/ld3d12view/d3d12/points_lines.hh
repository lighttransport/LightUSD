// ld3d12view - UsdGeomPoints / particle splats (camera-facing discs) and curve
// segments (camera-facing ribbons). Both are one instanced quad per element,
// read from a structured buffer, alpha blended and depth tested without depth
// writes. Elements have a one-pixel minimum screen size so thin data stays
// visible.
#pragma once

#include "scene_gpu.hh"

namespace ld3d12 {

struct PointLineParams {
  float view_proj[16];  // column-major proj * view
  float view[16];       // column-major view (camera right/up)
  float eye[3];
  float pixel_scale;    // world units per pixel at unit view depth (0 = ortho)
};

class PointLineRenderer {
 public:
  // Segments per frame that DrawOverlay can show.
  static constexpr uint32_t kMaxOverlaySegments = 256;

  bool Init(ID3D12Device* device, std::string* err);
  void Draw(ID3D12GraphicsCommandList* list, const PointLineParams& params,
            const SceneGPU& gpu);
  // Draws UI line segments (raw lmv_line_segment floats: 11 per segment) on top
  // of the scene, without depth testing. `slot` is the swapchain buffer index
  // (one upload region each). Extra segments beyond kMaxOverlaySegments are
  // dropped.
  void DrawOverlay(ID3D12GraphicsCommandList* list, const PointLineParams& params,
                   UINT slot, const std::vector<float>& segments);

 private:
  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_splat_;
  ComPtr<ID3D12PipelineState> pso_line_;
  ComPtr<ID3D12PipelineState> pso_overlay_;  // lines, no depth test
  ComPtr<ID3D12Resource> overlay_ring_;       // kFrameCount slots, persistently mapped
  uint8_t* overlay_mapped_ = nullptr;
};

}  // namespace ld3d12
