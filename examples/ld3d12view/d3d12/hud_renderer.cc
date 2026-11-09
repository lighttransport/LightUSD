#include "hud_renderer.hh"

#include <d3dcompiler.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

const char kShader[] = R"hlsl(
cbuffer C : register(b0) { float opacity; };
Texture2D hud : register(t0);
SamplerState point_clamp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint vid : SV_VertexID) {
  float2 p = float2((vid << 1) & 2, vid & 2);  // fullscreen triangle
  VSOut o;
  o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
  o.uv = p;
  return o;
}
float4 PSMain(VSOut i) : SV_Target {
  float4 c = hud.Sample(point_clamp, i.uv);
  return float4(c.rgb, c.a * opacity);  // panels are opaque in the texture
}
)hlsl";

bool Fail(std::string* err, const char* what, HRESULT hr,
          ID3DBlob* messages = nullptr) {
  if (err) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    *err = buf;
    if (messages && messages->GetBufferSize()) {
      err->append(": ");
      err->append(static_cast<const char*>(messages->GetBufferPointer()),
                  messages->GetBufferSize());
    }
  }
  return false;
}

bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>* out,
             std::string* err) {
  ComPtr<ID3DBlob> messages;
  const HRESULT hr =
      D3DCompile(kShader, sizeof(kShader) - 1, "hud.hlsl", nullptr, nullptr,
                 entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                 out->GetAddressOf(), messages.GetAddressOf());
  if (FAILED(hr)) return Fail(err, "D3DCompile", hr, messages.Get());
  return true;
}

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

}  // namespace

bool HudRenderer::Init(ID3D12Device* device, std::string* err) {
  ComPtr<ID3DBlob> vs, ps;
  if (!Compile("VSMain", "vs_5_1", &vs, err) ||
      !Compile("PSMain", "ps_5_1", &ps, err))
    return false;

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.OffsetInDescriptorsFromTableStart =
      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 1;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rsd = {};
  rsd.NumParameters = 2;
  rsd.pParameters = params;
  rsd.NumStaticSamplers = 1;
  rsd.pStaticSamplers = &sampler;
  ComPtr<ID3DBlob> blob, messages;
  HRESULT hr = D3D12SerializeRootSignature(
      &rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.GetAddressOf(),
      messages.GetAddressOf());
  if (FAILED(hr))
    return Fail(err, "D3D12SerializeRootSignature", hr, messages.Get());
  hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                   blob->GetBufferSize(),
                                   IID_PPV_ARGS(&root_signature_));
  if (FAILED(hr)) return Fail(err, "CreateRootSignature", hr);

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = root_signature_.Get();
  pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  D3D12_RENDER_TARGET_BLEND_DESC& rt = pd.BlendState.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
  rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  rt.BlendOp = D3D12_BLEND_OP_ADD;
  rt.SrcBlendAlpha = D3D12_BLEND_ONE;
  rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.DepthStencilState.DepthEnable = FALSE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = kBackbufferFormat;
  pd.DSVFormat = kDepthFormat;
  pd.SampleDesc.Count = 1;
  hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(hud)", hr);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_));
  if (FAILED(hr)) return Fail(err, "CreateDescriptorHeap(hud)", hr);
  return true;
}

bool HudRenderer::Resize(ID3D12Device* device, UINT width, UINT height,
                         std::string* err) {
  if (width == 0 || height == 0) return true;
  if (texture_ && width == width_ && height == height_) return true;
  texture_.Reset();
  upload_.Reset();
  upload_mapped_ = nullptr;
  width_ = width;
  height_ = height;

  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = width;
  rd.Height = height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rd.SampleDesc.Count = 1;
  HRESULT hr = device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd,
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
      IID_PPV_ARGS(&texture_));
  if (FAILED(hr)) return Fail(err, "CreateCommittedResource(hud texture)", hr);

  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = rd.Format;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(
      texture_.Get(), &sv, srv_heap_->GetCPUDescriptorHandleForHeapStart());

  row_pitch_ = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
               ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  slot_bytes_ = (UINT64(row_pitch_) * height +
                 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = slot_bytes_ * kFrameCount;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr = device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
      nullptr, IID_PPV_ARGS(&upload_));
  if (FAILED(hr)) return Fail(err, "CreateCommittedResource(hud upload)", hr);
  const D3D12_RANGE none = {0, 0};
  void* mapped = nullptr;
  hr = upload_->Map(0, &none, &mapped);
  if (FAILED(hr)) return Fail(err, "Map(hud upload)", hr);
  upload_mapped_ = static_cast<uint8_t*>(mapped);
  return true;
}

void HudRenderer::Upload(ID3D12GraphicsCommandList* list, UINT slot,
                         const uint32_t* pixels, int stride,
                         const std::vector<HudRows>& rows) {
  if (!texture_ || rows.empty() || !pixels) return;
  uint8_t* base = upload_mapped_ + UINT64(slot) * slot_bytes_;
  for (const HudRows& r : rows) {
    const UINT y0 = static_cast<UINT>(std::max(0, r.y0));
    const UINT y1 = std::min(height_, static_cast<UINT>(std::max(0, r.y1)));
    for (UINT y = y0; y < y1; ++y)
      std::memcpy(base + UINT64(y) * row_pitch_,
                  pixels + static_cast<size_t>(y) * stride, size_t(width_) * 4);
  }

  const D3D12_RESOURCE_BARRIER to_copy =
      Transition(texture_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_COPY_DEST);
  list->ResourceBarrier(1, &to_copy);
  for (const HudRows& r : rows) {
    const UINT y0 = static_cast<UINT>(std::max(0, r.y0));
    const UINT y1 = std::min(height_, static_cast<UINT>(std::max(0, r.y1)));
    if (y1 <= y0) continue;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = UINT64(slot) * slot_bytes_;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, width_,
                                     height_, 1, row_pitch_};
    const D3D12_BOX box = {0, y0, 0, width_, y1, 1};
    list->CopyTextureRegion(&dst, 0, y0, 0, &src, &box);
  }
  const D3D12_RESOURCE_BARRIER to_read =
      Transition(texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  list->ResourceBarrier(1, &to_read);
}

void HudRenderer::Draw(ID3D12GraphicsCommandList* list, float opacity) {
  if (!texture_) return;
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetPipelineState(pso_.Get());
  list->SetGraphicsRoot32BitConstants(0, 1, &opacity, 0);
  list->SetGraphicsRootDescriptorTable(
      1, srv_heap_->GetGPUDescriptorHandleForHeapStart());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace ld3d12
