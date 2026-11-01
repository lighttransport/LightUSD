// ld3d12view - view background: the dome light's environment image when the
// scene has one, otherwise a soft vertical gradient. One fullscreen triangle
// drawn first (no depth), reconstructing each pixel's world-space view ray from
// the inverse view-projection matrix.
#pragma once

#include "device.hh"

namespace ld3d12 {

struct BackgroundParams {
  float inv_view_proj[16];      // column-major
  float eye[3];
  float exposure_stops = 0.0f;
  int up_axis = 0;              // 0 = Y up, 1 = Z up (gradient direction)
  float env_intensity = 1.0f;
  float color_matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // working -> display
};

class BackgroundRenderer {
 public:
  bool Init(ID3D12Device* device, std::string* err);
  // Selects the environment image (an equirectangular 2D texture) or none. The
  // GPU must be idle: the previous view may still be referenced.
  void SetEnvironment(ID3D12Device* device, ID3D12Resource* environment);
  void Draw(ID3D12GraphicsCommandList* list, const BackgroundParams& params);

 private:
  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_;
  ComPtr<ID3D12DescriptorHeap> srv_heap_;
  bool has_env_ = false;
};

}  // namespace ld3d12
