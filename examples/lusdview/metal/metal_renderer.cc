// SPDX-License-Identifier: Apache-2.0
// Native Metal backend. Metal.hpp dispatches directly through Objective-C
// runtime selectors; there is no Vulkan/MoltenVK translation or SDK dependency.
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "renderer.hh"
#include "shaders.hh"

namespace lusdview {
void* AttachMetalSurface(GLFWwindow*, void*);
void DetachMetalSurface(GLFWwindow*, void*);
namespace {
template <class T>
struct Release {
  void operator()(T* p) const {
    if (p) p->release();
  }
};
template <class T>
using Owned = std::unique_ptr<T, Release<T>>;
using Pool = Owned<NS::AutoreleasePool>;
Pool MakePool() { return Pool(NS::AutoreleasePool::alloc()->init()); }
struct Vertex {
  DrawVertex v;
  float color[4];
};
struct Instance {
  float rows[12];
  float tint[4];
};
struct Uniforms {
  float view[16], proj[16];
  float camera[4], light[4], lightColor[4], base[4], emissive[4], options[4];
  float planes[32];
  int mode, planeCount, geometricNormal, padding;
};
static_assert(sizeof(Vertex) == 48 && sizeof(Instance) == 64);
static_assert(sizeof(ImDrawVert) == 20 && offsetof(ImDrawVert, col) == 16,
              "Metal GUI shader must match ImDrawVert");
static_assert(sizeof(Uniforms) == 368);

class MetalRenderer final : public Renderer {
 public:
  ~MetalRenderer() override { shutdown(); }
  bool init(GLFWwindow* window, std::string* err) override;
  bool initImGui(std::string* err) override;
  void setHeadlessSize(int w, int h) override {
    winW_ = std::max(1, w);
    winH_ = std::max(1, h);
  }
  bool resizeHeadless(int w, int h) override {
    if (window_ || w <= 0 || h <= 0) return false;
    setHeadlessSize(w, h);
    return true;
  }
  void beginScene(const std::vector<DrawMaterialCPU>& materials,
                  int count) override {
    meshes_.clear();
    textures_.clear();
    syncSceneResources(materials, count);
  }
  void syncSceneResources(const std::vector<DrawMaterialCPU>& materials,
                          int count) override {
    materials_ = materials;
    textures_.resize(std::max(0, count));
  }
  bool updateMaterialConstants(int id, const DrawMaterialCPU& m,
                               std::string* err) override {
    if (id < 0 || size_t(id) >= materials_.size()) {
      if (err) *err = "Metal material slot out of range";
      return false;
    }
    materials_[size_t(id)] = m;
    return true;
  }
  void appendMesh(const DrawMeshCPU& mesh) override;
  void updateMeshVertices(int index,
                          const std::vector<DrawVertex>& vertices) override;
  void updateMeshWorld(int index, const float world[16]) override;
  void updateInstanceVisibility(size_t index, const float* xforms,
                                const float* colors, const float* opacities,
                                uint32_t count) override;
  int meshCount() const override { return int(meshes_.size()); }
  void uploadTexture(int slot, const DrawTextureCPU& tex) override;
  void evictTexture(int slot) override {
    if (slot >= 0 && size_t(slot) < textures_.size())
      textures_[size_t(slot)].reset();
  }
  size_t textureResidentBytes(int slot) const override {
    auto* t = slot >= 0 && size_t(slot) < textures_.size()
                  ? textures_[size_t(slot)].get()
                  : nullptr;
    return t ? t->width() * t->height() * 4 : 0;
  }
  bool updateTextureRegion(int slot, int x, int y, int w, int h,
                           const uint8_t* rgba, size_t rowBytes) override;
  void resizeViewport(int w, int h) override;
  void viewportSize(int* w, int* h) const override {
    if (w) *w = width_;
    if (h) *h = height_;
  }
  void newFrame() override {
    if (window_) ImGui_ImplGlfw_NewFrame();
  }
  void renderFrame(const RenderFrameParams& params) override;
  ViewportTexHandle viewportTexture() const override {
    return uint64_t(uintptr_t(color_.get()));
  }
  bool uploadViewportImage(const uint8_t* rgba, int w, int h) override;
  void present() override;
  bool captureViewport(std::vector<uint8_t>* rgba, int* w, int* h) override {
    return readback(color_.get(), rgba, w, h);
  }
  bool captureWindow(std::vector<uint8_t>* rgba, int* w, int* h) override {
    return readback(composite_.get(), rgba, w, h);
  }
  const RendererCaps& caps() const override { return caps_; }
  void shutdown() override;

 private:
  struct Mesh {
    Owned<MTL::Buffer> vertices, indices, instances;
    size_t vertexCount{0}, indexCount{0}, instanceCount{0};
    bool instanced{false}, doubleSided{false}, geometricNormal{false};
    float flatColor[3]{1, 1, 1};
    float flatOpacity{1};
    std::vector<DrawSubmesh> submeshes;
  };
  Owned<MTL::Texture> texture(int w, int h, MTL::PixelFormat format,
                              bool target);
  Owned<MTL::RenderPipelineState> pipeline(MTL::Library* lib, const char* vs,
                                           const char* fs,
                                           MTL::PixelFormat format, bool depth,
                                           std::string* err);
  bool complete(MTL::CommandBuffer* cb);
  bool readback(MTL::Texture*, std::vector<uint8_t>*, int*, int*);
  void drawGui(MTL::CommandBuffer*, MTL::Texture*, ImDrawData*);
  void updateGuiTextures(ImDrawData*);
  void setInstances(Mesh&, const float*, const float*, const float*, uint32_t);
  GLFWwindow* window_{nullptr};
  Owned<CA::MetalLayer> layer_;
  Owned<MTL::Device> device_;
  Owned<MTL::CommandQueue> queue_;
  Owned<MTL::RenderPipelineState> scenePipeline_, guiPipeline_, blitPipeline_;
  Owned<MTL::DepthStencilState> depthState_;
  Owned<MTL::Texture> color_, depth_, composite_, white_;
  std::vector<Mesh> meshes_;
  std::vector<Owned<MTL::Texture>> textures_;
  // A GUI draw list can still reference the old viewport after an overlay resize.
  std::vector<Owned<MTL::Texture>> retiredTargets_;
  std::vector<DrawMaterialCPU> materials_;
  RendererCaps caps_;
  bool imgui_{false}, platform_{false};
  int width_{0}, height_{0}, winW_{1280}, winH_{800};
};

Owned<MTL::Texture> MetalRenderer::texture(int w, int h,
                                           MTL::PixelFormat format,
                                           bool target) {
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return {};
  auto desc = MTL::TextureDescriptor::texture2DDescriptor(format, size_t(w),
                                                          size_t(h), false);
  desc->setStorageMode(MTL::StorageModeShared);
  desc->setUsage(target ? MTL::TextureUsageRenderTarget |
                              MTL::TextureUsageShaderRead
                        : MTL::TextureUsageShaderRead);
  return Owned<MTL::Texture>(device_->newTexture(desc));
}
Owned<MTL::RenderPipelineState> MetalRenderer::pipeline(
    MTL::Library* lib, const char* vs, const char* fs, MTL::PixelFormat format,
    bool depth, std::string* err) {
  Owned<MTL::Function> v(
      lib->newFunction(NS::String::string(vs, NS::UTF8StringEncoding)));
  Owned<MTL::Function> f(
      lib->newFunction(NS::String::string(fs, NS::UTF8StringEncoding)));
  auto d = MTL::RenderPipelineDescriptor::alloc()->init();
  d->setVertexFunction(v.get());
  d->setFragmentFunction(f.get());
  if (depth) d->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
  auto a = d->colorAttachments()->object(0);
  a->setPixelFormat(format);
  a->setBlendingEnabled(true);
  a->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
  a->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
  a->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
  a->setDestinationAlphaBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
  NS::Error* error = nullptr;
  Owned<MTL::RenderPipelineState> result(
      device_->newRenderPipelineState(d, &error));
  d->release();
  if (!result && err)
    *err = error ? error->localizedDescription()->utf8String()
                 : "Metal pipeline creation failed";
  return result;
}
bool MetalRenderer::init(GLFWwindow* window, std::string* err) {
  auto pool = MakePool();
  window_ = window;
  device_.reset(MTL::CreateSystemDefaultDevice());
  if (!device_) {
    if (err) *err = "No native Metal device available";
    return false;
  }
  queue_.reset(device_->newCommandQueue());
  if (!queue_) {
    if (err) *err = "Metal command queue creation failed";
    return false;
  }
  NS::Error* error = nullptr;
  Owned<MTL::Library> lib(device_->newLibrary(
      NS::String::string(kMetalShaders, NS::UTF8StringEncoding), nullptr,
      &error));
  if (!lib) {
    if (err)
      *err = error ? error->localizedDescription()->utf8String()
                   : "Metal shader compilation failed";
    return false;
  }
  scenePipeline_ = pipeline(lib.get(), "sceneVertex", "sceneFragment",
                            MTL::PixelFormatRGBA8Unorm, true, err);
  guiPipeline_ = pipeline(lib.get(), "guiVertex", "guiFragment",
                          MTL::PixelFormatRGBA8Unorm, false, err);
  blitPipeline_ = pipeline(lib.get(), "guiVertex", "guiFragment",
                           MTL::PixelFormatBGRA8Unorm, false, err);
  if (!scenePipeline_ || !guiPipeline_ || !blitPipeline_) return false;
  auto d = MTL::DepthStencilDescriptor::alloc()->init();
  d->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
  d->setDepthWriteEnabled(true);
  depthState_.reset(device_->newDepthStencilState(d));
  d->release();
  white_ = texture(1, 1, MTL::PixelFormatRGBA8Unorm, false);
  if (!white_ || !depthState_) {
    if (err) *err = "Metal resource allocation failed";
    return false;
  }
  uint32_t white = 0xffffffffu;
  white_->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, &white, 4);
  if (window_)
    layer_.reset(static_cast<CA::MetalLayer*>(
        AttachMetalSurface(window_, device_.get())));
  caps_.backend_name = "Metal";
  caps_.gpu_name = device_->name()->utf8String();
  caps_.api_info = "Native Metal (metal-cpp)";
  caps_.usesZeroToOneDepth = true;
  caps_.transparencyMode = "unsorted";
  caps_.device_type = device_->hasUnifiedMemory() ? "integrated" : "discrete";
  return true;
}
bool MetalRenderer::initImGui(std::string* err) {
  if (window_) {
    platform_ = ImGui_ImplGlfw_InitForOther(window_, true);
    if (!platform_) {
      if (err) *err = "Metal GLFW ImGui initialization failed";
      return false;
    }
  }
  auto& io = ImGui::GetIO();
  io.BackendRendererName = "lusdview_metal_cpp";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset |
                     ImGuiBackendFlags_RendererHasTextures;
  ImGui::GetPlatformIO().Renderer_TextureMaxWidth = 16384;
  ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 16384;
  imgui_ = true;
  return true;
}
void MetalRenderer::setInstances(Mesh& m, const float* xforms,
                                 const float* colors, const float* opacities,
                                 uint32_t count) {
  std::vector<Instance> data(count);
  for (size_t i = 0; i < count; ++i) {
    std::memcpy(data[i].rows, xforms + 12 * i, 12 * sizeof(float));
    for (size_t c = 0; c < 3; ++c)
      data[i].tint[c] =
          colors ? colors[3 * i + c] : (m.instanced ? m.flatColor[c] : 1.f);
    data[i].tint[3] =
        opacities ? opacities[i] : (m.instanced ? m.flatOpacity : 1.f);
  }
  m.instances.reset(count ? device_->newBuffer(data.data(),
                                               data.size() * sizeof(Instance),
                                               MTL::ResourceStorageModeShared)
                          : nullptr);
  m.instanceCount = m.instances ? count : 0;
  if (count && !m.instances)
    reportSceneUploadError("Metal instance buffer allocation failed");
}
void MetalRenderer::appendMesh(const DrawMeshCPU& mesh) {
  auto pool = MakePool();
  Mesh m;
  m.doubleSided = mesh.doubleSided;
  m.geometricNormal = mesh.geometricNormal;
  m.instanced = !mesh.instanceXforms.empty();
  std::copy_n(mesh.flatColor, 3, m.flatColor);
  m.flatOpacity = mesh.flatOpacity;
  m.submeshes = mesh.submeshes;
  m.vertexCount = mesh.vertices.size();
  m.indexCount = mesh.indices.size();
  std::vector<Vertex> vertices(m.vertexCount);
  for (size_t i = 0; i < vertices.size(); ++i) {
    vertices[i].v = mesh.vertices[i];
    for (size_t c = 0; c < 3; ++c)
      vertices[i].color[c] = mesh.vertexColors.size() >= 3 * i + 3
                                 ? mesh.vertexColors[3 * i + c]
                                 : 1.f;
    vertices[i].color[3] =
        mesh.vertexAlpha.size() > i ? mesh.vertexAlpha[i] : 1.f;
  }
  if (!vertices.empty())
    m.vertices.reset(device_->newBuffer(vertices.data(),
                                        vertices.size() * sizeof(Vertex),
                                        MTL::ResourceStorageModeShared));
  if (!mesh.indices.empty()) {
    for (auto i : mesh.indices)
      if (i >= m.vertexCount) {
        reportSceneUploadError("Metal mesh index out of range");
        meshes_.push_back(std::move(m));
        return;
      }
    m.indices.reset(device_->newBuffer(mesh.indices.data(),
                                       mesh.indices.size() * sizeof(uint32_t),
                                       MTL::ResourceStorageModeShared));
  }
  if ((m.vertexCount && !m.vertices) || (m.indexCount && !m.indices))
    reportSceneUploadError("Metal mesh buffer allocation failed");
  if (m.instanced)
    setInstances(m, mesh.instanceXforms.data(),
                 mesh.instanceColors.size() >= mesh.instanceCount() * 3
                     ? mesh.instanceColors.data()
                     : nullptr,
                 mesh.instanceOpacities.size() >= mesh.instanceCount()
                     ? mesh.instanceOpacities.data()
                     : nullptr,
                 uint32_t(mesh.instanceCount()));
  else {
    float rows[12];
    for (size_t r = 0; r < 3; ++r)
      for (size_t c = 0; c < 4; ++c) rows[r * 4 + c] = mesh.world[c * 4 + r];
    setInstances(m, rows, nullptr, nullptr, 1);
  }
  meshes_.push_back(std::move(m));
}
void MetalRenderer::updateMeshVertices(
    int index, const std::vector<DrawVertex>& vertices) {
  if (index < 0 || size_t(index) >= meshes_.size()) return;
  auto& m = meshes_[size_t(index)];
  if (!m.vertices || vertices.size() != m.vertexCount) return;
  auto* dst = static_cast<Vertex*>(m.vertices->contents());
  for (size_t i = 0; i < vertices.size(); ++i) dst[i].v = vertices[i];
}
void MetalRenderer::updateMeshWorld(int index, const float world[16]) {
  if (index < 0 || size_t(index) >= meshes_.size() ||
      meshes_[size_t(index)].instanced)
    return;
  float rows[12];
  for (size_t r = 0; r < 3; ++r)
    for (size_t c = 0; c < 4; ++c) rows[r * 4 + c] = world[c * 4 + r];
  setInstances(meshes_[size_t(index)], rows, nullptr, nullptr, 1);
}
void MetalRenderer::updateInstanceVisibility(size_t index, const float* xforms,
                                             const float* colors,
                                             const float* opacities,
                                             uint32_t count) {
  if (index >= meshes_.size() || (count && !xforms)) return;
  setInstances(meshes_[index], xforms, colors, opacities, count);
}
void MetalRenderer::uploadTexture(int slot, const DrawTextureCPU& source) {
  auto pool = MakePool();
  if (slot < 0 || size_t(slot) >= textures_.size()) return;
  const auto& image = source.image;
  if (image.width <= 0 || image.height <= 0 || image.width > 16384 ||
      image.height > 16384 ||
      image.data.size() < size_t(image.width) * size_t(image.height) * 4)
    return;
  auto t = texture(image.width, image.height,
                   source.srgb ? MTL::PixelFormatRGBA8Unorm_sRGB
                               : MTL::PixelFormatRGBA8Unorm,
                   false);
  if (!t) {
    reportSceneUploadError("Metal texture allocation failed");
    return;
  }
  t->replaceRegion(
      MTL::Region::Make2D(0, 0, size_t(image.width), size_t(image.height)), 0,
      image.data.data(), size_t(image.width) * 4);
  textures_[size_t(slot)] = std::move(t);
}
bool MetalRenderer::updateTextureRegion(int slot, int x, int y, int w, int h,
                                        const uint8_t* rgba, size_t rowBytes) {
  if (slot < 0 || size_t(slot) >= textures_.size() || !rgba || x < 0 || y < 0 ||
      w <= 0 || h <= 0)
    return false;
  auto* t = textures_[size_t(slot)].get();
  if (!t || size_t(x) > t->width() || size_t(w) > t->width() - size_t(x) ||
      size_t(y) > t->height() || size_t(h) > t->height() - size_t(y))
    return false;
  if (!rowBytes) rowBytes = size_t(w) * 4;
  if (rowBytes < size_t(w) * 4) return false;
  t->replaceRegion(
      MTL::Region::Make2D(size_t(x), size_t(y), size_t(w), size_t(h)), 0, rgba,
      rowBytes);
  return true;
}
void MetalRenderer::resizeViewport(int w, int h) {
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
      (w == width_ && h == height_))
    return;
  auto pool = MakePool();
  auto color = texture(w, h, MTL::PixelFormatRGBA8Unorm, true);
  auto desc = MTL::TextureDescriptor::texture2DDescriptor(
      MTL::PixelFormatDepth32Float, size_t(w), size_t(h), false);
  desc->setStorageMode(MTL::StorageModePrivate);
  desc->setUsage(MTL::TextureUsageRenderTarget);
  Owned<MTL::Texture> depth(device_->newTexture(desc));
  if (!color || !depth) {
    reportSceneUploadError("Metal viewport allocation failed");
    return;
  }
  if (color_) retiredTargets_.push_back(std::move(color_));
  color_ = std::move(color);
  depth_ = std::move(depth);
  width_ = w;
  height_ = h;
}
bool MetalRenderer::complete(MTL::CommandBuffer* cb) {
  cb->commit();
  cb->waitUntilCompleted();
  if (cb->status() == MTL::CommandBufferStatusError) {
    auto* e = cb->error();
    reportSceneUploadError(e ? e->localizedDescription()->utf8String()
                             : "Metal command failed");
    return false;
  }
  return true;
}
void MetalRenderer::renderFrame(const RenderFrameParams& p) {
  if (!color_ || !p.view || !p.proj) return;
  auto pool = MakePool();
  auto* cb = queue_->commandBuffer();
  auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
  auto* a = pass->colorAttachments()->object(0);
  a->setTexture(color_.get());
  a->setLoadAction(MTL::LoadActionClear);
  a->setStoreAction(MTL::StoreActionStore);
  a->setClearColor(MTL::ClearColor(p.clearColor[0], p.clearColor[1],
                                   p.clearColor[2], p.clearColor[3]));
  pass->depthAttachment()->setTexture(depth_.get());
  pass->depthAttachment()->setLoadAction(MTL::LoadActionClear);
  pass->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
  pass->depthAttachment()->setClearDepth(1.0);
  auto* enc = cb->renderCommandEncoder(pass);
  enc->setRenderPipelineState(scenePipeline_.get());
  enc->setDepthStencilState(depthState_.get());
  enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
  enc->setViewport(MTL::Viewport{0, 0, double(width_), double(height_), 0, 1});
  Uniforms u{};
  std::copy_n(p.view, 16, u.view);
  std::copy_n(p.proj, 16, u.proj);
  std::copy_n(p.cameraPos, 3, u.camera);
  u.camera[3] = p.depthScale;
  std::copy_n(p.lightDir, 3, u.light);
  std::copy_n(p.lightColor, 3, u.lightColor);
  std::copy_n(p.clippingPlanes, 32, u.planes);
  u.planeCount = std::clamp(p.clippingPlaneCount, 0, 8);
  u.mode = int(p.mode);
  for (size_t i = 0; i < meshes_.size(); ++i) {
    auto& m = meshes_[i];
    if (!m.vertices || !m.indices || !m.instances || !m.indexCount ||
        !m.instanceCount ||
        (p.meshVisible && i < size_t(std::max(0, p.meshVisibleCount)) &&
         !p.meshVisible[i]))
      continue;
    enc->setVertexBuffer(m.vertices.get(), 0, 0);
    enc->setVertexBuffer(m.instances.get(), 0, 2);
    enc->setCullMode(m.doubleSided ? MTL::CullModeNone : MTL::CullModeBack);
    enc->setTriangleFillMode(p.wireMode || p.mode == RenderMode::Wireframe
                                 ? MTL::TriangleFillModeLines
                                 : MTL::TriangleFillModeFill);
    u.geometricNormal = m.geometricNormal;
    auto draw = [&](const DrawSubmesh& sub) {
      if (sub.indexOffset > m.indexCount ||
          sub.indexCount > m.indexCount - sub.indexOffset)
        return;
      DrawMaterialCPU fallback;
      const auto& mat =
          sub.materialId >= 0 && size_t(sub.materialId) < materials_.size()
              ? materials_[size_t(sub.materialId)]
              : fallback;
      std::copy_n(mat.baseColor, 3, u.base);
      u.base[3] = mat.alpha;
      std::copy_n(mat.emissive, 3, u.emissive);
      u.options[0] = mat.metallic;
      u.options[1] = mat.roughness;
      u.options[2] = p.exposure;
      u.options[3] =
          mat.alphaMode == int(AlphaMode::Mask) ? mat.alphaCutoff : 0;
      enc->setVertexBytes(&u, sizeof(u), 1);
      enc->setFragmentBytes(&u, sizeof(u), 1);
      auto* tex =
          mat.baseColorTex >= 0 && size_t(mat.baseColorTex) < textures_.size()
              ? textures_[size_t(mat.baseColorTex)].get()
              : nullptr;
      enc->setFragmentTexture(tex ? tex : white_.get(), 0);
      enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, sub.indexCount,
                                 MTL::IndexTypeUInt32, m.indices.get(),
                                 size_t(sub.indexOffset) * 4, m.instanceCount);
    };
    if (m.submeshes.empty())
      draw(DrawSubmesh{0, uint32_t(m.indexCount), -1, -1});
    else
      for (const auto& sub : m.submeshes) draw(sub);
  }
  // Debug grid, selection and skeleton overlays share the scene vertex ABI.
  auto lines = [&](const HelperVertex* src, int count, bool xray) {
    if (!src || count <= 0) return;
    std::vector<Vertex> v(static_cast<size_t>(count));
    for (size_t i = 0; i < v.size(); ++i) {
      v[i].v = {src[i].pos[0], src[i].pos[1], src[i].pos[2], 0, 1, 0, 0, 0};
      std::copy_n(src[i].col, 3, v[i].color);
      v[i].color[3] = 1;
    }
    Owned<MTL::Buffer> b(device_->newBuffer(v.data(), v.size() * sizeof(Vertex),
                                            MTL::ResourceStorageModeShared));
    if (!b) return;
    Instance identity{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {1, 1, 1, 1}};
    u.mode = -1;
    u.geometricNormal = 0;
    std::fill_n(u.base, 4, 1.f);
    u.options[3] = 0;
    enc->setDepthStencilState(xray ? nullptr : depthState_.get());
    enc->setVertexBuffer(b.get(), 0, 0);
    enc->setVertexBytes(&identity, sizeof(identity), 2);
    enc->setVertexBytes(&u, sizeof(u), 1);
    enc->setFragmentBytes(&u, sizeof(u), 1);
    enc->setFragmentTexture(white_.get(), 0);
    enc->setCullMode(MTL::CullModeNone);
    enc->drawPrimitives(MTL::PrimitiveTypeLine, NS::UInteger(0), v.size());
  };
  lines(p.helperLines, p.helperLineVertexCount, false);
  lines(p.highlightLines, p.highlightLineVertexCount, p.highlightXray);
  lines(p.overlayLines, p.overlayLineVertexCount, true);
  enc->endEncoding();
  complete(cb);
}
bool MetalRenderer::uploadViewportImage(const uint8_t* rgba, int w, int h) {
  if (!rgba || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
  resizeViewport(w, h);
  if (!color_ || width_ != w || height_ != h) return false;
  color_->replaceRegion(MTL::Region::Make2D(0, 0, size_t(w), size_t(h)), 0,
                        rgba, size_t(w) * 4);
  return true;
}
bool MetalRenderer::readback(MTL::Texture* tex, std::vector<uint8_t>* rgba,
                             int* w, int* h) {
  if (!tex || !rgba || !w || !h) return false;
  *w = int(tex->width());
  *h = int(tex->height());
  rgba->resize(size_t(*w) * size_t(*h) * 4);
  tex->getBytes(rgba->data(), size_t(*w) * 4,
                MTL::Region::Make2D(0, 0, tex->width(), tex->height()), 0);
  return true;
}
void MetalRenderer::updateGuiTextures(ImDrawData* data) {
  if (!data->Textures) return;
  for (auto* t : *data->Textures) {
    if (t->Status == ImTextureStatus_WantCreate) {
      auto gpu =
          texture(t->Width, t->Height, MTL::PixelFormatRGBA8Unorm, false);
      if (!gpu) continue;
      t->SetTexID(ImTextureID(uintptr_t(gpu.release())));
    }
    if (t->Status == ImTextureStatus_WantCreate ||
        t->Status == ImTextureStatus_WantUpdates) {
      auto* gpu = reinterpret_cast<MTL::Texture*>(uintptr_t(t->TexID));
      gpu->replaceRegion(
          MTL::Region::Make2D(0, 0, size_t(t->Width), size_t(t->Height)), 0,
          t->GetPixels(), size_t(t->Width) * 4);
      t->SetStatus(ImTextureStatus_OK);
    } else if (t->Status == ImTextureStatus_WantDestroy &&
               t->UnusedFrames > 0) {
      reinterpret_cast<MTL::Texture*>(uintptr_t(t->TexID))->release();
      t->SetTexID(ImTextureID_Invalid);
      t->SetStatus(ImTextureStatus_Destroyed);
    }
  }
}
void MetalRenderer::drawGui(MTL::CommandBuffer* cb, MTL::Texture* target,
                            ImDrawData* data) {
  updateGuiTextures(data);
  auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
  auto* a = pass->colorAttachments()->object(0);
  a->setTexture(target);
  a->setLoadAction(MTL::LoadActionClear);
  a->setStoreAction(MTL::StoreActionStore);
  a->setClearColor(MTL::ClearColor(.08, .08, .08, 1));
  auto* enc = cb->renderCommandEncoder(pass);
  auto setup = [&]() {
    enc->setRenderPipelineState(guiPipeline_.get());
    enc->setCullMode(MTL::CullModeNone);
    enc->setViewport(MTL::Viewport{0, 0, double(target->width()),
                                   double(target->height()), 0, 1});
    float transform[4] = {2 / data->DisplaySize.x, -2 / data->DisplaySize.y,
                          -1 - data->DisplayPos.x * 2 / data->DisplaySize.x,
                          1 + data->DisplayPos.y * 2 / data->DisplaySize.y};
    enc->setVertexBytes(transform, sizeof(transform), 1);
  };
  setup();
  for (auto* list : data->CmdLists) {
    if (list->VtxBuffer.empty() || list->IdxBuffer.empty()) continue;
    Owned<MTL::Buffer> vb(device_->newBuffer(
        list->VtxBuffer.Data, size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert),
        MTL::ResourceStorageModeShared));
    Owned<MTL::Buffer> ib(device_->newBuffer(
        list->IdxBuffer.Data, size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx),
        MTL::ResourceStorageModeShared));
    if (!vb || !ib) continue;
    for (const auto& cmd : list->CmdBuffer) {
      if (cmd.UserCallback) {
        if (cmd.UserCallback == ImDrawCallback_ResetRenderState)
          setup();
        else
          cmd.UserCallback(list, &cmd);
        continue;
      }
      const auto& r = cmd.ClipRect;
      float x =
          std::clamp((r.x - data->DisplayPos.x) * data->FramebufferScale.x, 0.f,
                     float(target->width()));
      float y =
          std::clamp((r.y - data->DisplayPos.y) * data->FramebufferScale.y, 0.f,
                     float(target->height()));
      float z =
          std::clamp((r.z - data->DisplayPos.x) * data->FramebufferScale.x, 0.f,
                     float(target->width()));
      float w =
          std::clamp((r.w - data->DisplayPos.y) * data->FramebufferScale.y, 0.f,
                     float(target->height()));
      if (z <= x || w <= y) continue;
      enc->setScissorRect(MTL::ScissorRect{
          size_t(x), size_t(y), size_t(z) - size_t(x), size_t(w) - size_t(y)});
      enc->setVertexBuffer(vb.get(), size_t(cmd.VtxOffset) * sizeof(ImDrawVert),
                           0);
      auto* tex = reinterpret_cast<MTL::Texture*>(uintptr_t(cmd.GetTexID()));
      enc->setFragmentTexture(tex ? tex : white_.get(), 0);
      enc->drawIndexedPrimitives(
          MTL::PrimitiveTypeTriangle, cmd.ElemCount,
          sizeof(ImDrawIdx) == 2 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32,
          ib.get(), size_t(cmd.IdxOffset) * sizeof(ImDrawIdx));
    }
  }
  enc->endEncoding();
}
void MetalRenderer::present() {
  auto pool = MakePool();
  auto* data = ImGui::GetDrawData();
  if (!data || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0) return;
  int w = winW_, h = winH_;
  if (window_) glfwGetFramebufferSize(window_, &w, &h);
  if (w <= 0 || h <= 0) return;
  if (!composite_ || int(composite_->width()) != w ||
      int(composite_->height()) != h)
    composite_ = texture(w, h, MTL::PixelFormatRGBA8Unorm, true);
  if (!composite_) return;
  auto* cb = queue_->commandBuffer();
  drawGui(cb, composite_.get(), data);
  if (layer_) {
    layer_->setDrawableSize(CGSizeMake(w, h));
    if (auto* drawable = layer_->nextDrawable()) {
      auto* pass = MTL::RenderPassDescriptor::renderPassDescriptor();
      auto* a = pass->colorAttachments()->object(0);
      a->setTexture(drawable->texture());
      a->setLoadAction(MTL::LoadActionDontCare);
      a->setStoreAction(MTL::StoreActionStore);
      auto* enc = cb->renderCommandEncoder(pass);
      enc->setRenderPipelineState(blitPipeline_.get());
      struct GuiVertex {
        float pos[2], uv[2];
        uint32_t color;
      };
      GuiVertex quad[] = {{{-1, -1}, {0, 1}, 0xffffffff},
                          {{1, -1}, {1, 1}, 0xffffffff},
                          {{-1, 1}, {0, 0}, 0xffffffff},
                          {{1, 1}, {1, 0}, 0xffffffff}};
      float transform[4] = {1, 1, 0, 0};
      enc->setVertexBytes(quad, sizeof(quad), 0);
      enc->setVertexBytes(transform, sizeof(transform), 1);
      enc->setFragmentTexture(composite_.get(), 0);
      enc->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, NS::UInteger(0),
                          NS::UInteger(4));
      enc->endEncoding();
      cb->presentDrawable(drawable);
    }
  }
  complete(cb);
  retiredTargets_.clear();
}
void MetalRenderer::shutdown() {
  auto pool = MakePool();
  if (imgui_ && ImGui::GetCurrentContext()) {
    for (auto* t : ImGui::GetPlatformIO().Textures)
      if (t->TexID != ImTextureID_Invalid) {
        reinterpret_cast<MTL::Texture*>(uintptr_t(t->TexID))->release();
        t->SetTexID(ImTextureID_Invalid);
        t->SetStatus(ImTextureStatus_Destroyed);
      }
    auto& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset |
                         ImGuiBackendFlags_RendererHasTextures);
  }
  imgui_ = false;
  if (platform_) ImGui_ImplGlfw_Shutdown();
  platform_ = false;
  if (layer_ && window_) DetachMetalSurface(window_, layer_.get());
  layer_.reset();
  window_ = nullptr;
  meshes_.clear();
  textures_.clear();
  materials_.clear();
  retiredTargets_.clear();
  composite_.reset();
  color_.reset();
  depth_.reset();
  white_.reset();
  scenePipeline_.reset();
  guiPipeline_.reset();
  blitPipeline_.reset();
  depthState_.reset();
  queue_.reset();
  device_.reset();
  width_ = height_ = 0;
}
}  // namespace
std::unique_ptr<Renderer> CreateMetalRenderer() {
  return std::make_unique<MetalRenderer>();
}
}  // namespace lusdview
