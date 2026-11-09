// ld3d12view - D3D12 device, swapchain and frame ring.
//
// One direct queue, a flip-model swapchain with kFrameCount buffers, a
// D32_FLOAT depth buffer and a per-backbuffer command allocator. Renderers
// record into the command list returned by BeginFrame().
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ld3d12 {

using Microsoft::WRL::ComPtr;

constexpr UINT kFrameCount = 3;
constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

struct DeviceDesc {
  HWND hwnd = nullptr;
  UINT width = 0;
  UINT height = 0;
  bool debug = false;  // enable the D3D12 debug layer when available
  bool warp = false;   // force the WARP software adapter
};

struct Image {
  UINT width = 0;
  UINT height = 0;
  std::vector<uint8_t> rgba;  // tightly packed, top row first
};

class Device {
 public:
  Device() = default;
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  bool Init(const DeviceDesc& desc, std::string* err);
  bool Resize(UINT width, UINT height, std::string* err);
  void WaitIdle();

  // Starts a frame: waits for the frame slot, transitions the backbuffer to
  // render target, binds RTV/DSV, clears and sets viewport/scissor. Returns
  // nullptr on failure.
  ID3D12GraphicsCommandList* BeginFrame(const float clear_rgba[4]);
  // Finishes and presents the frame. When `capture` is non-null the final
  // backbuffer contents are copied into it (synchronously).
  bool EndFrame(Image* capture, std::string* err);

  ID3D12Device* device() const { return device_.Get(); }
  // Executes a closed command list on the direct queue and blocks until the
  // GPU has finished it (resource uploads).
  bool ExecuteAndWait(ID3D12GraphicsCommandList* list, std::string* err);
  UINT frame_index() const { return frame_index_; }
  // Re-binds the backbuffer + depth buffer and full-window viewport/scissor
  // (after an off-screen pass such as the shadow map).
  void BindMainTargets(ID3D12GraphicsCommandList* list);
  UINT width() const { return width_; }
  UINT height() const { return height_; }
  const std::string& adapter_name() const { return adapter_name_; }

  // Debug layer: true when the layer is active (Graphics Tools installed).
  bool debug_active() const { return info_queue_ != nullptr; }
  // Prints and counts pending debug-layer messages; returns the running total
  // of ERROR/CORRUPTION messages. No-op (returns 0) without the debug layer.
  uint32_t DrainValidation();
  uint32_t validation_errors() const { return validation_errors_; }

 private:
  bool CreateTargets(std::string* err);
  void ReleaseTargets();
  void WaitForFence(UINT64 value);

  ComPtr<IDXGIFactory6> factory_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> swapchain_;
  ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  ComPtr<ID3D12DescriptorHeap> dsv_heap_;
  ComPtr<ID3D12Resource> backbuffers_[kFrameCount];
  ComPtr<ID3D12Resource> depth_;
  ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  UINT64 fence_values_[kFrameCount] = {};
  UINT64 next_fence_ = 0;
  UINT frame_index_ = 0;
  UINT rtv_stride_ = 0;
  UINT width_ = 0;
  UINT height_ = 0;
  std::string adapter_name_;
  ComPtr<ID3D12InfoQueue> info_queue_;
  uint32_t validation_errors_ = 0;
};

}  // namespace ld3d12
