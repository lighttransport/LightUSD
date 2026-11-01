#include "device.hh"

#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

bool Fail(std::string* err, const char* what, HRESULT hr) {
  if (err) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    *err = buf;
  }
  return false;
}

#define LD3D_TRY(expr, what)                         \
  do {                                               \
    const HRESULT hr_ = (expr);                      \
    if (FAILED(hr_)) return Fail(err, what, hr_);    \
  } while (0)

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

std::string NarrowName(const wchar_t* w) {
  char buf[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr,
                      nullptr);
  return buf;
}

}  // namespace

Device::~Device() {
  if (queue_ && fence_) WaitIdle();
  if (fence_event_) CloseHandle(fence_event_);
}

bool Device::Init(const DeviceDesc& desc, std::string* err) {
  UINT factory_flags = 0;
  if (desc.debug) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
  LD3D_TRY(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)),
           "CreateDXGIFactory2");

  ComPtr<IDXGIAdapter1> adapter;
  if (!desc.warp) {
    for (UINT i = 0; factory_->EnumAdapterByGpuPreference(
                         i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                         IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
      DXGI_ADAPTER_DESC1 ad = {};
      adapter->GetDesc1(&ad);
      if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                      __uuidof(ID3D12Device), nullptr))) {
        break;
      }
      adapter.Reset();
    }
  }
  if (!adapter) {
    LD3D_TRY(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),
             "EnumWarpAdapter");
  }
  DXGI_ADAPTER_DESC1 ad = {};
  adapter->GetDesc1(&ad);
  adapter_name_ = NarrowName(ad.Description);
  LD3D_TRY(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device_)),
           "D3D12CreateDevice");

  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  LD3D_TRY(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)),
           "CreateCommandQueue");

  width_ = desc.width ? desc.width : 1;
  height_ = desc.height ? desc.height : 1;
  DXGI_SWAP_CHAIN_DESC1 sd = {};
  sd.Width = width_;
  sd.Height = height_;
  sd.Format = kBackbufferFormat;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kFrameCount;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> sc1;
  LD3D_TRY(factory_->CreateSwapChainForHwnd(queue_.Get(), desc.hwnd, &sd,
                                            nullptr, nullptr, &sc1),
           "CreateSwapChainForHwnd");
  factory_->MakeWindowAssociation(desc.hwnd, DXGI_MWA_NO_ALT_ENTER);
  LD3D_TRY(sc1.As(&swapchain_), "IDXGISwapChain3");

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = kFrameCount;
  LD3D_TRY(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)),
           "CreateDescriptorHeap(RTV)");
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  hd.NumDescriptors = 1;
  LD3D_TRY(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)),
           "CreateDescriptorHeap(DSV)");
  rtv_stride_ = device_->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  for (UINT i = 0; i < kFrameCount; ++i) {
    LD3D_TRY(device_->CreateCommandAllocator(
                 D3D12_COMMAND_LIST_TYPE_DIRECT,
                 IID_PPV_ARGS(&allocators_[i])),
             "CreateCommandAllocator");
  }
  LD3D_TRY(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      allocators_[0].Get(), nullptr,
                                      IID_PPV_ARGS(&list_)),
           "CreateCommandList");
  list_->Close();
  LD3D_TRY(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                IID_PPV_ARGS(&fence_)),
           "CreateFence");
  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fence_event_) return Fail(err, "CreateEvent", E_FAIL);

  frame_index_ = swapchain_->GetCurrentBackBufferIndex();
  return CreateTargets(err);
}

bool Device::CreateTargets(std::string* err) {
  D3D12_CPU_DESCRIPTOR_HANDLE rtv =
      rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < kFrameCount; ++i) {
    LD3D_TRY(swapchain_->GetBuffer(i, IID_PPV_ARGS(&backbuffers_[i])),
             "IDXGISwapChain::GetBuffer");
    device_->CreateRenderTargetView(backbuffers_[i].Get(), nullptr, rtv);
    rtv.ptr += rtv_stride_;
  }

  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = width_;
  rd.Height = height_;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = kDepthFormat;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE cv = {};
  cv.Format = kDepthFormat;
  cv.DepthStencil.Depth = 1.0f;
  LD3D_TRY(device_->CreateCommittedResource(
               &hp, D3D12_HEAP_FLAG_NONE, &rd,
               D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depth_)),
           "CreateCommittedResource(depth)");
  device_->CreateDepthStencilView(
      depth_.Get(), nullptr, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
  return true;
}

void Device::ReleaseTargets() {
  for (auto& b : backbuffers_) b.Reset();
  depth_.Reset();
}

void Device::WaitForFence(UINT64 value) {
  if (fence_->GetCompletedValue() >= value) return;
  fence_->SetEventOnCompletion(value, fence_event_);
  WaitForSingleObject(fence_event_, INFINITE);
}

void Device::WaitIdle() {
  const UINT64 value = ++next_fence_;
  queue_->Signal(fence_.Get(), value);
  WaitForFence(value);
}

bool Device::ExecuteAndWait(ID3D12GraphicsCommandList* list,
                            std::string* err) {
  ID3D12CommandList* lists[] = {list};
  queue_->ExecuteCommandLists(1, lists);
  const UINT64 value = ++next_fence_;
  LD3D_TRY(queue_->Signal(fence_.Get(), value), "ID3D12CommandQueue::Signal");
  WaitForFence(value);
  return true;
}

bool Device::Resize(UINT width, UINT height, std::string* err) {
  if (!swapchain_) return false;
  if (width == 0 || height == 0) return true;  // minimized
  if (width == width_ && height == height_) return true;
  WaitIdle();
  ReleaseTargets();
  width_ = width;
  height_ = height;
  LD3D_TRY(swapchain_->ResizeBuffers(kFrameCount, width_, height_,
                                     kBackbufferFormat, 0),
           "ResizeBuffers");
  frame_index_ = swapchain_->GetCurrentBackBufferIndex();
  for (UINT64& v : fence_values_) v = 0;
  return CreateTargets(err);
}

void Device::BindMainTargets(ID3D12GraphicsCommandList* list) {
  D3D12_CPU_DESCRIPTOR_HANDLE rtv =
      rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(frame_index_) * rtv_stride_;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
      dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  const D3D12_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(width_),
                             static_cast<float>(height_), 0.0f, 1.0f};
  const D3D12_RECT sc = {0, 0, static_cast<LONG>(width_),
                         static_cast<LONG>(height_)};
  list->RSSetViewports(1, &vp);
  list->RSSetScissorRects(1, &sc);
}

ID3D12GraphicsCommandList* Device::BeginFrame(const float clear_rgba[4]) {
  WaitForFence(fence_values_[frame_index_]);
  if (FAILED(allocators_[frame_index_]->Reset())) return nullptr;
  if (FAILED(list_->Reset(allocators_[frame_index_].Get(), nullptr)))
    return nullptr;

  ID3D12Resource* bb = backbuffers_[frame_index_].Get();
  const D3D12_RESOURCE_BARRIER to_rt = Transition(
      bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  list_->ResourceBarrier(1, &to_rt);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv =
      rtv_heap_->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(frame_index_) * rtv_stride_;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
      dsv_heap_->GetCPUDescriptorHandleForHeapStart();
  list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  list_->ClearRenderTargetView(rtv, clear_rgba, 0, nullptr);
  list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                               nullptr);

  const D3D12_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(width_),
                             static_cast<float>(height_), 0.0f, 1.0f};
  const D3D12_RECT sc = {0, 0, static_cast<LONG>(width_),
                         static_cast<LONG>(height_)};
  list_->RSSetViewports(1, &vp);
  list_->RSSetScissorRects(1, &sc);
  return list_.Get();
}

bool Device::EndFrame(Image* capture, std::string* err) {
  ID3D12Resource* bb = backbuffers_[frame_index_].Get();
  ComPtr<ID3D12Resource> readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT64 total_bytes = 0;

  if (capture) {
    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    device_->GetCopyableFootprints(&bd, 0, 1, 0, &footprint, nullptr, nullptr,
                                   &total_bytes);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total_bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    LD3D_TRY(device_->CreateCommittedResource(
                 &hp, D3D12_HEAP_FLAG_NONE, &rd,
                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                 IID_PPV_ARGS(&readback)),
             "CreateCommittedResource(readback)");
    const D3D12_RESOURCE_BARRIER to_src =
        Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->ResourceBarrier(1, &to_src);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = bb;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER to_present =
        Transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &to_present);
  } else {
    const D3D12_RESOURCE_BARRIER to_present =
        Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &to_present);
  }

  LD3D_TRY(list_->Close(), "ID3D12GraphicsCommandList::Close");
  ID3D12CommandList* lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);

  const HRESULT phr = swapchain_->Present(1, 0);
  if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET) {
    return Fail(err, "Present (device removed)",
                device_->GetDeviceRemovedReason());
  }
  LD3D_TRY(phr, "Present");

  const UINT64 value = ++next_fence_;
  queue_->Signal(fence_.Get(), value);
  fence_values_[frame_index_] = value;

  if (capture) {
    WaitForFence(value);
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(total_bytes)};
    LD3D_TRY(readback->Map(0, &range, &mapped), "Map(readback)");
    capture->width = width_;
    capture->height = height_;
    capture->rgba.resize(static_cast<size_t>(width_) * height_ * 4);
    const uint8_t* srcp = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < height_; ++y) {
      std::memcpy(&capture->rgba[static_cast<size_t>(y) * width_ * 4],
                  srcp + footprint.Offset +
                      static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                  static_cast<size_t>(width_) * 4);
    }
    readback->Unmap(0, nullptr);
  }

  frame_index_ = swapchain_->GetCurrentBackBufferIndex();
  return true;
}

}  // namespace ld3d12
