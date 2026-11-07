#include "background.hh"

#include <d3dcompiler.h>

#include <climits>
#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

const char kShader[] = R"hlsl(
cbuffer C : register(b0) {
  float4x4 inv_view_proj;
  float4 eye;
  float4 params;  // x has env, y intensity, z up axis (1 = Z), w exposure stops
  float4 cm0;
  float4 cm1;
  float4 cm2;
};
Texture2D env : register(t0);
SamplerState linear_wrap : register(s0);
static const float PI = 3.14159265;

struct VSOut { float4 pos : SV_Position; float2 ndc : TEXCOORD0; };
VSOut VSMain(uint vid : SV_VertexID) {
  float2 p = float2((vid << 1) & 2, vid & 2);  // fullscreen triangle
  VSOut o;
  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
  o.ndc = p * 2.0 - 1.0;
  return o;
}

float3 LinearToSRGB(float3 c) {
  float3 v = saturate(c);
  return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}
// Khronos PBR Neutral tone mapping (same curve as the mesh shader).
float3 Tonemap(float3 color, float stops) {
  color *= exp2(stops);
  color = max(color, 0.0);
  const float start = 0.8 - 0.04;
  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;
  float peak = max(color.r, max(color.g, color.b));
  if (peak < start) return color;
  float d = 1.0 - start;
  float new_peak = 1.0 - d * d / (peak + d - start);
  color *= new_peak / peak;
  float g = 1.0 - 1.0 / (0.15 * (peak - new_peak) + 1.0);
  return lerp(color, float3(new_peak, new_peak, new_peak), g);
}

float4 PSMain(VSOut i) : SV_Target {
  float4 far_point = mul(inv_view_proj, float4(i.ndc, 0.5, 1.0));
  float3 dir = normalize(far_point.xyz / far_point.w - eye.xyz);
  float3 c;
  if (params.x > 0.5) {
    // Same equirect mapping as the mesh shader's reflections.
    float u = atan2(dir.z, dir.x) / (2.0 * PI) + 0.5;
    float v = 1.0 - (asin(clamp(dir.y, -1.0, 1.0)) / PI + 0.5);
    float3 radiance = env.SampleLevel(linear_wrap, float2(u, v), 1.0).rgb * params.y;
    c = float3(dot(cm0.xyz, radiance), dot(cm1.xyz, radiance), dot(cm2.xyz, radiance));
  } else {
    float up = saturate((params.z > 0.5 ? dir.z : dir.y) * 0.5 + 0.5);
    c = lerp(float3(0.055, 0.06, 0.07), float3(0.16, 0.175, 0.20), up);
  }
  return float4(LinearToSRGB(Tonemap(c, params.w)), 1.0);
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
      D3DCompile(kShader, sizeof(kShader) - 1, "background.hlsl", nullptr,
                 nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                 out->GetAddressOf(), messages.GetAddressOf());
  if (FAILED(hr)) return Fail(err, "D3DCompile", hr, messages.Get());
  return true;
}

}  // namespace

bool BackgroundRenderer::Init(ID3D12Device* device, std::string* err) {
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
  params[0].Constants.Num32BitValues = 36;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_WRAP;
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
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
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
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState(background)", hr);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_));
  if (FAILED(hr)) return Fail(err, "CreateDescriptorHeap(background)", hr);
  SetEnvironment(device, nullptr);
  return true;
}

void BackgroundRenderer::SetEnvironment(ID3D12Device* device,
                                        ID3D12Resource* environment) {
  has_env_ = environment != nullptr;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // placeholder for the null view
  sv.Texture2D.MipLevels = 1;
  if (environment) {
    const D3D12_RESOURCE_DESC rd = environment->GetDesc();
    sv.Format = rd.Format;
    sv.Texture2D.MipLevels = rd.MipLevels;
  }
  device->CreateShaderResourceView(
      environment, &sv, srv_heap_->GetCPUDescriptorHandleForHeapStart());
}

void BackgroundRenderer::Draw(ID3D12GraphicsCommandList* list,
                              const BackgroundParams& p) {
  float constants[36] = {};
  std::memcpy(constants, p.inv_view_proj, sizeof(p.inv_view_proj));
  constants[16] = p.eye[0];
  constants[17] = p.eye[1];
  constants[18] = p.eye[2];
  constants[20] = has_env_ ? 1.0f : 0.0f;
  constants[21] = p.env_intensity;
  constants[22] = p.up_axis == 1 ? 1.0f : 0.0f;
  constants[23] = p.exposure_stops;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) constants[24 + r * 4 + c] = p.color_matrix[r * 3 + c];

  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetPipelineState(pso_.Get());
  list->SetGraphicsRoot32BitConstants(0, 36, constants, 0);
  list->SetGraphicsRootDescriptorTable(
      1, srv_heap_->GetGPUDescriptorHandleForHeapStart());
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
}

}  // namespace ld3d12
