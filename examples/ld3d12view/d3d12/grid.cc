#include "grid.hh"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace ld3d12 {
namespace {

// 16 root constants: the column-major view-projection matrix. HLSL float4x4
// is column-major by default, so mul(view_proj, v) matches the C ABI.
const char kShader[] = R"hlsl(
cbuffer Frame : register(b0) { float4x4 view_proj; };
struct VSIn  { float3 pos : POSITION; float4 color : COLOR; };
struct VSOut { float4 pos : SV_Position; float4 color : COLOR; };
VSOut VSMain(VSIn i) {
  VSOut o;
  o.pos = mul(view_proj, float4(i.pos, 1.0));
  o.color = i.color;
  return o;
}
float4 PSMain(VSOut i) : SV_Target { return i.color; }
)hlsl";

struct Vertex {
  float pos[3];
  float color[4];
};

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
      D3DCompile(kShader, sizeof(kShader) - 1, "grid.hlsl", nullptr, nullptr,
                 entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                 out->GetAddressOf(), messages.GetAddressOf());
  if (FAILED(hr)) return Fail(err, "D3DCompile", hr, messages.Get());
  return true;
}

}  // namespace

bool GridRenderer::Init(ID3D12Device* device, int half_extent,
                        std::string* err) {
  ComPtr<ID3DBlob> vs, ps;
  if (!Compile("VSMain", "vs_5_1", &vs, err)) return false;
  if (!Compile("PSMain", "ps_5_1", &ps, err)) return false;

  D3D12_ROOT_PARAMETER param = {};
  param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  param.Constants.Num32BitValues = 16;
  param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  D3D12_ROOT_SIGNATURE_DESC rsd = {};
  rsd.NumParameters = 1;
  rsd.pParameters = &param;
  rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, messages;
  HRESULT hr = D3D12SerializeRootSignature(
      &rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, blob.GetAddressOf(),
      messages.GetAddressOf());
  if (FAILED(hr)) return Fail(err, "D3D12SerializeRootSignature", hr,
                              messages.Get());
  hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                   blob->GetBufferSize(),
                                   IID_PPV_ARGS(&root_signature_));
  if (FAILED(hr)) return Fail(err, "CreateRootSignature", hr);

  const D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
  pd.pRootSignature = root_signature_.Get();
  pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.SampleMask = UINT_MAX;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.DepthStencilState.DepthEnable = TRUE;
  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pd.InputLayout = {layout, 2};
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = kBackbufferFormat;
  pd.DSVFormat = kDepthFormat;
  pd.SampleDesc.Count = 1;
  hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));
  if (FAILED(hr)) return Fail(err, "CreateGraphicsPipelineState", hr);

  // Geometry: grid lines (the two through the origin are coloured axes) and a
  // unit-ish axis triad that rises above the plane to show depth testing.
  std::vector<Vertex> v;
  const float n = static_cast<float>(half_extent);
  const float grey[4] = {0.38f, 0.38f, 0.40f, 1.0f};
  auto line = [&](float ax, float ay, float az, float bx, float by, float bz,
                  const float c[4]) {
    v.push_back({{ax, ay, az}, {c[0], c[1], c[2], c[3]}});
    v.push_back({{bx, by, bz}, {c[0], c[1], c[2], c[3]}});
  };
  for (int i = -half_extent; i <= half_extent; ++i) {
    if (i == 0) continue;
    const float f = static_cast<float>(i);
    line(-n, 0, f, n, 0, f, grey);
    line(f, 0, -n, f, 0, n, grey);
  }
  const float red[4] = {0.85f, 0.25f, 0.25f, 1.0f};
  const float green[4] = {0.30f, 0.80f, 0.30f, 1.0f};
  const float blue[4] = {0.30f, 0.45f, 0.90f, 1.0f};
  line(-n, 0, 0, n, 0, 0, red);
  line(0, 0, -n, 0, 0, n, blue);
  line(0, 0, 0, 0, 2, 0, green);
  vertex_count_ = static_cast<UINT>(v.size());

  const UINT64 bytes = v.size() * sizeof(Vertex);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = bytes;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr = device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
      nullptr, IID_PPV_ARGS(&vertex_buffer_));
  if (FAILED(hr)) return Fail(err, "CreateCommittedResource(grid vb)", hr);
  void* mapped = nullptr;
  const D3D12_RANGE none = {0, 0};
  hr = vertex_buffer_->Map(0, &none, &mapped);
  if (FAILED(hr)) return Fail(err, "Map(grid vb)", hr);
  std::memcpy(mapped, v.data(), static_cast<size_t>(bytes));
  vertex_buffer_->Unmap(0, nullptr);
  vbv_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
  vbv_.SizeInBytes = static_cast<UINT>(bytes);
  vbv_.StrideInBytes = sizeof(Vertex);
  return true;
}

void GridRenderer::Draw(ID3D12GraphicsCommandList* list,
                        const float view_proj[16]) {
  list->SetGraphicsRootSignature(root_signature_.Get());
  list->SetPipelineState(pso_.Get());
  list->SetGraphicsRoot32BitConstants(0, 16, view_proj, 0);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
  list->IASetVertexBuffers(0, 1, &vbv_);
  list->DrawInstanced(vertex_count_, 1, 0, 0);
}

}  // namespace ld3d12
