// SPDX-License-Identifier: Apache-2.0
// Real GPU tests: offscreen raster, depth, transforms, instancing, streamed
// textures, clipping, resize and ImGui's dynamic font atlas. No display needed.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imgui.h"
#include "renderer.hh"

namespace {
void Check(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
lusdview::DrawMeshCPU Quad(float z, int material) {
  lusdview::DrawMeshCPU m;
  m.vertices = {{-.8f, -.8f, z, 0, 0, 1, 0, 1},
                {.8f, -.8f, z, 0, 0, 1, 1, 1},
                {.8f, .8f, z, 0, 0, 1, 1, 0},
                {-.8f, .8f, z, 0, 0, 1, 0, 0}};
  m.indices = {0, 1, 2, 0, 2, 3};
  m.submeshes = {{0, 6, material, -1}};
  m.world[0] = m.world[5] = m.world[10] = m.world[15] = 1;
  m.doubleSided = false;
  return m;
}
}  // namespace
int main() {
  using namespace lusdview;
  auto r = CreateMetalRenderer();
  std::string error;
  if (!r->init(nullptr, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return error == "No native Metal device available" ? 77 : 1;
  }
  std::printf("GPU: %s\n", r->caps().gpu_name.c_str());
  Check(r->caps().usesZeroToOneDepth && !r->caps().flipViewportV,
        "Metal coordinate conventions");
  DrawMaterialCPU red, green;
  red.baseColor[0] = 1;
  red.baseColor[1] = red.baseColor[2] = 0;
  green.baseColor[1] = 1;
  green.baseColor[0] = green.baseColor[2] = 0;
  r->beginScene({red, green}, 1);
  r->appendMesh(Quad(.2f, 0));
  r->appendMesh(Quad(.7f, 1));
  r->resizeViewport(64, 48);
  float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  RenderFrameParams p;
  p.view = identity;
  p.proj = identity;
  p.mode = RenderMode::Albedo;
  p.clearColor[0] = p.clearColor[1] = p.clearColor[2] = 0;
  std::vector<uint8_t> pixels;
  int w = 0, h = 0;
  auto render = [&]() {
    r->renderFrame(p);
    Check(r->sceneUploadError().empty(), r->sceneUploadError().c_str());
    Check(r->captureViewport(&pixels, &w, &h), "readback");
    return pixels.data() + (size_t(h / 2) * size_t(w) + size_t(w / 2)) * 4;
  };
  auto* c = render();
  Check(w == 64 && h == 48 && c[0] > 245 && c[1] < 5,
        "near red surface occludes far green surface");
  float translated[16];
  std::memcpy(translated, identity, sizeof(identity));
  translated[12] = 3;
  r->updateMeshWorld(0, translated);
  c = render();
  Check(c[1] > 245 && c[0] < 5, "world transform updates");
  r->updateMeshWorld(0, identity);
  uint8_t mask[2] = {0, 1};
  p.meshVisible = mask;
  p.meshVisibleCount = 2;
  c = render();
  Check(c[1] > 245 && c[0] < 5, "mesh visibility");
  p.meshVisible = nullptr;
  red.baseColor[0] = red.baseColor[1] = red.baseColor[2] = 1;
  red.baseColorTex = 0;
  Check(r->updateMaterialConstants(0, red, &error), "material update");
  DrawTextureCPU tex;
  tex.image.width = tex.image.height = 1;
  tex.image.channels = 4;
  tex.image.data = {0, 0, 255, 255};
  r->uploadTexture(0, tex);
  c = render();
  Check(c[2] > 245 && c[0] < 5, "native sampled texture");
  Check(r->textureResidentBytes(0) == 4, "texture residency accounting");
  uint8_t yellow[4] = {255, 255, 0, 255};
  Check(!r->updateTextureRegion(0, 1, 0, 1, 1, yellow, 4),
        "out of bounds texture update rejected");
  Check(!r->updateTextureRegion(0, 0, 0, 1, 1, yellow, 3),
        "short row rejected");
  Check(r->updateTextureRegion(0, 0, 0, 1, 1, yellow, 4),
        "texture region update");
  c = render();
  Check(c[0] > 245 && c[1] > 245 && c[2] < 5, "updated texture sampled");
  p.clippingPlaneCount = 1;
  p.clippingPlanes[0] = 1;
  p.clippingPlanes[3] = -2;
  c = render();
  Check(c[0] < 5 && c[1] < 5 && c[2] < 5, "world clipping plane");
  p.clippingPlaneCount = 0;
  r->evictTexture(0);
  Check(r->textureResidentBytes(0) == 0, "texture eviction");
  c = render();
  Check(c[0] > 245 && c[1] > 245 && c[2] > 245,
        "evicted texture uses white fallback");
  // Single GPU draw with two independently transformed instances and tints.
  auto instanced = Quad(.2f, 0);
  instanced.instanceXforms = {1, 0, 0, -.6f, 0, 1, 0, 0, 0, 0, 1, 0,
                              1, 0, 0, .6f,  0, 1, 0, 0, 0, 0, 1, 0};
  instanced.instanceColors = {1, 0, 0, 0, 1, 0};
  r->beginScene({red}, 0);
  r->appendMesh(instanced);
  render();
  auto at = [&](int x, int y) {
    return pixels.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
  };
  Check(at(8, 24)[0] > 245 && at(56, 24)[1] > 245,
        "instanced transforms and colors");
  r->updateInstanceVisibility(0, nullptr, nullptr, nullptr, 0);
  c = render();
  Check(c[0] < 5 && c[1] < 5 && c[2] < 5,
        "empty instance visibility removes all draws");
  r->resizeViewport(31, 29);
  render();
  Check(w == 31 && h == 29, "viewport resize");
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  Check(r->initImGui(&error), "headless ImGui init");
  r->setHeadlessSize(80, 60);
  for (int i = 0; i < 3; ++i) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(80, 60);
    io.DeltaTime = 1.f / 60;
    r->newFrame();
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled(
        ImVec2(10, 10), ImVec2(40, 40), IM_COL32(0, 0, 255, 255));
    ImGui::GetForegroundDrawList()->AddText(ImVec2(2, 42), IM_COL32_WHITE,
                                            "Metal");
    ImGui::Render();
    r->present();
    Check(r->captureWindow(&pixels, &w, &h), "ImGui composite readback");
    Check(w == 80 && h == 60 && at(20, 20)[2] > 245 && at(20, 20)[0] < 5,
          "ImGui composition and dynamic atlas");
  }
  r->shutdown();
  r->shutdown();
  ImGui::DestroyContext();
  std::puts("PASS: native Metal renderer");
}
