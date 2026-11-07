#include "points_lines.hh"

#include <d3dcompiler.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

// Root: b0 = 32 DWORDs (view_proj, right, up, eye, misc), t0 = element data.
// lmv_splat is 8 floats (xyz, radius, rgba); lmv_line_segment is 11 floats
// (p0, p1, width, rgba), read as raw floats to avoid structured-buffer packing
// questions.
const char kShader[] = R"hlsl(
cbuffer C : register(b0) {
  float4x4 view_proj;
  float4 right;
  float4 up;
  float4 eye;
  float4 misc;   // x = pixel scale
};
StructuredBuffer<float> data : register(t0);
struct VSOut {
  float4 pos : SV_Position;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};
static const float2 kCorner[4] = {float2(-1, -1), float2(1, -1),
                                  float2(-1, 1), float2(1, 1)};

VSOut SplatVS(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
  uint b = inst * 8;
  float3 c = float3(data[b], data[b + 1], data[b + 2]);
  float r = data[b + 3];
  float4 clip = mul(view_proj, float4(c, 1.0));
  r = max(r, 0.75 * misc.x * clip.w);  // at least ~1.5 px across
  float2 q = kCorner[vid];
  float3 wp = c + (right.xyz * q.x + up.xyz * q.y) * r;
  VSOut o;
  o.pos = mul(view_proj, float4(wp, 1.0));
  o.color = float4(data[b + 4], data[b + 5], data[b + 6], data[b + 7]);
  o.uv = q;
  return o;
}

VSOut LineVS(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
  uint b = inst * 11;
  float3 p0 = float3(data[b], data[b + 1], data[b + 2]);
  float3 p1 = float3(data[b + 3], data[b + 4], data[b + 5]);
  float width = data[b + 6];
  float3 mid = 0.5 * (p0 + p1);
  float4 clip = mul(view_proj, float4(mid, 1.0));
  width = max(width, misc.x * clip.w);  // at least ~1 px wide
  float3 dir = p1 - p0;
  float3 side = cross(dir, eye.xyz - mid);
  side = dot(side, side) > 1e-20 ? normalize(side) : right.xyz;
  float3 base = (vid & 1) ? p1 : p0;
  float sgn = vid < 2 ? -1.0 : 1.0;
  VSOut o;
  o.pos = mul(view_proj, float4(base + side * (0.5 * width * sgn), 1.0));
  o.color = float4(data[b + 7], data[b + 8], data[b + 9], data[b + 10]);
  o.uv = float2(0.0, 0.0);
  return o;
}

float3 LinearToSRGB(float3 c) {
  float3 v = saturate(c);
  return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
float4 SplatPS(VSOut i) : SV_Target {
  float d = dot(i.uv, i.uv);
  if (d > 1.0) discard;
  return float4(LinearToSRGB(i.color.rgb), i.color.a);
}
float4 LinePS(VSOut i) : SV_Target {
  return float4(LinearToSRGB(i.color.rgb), i.color.a);
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
      D3DCompile(kShader, sizeof(kShader) - 1, "points_lines.hlsl", nullptr,
                 nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                 out->GetAddressOf(), messages.GetAddressOf());
  if (FAILED(hr)) return Fail(err, "D3DCompile", hr, messages.Get());
  return true;
}

}  // namespace

bool PointLineRenderer::Init(ID3D12Device* device, std::string* err) {
  ComPtr<ID3DBlob> splat_vs, splat_ps, line_vs, line_ps;
  if (!Compile("SplatVS", "vs_5_1", &splat_vs, err) ||
      !Compile("SplatPS", "ps_5_1", &splat_ps, err) ||
      !Compile("LineVS", "vs_5_1", &line_vs, err) ||
      !Compile("LinePS", "ps_5_1", &line_ps, err))
    return false;

  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.Num32BitValues = 32;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor.ShaderRegister = 0;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_ROOT_SIGNATURE_DESC rsd = {};
  rsd.NumParameters = 2;
  rsd.pParameters = params;
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
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.DepthStencilState.DepthEnable = TRUE;
  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = kBackbufferFormat;
  pd.DSVFormat = kDepthFormat;
  pd.SampleDesc.Count = 1;

  pd.VS = {splat_vs->GetBufferPointer(), splat_vs->GetBufferSize()};
  pd.PS = {splat_ps->GetBufferPointer(), splat_ps->GetBufferSize()};
  hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_splat_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(splat)", hr);
  pd.VS = {line_vs->GetBufferPointer(), line_vs->GetBufferSize()};
  pd.PS = {line_ps->GetBufferPointer(), line_ps->GetBufferSize()};
  hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_line_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(line)", hr);

  // UI overlay: the same line shader, drawn over everything.
  pd.DepthStencilState.DepthEnable = FALSE;
  hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_overlay_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(overlay)", hr);

  constexpr UINT64 kSlotBytes = UINT64(kMaxOverlaySegments) * 11 * sizeof(float);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = kSlotBytes * kFrameCount;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_PPV_ARGS(&overlay_ring_));
  if (FAILED(hr)) return Fail(err, "CreateCommittedResource(overlay)", hr);
  const D3D12_RANGE none = {0, 0};
  void* mapped = nullptr;
  hr = overlay_ring_->Map(0, &none, &mapped);
  if (FAILED(hr)) return Fail(err, "Map(overlay)", hr);
  overlay_mapped_ = static_cast<uint8_t*>(mapped);
  return true;
}

void PointLineRenderer::DrawOverlay(ID3D12GraphicsCommandList* list,
                                    const PointLineParams& params, UINT slot,
                                    const std::vector<float>& segments) {
  const uint32_t count = static_cast<uint32_t>(
      std::min<size_t>(segments.size() / 11, kMaxOverlaySegments));
  if (count == 0 || !overlay_mapped_) return;
  constexpr UINT64 kSlotBytes = UINT64(kMaxOverlaySegments) * 11 * sizeof(float);
  std::memcpy(overlay_mapped_ + UINT64(slot) * kSlotBytes, segments.data(),
              size_t(count) * 11 * sizeof(float));

  float constants[32] = {};
  std::memcpy(constants, params.view_proj, sizeof(params.view_proj));
  for (int i = 0; i < 3; ++i) {
    constants[16 + i] = params.view[i * 4 + 0];
    constants[20 + i] = params.view[i * 4 + 1];
    constants[24 + i] = params.eye[i];
  }
  constants[28] = params.pixel_scale;
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetGraphicsRoot32BitConstants(0, 32, constants, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  list->SetPipelineState(pso_overlay_.Get());
  list->SetGraphicsRootShaderResourceView(
      1, overlay_ring_->GetGPUVirtualAddress() + UINT64(slot) * kSlotBytes);
  list->DrawInstanced(4, count, 0, 0);
}

void PointLineRenderer::Draw(ID3D12GraphicsCommandList* list,
                             const PointLineParams& params,
                             const SceneGPU& gpu) {
  if (!gpu.splat_buffer && !gpu.line_buffer) return;
  float constants[32] = {};
  std::memcpy(constants, params.view_proj, sizeof(params.view_proj));
  // Camera right/up in world space: rows 0/1 of the view rotation.
  for (int i = 0; i < 3; ++i) {
    constants[16 + i] = params.view[i * 4 + 0];
    constants[20 + i] = params.view[i * 4 + 1];
    constants[24 + i] = params.eye[i];
  }
  constants[28] = params.pixel_scale;

  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetGraphicsRoot32BitConstants(0, 32, constants, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  if (gpu.splat_buffer && gpu.splat_count) {
    list->SetPipelineState(pso_splat_.Get());
    list->SetGraphicsRootShaderResourceView(
        1, gpu.splat_buffer->GetGPUVirtualAddress());
    list->DrawInstanced(4, gpu.splat_count, 0, 0);
  }
  if (gpu.line_buffer && gpu.line_count) {
    list->SetPipelineState(pso_line_.Get());
    list->SetGraphicsRootShaderResourceView(
        1, gpu.line_buffer->GetGPUVirtualAddress());
    list->DrawInstanced(4, gpu.line_count, 0, 0);
  }
}

}  // namespace ld3d12
