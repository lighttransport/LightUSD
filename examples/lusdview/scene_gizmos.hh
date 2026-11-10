// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

#include "gpu_scene.hh"

namespace lusdview {
// Emit world-space, backend-neutral line segments from the retained core
// records. Clip distances are deliberately bounded by the scene-sized display
// length.
template <class Emit>
void AppendSceneGizmos(const DrawScene& scene, bool lights, bool cameras,
                       float displayLength, Emit emit,
                       size_t maxSegments = 65536) {
  size_t emitted = 0;
  if (!std::isfinite(displayLength) || displayLength <= 0) return;
  struct V {
    float x, y, z;
  };
  const auto add = [](V a, V b) { return V{a.x + b.x, a.y + b.y, a.z + b.z}; };
  const auto mul = [](V a, float b) { return V{a.x * b, a.y * b, a.z * b}; };
  const auto cross = [](V a, V b) {
    return V{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x};
  };
  const auto unit = [&](V a, V fallback) {
    const float n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return n > 1e-8f && std::isfinite(n) ? mul(a, 1 / n) : fallback;
  };
  const auto line = [&](V a, V b, float r, float g, float blue) {
    if (emitted >= maxSegments) return;
    for (float v : {a.x, a.y, a.z, b.x, b.y, b.z})
      if (!std::isfinite(v)) return;
    ++emitted;
    emit(a.x, a.y, a.z, b.x, b.y, b.z, r, g, blue);
  };
  if (lights)
    for (const auto& light : scene.lights) {
      if (emitted >= maxSegments) break;
      const V p{light.position[0], light.position[1], light.position[2]};
      const float s = displayLength * 0.15f;
      line(add(p, {-s, 0, 0}), add(p, {s, 0, 0}), 1, 0.85f, 0.15f);
      line(add(p, {0, -s, 0}), add(p, {0, s, 0}), 1, 0.85f, 0.15f);
      line(add(p, {0, 0, -s}), add(p, {0, 0, s}), 1, 0.85f, 0.15f);
      if (light.type == DrawLightCPU::Type::Dome) continue;
      const V d =
          unit({light.direction[0], light.direction[1], light.direction[2]},
               {0, -1, 0});
      const V right = unit(
          cross(d, std::fabs(d.y) < 0.9f ? V{0, 1, 0} : V{1, 0, 0}), {1, 0, 0});
      const V tip = add(p, mul(d, displayLength));
      if (light.type == DrawLightCPU::Type::Distant ||
          light.type == DrawLightCPU::Type::Rect ||
          light.type == DrawLightCPU::Type::Disk ||
          light.type == DrawLightCPU::Type::Portal || light.hasShaping) {
        line(p, tip, 1, 0.85f, 0.15f);
        line(tip, add(add(p, mul(d, displayLength * 0.7f)), mul(right, s)), 1,
             0.85f, 0.15f);
        line(tip, add(add(p, mul(d, displayLength * 0.7f)), mul(right, -s)), 1,
             0.85f, 0.15f);
      }
      // Area emitters retain the authored transform, including nonuniform
      // scale.
      if (light.type == DrawLightCPU::Type::Rect ||
          light.type == DrawLightCPU::Type::Portal) {
        V u{light.transform[0], light.transform[1], light.transform[2]};
        V v{light.transform[4], light.transform[5], light.transform[6]};
        u = mul(u, light.width * 0.5f);
        v = mul(v, light.height * 0.5f);
        const V corners[] = {add(add(p, u), v), add(add(p, mul(u, -1)), v),
                             add(add(p, mul(u, -1)), mul(v, -1)),
                             add(add(p, u), mul(v, -1))};
        for (int i = 0; i < 4; ++i)
          line(corners[i], corners[(i + 1) % 4], 1, 0.85f, 0.15f);
      } else if (light.type == DrawLightCPU::Type::Disk ||
                 light.type == DrawLightCPU::Type::Sphere ||
                 light.type == DrawLightCPU::Type::Cylinder) {
        const float radius =
            std::isfinite(light.radius) ? std::max(light.radius, 0.0f) : 0;
        const V x{light.transform[0], light.transform[1], light.transform[2]};
        const V y{light.transform[4], light.transform[5], light.transform[6]};
        const V z{light.transform[8], light.transform[9], light.transform[10]};
        const auto circle = [&](V center, V u, V v) {
          for (int i = 0; i < 32; ++i) {
            const float a = float(i) * 6.28318530718f / 32,
                        b = float(i + 1) * 6.28318530718f / 32;
            line(add(center, add(mul(u, radius * std::cos(a)),
                                 mul(v, radius * std::sin(a)))),
                 add(center, add(mul(u, radius * std::cos(b)),
                                 mul(v, radius * std::sin(b)))),
                 1, 0.85f, 0.15f);
          }
        };
        if (light.type == DrawLightCPU::Type::Cylinder) {
          const V offset = mul(x, light.length * 0.5f);
          circle(add(p, offset), y, z);
          circle(add(p, mul(offset, -1)), y, z);
          for (V r : {mul(y, radius), mul(y, -radius), mul(z, radius),
                      mul(z, -radius)})
            line(add(add(p, r), offset), add(add(p, r), mul(offset, -1)), 1,
                 0.85f, 0.15f);
        } else {
          circle(p, x, y);
          if (light.type == DrawLightCPU::Type::Sphere) {
            circle(p, x, z);
            circle(p, y, z);
          }
        }
      }
    }
  if (cameras)
    for (const auto& camera : scene.cameras) {
      if (emitted >= maxSegments) break;
      const V p{camera.eye[0], camera.eye[1], camera.eye[2]};
      const V d =
          unit({camera.forward[0], camera.forward[1], camera.forward[2]},
               {0, 0, -1});
      const V right =
          unit(cross(d, {camera.up[0], camera.up[1], camera.up[2]}),
               unit(cross(d, std::fabs(d.y) < 0.9f ? V{0, 1, 0} : V{1, 0, 0}),
                    {1, 0, 0}));
      const V up = cross(right, d);
      if (!std::isfinite(camera.horizontalAperture) ||
          !std::isfinite(camera.verticalAperture) ||
          camera.horizontalAperture <= 0 || camera.verticalAperture <= 0)
        continue;
      const bool ortho =
          camera.projection == DrawCameraCPU::Projection::Orthographic;
      if (!ortho &&
          (!std::isfinite(camera.focalLength) || camera.focalLength <= 0))
        continue;
      const float factor = ortho ? 0.1f : displayLength / camera.focalLength;
      const V center =
          add(add(add(p, mul(d, displayLength)),
                  mul(right, camera.horizontalApertureOffset * factor)),
              mul(up, camera.verticalApertureOffset * factor));
      const V u = mul(right, camera.horizontalAperture * factor * 0.5f),
              v = mul(up, camera.verticalAperture * factor * 0.5f);
      const V corners[] = {add(add(center, u), v),
                           add(add(center, mul(u, -1)), v),
                           add(add(center, mul(u, -1)), mul(v, -1)),
                           add(add(center, u), mul(v, -1))};
      for (int i = 0; i < 4; ++i) {
        line(corners[i], corners[(i + 1) % 4], 0.3f, 0.8f, 1);
        line(ortho ? add(corners[i], mul(d, -displayLength)) : p, corners[i],
             0.3f, 0.8f, 1);
      }
    }
}
}  // namespace lusdview
