// SPDX-License-Identifier: Apache-2.0
// Backend-neutral bounded direct-light records for real-time raster shading.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "gpu_scene.hh"
#include "lighting_ies.hh"
#include "light3d/math.h"

namespace lusdview {

constexpr int kMaxRasterLights = 16;

// Thirteen vec4s per light. This layout is deliberately friendly to both GLSL
// uniform arrays (OpenGL) and std140 Frame UBO arrays (Vulkan). The last six
// rows are the canonical 6x4 IES LUT at horizontal angles 0..300 degrees.
struct alignas(16) RasterLightGPU {
  float positionType[4]{};   // xyz position, w DrawLightCPU::Type
  float directionAngle[4]{}; // xyz direction, w shaping cone angle in degrees
  float colorDiffuse[4]{};   // rgb normalized radiance, w diffuse multiplier
  float specularShape[4]{};  // x specular, y cone softness, z focus, w hasShaping
  float areaParams[4]{};     // radius, width, height, length
  float iesAxisX[4]{};       // authored photometric +X axis, xyz
  float iesAxisY[4]{};       // authored photometric +Y axis, xyz
  float iesProfile[24]{};    // indexed vertical*6 + horizontal
};
static_assert(sizeof(RasterLightGPU) == 52 * sizeof(float),
              "RasterLightGPU must be thirteen tightly packed vec4s");

struct RasterLightSet {
  std::array<RasterLightGPU, kMaxRasterLights> lights{};
  int count{0};
  int truncated{0};
  // GeometryLight requires triangle sampling. Raster backends omit it until a
  // mesh-sample payload is available instead of treating its transform origin
  // as a point emitter.
  int omittedGeometryLights{0};
  // One 16-bit stage-order direct-light mask per mesh. A set bit means that
  // light's collection includes the mesh. Empty when meshCount == 0.
  std::vector<uint32_t> meshMasks;
  // Equivalent collection mask for shadow visibility. This is intentionally
  // separate: USD permits a mesh to receive a light while being excluded from
  // that light's shadow collection (and vice versa).
  std::vector<uint32_t> shadowMeshMasks;
  std::vector<std::pair<std::string, uint32_t>> carrierPathMasks;
  uint32_t carrierExplicitMask{0u};
  std::vector<std::pair<std::string, uint32_t>> shadowCarrierPathMasks;
  uint32_t shadowCarrierExplicitMask{0u};
  // Direct-light slot selected for the single deterministic raster shadow map.
  // Prefer the first enabled distant light; otherwise use the first enabled
  // finite light supported by the single-map approximation.
  int shadowLightSlot{-1};
};

inline void PrepareRasterGeometryLightSamples(
    std::vector<DrawLightCPU>* lights, const std::vector<DrawMeshCPU>& meshes) {
  if (!lights) return;
  auto transformPoint = [](const DrawMeshCPU& mesh, const DrawVertex& v,
                           float out[3]) {
    if (!mesh.instanceXforms.empty()) {
      const float* m = mesh.instanceXforms.data();
      out[0] = m[0] * v.px + m[1] * v.py + m[2] * v.pz + m[3];
      out[1] = m[4] * v.px + m[5] * v.py + m[6] * v.pz + m[7];
      out[2] = m[8] * v.px + m[9] * v.py + m[10] * v.pz + m[11];
    } else {
      out[0] = mesh.world[0] * v.px + mesh.world[4] * v.py +
               mesh.world[8] * v.pz + mesh.world[12];
      out[1] = mesh.world[1] * v.px + mesh.world[5] * v.py +
               mesh.world[9] * v.pz + mesh.world[13];
      out[2] = mesh.world[2] * v.px + mesh.world[6] * v.py +
               mesh.world[10] * v.pz + mesh.world[14];
    }
  };
  auto radicalInverse2 = [](unsigned value) {
    value = ((value & 0x55555555u) << 1u) |
            ((value & 0xaaaaaaaau) >> 1u);
    value = ((value & 0x33333333u) << 2u) |
            ((value & 0xccccccccu) >> 2u);
    value = ((value & 0x0f0f0f0fu) << 4u) |
            ((value & 0xf0f0f0f0u) >> 4u);
    value = ((value & 0x00ff00ffu) << 8u) |
            ((value & 0xff00ff00u) >> 8u);
    value = (value << 16u) | (value >> 16u);
    return static_cast<float>(value) * 2.3283064365386963e-10f;
  };

  for (DrawLightCPU& light : *lights) {
    light.geometryRasterSampleCount = 0;
    if (light.type != DrawLightCPU::Type::Geometry || light.geometryMesh < 0 ||
        static_cast<size_t>(light.geometryMesh) >= meshes.size())
      continue;
    const DrawMeshCPU& mesh = meshes[static_cast<size_t>(light.geometryMesh)];
    const size_t triangleCount = mesh.indices.size() / 3;
    if (triangleCount == 0 || mesh.vertices.empty()) continue;

    std::vector<float> cumulativeArea(triangleCount, 0.0f);
    float totalArea = 0.0f;
    for (size_t triangle = 0; triangle < triangleCount; ++triangle) {
      const uint32_t ia = mesh.indices[triangle * 3 + 0];
      const uint32_t ib = mesh.indices[triangle * 3 + 1];
      const uint32_t ic = mesh.indices[triangle * 3 + 2];
      if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() ||
          ic >= mesh.vertices.size()) {
        cumulativeArea[triangle] = totalArea;
        continue;
      }
      float a[3], b[3], c[3];
      transformPoint(mesh, mesh.vertices[ia], a);
      transformPoint(mesh, mesh.vertices[ib], b);
      transformPoint(mesh, mesh.vertices[ic], c);
      const float ab[3]{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
      const float ac[3]{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const float cross[3]{ab[1] * ac[2] - ab[2] * ac[1],
                           ab[2] * ac[0] - ab[0] * ac[2],
                           ab[0] * ac[1] - ab[1] * ac[0]};
      totalArea += 0.5f * std::sqrt(cross[0] * cross[0] +
                                   cross[1] * cross[1] +
                                   cross[2] * cross[2]);
      cumulativeArea[triangle] = totalArea;
    }
    if (!(totalArea > 1.0e-12f)) continue;

    for (int sample = 0; sample < 8; ++sample) {
      const float target = totalArea * (static_cast<float>(sample) + 0.5f) / 8.0f;
      const auto found = std::lower_bound(cumulativeArea.begin(),
                                          cumulativeArea.end(), target);
      const size_t triangle = static_cast<size_t>(
          found == cumulativeArea.end() ? cumulativeArea.size() - 1
                                        : found - cumulativeArea.begin());
      const uint32_t ia = mesh.indices[triangle * 3 + 0];
      const uint32_t ib = mesh.indices[triangle * 3 + 1];
      const uint32_t ic = mesh.indices[triangle * 3 + 2];
      if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() ||
          ic >= mesh.vertices.size())
        continue;
      float a[3], b[3], c[3];
      transformPoint(mesh, mesh.vertices[ia], a);
      transformPoint(mesh, mesh.vertices[ib], b);
      transformPoint(mesh, mesh.vertices[ic], c);
      const float root = std::sqrt((static_cast<float>(sample) + 0.5f) / 8.0f);
      const float r2 = radicalInverse2(static_cast<unsigned>(sample));
      const float weights[3]{1.0f - root, root * (1.0f - r2), root * r2};
      for (int axis = 0; axis < 3; ++axis) {
        light.geometryRasterSamples[sample * 3 + axis] =
            weights[0] * a[axis] + weights[1] * b[axis] + weights[2] * c[axis];
      }
      ++light.geometryRasterSampleCount;
    }
  }
}

inline bool IsRasterDirectLight(const DrawLightCPU& light) {
  return light.type != DrawLightCPU::Type::Dome &&
         (light.type != DrawLightCPU::Type::Geometry ||
          light.geometryRasterSampleCount == 8);
}

inline bool IsRasterFiniteShadowLight(const DrawLightCPU& light) {
  return light.hasShaping || light.type == DrawLightCPU::Type::Point ||
         light.type == DrawLightCPU::Type::Sphere ||
         light.type == DrawLightCPU::Type::Disk ||
         light.type == DrawLightCPU::Type::Rect ||
         light.type == DrawLightCPU::Type::Cylinder;
}

inline RasterLightSet PackRasterLights(const std::vector<DrawLightCPU>& src,
                                       size_t meshCount) {
  RasterLightSet out;
  int finiteFallback = -1;
  out.meshMasks.assign(meshCount, 0u);
  out.shadowMeshMasks.assign(meshCount, 0u);
  for (const DrawLightCPU& light : src) {
    if (light.type == DrawLightCPU::Type::Geometry &&
        light.geometryRasterSampleCount != 8) {
      ++out.omittedGeometryLights;
      continue;
    }
    if (!IsRasterDirectLight(light)) continue;
    if (out.count >= kMaxRasterLights) {
      ++out.truncated;
      continue;
    }
    const int slot = out.count++;
    if (light.shadowEnable) {
      if (light.type == DrawLightCPU::Type::Distant &&
          out.shadowLightSlot < 0) {
        out.shadowLightSlot = slot;
      } else if (IsRasterFiniteShadowLight(light) && finiteFallback < 0) {
        finiteFallback = slot;
      }
    }
    RasterLightGPU& dst = out.lights[static_cast<size_t>(slot)];
    for (int i = 0; i < 3; ++i) {
      dst.positionType[i] = light.position[i];
      // DrawLightCPU stores the emission direction. Distant-light shading needs
      // the opposite vector (surface -> light); finite-light shaping needs the
      // authored emission direction as-is.
      dst.directionAngle[i] =
          light.type == DrawLightCPU::Type::Distant ? -light.direction[i]
                                                     : light.direction[i];
      dst.colorDiffuse[i] = light.normalizedColor[i];
    }
    dst.positionType[3] = static_cast<float>(light.type);
    dst.directionAngle[3] = light.shapingConeAngle;
    dst.colorDiffuse[3] = light.diffuse;
    dst.specularShape[0] = light.specular;
    dst.specularShape[1] = light.shapingConeSoftness;
    dst.specularShape[2] = light.shapingFocus;
    dst.specularShape[3] = light.hasShaping ? 1.0f : 0.0f;
    dst.areaParams[0] = std::max(light.radius, 0.0f);
    dst.areaParams[1] = std::max(light.width, 0.0f);
    dst.areaParams[2] = std::max(light.height, 0.0f);
    dst.areaParams[3] = std::max(light.length, 0.0f);
    dst.iesAxisX[0] = light.transform[0];
    dst.iesAxisX[1] = light.transform[1];
    dst.iesAxisX[2] = light.transform[2];
    dst.iesAxisY[0] = light.transform[4];
    dst.iesAxisY[1] = light.transform[5];
    dst.iesAxisY[2] = light.transform[6];
    const float axisXLen = std::sqrt(dst.iesAxisX[0] * dst.iesAxisX[0] +
                                     dst.iesAxisX[1] * dst.iesAxisX[1] +
                                     dst.iesAxisX[2] * dst.iesAxisX[2]);
    const float axisYLen = std::sqrt(dst.iesAxisY[0] * dst.iesAxisY[0] +
                                     dst.iesAxisY[1] * dst.iesAxisY[1] +
                                     dst.iesAxisY[2] * dst.iesAxisY[2]);
    if (axisXLen > 1.0e-6f && axisYLen > 1.0e-6f) {
      for (int i = 0; i < 3; ++i) {
        dst.iesAxisX[i] /= axisXLen;
        dst.iesAxisY[i] /= axisYLen;
      }
    } else {
      dst.iesAxisX[0] = 1.0f;
      dst.iesAxisX[1] = dst.iesAxisX[2] = 0.0f;
      dst.iesAxisY[0] = 0.0f;
      dst.iesAxisY[1] = 1.0f;
      dst.iesAxisY[2] = 0.0f;
    }
    if (light.iesValid) {
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 6; ++x) {
          dst.iesProfile[y * 6 + x] = EvaluateIesProfile(
              light, 60.0f * static_cast<float>(y),
              60.0f * static_cast<float>(x));
        }
      }
    }
    if (light.type == DrawLightCPU::Type::Geometry &&
        light.geometryRasterSampleCount == 8) {
      std::copy_n(light.geometryRasterSamples, 24, dst.iesProfile);
    }

    const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(slot);
    if (light.lightLinksAll) {
      for (uint32_t& mask : out.meshMasks) mask |= bit;
    } else {
      for (int meshIndex : light.lightLinkMeshIndices) {
        if (meshIndex >= 0 && static_cast<size_t>(meshIndex) < meshCount)
          out.meshMasks[static_cast<size_t>(meshIndex)] |= bit;
      }
    }
    if (light.shadowLinksAll) {
      for (uint32_t& mask : out.shadowMeshMasks) mask |= bit;
    } else {
      for (int meshIndex : light.shadowLinkMeshIndices) {
        if (meshIndex >= 0 && static_cast<size_t>(meshIndex) < meshCount)
          out.shadowMeshMasks[static_cast<size_t>(meshIndex)] |= bit;
      }
    }
    if (!light.lightLinksAll) {
      out.carrierExplicitMask |= bit;
      for (const std::string& path : light.lightLinkPaths) {
        if (!path.empty()) out.carrierPathMasks.emplace_back(path, bit);
      }
    }
    if (!light.shadowLinksAll) {
      out.shadowCarrierExplicitMask |= bit;
      for (const std::string& path : light.shadowLinkPaths) {
        if (!path.empty()) out.shadowCarrierPathMasks.emplace_back(path, bit);
      }
    }
  }
  if (out.shadowLightSlot < 0) out.shadowLightSlot = finiteFallback;
  return out;
}

inline uint32_t RasterShadowMaskForPath(const RasterLightSet& lights,
                                        const std::string& carrierPath) {
  uint32_t mask = 0u;
  for (const auto& entry : lights.shadowCarrierPathMasks) {
    const std::string& root = entry.first;
    if (carrierPath == root ||
        (carrierPath.size() > root.size() &&
         carrierPath.compare(0, root.size(), root) == 0 &&
         carrierPath[root.size()] == '/'))
      mask |= entry.second;
  }
  for (int i = 0; i < lights.count; ++i) {
    const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(i);
    if ((lights.shadowCarrierExplicitMask & bit) == 0u) mask |= bit;
  }
  return mask;
}

inline bool RasterShadowIncludesPath(const RasterLightSet& lights,
                                     const std::string& carrierPath) {
  if (lights.shadowLightSlot < 0) return false;
  const uint32_t bit = uint32_t{1} <<
      static_cast<uint32_t>(lights.shadowLightSlot);
  return (RasterShadowMaskForPath(lights, carrierPath) & bit) != 0u;
}

inline uint32_t RasterLightMaskForPath(const RasterLightSet& lights,
                                       const std::string& carrierPath) {
  uint32_t mask = 0u;
  for (const auto& entry : lights.carrierPathMasks) {
    const std::string& root = entry.first;
    if (carrierPath == root ||
        (carrierPath.size() > root.size() &&
         carrierPath.compare(0, root.size(), root) == 0 &&
         carrierPath[root.size()] == '/'))
      mask |= entry.second;
  }
  for (int i = 0; i < lights.count; ++i) {
    const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(i);
    if ((lights.carrierExplicitMask & bit) == 0u) mask |= bit;
  }
  return mask;
}

inline bool RasterShadowIncludesMesh(const RasterLightSet& lights,
                                     int meshIndex) {
  if (lights.shadowLightSlot < 0) return false;
  if (meshIndex < 0 ||
      static_cast<size_t>(meshIndex) >= lights.shadowMeshMasks.size())
    return true;
  const uint32_t bit = uint32_t{1} <<
      static_cast<uint32_t>(lights.shadowLightSlot);
  return (lights.shadowMeshMasks[static_cast<size_t>(meshIndex)] & bit) != 0u;
}

inline uint32_t RasterLightMaskForMesh(const RasterLightSet& lights,
                                       int meshIndex) {
  if (meshIndex >= 0 && static_cast<size_t>(meshIndex) < lights.meshMasks.size())
    return lights.meshMasks[static_cast<size_t>(meshIndex)];
  return lights.count >= 32 ? ~uint32_t{0}
                            : ((uint32_t{1} << lights.count) - uint32_t{1});
}

struct RasterShadowCamera {
  light3d::Mat4 viewProj;
  light3d::Vec3 eye{0.0f, 0.0f, 0.0f};
  light3d::Vec3 right{1.0f, 0.0f, 0.0f};
  light3d::Vec3 up{0.0f, 1.0f, 0.0f};
  int lightSlot{-1};
  float nearPlane{0.01f};
  float farPlane{1.0f};
  bool perspective{false};
};

// The six world->clip transforms used for an exact point-light cube shadow.
// Face order follows the OpenGL/Vulkan cube convention: +X, -X, +Y, -Y, +Z,
// -Z. Both raster backends consume this shared order so depth rendering and
// sampling cannot silently disagree about a face's orientation.
struct RasterPointShadowCameras {
  std::array<light3d::Mat4, 6> viewProj{};
  light3d::Vec3 eye{0.0f, 0.0f, 0.0f};
  std::array<light3d::Vec3, 6> right{};
  std::array<light3d::Vec3, 6> up{};
  int lightSlot{-1};
  float nearPlane{0.01f};
  float farPlane{1.0f};
};

inline light3d::Mat4 ShadowOrtho(float left, float right, float bottom,
                                 float top, float nearPlane, float farPlane,
                                 bool zeroToOne) {
  light3d::Mat4 p{};
  p.m[0] = 2.0f / (right - left);
  p.m[5] = 2.0f / (top - bottom);
  p.m[10] = (zeroToOne ? -1.0f : -2.0f) / (farPlane - nearPlane);
  p.m[12] = -(right + left) / (right - left);
  p.m[13] = -(top + bottom) / (top - bottom);
  p.m[14] = zeroToOne ? -nearPlane / (farPlane - nearPlane)
                      : -(farPlane + nearPlane) / (farPlane - nearPlane);
  p.m[15] = 1.0f;
  return p;
}

inline bool BuildRasterPointShadowCameras(const RasterLightSet& lights,
                                          const float sceneMin[3],
                                          const float sceneExtent[3],
                                          bool zeroToOne,
                                          RasterPointShadowCameras* out) {
  if (!out || lights.shadowLightSlot < 0 ||
      lights.shadowLightSlot >= lights.count) return false;
  const RasterLightGPU& light =
      lights.lights[static_cast<size_t>(lights.shadowLightSlot)];
  const int type = static_cast<int>(light.positionType[3] + 0.5f);
  // Raster direct lighting uses deterministic four-point area samples. Reuse
  // the omnidirectional cube-shadow path for analytic area lights too; this
  // gives them coverage while retaining a single center-based shadow map.
  if (type != static_cast<int>(DrawLightCPU::Type::Point) &&
      type != static_cast<int>(DrawLightCPU::Type::Sphere) &&
      type != static_cast<int>(DrawLightCPU::Type::Disk) &&
      type != static_cast<int>(DrawLightCPU::Type::Rect) &&
      type != static_cast<int>(DrawLightCPU::Type::Cylinder) &&
      type != static_cast<int>(DrawLightCPU::Type::Geometry) &&
      type != static_cast<int>(DrawLightCPU::Type::Portal)) {
    return false;
  }
  const light3d::Vec3 eye{light.positionType[0], light.positionType[1],
                           light.positionType[2]};
  float farPlane = 0.01f;
  for (int corner = 0; corner < 8; ++corner) {
    const light3d::Vec3 p{
        sceneMin[0] + ((corner & 1) ? sceneExtent[0] : 0.0f),
        sceneMin[1] + ((corner & 2) ? sceneExtent[1] : 0.0f),
        sceneMin[2] + ((corner & 4) ? sceneExtent[2] : 0.0f)};
    farPlane = std::max(farPlane, light3d::length(p - eye));
  }
  // Guard band keeps PCF samples at the scene edge within the depth range.
  farPlane = std::max(0.02f, farPlane * 1.01f);
  const float nearPlane = std::max(0.001f, std::min(0.01f, farPlane * 0.01f));
  constexpr float kQuarterTurn = 1.57079632679489661923f;
  const light3d::Mat4 proj = zeroToOne
      ? light3d::perspectiveZeroOne(kQuarterTurn, 1.0f, nearPlane, farPlane)
      : light3d::perspective(kQuarterTurn, 1.0f, nearPlane, farPlane);
  const std::array<light3d::Vec3, 6> directions{{
      { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
      { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
      { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
  }};
  const std::array<light3d::Vec3, 6> ups{{
      {0.0f, -1.0f,  0.0f}, {0.0f, -1.0f,  0.0f},
      {0.0f,  0.0f,  1.0f}, {0.0f,  0.0f, -1.0f},
      {0.0f, -1.0f,  0.0f}, {0.0f, -1.0f,  0.0f},
  }};
  for (size_t face = 0; face < directions.size(); ++face) {
    out->viewProj[face] = proj * light3d::lookAt(
        eye, eye + directions[face], ups[face]);
    out->right[face] = light3d::normalize(light3d::cross(
        directions[face], ups[face]));
    out->up[face] = ups[face];
  }
  out->eye = eye;
  out->lightSlot = lights.shadowLightSlot;
  out->nearPlane = nearPlane;
  out->farPlane = farPlane;
  return true;
}

// Fit the one supported shadow camera to the scene bounds. `zeroToOne` selects
// Vulkan depth convention; GL uses the default -1..1 convention.
inline bool BuildRasterShadowCamera(const RasterLightSet& lights,
                                    const float sceneMin[3],
                                    const float sceneExtent[3], bool zeroToOne,
                                    RasterShadowCamera* out) {
  if (!out || lights.shadowLightSlot < 0 ||
      lights.shadowLightSlot >= lights.count) return false;
  const RasterLightGPU& light =
      lights.lights[static_cast<size_t>(lights.shadowLightSlot)];
  const int type = static_cast<int>(light.positionType[3] + 0.5f);
  const light3d::Vec3 center{
      sceneMin[0] + sceneExtent[0] * 0.5f,
      sceneMin[1] + sceneExtent[1] * 0.5f,
      sceneMin[2] + sceneExtent[2] * 0.5f};
  const float radius = std::max(
      0.01f, 0.5f * std::sqrt(sceneExtent[0] * sceneExtent[0] +
                              sceneExtent[1] * sceneExtent[1] +
                              sceneExtent[2] * sceneExtent[2]));
  light3d::Vec3 direction{light.directionAngle[0], light.directionAngle[1],
                         light.directionAngle[2]};
  if (light3d::length(direction) < 1.0e-6f) direction = {0.0f, -1.0f, 0.0f};
  direction = light3d::normalize(direction);
  const light3d::Vec3 up = std::fabs(direction.y) > 0.99f
                               ? light3d::Vec3{1.0f, 0.0f, 0.0f}
                               : light3d::Vec3{0.0f, 1.0f, 0.0f};
  light3d::Mat4 view;
  light3d::Mat4 proj;
  float nearPlane = 0.01f;
  float farPlane = radius * 4.0f;
  bool perspective = false;
  light3d::Vec3 eye;
  light3d::Vec3 shadowUp = up;
  if (type == static_cast<int>(DrawLightCPU::Type::Distant)) {
    eye = center + direction * (radius * 2.0f);
    view = light3d::lookAt(eye, center, up);
    // A square scene-enclosing volume is stable under light rotation and leaves
    // a small guard band for PCF taps at the fitted edge.
    const float span = radius * 1.05f;
    nearPlane = std::max(0.01f, radius * 0.95f);
    farPlane = radius * 3.05f;
    proj = ShadowOrtho(-span, span, -span, span, nearPlane, farPlane,
                       zeroToOne);
  } else if (light.specularShape[3] > 0.5f ||
             (type >= static_cast<int>(DrawLightCPU::Type::Point) &&
              type <= static_cast<int>(DrawLightCPU::Type::Cylinder))) {
    eye = {light.positionType[0], light.positionType[1], light.positionType[2]};
    const bool unshapedFinite = light.specularShape[3] <= 0.5f;
    if (unshapedFinite && type != static_cast<int>(DrawLightCPU::Type::Rect) &&
        type != static_cast<int>(DrawLightCPU::Type::Disk)) {
      const light3d::Vec3 towardScene = center - eye;
      if (light3d::length(towardScene) > 1.0e-6f) direction =
          light3d::normalize(towardScene);
      else direction = {0.0f, -1.0f, 0.0f};
    }
    shadowUp = std::fabs(direction.y) > 0.99f
                   ? light3d::Vec3{1.0f, 0.0f, 0.0f}
                   : light3d::Vec3{0.0f, 1.0f, 0.0f};
    view = light3d::lookAt(eye, eye + direction, shadowUp);
    farPlane = std::max(radius * 4.0f, light3d::length(center - eye) + radius);
    float nearestForward = farPlane;
    bool boundsBehindLight = false;
    for (int corner = 0; corner < 8; ++corner) {
      const light3d::Vec3 p{
          sceneMin[0] + ((corner & 1) ? sceneExtent[0] : 0.0f),
          sceneMin[1] + ((corner & 2) ? sceneExtent[1] : 0.0f),
          sceneMin[2] + ((corner & 4) ? sceneExtent[2] : 0.0f)};
      const float forward = light3d::dot(p - eye, direction);
      if (forward <= 0.0f) boundsBehindLight = true;
      else nearestForward = std::min(nearestForward, forward);
    }
    nearPlane = boundsBehindLight ? 0.01f
                                  : std::max(0.01f, nearestForward * 0.5f);
    if (unshapedFinite) {
      const float span = radius * 1.05f;
      proj = ShadowOrtho(-span, span, -span, span, nearPlane, farPlane,
                         zeroToOne);
    } else {
      perspective = true;
      const float halfAngle = std::max(
          0.5f, std::min(light.directionAngle[3], 89.0f));
      constexpr float kDegToRad = 0.01745329251994329577f;
      proj = zeroToOne
                 ? light3d::perspectiveZeroOne(2.0f * halfAngle * kDegToRad,
                                               1.0f, nearPlane, farPlane)
                 : light3d::perspective(2.0f * halfAngle * kDegToRad, 1.0f,
                                        nearPlane, farPlane);
    }
  } else {
    return false;
  }
  out->viewProj = proj * view;
  out->eye = eye;
  out->right = light3d::normalize(light3d::cross(direction, shadowUp));
  out->up = shadowUp;
  out->lightSlot = lights.shadowLightSlot;
  out->nearPlane = nearPlane;
  out->farPlane = farPlane;
  out->perspective = perspective;
  return true;
}

}  // namespace lusdview
