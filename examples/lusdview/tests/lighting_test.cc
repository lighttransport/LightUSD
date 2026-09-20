// SPDX-License-Identifier: Apache-2.0
#include "mesh_build.hh"
#include "dome_light.hh"
#include "lighting_eval.hh"
#include "lighting_ies.hh"
#include "raster_lighting.hh"
#include "rt_scene_build.hh"
#include "texture_tools.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "tydra/render-data.hh"

namespace {

namespace tydra = lightusd::tydra;

bool Near(float a, float b, float eps = 1.0e-5f) {
  return std::fabs(a - b) <= eps;
}

void Identity(float m[16]) {
  for (int i = 0; i < 16; ++i) m[i] = 0.0f;
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

int AddImage(tydra::RenderScene* scene) {
  tydra::BufferData buf;
  buf.data = {
      255, 128, 64, 255,
      32, 64, 128, 255,
  };
  const int bufferId = static_cast<int>(scene->buffers.size());
  scene->buffers.push_back(std::move(buf));

  tydra::TextureImage img;
  img.buffer_id = bufferId;
  img.decoded = true;
  img.width = 2;
  img.height = 1;
  img.channels = 4;
  img.texelComponentType = tydra::ComponentType::UInt8;
  img.colorSpace = tydra::ColorSpace::Lin_sRGB;
  const int imageId = static_cast<int>(scene->images.size());
  scene->images.push_back(std::move(img));
  return imageId;
}

}  // namespace

int main() {
  {
    lusdview::DrawLightCPU base;
    base.type = lusdview::DrawLightCPU::Type::Dome;
    base.effectiveColor[0] = 1.0f;
    base.effectiveColor[1] = 2.0f;
    base.effectiveColor[2] = 3.0f;
    base.normalizedColor[0] = 1.0f;
    base.normalizedColor[1] = 2.0f;
    base.normalizedColor[2] = 3.0f;
    base.effectiveIntensity = 2.0f;
    Identity(base.transform);
    lusdview::DrawLightCPU edited;
    lusdview::ApplyDomeLightControls(0.5f, 90.0f, base, &edited);
    if (std::fabs(edited.effectiveColor[2] - 1.5f) > 1.0e-5f ||
        std::fabs(edited.effectiveIntensity - 1.0f) > 1.0e-5f ||
        std::fabs(edited.transform[2] + 1.0f) > 1.0e-5f ||
        std::fabs(edited.transform[8] - 1.0f) > 1.0e-5f) {
      std::fprintf(stderr, "runtime dome controls are incorrect\n");
      return 1;
    }
    std::string presetError;
    lusdview::DrawLightCPU furnace;
    if (!lusdview::BuildWhiteFurnaceDome(false, &furnace, &presetError) ||
        !furnace.ibl.valid || furnace.ibl.envCube.empty() ||
        furnace.ibl.envLatlongWidth != 32 ||
        furnace.ibl.envLatlongHeight != 16 ||
        furnace.ibl.envLatlongRgbe.size() != 32u * 16u * 4u ||
        furnace.ibl.envLatlongRgbe[0] < 127u ||
        furnace.ibl.envLatlongRgbe[3] != 129u) {
      std::fprintf(stderr, "white-furnace preset failed: %s\n",
                   presetError.c_str());
      return 1;
    }
  }
  tydra::RenderScene scene;

  tydra::RenderLight rect;
  rect.name = "rect_key";
  rect.type = tydra::RenderLight::Type::Rect;
  rect.color = {0.25f, 0.5f, 1.0f};
  rect.intensity = 4.0f;
  rect.exposure = 1.0f;
  rect.normalize = true;
  rect.width = 2.0f;
  rect.height = 4.0f;
  rect.position = {0.0f, 4.0f, 0.0f};
  rect.shapingConeAngle = 30.0f;
  rect.shapingFocusTint = {0.1f, 0.2f, 0.3f};
  rect.shapingIesFile = "profiles/key.ies";
  rect.shapingIesAngleScale = 1.25f;
  rect.shapingIesNormalize = true;
  rect.shadowColor = {0.05f, 0.06f, 0.07f};
  rect.shadowDistance = 12.0f;
  rect.shadowFalloff = 3.0f;
  rect.shadowFalloffGamma = 2.0f;
  rect.light_links_all = false;
  rect.light_link_mesh_indices = {3, 5};
  tydra::SpectralEmission rectSpd;
  rectSpd.samples = {
      tydra::vec2{450.0f, 0.25f},
      tydra::vec2{550.0f, 1.0f},
  };
  rect.spd_emission = rectSpd;
  scene.lights.push_back(rect);

  tydra::RenderLight dome;
  dome.name = "dome_env";
  dome.type = tydra::RenderLight::Type::Dome;
  dome.transform = tydra::mat4::identity();
  dome.envmap_texture_id = AddImage(&scene);
  dome.domeTextureFormat = tydra::RenderLight::DomeTextureFormat::Latlong;
  scene.lights.push_back(dome);

  lusdview::DrawScene draw;
  lusdview::BuildDrawScene(scene, &draw);

  if (draw.lights.size() != 2) {
    std::fprintf(stderr, "expected two lights, got %zu\n", draw.lights.size());
    return 1;
  }

  const lusdview::DrawLightCPU& key = draw.lights[0];
  if (!Near(key.effectiveIntensity, 8.0f) ||
      !Near(key.effectiveColor[0], 2.0f) ||
      !Near(key.effectiveColor[1], 4.0f) ||
      !Near(key.effectiveColor[2], 8.0f) ||
      !Near(key.area, 8.0f) || !Near(key.invArea, 0.125f) ||
      !Near(key.normalizedColor[0], 0.25f) ||
      !Near(key.normalizedColor[1], 0.5f) ||
      !Near(key.normalizedColor[2], 1.0f)) {
    std::fprintf(stderr, "derived rect light values are wrong\n");
    return 1;
  }

  // The legacy and `--next` loaders share this derivation contract. Portal
  // rectangles use the same area normalization as authored rect lights, while
  // invalid negative dimensions/radii are clamped instead of producing a
  // physically nonsensical positive area.
  lusdview::DrawLightCPU portal;
  portal.type = lusdview::DrawLightCPU::Type::Portal;
  portal.width = 3.0f;
  portal.height = 2.0f;
  portal.normalize = true;
  portal.intensity = 6.0f;
  lusdview::ApplyDerivedLightParams(&portal);
  if (!Near(portal.area, 6.0f) || !Near(portal.invArea, 1.0f / 6.0f) ||
      !Near(portal.effectiveIntensity, 6.0f)) {
    std::fprintf(stderr, "canonical portal-light derivation mismatch\n");
    return 1;
  }
  lusdview::DrawLightCPU invalid;
  invalid.type = lusdview::DrawLightCPU::Type::Rect;
  invalid.width = -2.0f;
  invalid.height = 3.0f;
  lusdview::ApplyDerivedLightParams(&invalid);
  if (!Near(invalid.area, 0.0f) || !Near(invalid.invArea, 0.0f)) {
    std::fprintf(stderr, "invalid light dimensions were not clamped\n");
    return 1;
  }

  const std::string iesPath = "/tmp/lusdview-lighting-test.ies";
  {
    std::ofstream ies(iesPath);
    ies << "IESNA:LM-63-1995\nTILT=NONE\n"
        << "1 1000 1 3 1 1 2 1 1 1 1 1 1\n"
        << "0 45 90\n0\n1 0.5 0.25\n";
  }
  lusdview::DrawLightCPU iesLight;
  iesLight.shapingIesNormalize = true;
  iesLight.shapingIesAngleScale = 1.2f;
  std::string iesError;
  const bool iesLoaded = lusdview::LoadIesProfile(iesPath, &iesLight, &iesError);
  const float iesMid = lusdview::EvaluateIesProfile(iesLight, 45.0f, 0.0f);
  const float iesTop = lusdview::EvaluateIesProfile(iesLight, 90.0f, 180.0f);
  if (!iesLoaded || !Near(iesMid, 0.6f) || !Near(iesTop, 0.3f)) {
    std::fprintf(stderr, "IES profile parsing/interpolation mismatch: loaded=%d mid=%f top=%f (%s)\n",
                 iesLoaded ? 1 : 0, iesMid, iesTop, iesError.c_str());
    std::remove(iesPath.c_str());
    return 1;
  }
  std::remove(iesPath.c_str());
  lusdview::DrawLightCPU cachedIesLight;
  const bool cachedIesLoaded =
      lusdview::LoadIesProfile(iesPath, &cachedIesLight, &iesError);
  if (!cachedIesLoaded || !cachedIesLight.iesValid) {
    std::fprintf(stderr, "IES profile cache lookup failed: %s\n",
                 iesError.c_str());
    return 1;
  }
  std::vector<float> iesPacked(lusdview::kRtLightParamFloats, 0.0f);
  lusdview::PackRtLightParams(iesLight, -1, iesPacked.data());
  if (!Near(iesPacked[52], 2.0f) || !Near(iesPacked[53], 4.0f) ||
      !Near(iesPacked[54], 6.0f) || !Near(iesPacked[56], 1.2f) ||
      !Near(iesPacked[74], 0.3f)) {
    std::fprintf(stderr, "IES LUT packing mismatch\n");
    return 1;
  }

  const std::string iesAzimuthPath = "/tmp/lusdview-lighting-azimuth.ies";
  {
    std::ofstream ies(iesAzimuthPath);
    ies << "IESNA:LM-63-1995\nTILT=NONE\n"
        << "1 1000 1 3 2 1 2 1 1 1 1 1 1\n"
        << "0 45 90\n0 180\n"
        << "1 0.5 0.25 0.2 0.4 0.8\n";
  }
  lusdview::DrawLightCPU azimuthLight;
  azimuthLight.shapingIesNormalize = true;
  std::string azimuthError;
  const bool azimuthLoaded =
      lusdview::LoadIesProfile(iesAzimuthPath, &azimuthLight, &azimuthError);
  const float azimuth0 =
      lusdview::EvaluateIesProfile(azimuthLight, 0.0f, 0.0f);
  const float azimuth180 =
      lusdview::EvaluateIesProfile(azimuthLight, 0.0f, 180.0f);
  const float azimuth240 =
      lusdview::EvaluateIesProfile(azimuthLight, 0.0f, 240.0f);
  std::remove(iesAzimuthPath.c_str());
  if (!azimuthLoaded || !Near(azimuth0, 1.0f) || !Near(azimuth180, 0.2f) ||
      !Near(azimuth240, 0.46666667f, 1.0e-4f)) {
    std::fprintf(stderr,
                 "IES azimuth interpolation mismatch: loaded=%d h0=%f h180=%f h240=%f (%s)\n",
                 azimuthLoaded ? 1 : 0, azimuth0, azimuth180, azimuth240,
                 azimuthError.c_str());
    return 1;
  }
  azimuthLight.transform[0] = 1.0f;
  azimuthLight.transform[5] = 1.0f;
  const lusdview::RasterLightSet azimuthRaster =
      lusdview::PackRasterLights({azimuthLight}, 0);
  if (azimuthRaster.count != 1 ||
      !Near(azimuthRaster.lights[0].iesProfile[0], 1.0f) ||
      !Near(azimuthRaster.lights[0].iesProfile[3], 0.2f) ||
      !Near(azimuthRaster.lights[0].iesProfile[4], 0.46666667f, 1.0e-4f) ||
      !Near(azimuthRaster.lights[0].iesAxisX[0], 1.0f) ||
      !Near(azimuthRaster.lights[0].iesAxisY[1], 1.0f)) {
    std::fprintf(stderr, "raster IES azimuth packing mismatch\n");
    return 1;
  }

  const std::string malformedIesPath = "/tmp/lusdview-lighting-malformed.ies";
  {
    std::ofstream ies(malformedIesPath);
    ies << "IESNA:LM-63-1995\nTILT=NONE\n"
        << "1 1000 1 3 1 1 2 1 1 1 1 1 1\n"
        << "0 45 45\n0\n1 0.5 0.25\n";
  }
  const bool malformedLoaded =
      lusdview::LoadIesProfile(malformedIesPath, &azimuthLight, &azimuthError);
  std::remove(malformedIesPath.c_str());
  if (malformedLoaded || azimuthLight.iesValid ||
      !azimuthLight.iesCandela.empty()) {
    std::fprintf(stderr, "malformed IES reload retained stale profile\n");
    return 1;
  }
  lusdview::DrawLightCPU emptyPathReload = iesLight;
  const bool emptyPathLoaded =
      lusdview::LoadIesProfile(std::string(), &emptyPathReload, &iesError);
  if (emptyPathLoaded || emptyPathReload.iesValid ||
      !emptyPathReload.iesCandela.empty()) {
    std::fprintf(stderr, "empty IES reload retained stale profile\n");
    return 1;
  }
  const std::string crlfIesPath = "/tmp/lusdview-lighting-crlf.ies";
  {
    std::ofstream ies(crlfIesPath, std::ios::binary);
    ies << "IESNA:LM-63-1995\r\nTILT=NONE\r\n"
        << "1 1000 1 3 1 1 2 1 1 1 1 1 1\r\n"
        << "0 45 90\r\n0\r\n1 0.5 0.25\r\n";
  }
  lusdview::DrawLightCPU crlfLight;
  const bool crlfLoaded =
      lusdview::LoadIesProfile(crlfIesPath, &crlfLight, &iesError);
  std::remove(crlfIesPath.c_str());
  if (!crlfLoaded || !crlfLight.iesValid) {
    std::fprintf(stderr, "CRLF IES profile was rejected: %s\n",
                 iesError.c_str());
    return 1;
  }
  const std::string tiltIesPath = "/tmp/lusdview-lighting-tilt.ies";
  {
    std::ofstream ies(tiltIesPath);
    ies << "IESNA:LM-63-1995\nTILT=INCLUDE\n"
        << "2\n0 90\n1 0.5\n"
        << "1 1000 1 3 1 1 2 1 1 1 1 1 1\n"
        << "0 45 90\n0\n1 0.5 0.25\n";
  }
  lusdview::DrawLightCPU tiltLight;
  const bool tiltLoaded =
      lusdview::LoadIesProfile(tiltIesPath, &tiltLight, &iesError);
  std::remove(tiltIesPath.c_str());
  if (!tiltLoaded || !Near(lusdview::EvaluateIesProfile(tiltLight, 90.0f, 0.0f),
                           0.125f)) {
    std::fprintf(stderr, "IES TILT interpolation mismatch: loaded=%d value=%f (%s)\n",
                 tiltLoaded ? 1 : 0,
                 lusdview::EvaluateIesProfile(tiltLight, 90.0f, 0.0f),
                 iesError.c_str());
    return 1;
  }

  const std::string tiltIncludeTable = "/tmp/lusdview-lighting-tilt.tbl";
  const std::string tiltIncludeProfile = "/tmp/lusdview-lighting-tilt-file.ies";
  {
    std::ofstream table(tiltIncludeTable);
    table << "2\n0 90\n1 0.5\n";
    std::ofstream ies(tiltIncludeProfile);
    ies << "IESNA:LM-63-1995\nTILT=INCLUDE\n"
        << "  \"lusdview-lighting-tilt.tbl\"  \n"
        << "1 1000 1 3 1 1 2 1 1 1 1 1 1\n"
        << "0 45 90\n0\n1 0.5 0.25\n";
  }
  lusdview::DrawLightCPU externalTiltLight;
  const bool externalTiltLoaded = lusdview::LoadIesProfile(
      tiltIncludeProfile, &externalTiltLight, &iesError);
  std::remove(tiltIncludeTable.c_str());
  std::remove(tiltIncludeProfile.c_str());
  if (!externalTiltLoaded ||
      !Near(lusdview::EvaluateIesProfile(externalTiltLight, 90.0f, 0.0f),
            0.125f)) {
    std::fprintf(stderr, "external IES TILT include mismatch: loaded=%d value=%f (%s)\n",
                 externalTiltLoaded ? 1 : 0,
                 lusdview::EvaluateIesProfile(externalTiltLight, 90.0f, 0.0f),
                 iesError.c_str());
    return 1;
  }

  lusdview::DrawLightCPU geometry;
  geometry.type = lusdview::DrawLightCPU::Type::Geometry;
  geometry.geometryTriOffset = 17;
  geometry.geometryTriCount = 9;
  geometry.geometryInstance = 3;
  std::vector<float> geometryPacked(lusdview::kRtLightParamFloats, 0.0f);
  lusdview::PackRtLightParams(geometry, -1, geometryPacked.data());
  if (!Near(geometryPacked[52], 3.0f) ||
      !Near(geometryPacked[53], 17.0f) ||
      !Near(geometryPacked[54], 9.0f) ||
      !Near(geometryPacked[55], 3.0f)) {
    std::fprintf(stderr, "GeometryLight payload packing mismatch\n");
    return 1;
  }

  lusdview::DrawMeshCPU geometryMesh;
  Identity(geometryMesh.world);
  geometryMesh.vertices = {
      {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      { 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      { 1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
      {-1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
  };
  geometryMesh.indices = {0, 1, 2, 0, 2, 3};
  lusdview::DrawLightCPU sampledGeometry = geometry;
  sampledGeometry.geometryMesh = 0;
  std::vector<lusdview::DrawLightCPU> sampledGeometryLights{sampledGeometry};
  lusdview::PrepareRasterGeometryLightSamples(&sampledGeometryLights,
                                               {geometryMesh});
  const lusdview::RasterLightSet sampledGeometryRaster =
      lusdview::PackRasterLights(sampledGeometryLights, 1);
  if (sampledGeometryLights[0].geometryRasterSampleCount != 8 ||
      sampledGeometryRaster.count != 1 ||
      sampledGeometryRaster.omittedGeometryLights != 0) {
    std::fprintf(stderr, "raster GeometryLight sample preparation mismatch\n");
    return 1;
  }
  for (int i = 0; i < 8; ++i) {
    const float x = sampledGeometryRaster.lights[0].iesProfile[i * 3 + 0];
    const float y = sampledGeometryRaster.lights[0].iesProfile[i * 3 + 1];
    const float z = sampledGeometryRaster.lights[0].iesProfile[i * 3 + 2];
    if (x < -1.0f || x > 1.0f || y < -1.0f || y > 1.0f || !Near(z, 0.0f)) {
      std::fprintf(stderr, "raster GeometryLight sample escaped source mesh\n");
      return 1;
    }
  }

  // Raster light packing is shared by GL/Vulkan. Dome lights are excluded from
  // direct evaluation, and authored collections become a compact per-mesh mask.
  const lusdview::RasterLightSet raster =
      lusdview::PackRasterLights(draw.lights, 7);
  if (raster.count != 1 || raster.truncated != 0 ||
      raster.meshMasks.size() != 7 || raster.meshMasks[3] != 1u ||
      raster.meshMasks[5] != 1u || raster.meshMasks[0] != 0u) {
    std::fprintf(stderr, "raster light packing/link mask mismatch\n");
    return 1;
  }

  std::vector<lusdview::DrawLightCPU> rasterFallbacks = draw.lights;
  lusdview::DrawLightCPU geometryRaster = geometry;
  geometryRaster.position[0] = 2.0f;
  geometryRaster.normalizedColor[0] = 0.8f;
  geometryRaster.normalizedColor[1] = 0.7f;
  geometryRaster.normalizedColor[2] = 0.6f;
  lusdview::DrawLightCPU portalRaster = portal;
  portalRaster.position[2] = 3.0f;
  portalRaster.normalizedColor[0] = 0.4f;
  portalRaster.normalizedColor[1] = 0.5f;
  portalRaster.normalizedColor[2] = 0.6f;
  rasterFallbacks.push_back(geometryRaster);
  rasterFallbacks.push_back(portalRaster);
  const lusdview::RasterLightSet specialLights =
      lusdview::PackRasterLights(rasterFallbacks, 1);
  if (specialLights.count != 2 || specialLights.omittedGeometryLights != 1 ||
      static_cast<int>(specialLights.lights[1].positionType[3] + 0.5f) !=
          static_cast<int>(lusdview::DrawLightCPU::Type::Portal) ||
      !Near(specialLights.lights[1].areaParams[1], portalRaster.width) ||
      !Near(specialLights.lights[1].areaParams[2], portalRaster.height)) {
    std::fprintf(stderr, "raster GeometryLight omission/PortalLight packing mismatch\n");
    return 1;
  }
  if (!Near(raster.lights[0].positionType[0], key.position[0]) ||
      !Near(raster.lights[0].colorDiffuse[0], key.normalizedColor[0]) ||
      !Near(raster.lights[0].colorDiffuse[3], key.diffuse) ||
      !Near(raster.lights[0].specularShape[0], key.specular) ||
      raster.lights[0].specularShape[3] != 1.0f ||
      !Near(raster.lights[0].areaParams[1], key.width) ||
      !Near(raster.lights[0].areaParams[2], key.height)) {
    std::fprintf(stderr, "raster packed light fields mismatch\n");
    return 1;
  }

  // Packing is deterministic and bounded. DistantLight stores its authored
  // emission direction in DrawLightCPU, so the raster record must expose the
  // opposite surface-to-light vector used by the BRDF.
  std::vector<lusdview::DrawLightCPU> manyLights;
  for (int i = 0; i < lusdview::kMaxRasterLights + 2; ++i) {
    lusdview::DrawLightCPU light;
    light.type = lusdview::DrawLightCPU::Type::Distant;
    light.direction[0] = 0.0f;
    light.direction[1] = -1.0f;
    light.direction[2] = static_cast<float>(i);
    manyLights.push_back(light);
  }
  const lusdview::RasterLightSet bounded =
      lusdview::PackRasterLights(manyLights, 1);
  if (bounded.count != lusdview::kMaxRasterLights || bounded.truncated != 2 ||
      bounded.meshMasks.size() != 1 || bounded.meshMasks[0] != 0xffffu ||
      !Near(bounded.lights[0].directionAngle[0], 0.0f) ||
      !Near(bounded.lights[0].directionAngle[1], 1.0f) ||
      !Near(bounded.lights[1].directionAngle[2], -1.0f)) {
    std::fprintf(stderr, "raster light bound/direction mismatch\n");
    return 1;
  }
  if (bounded.shadowLightSlot != 0) {
    std::fprintf(stderr, "first enabled distant shadow selection mismatch\n");
    return 1;
  }
  std::vector<lusdview::DrawLightCPU> shadowFallback(3);
  shadowFallback[0].type = lusdview::DrawLightCPU::Type::Sphere;
  shadowFallback[0].hasShaping = true;
  shadowFallback[0].shadowEnable = false;
  shadowFallback[1].type = lusdview::DrawLightCPU::Type::Sphere;
  shadowFallback[1].hasShaping = true;
  shadowFallback[2].type = lusdview::DrawLightCPU::Type::Rect;
  const lusdview::RasterLightSet fallback =
      lusdview::PackRasterLights(shadowFallback, 0);
  if (fallback.shadowLightSlot != 1) {
    std::fprintf(stderr, "shaped-sphere shadow fallback selection mismatch\n");
    return 1;
  }
  std::vector<lusdview::DrawLightCPU> areaFallback(2);
  areaFallback[0].type = lusdview::DrawLightCPU::Type::Point;
  areaFallback[0].shadowEnable = false;
  areaFallback[1].type = lusdview::DrawLightCPU::Type::Rect;
  areaFallback[1].position[1] = 4.0f;
  areaFallback[1].direction[1] = -1.0f;
  const lusdview::RasterLightSet areaLights =
      lusdview::PackRasterLights(areaFallback, 0);
  if (areaLights.shadowLightSlot != 1) {
    std::fprintf(stderr, "one-sided area-light shadow fallback mismatch\n");
    return 1;
  }
  std::vector<lusdview::DrawLightCPU> linkedShadow(1);
  linkedShadow[0].type = lusdview::DrawLightCPU::Type::Distant;
  linkedShadow[0].shadowLinksAll = false;
  linkedShadow[0].shadowLinkMeshIndices = {1};
  const lusdview::RasterLightSet linked =
      lusdview::PackRasterLights(linkedShadow, 3);
  if (lusdview::RasterShadowIncludesMesh(linked, 0) ||
      !lusdview::RasterShadowIncludesMesh(linked, 1) ||
      lusdview::RasterShadowIncludesMesh(linked, 2)) {
    std::fprintf(stderr, "raster shadow-link mask mismatch\n");
    return 1;
  }
  linkedShadow[0].lightLinksAll = false;
  linkedShadow[0].lightLinkMeshIndices = {0, 2};
  linkedShadow.push_back(lusdview::DrawLightCPU{});
  linkedShadow[1].lightLinksAll = true;
  linkedShadow[1].shadowLinksAll = false;
  linkedShadow[1].shadowLinkMeshIndices = {2};
  if (lusdview::RtLightCollectionMaskForMesh(linkedShadow, 0, false) != 3u ||
      lusdview::RtLightCollectionMaskForMesh(linkedShadow, 1, false) != 2u ||
      lusdview::RtLightCollectionMaskForMesh(linkedShadow, 2, true) != 2u ||
      lusdview::RtLightCollectionMaskForMesh(linkedShadow, 1, true) != 1u) {
    std::fprintf(stderr, "RT light/shadow collection mask mismatch\n");
    return 1;
  }
  linkedShadow[0].lightLinkPaths = {"/World/Points"};
  linkedShadow[0].shadowLinkPaths = {"/World/Points"};
  if (lusdview::RtLightCollectionMaskForPath(linkedShadow, "/World/Points/Carrier",
                                             false) != 3u ||
      lusdview::RtLightCollectionMaskForPath(linkedShadow, "/World/Other",
                                             false) != 2u) {
    std::fprintf(stderr, "native carrier light-link path mask mismatch\n");
    return 1;
  }
  const lusdview::RasterLightSet carrierRaster =
      lusdview::PackRasterLights(linkedShadow, 0);
  if (lusdview::RasterLightMaskForPath(carrierRaster,
                                       "/World/Points/Carrier") != 3u ||
      lusdview::RasterLightMaskForPath(carrierRaster, "/World/Other") != 2u) {
    std::fprintf(stderr, "raster carrier light-link path mask mismatch\n");
    return 1;
  }
  if (lusdview::RasterShadowMaskForPath(carrierRaster,
                                        "/World/Points/Carrier") != 1u ||
      lusdview::RasterShadowMaskForPath(carrierRaster, "/World/Other") != 0u ||
      !lusdview::RasterShadowIncludesPath(carrierRaster,
                                          "/World/Points/Carrier") ||
      lusdview::RasterShadowIncludesPath(carrierRaster, "/World/Other")) {
    std::fprintf(stderr, "raster carrier shadow-link path mask mismatch\n");
    return 1;
  }
  const float shadowMin[3] = {-2.0f, -1.0f, -3.0f};
  const float shadowExtent[3] = {4.0f, 2.0f, 6.0f};
  std::vector<lusdview::DrawLightCPU> pointShadow(1);
  pointShadow[0].type = lusdview::DrawLightCPU::Type::Point;
  pointShadow[0].position[1] = 4.0f;
  const lusdview::RasterLightSet pointLights =
      lusdview::PackRasterLights(pointShadow, 0);
  lusdview::RasterShadowCamera pointShadowCamera;
  if (pointLights.shadowLightSlot != 0 ||
      !lusdview::BuildRasterShadowCamera(pointLights, shadowMin, shadowExtent,
                                         true, &pointShadowCamera) ||
      pointShadowCamera.perspective) {
    std::fprintf(stderr, "point-light shadow approximation mismatch\n");
    return 1;
  }
  lusdview::RasterPointShadowCameras pointCubeGl;
  lusdview::RasterPointShadowCameras pointCubeVk;
  bool oppositeFacesDiffer = false;
  if (!lusdview::BuildRasterPointShadowCameras(pointLights, shadowMin,
                                                shadowExtent, false,
                                                &pointCubeGl) ||
      !lusdview::BuildRasterPointShadowCameras(pointLights, shadowMin,
                                                shadowExtent, true,
                                                &pointCubeVk) ||
      pointCubeGl.lightSlot != 0 || pointCubeVk.lightSlot != 0 ||
      !(pointCubeGl.farPlane > pointCubeGl.nearPlane) ||
      !(pointCubeVk.farPlane > pointCubeVk.nearPlane)) {
    std::fprintf(stderr, "point-light cube shadow camera mismatch\n");
    return 1;
  }
  for (int i = 0; i < 16; ++i) {
    if (!Near(pointCubeGl.viewProj[0].m[i], pointCubeGl.viewProj[1].m[i])) {
      oppositeFacesDiffer = true;
      break;
    }
  }
  if (!oppositeFacesDiffer) {
    std::fprintf(stderr, "opposite point-light cube faces are identical\n");
    return 1;
  }
  std::vector<lusdview::DrawLightCPU> rectShadow(1);
  rectShadow[0].type = lusdview::DrawLightCPU::Type::Rect;
  rectShadow[0].position[1] = 4.0f;
  const lusdview::RasterLightSet rectShadowLights =
      lusdview::PackRasterLights(rectShadow, 0);
  if (!lusdview::BuildRasterPointShadowCameras(
          rectShadowLights, shadowMin, shadowExtent, true, &pointCubeVk)) {
    std::fprintf(stderr, "area-light center shadow camera was not built\n");
    return 1;
  }
  if (lusdview::BuildRasterPointShadowCameras(bounded, shadowMin,
                                               shadowExtent, true,
                                               &pointCubeVk)) {
    std::fprintf(stderr, "non-point light unexpectedly produced cube shadows\n");
    return 1;
  }
  lusdview::RasterShadowCamera glShadow;
  lusdview::RasterShadowCamera vkShadow;
  if (!lusdview::BuildRasterShadowCamera(bounded, shadowMin, shadowExtent,
                                         false, &glShadow) ||
      !lusdview::BuildRasterShadowCamera(bounded, shadowMin, shadowExtent,
                                         true, &vkShadow) ||
      glShadow.lightSlot != 0 || glShadow.perspective ||
      vkShadow.lightSlot != 0 || vkShadow.perspective ||
      !(glShadow.farPlane > glShadow.nearPlane)) {
    std::fprintf(stderr, "distant shadow camera fitting mismatch\n");
    return 1;
  }
  lusdview::RasterShadowCamera sphereShadow;
  if (!lusdview::BuildRasterShadowCamera(fallback, shadowMin, shadowExtent,
                                         false, &sphereShadow) ||
      sphereShadow.lightSlot != 1 || !sphereShadow.perspective ||
      !(sphereShadow.farPlane > sphereShadow.nearPlane)) {
    std::fprintf(stderr, "sphere shadow camera fitting mismatch\n");
    return 1;
  }
  lusdview::RasterShadowCamera areaShadow;
  if (!lusdview::BuildRasterShadowCamera(areaLights, shadowMin, shadowExtent,
                                         true, &areaShadow) ||
      areaShadow.lightSlot != 1 || areaShadow.perspective ||
      !(areaShadow.farPlane > areaShadow.nearPlane)) {
    std::fprintf(stderr, "area-light shadow camera fitting mismatch\n");
    return 1;
  }
  if (!key.hasShaping || key.shapingIesFile != "profiles/key.ies" ||
      !Near(key.shapingIesAngleScale, 1.25f) || !key.shapingIesNormalize ||
      key.lightLinksAll || !key.hasSpectralEmission ||
      key.lightLinkMeshIndices.size() != 2) {
    std::fprintf(stderr, "light shaping/linking metadata was not preserved\n");
    return 1;
  }
  if (!draw.hasPreviewLight ||
      !Near(draw.previewLightDir[0], 0.0f) ||
      !Near(draw.previewLightDir[1], 1.0f) ||
      !Near(draw.previewLightDir[2], 0.0f) ||
      !Near(draw.previewLightColor[0], 0.25f) ||
      !Near(draw.previewLightColor[1], 0.5f) ||
      !Near(draw.previewLightColor[2], 1.0f)) {
    std::fprintf(stderr, "preview key light was not derived from USD light\n");
    return 1;
  }

  const lusdview::DrawLightCPU& env = draw.lights[1];
  if (env.renderEnvmapImage != 0 || env.envmapTexture < 0 ||
      static_cast<size_t>(env.envmapTexture) >= draw.textures.size()) {
    std::fprintf(stderr, "dome envmap image was not decoded into DrawScene\n");
    return 1;
  }
  const lusdview::DrawTextureCPU& tex =
      draw.textures[static_cast<size_t>(env.envmapTexture)];
  if (tex.image.width != 2 || tex.image.height != 1 || tex.image.channels != 4 ||
      tex.image.data.size() != 8) {
    std::fprintf(stderr, "unexpected decoded dome texture shape\n");
    return 1;
  }

  // Split-sum IBL bake (textools builds only; default domeIbl quality = high).
  if (lusdview::TexToolsAvailable()) {
    const lusdview::DomeIblCPU& ibl = env.ibl;
    if (!ibl.valid) {
      std::fprintf(stderr, "dome IBL was not baked\n");
      return 1;
    }
    if (ibl.specFaceSize != 64 || ibl.specLevels.size() != 5) {
      std::fprintf(stderr, "unexpected IBL spec chain shape\n");
      return 1;
    }
    for (size_t l = 0; l < ibl.specLevels.size(); ++l) {
      const size_t fs = static_cast<size_t>(ibl.specFaceSize) >> l;
      if (ibl.specLevels[l].size() != 6 * fs * fs * 3) {
        std::fprintf(stderr, "IBL spec level %zu has wrong size\n", l);
        return 1;
      }
    }
    if (ibl.irrFaceSize != 32 ||
        ibl.irradiance.size() !=
            6u * static_cast<size_t>(ibl.irrFaceSize) * ibl.irrFaceSize * 3u) {
      std::fprintf(stderr, "IBL irradiance has wrong size\n");
      return 1;
    }
    float irrMax = 0.0f;
    for (float v : ibl.irradiance) {
      if (v < 0.0f) {
        std::fprintf(stderr, "IBL irradiance has negative texels\n");
        return 1;
      }
      irrMax = std::max(irrMax, v);
    }
    if (!(irrMax > 0.0f)) {
      std::fprintf(stderr, "IBL irradiance is all zero\n");
      return 1;
    }
    if (ibl.lutSize != 64 ||
        ibl.brdfLut.size() != 2u * static_cast<size_t>(ibl.lutSize) * ibl.lutSize) {
      std::fprintf(stderr, "IBL BRDF LUT has wrong size\n");
      return 1;
    }
    // LUT indexed [roughness][NdotV] row-major: (rough~0, NdotV~1) must be
    // near (scale=1, bias=0).
    const size_t corner = (0u * static_cast<size_t>(ibl.lutSize) +
                           static_cast<size_t>(ibl.lutSize) - 1u) * 2u;
    const float scale = ibl.brdfLut[corner + 0];
    const float bias = ibl.brdfLut[corner + 1];
    if (!(scale > 0.9f && scale <= 1.05f && bias >= 0.0f && bias < 0.1f)) {
      std::fprintf(stderr, "IBL BRDF LUT corner is wrong: %f %f\n",
                   static_cast<double>(scale), static_cast<double>(bias));
      return 1;
    }
  }

  tydra::RenderScene formatScene;
  tydra::RenderLight mirrored;
  mirrored.name = "dome_mirrored";
  mirrored.type = tydra::RenderLight::Type::Dome;
  mirrored.envmap_texture_id = AddImage(&formatScene);
  mirrored.domeTextureFormat = tydra::RenderLight::DomeTextureFormat::MirroredBall;
  formatScene.lights.push_back(mirrored);
  tydra::RenderLight angular;
  angular.name = "dome_angular";
  angular.type = tydra::RenderLight::Type::Dome;
  angular.envmap_texture_id = AddImage(&formatScene);
  angular.domeTextureFormat = tydra::RenderLight::DomeTextureFormat::Angular;
  formatScene.lights.push_back(angular);
  lusdview::TextureRuntimeOptions formatOpt;
  formatOpt.domeIbl = 0;
  lusdview::DrawScene formatDraw;
  lusdview::BuildDrawScene(formatScene, &formatDraw, nullptr, nullptr,
                           formatOpt);
  if (formatDraw.lights.size() != 2 || formatDraw.textures.size() != 2) {
    std::fprintf(stderr, "dome format scene did not preserve lights/textures\n");
    return 1;
  }
  if (formatDraw.lights[0].domeTextureFormat !=
          lusdview::DrawLightCPU::DomeTextureFormat::MirroredBall ||
      formatDraw.lights[1].domeTextureFormat !=
          lusdview::DrawLightCPU::DomeTextureFormat::Angular ||
      formatDraw.lights[0].envmapTexture < 0 ||
      formatDraw.lights[1].envmapTexture < 0) {
    std::fprintf(stderr, "MirroredBall/Angular dome formats were not preserved\n");
    return 1;
  }

  lusdview::DrawMaterialCPU mat;
  mat.name = "mat";
  draw.materials.push_back(mat);

  lusdview::DrawMeshCPU mesh;
  mesh.name = "tri";
  Identity(mesh.world);
  Identity(mesh.skinGeomBind);
  mesh.vertices.resize(3);
  mesh.vertices[0].px = 0.0f;
  mesh.vertices[0].py = 0.0f;
  mesh.vertices[0].pz = 0.0f;
  mesh.vertices[1].px = 1.0f;
  mesh.vertices[1].py = 0.0f;
  mesh.vertices[1].pz = 0.0f;
  mesh.vertices[2].px = 0.0f;
  mesh.vertices[2].py = 1.0f;
  mesh.vertices[2].pz = 0.0f;
  for (lusdview::DrawVertex& v : mesh.vertices) {
    v.nz = 1.0f;
  }
  mesh.indices = {0, 1, 2};
  lusdview::DrawSubmesh sub;
  sub.indexOffset = 0;
  sub.indexCount = 3;
  sub.materialId = 0;
  mesh.submeshes.push_back(sub);
  draw.meshes.push_back(mesh);

  lusdview::HostScene host;
  std::string err;
  if (!lusdview::BuildHostScene(draw, 0, 0, 0.0f, &host, &err)) {
    std::fprintf(stderr, "BuildHostScene failed: %s\n", err.c_str());
    return 1;
  }
  // Vulkan compute-BVH passes the interactive visibility and purpose masks to
  // the shared host builder. A hidden proxy must never enter its TLAS.
  lusdview::DrawMeshCPU proxy = mesh;
  proxy.name = "proxy-tri";
  proxy.purpose = "proxy";
  proxy.world[12] = 2.0f;
  draw.meshes.push_back(proxy);
  lusdview::HostScene filtered;
  if (!lusdview::BuildHostScene(draw, 0, 0, 0.0f, &filtered, &err) ||
      filtered.triCount != 1) {
    std::fprintf(stderr, "default RT purpose mask retained proxy geometry\n");
    return 1;
  }
  filtered = lusdview::HostScene{};
  if (!lusdview::BuildHostScene(draw, 0, 0, 0.0f, &filtered, &err,
                                nullptr, nullptr, 0, nullptr,
                                /*default + render + guide=*/0xbu) ||
      filtered.triCount != 1) {
    std::fprintf(stderr, "RT purpose filtering retained proxy geometry\n");
    return 1;
  }
  const std::vector<uint8_t> visible = {0, 1};
  filtered = lusdview::HostScene{};
  if (!lusdview::BuildHostScene(draw, 0, 0, 0.0f, &filtered, &err,
                                nullptr, nullptr, 0, &visible, 0xfu) ||
      filtered.triCount != 1) {
    std::fprintf(stderr, "RT mesh visibility filtering failed\n");
    return 1;
  }
  if (host.numLights != 2 ||
      host.lightParams.size() <
          2u * static_cast<size_t>(lusdview::kRtLightParamFloats)) {
    std::fprintf(stderr, "RT light buffer was not packed\n");
    return 1;
  }

  const float* lp = host.lightParams.data();
  const int keyFlags = static_cast<int>(lp[1] + 0.5f);
  if (!Near(lp[0], static_cast<float>(lusdview::DrawLightCPU::Type::Rect)) ||
      (keyFlags & (1 << 0)) == 0 || (keyFlags & (1 << 2)) == 0 ||
      (keyFlags & (1 << 3)) == 0 ||
      (keyFlags & (1 << 5)) != 0 ||
      !Near(lp[12], 0.25f) || !Near(lp[13], 0.5f) ||
      !Near(lp[14], 1.0f) || !Near(lp[15], 8.0f) ||
      !Near(lp[16], 2.0f) || !Near(lp[17], 4.0f) ||
      !Near(lp[18], 8.0f) || !Near(lp[19], 0.125f) ||
      !Near(lp[20], 2.0f) || !Near(lp[21], 4.0f) ||
      !Near(lp[23], 8.0f) || !Near(lp[24], 30.0f) ||
      !Near(lp[27], 1.25f) ||
      !Near(lp[32], 0.05f) || !Near(lp[33], 0.06f) ||
      !Near(lp[34], 0.07f) || !Near(lp[35], 12.0f) ||
      !Near(lp[36], 3.0f) || !Near(lp[37], 2.0f) ||
      !Near(lp[38], 1.0f)) {
    std::fprintf(stderr, "RT key light params were not packed from USD light\n");
    return 1;
  }

  const float* domeLp = lp + lusdview::kRtLightParamFloats;
  const int domeFlags = static_cast<int>(domeLp[1] + 0.5f);
  if (!Near(domeLp[0], static_cast<float>(lusdview::DrawLightCPU::Type::Dome)) ||
      (domeFlags & (1 << 4)) == 0 || !Near(domeLp[2], 0.0f)) {
    std::fprintf(stderr, "RT dome light envmap texture was not mapped\n");
    return 1;
  }

  // Rows 10-12: world->environment rotation (identity for the untransformed
  // dome). Rows 13-19: SH irradiance (bit 7 + non-zero coeffs when the IBL was
  // baked, i.e. when textools is available).
  if (!Near(domeLp[40], 1.0f) || !Near(domeLp[41], 0.0f) ||
      !Near(domeLp[42], 0.0f) || !Near(domeLp[45], 1.0f) ||
      !Near(domeLp[50], 1.0f)) {
    std::fprintf(stderr, "RT dome rotation rows are not identity\n");
    return 1;
  }
  if (lusdview::TexToolsAvailable()) {
    if ((domeFlags & (1 << 7)) == 0) {
      std::fprintf(stderr, "RT dome IBL-baked flag (bit 7) not set\n");
      return 1;
    }
    float shMag = 0.0f;
    for (int i = 0; i < 27; ++i) shMag += std::fabs(domeLp[52 + i]);
    if (!(shMag > 0.0f)) {
      std::fprintf(stderr, "RT dome SH irradiance rows are all zero\n");
      return 1;
    }
    if (!(domeLp[51] > 0.0f) || !std::isfinite(domeLp[51]) ||
        !(domeLp[79] > 0.0f) || !std::isfinite(domeLp[79]) ||
        domeLp[51] * 32.0f < domeLp[79]) {
      std::fprintf(stderr,
                   "RT dome environment proposal envelope is invalid\n");
      return 1;
    }
  }

  // Non-mesh carriers become camera-independent solid RT proxies: an
  // octahedron per point and a four-sided tube per tessellated curve segment.
  lusdview::DrawScene carrierScene;
  carrierScene.materials.push_back(lusdview::DrawMaterialCPU{});
  lusdview::DrawPointsCPU points;
  points.name = "point";
  points.points = {0.0f, 0.0f, 0.0f};
  points.widths = {2.0f};
  points.colors = {0.2f, 0.4f, 0.8f};
  points.opacities = {0.25f};
  points.materialId = 0;
  Identity(points.world);
  carrierScene.points.push_back(points);
  lusdview::DrawCurvesCPU curves;
  curves.name = "curve";
  curves.vertexCounts = {2};
  curves.points = {0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f};
  curves.widths = {0.5f};
  curves.colors = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
  curves.opacities = {0.2f, 0.6f};
  curves.materialId = 0;
  Identity(curves.world);
  carrierScene.curves.push_back(curves);
  lusdview::HostScene carrierHost;
  if (!lusdview::BuildHostScene(carrierScene, 0, 0, 0.0f, &carrierHost,
                                &err) ||
      carrierHost.triCount != 48 || carrierHost.instCount != 2) {
    std::fprintf(stderr, "non-mesh RT proxy build failed: %s (%zu tris, %zu inst)\n",
                 err.c_str(), carrierHost.triCount, carrierHost.instCount);
    return 1;
  }
  bool foundPointOpacity = false;
  bool foundCurveOpacity = false;
  for (size_t i = 3; i < carrierHost.cols.size(); i += 4) {
    foundPointOpacity |= Near(carrierHost.cols[i], 0.25f);
    foundCurveOpacity |= Near(carrierHost.cols[i], 0.4f);
  }
  if (!foundPointOpacity || !foundCurveOpacity) {
    std::fprintf(stderr,
                 "non-mesh displayOpacity was not retained in RT proxies\n");
    return 1;
  }

  // Light-specific diagnostics retain authored feature identity alongside the
  // generic skipped count, so unresolved GeometryLight/IES paths are
  // actionable without losing the PortalLight and emissive-mesh inventory.
  lusdview::DrawScene diagnosticScene;
  lusdview::DrawLightCPU diagnosticGeometry;
  diagnosticGeometry.type = lusdview::DrawLightCPU::Type::Geometry;
  diagnosticGeometry.geometryMesh = 0;
  diagnosticScene.lights.push_back(diagnosticGeometry);
  lusdview::DrawLightCPU diagnosticPortal;
  diagnosticPortal.type = lusdview::DrawLightCPU::Type::Portal;
  diagnosticScene.lights.push_back(diagnosticPortal);
  lusdview::DrawLightCPU diagnosticIes;
  diagnosticIes.shapingIesFile = "fixture.ies";
  diagnosticIes.iesValid = true;
  diagnosticScene.lights.push_back(diagnosticIes);
  lusdview::LoadDiagnostics lightDiagnostics =
      lusdview::CategorizeLoadWarnings(
          "", {"GeometryLight '/World/Emitter': emissive-mesh target could "
               "not be resolved",
               "Light '/World/Key': IES profile unavailable (missing file)"});
  lusdview::AddLightDiagnostics(diagnosticScene, &lightDiagnostics);
  if (lightDiagnostics.skipped != 2 || lightDiagnostics.geometry_lights != 1 ||
      lightDiagnostics.portal_lights != 1 || lightDiagnostics.ies_profiles != 1 ||
      lightDiagnostics.emissive_mesh_lights != 1) {
    std::fprintf(stderr, "light-specific diagnostics were not classified\n");
    return 1;
  }

  return 0;
}
