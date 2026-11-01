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
#include "tydra/next/texture-cache.hh"
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
  for (const auto* option :
       {"--preview-volume-steps", "--preview-shadow-steps",
        "--preview-volume-bounces", "--preview-volume-step-size",
        "--preview-volume-error", "--preview-volume-cutoff"}) {
    const std::string name = option;
    const bool steps =
        name == "--preview-volume-steps" || name == "--preview-shadow-steps";
    const bool bounces = name == "--preview-volume-bounces";
    for (const auto* value :
         {"0", "1", "65536", "65537", "256", "257", ".125", "-1", "nan"}) {
      std::string flag = option, number = value;
      char program[] = "preview", scene[] = "scene.usda";
      char* arguments[] = {program, flag.data(), number.data(), scene};
      int argc = 4;
      td::ScenePreviewRequest request;
      std::string error;
      const bool valid =
          steps     ? (number == "1" || number == "256" || number == "257" ||
                       number == "65536")
          : bounces ? (number == "1" || number == "256")
          : name == "--preview-volume-step-size"
              ? (number == "1" || number == ".125")
              : (number == "0" || number == "1" || number == ".125");
      Require(td::ExtractScenePreviewArguments(&argc, arguments, &request,
                                               &error) == valid,
              "bounded medium integration CLI options");
      if (valid)
        Require(argc == 2 && std::string(arguments[1]) == "scene.usda",
                "integration options preserve application arguments");
    }
  }
  for (const auto* count : {"1", "64", "0", "65", "-1", "invalid"}) {
    char program[] = "preview", flag[] = "--preview-light-samples",
         scene[] = "scene.usda";
    std::string number = count;
    char* arguments[] = {program, flag, number.data(), scene};
    int argc = 4;
    td::ScenePreviewRequest request;
    std::string error;
    const bool valid = number == "1" || number == "64";
    const bool parsed =
        td::ExtractScenePreviewArguments(&argc, arguments, &request, &error);
    Require(parsed == valid, "bounded light-sample CLI values");
    if (valid)
      Require(argc == 2 && std::string(arguments[1]) == "scene.usda" &&
                  request.volume.light_samples ==
                      static_cast<uint32_t>(std::stoul(number)),
              "light sampling option preserves application arguments");
  }

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
  options.compute_backend = td::PreviewComputeBackend::Cpu;
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
  const auto inactive_source = scratch / "inactive-instancer.usda";
  Write(inactive_source, "#usda 1.0\n" + Quad("Prototype") +
      "def PointInstancer \"Instances\" (inactiveIds = [9007199254740993]) {\n"
      "rel prototypes = [</Prototype>]\nint[] protoIndices = [0, 0]\n"
      "point3f[] positions = [(0,0,0), (100,0,0)]\n"
      "int64[] ids = [11, 9007199254740993]\n}\n");
  auto inactive_options = options;
  inactive_options.kind = td::ScenePreviewKind::Gaussian;
  inactive_options.resolution = 32;
  auto inactive_preview = td::ConvertScenePreview(inactive_source.string(), resolver,
                                                  inactive_options);
  Require(inactive_preview.success, inactive_preview.error);
  Require(inactive_preview.source_instances == 1,
          "only the active PointInstancer placement is sampled");
  double inactive_min = 0, inactive_max = 0;
  Require(ParticleCount(inactive_preview.usd_path, &inactive_min, &inactive_max) > 0 &&
              inactive_min > -2 && inactive_max < 2,
          "inactiveIds metadata excludes distant instance from preview samples");
  const fs::path inactive_manifest = fs::path(inactive_preview.usd_path).parent_path() /
                                      "manifest.json";
  std::string legacy_manifest = Read(inactive_manifest);
  const auto version = legacy_manifest.find("\"version\": 4");
  Require(version != std::string::npos, "current visibility-aware cache version");
  legacy_manifest.replace(version, 12, "\"version\": 3");
  Write(inactive_manifest, legacy_manifest);
  const auto rebuilt_inactive = td::ConvertScenePreview(inactive_source.string(), resolver,
                                                        inactive_options);
  Require(rebuilt_inactive.success && !rebuilt_inactive.cache_hit,
          "pre-inactiveIds preview cache is rebuilt");

  auto refresh_samples = options;
  refresh_samples.kind = td::ScenePreviewKind::Gaussian;
  refresh_samples.refresh = true;
  const auto fresh_samples =
      td::ConvertScenePreview(input.string(), resolver, refresh_samples);
  Require(fresh_samples.success, fresh_samples.error);
  for (auto derived_kind :
       {td::ScenePreviewKind::Points, td::ScenePreviewKind::Volume}) {
    auto derived_options = options;
    derived_options.kind = derived_kind;
    const auto derived =
        td::ConvertScenePreview(input.string(), resolver, derived_options);
    Require(derived.success && !derived.cache_hit,
            "canonical refresh invalidates derived previews");
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
  td::ScenePreviewLodCamera source_camera;
  source_camera.eye = {0, 0, .1};
  source_camera.focal_pixels = 500;
  const auto regional = td::RefineScenePreview(
      input.string(), refined, refined_options, source_camera, 640, 480);
  Require(regional.success, regional.error);
  Require(regional.voxel_size < refined.voxel_size,
          "camera refinement exceeds cached finest resolution");
  Require(regional.samples > refined.samples,
          "regional result preserves background and adds finer source samples");
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

  td::ScenePreviewLodCamera instance_camera;
  instance_camera.eye = {42, 0, .2};
  instance_camera.near_plane = .001;
  const auto instance_refined =
      td::RefineScenePreview((scratch / "native-target.usda").string(), aliased,
                             options, instance_camera, 640, 480);
  Require(instance_refined.success, instance_refined.error);
  Require(instance_refined.samples > aliased.samples,
          "source refinement samples original instanced triangles");

  Write(scratch / "nested-source.usda", R"USD(#usda 1.0
 def Xform "Asset" {
   def Xform "Inner" (references=@asset.usda@</Asset> instanceable=true) {}
   def Xform "Copy" (references=@asset.usda@</Asset> instanceable=true) {
     double3 xformOp:translate=(4,0,0)
     uniform token[] xformOpOrder=["xformOp:translate"]
   }
 }
)USD");
  Write(scratch / "nested-instances.usda", R"USD(#usda 1.0
 def Xform "A" (references=@nested-source.usda@</Asset> instanceable=true) {}
 def Xform "B" (references=@nested-source.usda@</Asset> instanceable=true) {
   double3 xformOp:translate=(20,0,0)
   uniform token[] xformOpOrder=["xformOp:translate"]
 }
)USD");
  const auto nested = td::ConvertScenePreview(
      (scratch / "nested-instances.usda").string(), resolver, options);
  Require(nested.success, nested.error);
  instance_camera.eye = {24, 0, .2};
  const auto nested_fine =
      td::RefineScenePreview((scratch / "nested-instances.usda").string(),
                             nested, options, instance_camera, 640, 480);
  Require(nested_fine.success, nested_fine.error);
  Require(nested_fine.samples > nested.samples,
          "nested prototype source triangle graph refines placements");

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
  {
    std::string light_pixels = "P6\n64 32\n255\n";
    std::string light_rgb(64 * 32 * 3, '\0');
    light_rgb[0] = char(255);
    light_rgb[5] = char(255);
    light_pixels += light_rgb;
    Write(scratch / "light.ppm", light_pixels);
    Write(scratch / "lit.usda", "#usda 1.0\n" + Quad("Surface") +
                                    "def DomeLight \"Light\" { asset "
                                    "inputs:texture:file=@light.ppm@ }\n");
    auto lit = td::ConvertScenePreview((scratch / "lit.usda").string(),
                                       resolver, options);
    Require(lit.success, lit.error);
    const auto portable = scratch / "portable-lights";
    std::string light_error;
    Require(td::ExportScenePreview(lit, portable.string(), &light_error),
            light_error);
    auto portable_stage = Load((portable / "root.usdc").string());
    auto portable_light = portable_stage.GetPrimAtPath(usd::Path("/Light"));
    const auto* emission =
        portable_light.GetPropertyValue("inputs:texture:file");
    Require(emission && emission->as_asset_path(),
            "preserve baked light texture");
    const fs::path sidecar(emission->as_asset_path());
    Require(!sidecar.is_absolute() && sidecar.extension() == ".hdr",
            "relative linear HDR light sidecar");
    fs::remove(scratch / "light.ppm");
    td::TextureDecoder decoder({});
    td::DecodedImage image;
    Require(decoder.Decode((portable / sidecar).string(), false, &image) &&
                image.hdr && image.width == 64 && image.height == 32 &&
                image.float_pixels[0] > .9f && image.float_pixels[1] < .01f &&
                image.float_pixels[5] > .9f,
            "exported emission survives source texture removal");
    for (size_t i = 0; i < image.float_pixels.size(); ++i)
      Require(std::isfinite(image.float_pixels[i]) &&
                  ((i == 0 || i == 5) ? image.float_pixels[i] > .9f
                                      : image.float_pixels[i] == 0),
              "large HDR export initializes every texel");
    Write(scratch / "light.ppm", light_pixels);
    // Simulate a pre-lighting cache, with matching geometry and dependency
    // stamps.
    lit = td::ConvertScenePreview((scratch / "lit.usda").string(), resolver,
                                  options);
    const auto manifest =
        fs::path(lit.usd_path).parent_path() / "manifest.json";
    std::ifstream old_manifest(manifest);
    std::string contents((std::istreambuf_iterator<char>(old_manifest)), {});
    auto version = contents.find("\"lighting_version\": 2");
    Require(version != std::string::npos, "lighting cache version marker");
    contents.replace(version, std::string("\"lighting_version\": 2").size(),
                     "\"lighting_version\": 0");
    Write(manifest, contents);
    bool upgraded = false;
    options.progress = [&](const std::string& phase, uint64_t) {
      if (phase == "light-upgrade") upgraded = true;
      return phase != "sample";
    };
    const auto upgrade = td::ConvertScenePreview(
        (scratch / "lit.usda").string(), resolver, options);
    Require(upgrade.success && upgraded && upgrade.samples == lit.samples,
            "legacy lighting upgrade reuses sampled geometry");
    options.progress = {};
  }
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
