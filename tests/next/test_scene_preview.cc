// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/schema/usd-vol.hh"
#include "next/stage/stage.hh"
#include "tydra/next/scene-preview.hh"
#include "tydra/vol/volume-field.hh"

namespace fs = std::filesystem;
namespace usd = lightusd::next;
namespace td = lightusd::tydra::next;
namespace {
void Require(bool ok, const std::string& message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
  }
}
void Write(const fs::path& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  Require(bool(f), "fixture write");
}
std::string Read(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
std::string Quad(const std::string& name) {
  return "def Mesh \"" + name +
         "\" {\n point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]\n "
         "int[] faceVertexCounts = [4]\n int[] faceVertexIndices = [0,1,2,3]\n "
         "float3[] extent = [(-1,-1,0),(1,1,0)]\n float2[] primvars:st = "
         "[(0,0),(1,0),(1,1),(0,1)] (interpolation=\"vertex\")\n uniform token "
         "subdivisionScheme = \"none\"\n}\n";
}
usd::Stage Load(const std::string& path,
                const td::ScenePreviewLodSelections& variants = {}) {
  usd::AssetResolver resolver;
  usd::Stage stage;
  std::string warning, error;
  usd::pcp::CompositionOptions options;
  options.num_threads = 1;
  options.variant_overrides_by_path = variants;
  Require(usd::pcp::ComposeStageFromFile(path, resolver, &stage, options,
                                         &warning, &error),
          "proxy reopen: " + error);
  return stage;
}
size_t ParticleCount(const std::string& file, double* minimum = nullptr,
                     double* maximum = nullptr) {
  auto stage = Load(file);
  size_t count = 0;
  if (minimum) *minimum = 1e300;
  if (maximum) *maximum = -1e300;
  stage.Traverse([&](const usd::UsdPrim& prim) {
    if (usd::IsParticleField(prim)) {
      const auto* p = prim.GetPropertyValue("positions");
      Require(p != nullptr, "particle positions");
      const auto a = p->view().as_float_array();
      count += a.size() / 3;
      for (size_t i = 0; i < a.size(); i += 3) {
        if (minimum) *minimum = std::min(*minimum, double(a[i]));
        if (maximum) *maximum = std::max(*maximum, double(a[i]));
      }
    }
    return true;
  });
  return count;
}
}  // namespace
int main() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path scratch =
      fs::temp_directory_path() /
      ("lightusd-scene-preview-test-" + std::to_string(nonce));
  std::error_code ec;
  Require(fs::create_directory(scratch, ec), "scratch directory");
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(p, e);
    }
  } cleanup{scratch};
  usd::AssetResolver resolver;
  td::ScenePreviewOptions options;
  options.cache_directory = (scratch / "cache").string();
  options.gpu_budget_bytes = uint64_t(128) << 20;
  options.composition.num_threads = 1;
  const auto input = scratch / "plain.usda";
  Write(input, "#usda 1.0\n" + Quad("Surface"));
  for (auto kind :
       {td::ScenePreviewKind::Gaussian, td::ScenePreviewKind::Points,
        td::ScenePreviewKind::Volume}) {
    options.kind = kind;
    const auto first =
        td::ConvertScenePreview(input.string(), resolver, options);
    Require(first.success, first.error);
    Require(!first.cache_hit, "first conversion is not cached");
    Require(first.samples > 0, "nonempty proxy");
    Require(first.estimated_gpu_bytes <= options.gpu_budget_bytes,
            "planned renderer budget");
    const auto second =
        td::ConvertScenePreview(input.string(), resolver, options);
    Require(second.success && second.cache_hit, "warm cache hit");
    if (kind != td::ScenePreviewKind::Volume)
      Require(ParticleCount(first.usd_path) == first.samples,
              "particle count roundtrip");
    else {
      auto volume = Load(first.usd_path);
      bool found = false;
      volume.Traverse([&](const usd::UsdPrim& p) {
        found = found || p.GetTypeName() == "Volume";
        return true;
      });
      Require(found, "UsdVol volume authored");
      auto render = Load(first.render_usd_path);
      found = false;
      render.Traverse([&](const usd::UsdPrim& p) {
        found = found || p.GetTypeName() == "Mesh";
        return true;
      });
      Require(found, "bounded surface carrier");
    }
    const auto pyramid = td::BuildScenePreviewLods(first, options);
    Require(pyramid.success, pyramid.error);
    auto pyramid_stage = Load(pyramid.root_path);
    Require(
        bool(pyramid_stage.GetPrimAtPath(usd::Path("/ScenePreview/Tile_0"))),
        "portable LOD variant geometry");
    const fs::path exported =
        scratch / (std::string("export-") + td::ScenePreviewKindName(kind));
    std::string error;
    Require(td::ExportScenePreview(first, exported.string(), &error), error);
    const auto portable_lods =
        Load((exported / fs::path(pyramid.root_path).parent_path().filename() /
              "root.usdc")
                 .string());
    Require(
        bool(portable_lods.GetPrimAtPath(usd::Path("/ScenePreview/Tile_0"))),
        "exported LOD references remain relative");
    Require(!td::ExportScenePreview(first, exported.string(), &error),
            "export must not overwrite");
  }
  Write(scratch / "mixed-scales.usda",
        "#usda 1.0\n" + Quad("Room") +
            "def Sphere \"Environment\" { double radius=1000000\n double3 "
            "xformOp:translate=(3000000,0,0)\n uniform token[] "
            "xformOpOrder=[\"xformOp:translate\"]\n }\n");
  auto multiscale_options = options;
  multiscale_options.kind = td::ScenePreviewKind::Volume;
  multiscale_options.gpu_budget_bytes = uint64_t(2) << 30;
  const auto multiscale = td::ConvertScenePreview(
      (scratch / "mixed-scales.usda").string(), resolver, multiscale_options);
  Require(multiscale.success, multiscale.error);
  Require(multiscale.voxel_size < 1,
          "distant environment does not erase fine volume detail");
  auto multiscale_stage = Load(multiscale.render_usd_path);
  bool room_retained = false;
  multiscale_stage.Traverse([&](const usd::UsdPrim& prim) {
    if (prim.GetTypeName() == "Mesh") {
      const auto* p = prim.GetPropertyValue("points");
      if (p) {
        const auto v = p->view().as_float_array();
        for (size_t i = 0; i + 2 < v.size(); i += 3)
          if (std::abs(v[i]) < 5 && std::abs(v[i + 1]) < 5 &&
              std::abs(v[i + 2]) < 5)
            room_retained = true;
      }
    }
    return true;
  });
  Require(room_retained, "small object remains in mixed-scale volume carrier");
  options.kind = td::ScenePreviewKind::Gaussian;
  const auto cached =
      td::ConvertScenePreview(input.string(), resolver, options);
  auto refined_options = options;
  refined_options.resolution = 64;
  const auto refined =
      td::ConvertScenePreview(input.string(), resolver, refined_options);
  Require(refined.success && refined.samples > cached.samples,
          "explicit resolution refines sampling");
  const auto lods = td::BuildScenePreviewLods(refined, refined_options);
  Require(lods.success, lods.error);
  td::ScenePreviewLodCamera near_camera, far_camera;
  near_camera.eye = {0, 0, .01};
  far_camera.eye = {0, 0, 1000000};
  const auto near_lods = td::SelectScenePreviewLods(lods, near_camera);
  const auto far_lods = td::SelectScenePreviewLods(lods, far_camera);
  Require(!near_lods.empty() && !far_lods.empty() && near_lods != far_lods,
          "camera selects finer nearby LOD");
  auto count_particles = [](usd::Stage& stage) {
    size_t count = 0;
    stage.Traverse([&](const usd::UsdPrim& prim) {
      if (usd::IsParticleField(prim))
        count += prim.GetPropertyValue("positions")->array_size();
      return true;
    });
    return count;
  };
  auto near_stage = Load(lods.root_path, near_lods),
       far_stage = Load(lods.root_path, far_lods);
  Require(count_particles(near_stage) > count_particles(far_stage),
          "USD LOD variants refine particle detail");
  Require(td::BuildScenePreviewLods(refined, refined_options).success,
          "warm LOD cache");
  td::ScenePreviewLods constrained;
  constrained.success = true;
  constrained.cell_size = 1;
  constrained.gpu_budget_bytes = uint64_t(128) << 20;
  td::ScenePreviewLodTile constrained_tile;
  constrained_tile.path = "/Tile";
  constrained_tile.minimum = {-1, -1, -1};
  constrained_tile.maximum = {1, 1, 1};
  constrained_tile.bytes = {uint64_t(80) << 20, uint64_t(80) << 20,
                            uint64_t(20) << 20, uint64_t(10) << 20};
  constrained.tiles.push_back(constrained_tile);
  Require(td::SelectScenePreviewLods(constrained, near_camera)
                  .at("/Tile")
                  .at("previewLod") == "lod2",
          "budget reduction skips equal-cost levels");
  constrained.gpu_budget_bytes = uint64_t(512) << 20;
  auto boundary_camera = near_camera;
  boundary_camera.eye = {0, 0, 19};
  boundary_camera.focal_pixels = 10;
  td::ScenePreviewLodSelections previous;
  previous["/Tile"]["previewLod"] = "lod0";
  Require(
      td::SelectScenePreviewLods(constrained, boundary_camera)
                  .at("/Tile")
                  .at("previewLod") == "lod1" &&
          td::SelectScenePreviewLods(constrained, boundary_camera, 2, previous)
                  .at("/Tile")
                  .at("previewLod") == "lod0",
      "LOD hysteresis prevents threshold chatter");
  Write(fs::path(lods.root_path).parent_path() / "manifest.json",
        R"({"version":2,"tiles":"invalid"})");
  Require(td::BuildScenePreviewLods(refined, refined_options).success,
          "malformed LOD cache regenerates");
  const fs::path manifest =
      fs::path(cached.usd_path).parent_path() / "manifest.json";
  Write(manifest, R"({"version":2,"dependencies":[],"statistics":"invalid"})");
  const auto repaired =
      td::ConvertScenePreview(input.string(), resolver, options);
  Require(repaired.success && !repaired.cache_hit,
          "malformed cache manifest regenerates safely");
  // Every generated sidecar is part of cache validation, not only root.usdc.
  auto sidecar = fs::path(repaired.usd_path).parent_path() / "particles_0.usdc";
  Write(sidecar, "broken");
  const auto restored =
      td::ConvertScenePreview(input.string(), resolver, options);
  Require(restored.success && !restored.cache_hit,
          "truncated cache sidecar regenerates");
  auto policy_options = options;
  policy_options.composition.payload_policy =
      [](const usd::Path&, const std::string&) { return true; };
  Require(!td::ConvertScenePreview(input.string(), resolver, policy_options)
               .success,
          "uncacheable policy callback rejected");
  Write(scratch / "asset.usda",
        "#usda 1.0\ndef Xform \"Asset\" {\n" + Quad("Surface") + "}\n");
  Write(scratch / "instances.usda", R"USD(#usda 1.0
def Xform "World" {
 def Xform "A" (references=@asset.usda@</Asset> instanceable=true) {}
 def Xform "B" (references=@asset.usda@</Asset> instanceable=true) {
  double3 xformOp:translate=(10,0,0)
  uniform token[] xformOpOrder=["xformOp:translate"]
 }
}
)USD");
  options.batch_roots = 1;
  const auto instanced = td::ConvertScenePreview(
      (scratch / "instances.usda").string(), resolver, options);
  Require(instanced.success, instanced.error);
  double lo = 0, hi = 0;
  Require(ParticleCount(instanced.usd_path, &lo, &hi) > 0 && lo < 0 && hi > 9,
          "cross-batch native instance placement");
  Write(scratch / "points.usda",
        "#usda 1.0\ndef PointInstancer \"Instances\" {\n rel "
        "prototypes=[</Prototype>]\n int[] protoIndices=[0,0]\n point3f[] "
        "positions=[(0,0,0),(10,0,0)]\n}\ndef Xform \"Prototype\" {\n" +
            Quad("Surface") + "}\n");
  const auto pi = td::ConvertScenePreview((scratch / "points.usda").string(),
                                          resolver, options);
  Require(pi.success, pi.error);
  Require(pi.source_instances == 2, "PointInstancer count");
  Require(ParticleCount(pi.usd_path, &lo, &hi) > 0 && lo < 0 && hi > 9,
          "forward PointInstancer prototype placement");

  Write(scratch / "native-prototype.usda",
        "#usda 1.0\ndef Xform \"Asset\" {\n def Xform \"Prototype\" {\n "
        "double3 xformOp:translate=(2,0,0)\n uniform token[] "
        "xformOpOrder=[\"xformOp:translate\"]\n" +
            Quad("Surface") + "}\n}\n");
  Write(scratch / "native-target.usda", R"USD(#usda 1.0
def Xform "A" (references=@native-prototype.usda@</Asset> instanceable=true) {}
def Xform "B" (references=@native-prototype.usda@</Asset> instanceable=true) {
 double3 xformOp:translate=(20,0,0)
 uniform token[] xformOpOrder=["xformOp:translate"]
}
def PointInstancer "Instances" {
 rel prototypes=[</B/Prototype>]
 int[] protoIndices=[0]
 point3f[] positions=[(40,0,0)]
}
)USD");
  const auto aliased = td::ConvertScenePreview(
      (scratch / "native-target.usda").string(), resolver, options);
  Require(aliased.success, aliased.error);
  Require(ParticleCount(aliased.usd_path, &lo, &hi) > 0 && hi > 42.5 && hi < 44,
          "native descendant prototype resolves with its local transform");

  // Bake a texture through a real PreviewSurface connection, then change the
  // texture without touching USD. A warm cache must invalidate that dependency.
  std::string colored = "#usda 1.0\n" + Quad("Surface");
  colored += "over \"Surface\" { rel material:binding=</Material> }\n";
  colored += R"USD(def Material "Material" {
 token outputs:surface.connect=</Material/Shader.outputs:surface>
 def Shader "Shader" {
  uniform token info:id="UsdPreviewSurface"
  color3f inputs:diffuseColor.connect=</Material/Texture.outputs:rgb>
  token outputs:surface
 }
 def Shader "Texture" {
  uniform token info:id="UsdUVTexture"
  asset inputs:file=@color.ppm@
  token inputs:sourceColorSpace="raw"
  float3 outputs:rgb
 }
}
)USD";
  Write(scratch / "color.usda", colored);
  Write(scratch / "color.ppm",
        std::string("P6\n1 1\n255\n") + char(255) + char(0) + char(0));
  const auto red = td::ConvertScenePreview((scratch / "color.usda").string(),
                                           resolver, options);
  Require(red.success, red.error);
  auto redstage = Load(red.usd_path);
  bool baked = false;
  redstage.Traverse([&](const usd::UsdPrim& p) {
    if (usd::IsParticleField(p)) {
      const auto* c =
          p.GetPropertyValue("radiance:sphericalHarmonicsCoefficients");
      const auto a = c->view().as_float_array();
      Require(a.size() >= 3, "color coefficients");
      Require(.5 + .2820947918 * a[0] > .9 && .5 + .2820947918 * a[1] < .1,
              "baked red texture");
      baked = true;
    }
    return true;
  });
  Require(baked, "textured samples");
  Write(scratch / "color.ppm",
        std::string("P6\n1 1\n255\n") + char(0) + char(0) + char(255) + "\n");
  const auto blue = td::ConvertScenePreview((scratch / "color.usda").string(),
                                            resolver, options);
  Require(blue.success && !blue.cache_hit,
          "texture edit invalidates proxy cache");
  const auto raw_token = colored.find("sourceColorSpace=\"raw\"");
  Require(raw_token != std::string::npos, "source colorspace fixture");
  colored.replace(raw_token, std::string("sourceColorSpace=\"raw\"").size(),
                  "sourceColorSpace=\"lin_rec709_scene\"");
  Write(scratch / "color.usda", colored);
  Write(scratch / "color.ppm",
        std::string("P6\n1 1\n255\n") + char(128) + char(128) + char(128));
  const auto linear = td::ConvertScenePreview((scratch / "color.usda").string(),
                                              resolver, options);
  Require(linear.success, linear.error);
  auto linear_stage = Load(linear.usd_path);
  linear_stage.Traverse([&](const usd::UsdPrim& p) {
    if (usd::IsParticleField(p)) {
      const auto a =
          p.GetPropertyValue("radiance:sphericalHarmonicsCoefficients")
              ->view()
              .as_float_array();
      Require(
          a.size() >= 3 && std::abs(.5 + .2820947918 * a[0] - 128. / 255) < .01,
          "explicit linear texture is not decoded as sRGB");
    }
    return true;
  });
  fs::remove(scratch / "color.ppm", ec);
  const auto missing_texture = td::ConvertScenePreview(
      (scratch / "color.usda").string(), resolver, options);
  Require(missing_texture.success, missing_texture.error);
  Write(scratch / "color.ppm",
        std::string("P6\n1 1\n255\n") + char(128) + char(128) + char(128));
  const auto added_texture = td::ConvertScenePreview(
      (scratch / "color.usda").string(), resolver, options);
  Require(added_texture.success && !added_texture.cache_hit,
          "newly available texture invalidates fallback cache");
  Write(scratch / "extras.usda",
        "#usda 1.0\n" + Quad("Surface") +
            "def Camera \"Camera\" { double focalLength=35 }\n"
            "def DomeLight \"Light\" { asset "
            "inputs:texture:file=@missing-environment.hdr@ }\n");
  const auto extras = td::ConvertScenePreview(
      (scratch / "extras.usda").string(), resolver, options);
  Require(extras.success, extras.error);
  auto extra_stage = Load(extras.usd_path);
  Require(bool(extra_stage.GetPrimAtPath(usd::Path("/Camera"))),
          "preserve camera");
  auto light = extra_stage.GetPrimAtPath(usd::Path("/Light"));
  Require(bool(light) && !light.GetPropertyValue("inputs:texture:file"),
          "portable untextured light fallback");
  options.refresh = true;
  options.progress = [](const std::string&, uint64_t) { return false; };
  const auto cancelled =
      td::ConvertScenePreview(input.string(), resolver, options);
  Require(!cancelled.success &&
              cancelled.error.find("cancelled") != std::string::npos,
          "cancellation");
  options.progress = {};
  options.gpu_budget_bytes = 0;
  Require(!td::ConvertScenePreview(input.string(), resolver, options).success,
          "zero GPU budget rejected");
  Require(td::ScenePreviewGpuBudget(uint64_t(8) << 30) == uint64_t(2) << 30,
          "quarter VRAM default");
  Require(td::ScenePreviewGpuBudget(uint64_t(8) << 30, uint64_t(1024) << 20) ==
              uint64_t(512) << 20,
          "free VRAM headroom");
  std::puts(
      "scene preview: representations, instances, texture bake/cache, export "
      "and budgets passed");
  return 0;
}
