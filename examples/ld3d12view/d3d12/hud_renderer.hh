// ld3d12view - composites the lightui HUD surface over the 3D view.
//
// The HUD is a window-sized B8G8R8A8 texture in default-heap memory. Changed
// rows are written into a per-swapchain-slot upload buffer and copied into the
// texture on the frame's command list; one alpha-blended fullscreen triangle
// then draws it with a constant opacity.
#pragma once

#include "../hud.hh"
#include "device.hh"

namespace ld3d12 {

class HudRenderer {
 public:
  bool Init(ID3D12Device* device, std::string* err);
  // (Re)creates the texture and upload ring for a `width` x `height` surface.
  // The GPU must be idle (the old texture is released).
  bool Resize(ID3D12Device* device, UINT width, UINT height, std::string* err);
  // Copies the given full-width row ranges of `pixels` (0xAARRGGBB, `stride`
  // in uint32 units) into the texture. Record before Draw, on the main list.
  void Upload(ID3D12GraphicsCommandList* list, UINT slot, const uint32_t* pixels,
              int stride, const std::vector<HudRows>& rows);
  void Draw(ID3D12GraphicsCommandList* list, float opacity);

 private:
  ComPtr<ID3D12RootSignature> root_signature_;
  ComPtr<ID3D12PipelineState> pso_;
  ComPtr<ID3D12DescriptorHeap> srv_heap_;
  ComPtr<ID3D12Resource> texture_;
  ComPtr<ID3D12Resource> upload_;
  uint8_t* upload_mapped_ = nullptr;
  UINT width_ = 0, height_ = 0;
  UINT row_pitch_ = 0;
  UINT64 slot_bytes_ = 0;
};

}  // namespace ld3d12
