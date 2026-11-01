// ld3d12view - ground grid + axis triad (line list). Also the first pipeline
// of the viewer: it exercises the root signature, runtime HLSL compile (D3DCompile),
// PSO creation and depth testing before any scene content exists.
#pragma once

#include "device.hh"

namespace ld3d12 {

class GridRenderer {
 public:
  // Y-up grid on the XZ plane, `half_extent` units each way, 1 unit spacing.
  bool Init(ID3D12Device* device, int half_extent, std::string* err);
  // `view_proj` is column-major (proj * view).
  void Draw(ID3D12GraphicsCommandList* list, const float view_proj[16]);

 private:
  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_;
  ComPtr<ID3D12Resource> vertex_buffer_;
  D3D12_VERTEX_BUFFER_VIEW vbv_ = {};
  UINT vertex_count_ = 0;
};

}  // namespace ld3d12
