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
  bool Init(ID3D12Device* device, std::string* err);
  void Draw(ID3D12GraphicsCommandList* list, const PointLineParams& params,
            const SceneGPU& gpu);

 private:
  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_splat_;
  ComPtr<ID3D12PipelineState> pso_line_;
};

}  // namespace ld3d12
