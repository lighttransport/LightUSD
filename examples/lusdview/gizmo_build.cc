// SPDX-License-Identifier: Apache-2.0
#include "gizmo_build.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "light3d/math.h"

namespace lusdview {

namespace {

// Column-major float[16] -> light3d::Mat4
light3d::Mat4 ColMajorToMat4(const float m[16]) {
  light3d::Mat4 out;
  std::memcpy(out.m, m, sizeof(float) * 16);
  return out;
}

light3d::Vec3 XformPoint(const float world[16], light3d::Vec3 p) {
  light3d::Mat4 W = ColMajorToMat4(world);
  return light3d::transformPoint(W, p);
}

// Append a line segment between two world-space points.
void AddLine(std::vector<HelperVertex>& out,
             float ax, float ay, float az,
             float bx, float by, float bz,
             float cr, float cg, float cb) {
  out.push_back(HelperVertex{{ax, ay, az}, {cr, cg, cb}});
  out.push_back(HelperVertex{{bx, by, bz}, {cr, cg, cb}});
}

// Append a line between two light3d::Vec3 points.
void AddLineV(std::vector<HelperVertex>& out,
              const light3d::Vec3& a, const light3d::Vec3& b,
              float cr, float cg, float cb) {
  AddLine(out, a.x, a.y, a.z, b.x, b.y, b.z, cr, cg, cb);
}

// Generate a wireframe circle in the XY plane (Z=0).
void AddCircle(std::vector<HelperVertex>& out, float radius, int segCount,
               float cr, float cg, float cb,
               const float world[16]) {
  const float twoPi = 6.2831853f;
  for (int i = 0; i < segCount; ++i) {
    float a0 = twoPi * static_cast<float>(i) / static_cast<float>(segCount);
    float a1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segCount);
    light3d::Vec3 p0 = XformPoint(world, {radius * std::cos(a0), radius * std::sin(a0), 0.0f});
    light3d::Vec3 p1 = XformPoint(world, {radius * std::cos(a1), radius * std::sin(a1), 0.0f});
    AddLineV(out, p0, p1, cr, cg, cb);
  }
}

// Generate a wireframe circle in the XZ plane (Y=0).
void AddCircleXZ(std::vector<HelperVertex>& out, float radius, int segCount,
                  float cr, float cg, float cb,
                  const float world[16]) {
  const float twoPi = 6.2831853f;
  for (int i = 0; i < segCount; ++i) {
    float a0 = twoPi * static_cast<float>(i) / static_cast<float>(segCount);
    float a1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segCount);
    light3d::Vec3 p0 = XformPoint(world, {radius * std::cos(a0), 0.0f, radius * std::sin(a0)});
    light3d::Vec3 p1 = XformPoint(world, {radius * std::cos(a1), 0.0f, radius * std::sin(a1)});
    AddLineV(out, p0, p1, cr, cg, cb);
  }
}

// Generate a wireframe sphere (3 orthogonal circles).
void AddSphere(std::vector<HelperVertex>& out, float radius, int segCount,
               float cr, float cg, float cb,
               const float world[16]) {
  AddCircle(out, radius, segCount, cr, cg, cb, world);
  AddCircleXZ(out, radius, segCount, cr, cg, cb, world);
  // YZ circle
  const float twoPi = 6.2831853f;
  for (int i = 0; i < segCount; ++i) {
    float a0 = twoPi * static_cast<float>(i) / static_cast<float>(segCount);
    float a1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segCount);
    light3d::Vec3 p0 = XformPoint(world, {0.0f, radius * std::cos(a0), radius * std::sin(a0)});
    light3d::Vec3 p1 = XformPoint(world, {0.0f, radius * std::cos(a1), radius * std::sin(a1)});
    AddLineV(out, p0, p1, cr, cg, cb);
  }
}

// Generate a wireframe cylinder along the Y axis.
void AddCylinder(std::vector<HelperVertex>& out, float radius, float halfLen,
                 int segCount, float cr, float cg, float cb,
                 const float world[16]) {
  const float twoPi = 6.2831853f;
  for (int i = 0; i < segCount; ++i) {
    float a0 = twoPi * static_cast<float>(i) / static_cast<float>(segCount);
    float a1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segCount);
    float c0 = std::cos(a0), s0 = std::sin(a0);
    float c1 = std::cos(a1), s1 = std::sin(a1);
    light3d::Vec3 t0 = XformPoint(world, {radius * c0, halfLen, radius * s0});
    light3d::Vec3 t1 = XformPoint(world, {radius * c1, halfLen, radius * s1});
    AddLineV(out, t0, t1, cr, cg, cb);
    light3d::Vec3 b0 = XformPoint(world, {radius * c0, -halfLen, radius * s0});
    light3d::Vec3 b1 = XformPoint(world, {radius * c1, -halfLen, radius * s1});
    AddLineV(out, b0, b1, cr, cg, cb);
    if (i % (segCount / 4) == 0) {
      AddLineV(out, t0, b0, cr, cg, cb);
    }
  }
}

// Generate a wireframe rectangle in the XY plane centered at origin.
void AddRect(std::vector<HelperVertex>& out, float halfW, float halfH,
             float cr, float cg, float cb,
             const float world[16]) {
  light3d::Vec3 tl = XformPoint(world, {-halfW, halfH, 0.0f});
  light3d::Vec3 tr = XformPoint(world, { halfW, halfH, 0.0f});
  light3d::Vec3 br = XformPoint(world, { halfW,-halfH, 0.0f});
  light3d::Vec3 bl = XformPoint(world, {-halfW,-halfH, 0.0f});
  AddLineV(out, tl, tr, cr, cg, cb);
  AddLineV(out, tr, br, cr, cg, cb);
  AddLineV(out, br, bl, cr, cg, cb);
  AddLineV(out, bl, tl, cr, cg, cb);
  // Center cross
  light3d::Vec3 c = XformPoint(world, {0.0f, 0.0f, 0.0f});
  AddLineV(out, c, XformPoint(world, {0.0f, halfH, 0.0f}), cr, cg, cb);
  AddLineV(out, c, XformPoint(world, {0.0f,-halfH, 0.0f}), cr, cg, cb);
  AddLineV(out, c, XformPoint(world, {halfW, 0.0f, 0.0f}), cr, cg, cb);
  AddLineV(out, c, XformPoint(world, {-halfW, 0.0f, 0.0f}), cr, cg, cb);
}

// Small axis indicator cross at origin (3 colored axes).
void AddAxisCross(std::vector<HelperVertex>& out, float size,
                  const float world[16]) {
  light3d::Vec3 o = XformPoint(world, {0, 0, 0});
  light3d::Vec3 x = XformPoint(world, {size, 0, 0});
  light3d::Vec3 y = XformPoint(world, {0, size, 0});
  light3d::Vec3 z = XformPoint(world, {0, 0, size});
  AddLineV(out, o, x, 0.9f, 0.2f, 0.2f);
  AddLineV(out, o, y, 0.2f, 0.85f, 0.25f);
  AddLineV(out, o, z, 0.3f, 0.5f, 1.0f);
}

// Directional arrow pointing along -Y.
void AddDirectionalArrow(std::vector<HelperVertex>& out, float len,
                         float headSize, float cr, float cg, float cb,
                         const float world[16]) {
  light3d::Vec3 top = XformPoint(world, {0, len * 0.5f, 0});
  light3d::Vec3 bot = XformPoint(world, {0, -len * 0.5f, 0});
  AddLineV(out, top, bot, cr, cg, cb);
  light3d::Vec3 tip = XformPoint(world, {0, -len * 0.5f, 0});
  AddLineV(out, tip, XformPoint(world, { headSize, -len * 0.5f + headSize, 0}), cr, cg, cb);
  AddLineV(out, tip, XformPoint(world, {-headSize, -len * 0.5f + headSize, 0}), cr, cg, cb);
  AddLineV(out, tip, XformPoint(world, {0, -len * 0.5f + headSize,  headSize}), cr, cg, cb);
  AddLineV(out, tip, XformPoint(world, {0, -len * 0.5f + headSize, -headSize}), cr, cg, cb);
}

// Sun circle for DistantLight.
void AddSunCircle(std::vector<HelperVertex>& out, float radius, int segCount,
                  float cr, float cg, float cb,
                  const float world[16]) {
  const float twoPi = 6.2831853f;
  for (int i = 0; i < segCount; ++i) {
    float a0 = twoPi * static_cast<float>(i) / static_cast<float>(segCount);
    float a1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segCount);
    light3d::Vec3 p0 = XformPoint(world, {radius * std::cos(a0), 0.0f, radius * std::sin(a0)});
    light3d::Vec3 p1 = XformPoint(world, {radius * std::cos(a1), 0.0f, radius * std::sin(a1)});
    AddLineV(out, p0, p1, cr, cg, cb);
  }
}

// Maya-like light gizmo colors.
constexpr float kColorSphere[3]  = {1.0f, 0.85f, 0.2f};
constexpr float kColorCylinder[3] = {1.0f, 0.85f, 0.2f};
constexpr float kColorRect[3]    = {1.0f, 0.85f, 0.2f};
constexpr float kColorDisk[3]    = {1.0f, 0.85f, 0.2f};
constexpr float kColorDistant[3] = {1.0f, 0.6f, 0.1f};
constexpr float kColorDome[3]    = {0.3f, 0.7f, 0.8f};
constexpr float kColorGeometry[3] = {0.8f, 0.3f, 0.8f};
constexpr float kCameraColor[3]  = {0.2f, 0.85f, 0.9f};

constexpr int kCircleSegs = 24;

}  // namespace

void CollectLightCameraTransforms(
    const lightusd::legacy::tydra::Node& node,
    std::unordered_map<int, std::array<float, 16>>& lightXforms,
    std::unordered_map<int, std::array<float, 16>>& camXforms) {
  using namespace lightusd::tydra;
  using namespace lightusd::legacy::tydra;  // NOLINT
  switch (node.nodeType) {
    case NodeType::PointLight:
    case NodeType::DirectionalLight:
    case NodeType::EnvmapLight:
    case NodeType::RectLight:
    case NodeType::DiskLight:
    case NodeType::CylinderLight:
    case NodeType::GeometryLight:
      if (node.id >= 0) {
        std::array<float, 16> m{};
        for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j)
            m[i * 4 + j] = static_cast<float>(node.global_matrix.m[i][j]);
        lightXforms[node.id] = m;
      }
      break;
    case NodeType::Camera:
      if (node.id >= 0) {
        std::array<float, 16> m{};
        for (int i = 0; i < 4; ++i)
          for (int j = 0; j < 4; ++j)
            m[i * 4 + j] = static_cast<float>(node.global_matrix.m[i][j]);
        camXforms[node.id] = m;
      }
      break;
    default:
      break;
  }
  for (const auto& c : node.children) {
    CollectLightCameraTransforms(c, lightXforms, camXforms);
  }
}

void BuildLightGizmos(const lightusd::legacy::tydra::RenderScene& rs,
                      const std::unordered_map<int, std::array<float, 16>>& lightXforms,
                      std::vector<HelperVertex>& out) {
  using lightusd::legacy::tydra::RenderLight;

  for (size_t i = 0; i < rs.lights.size(); ++i) {
    auto it = lightXforms.find(static_cast<int>(i));
    if (it == lightXforms.end()) continue;
    const float* w = it->second.data();
    const RenderLight& light = rs.lights[i];

    switch (light.type) {
      case RenderLight::Type::Sphere: {
        const float* c = kColorSphere;
        float r = std::max(light.radius, 0.05f);
        AddSphere(out, r, kCircleSegs, c[0], c[1], c[2], w);
        light3d::Vec3 center = XformPoint(w, {0, 0, 0});
        float cs = r * 0.3f;
        AddLine(out, center.x - cs, center.y, center.z, center.x + cs, center.y, center.z, c[0], c[1], c[2]);
        AddLine(out, center.x, center.y - cs, center.z, center.x, center.y + cs, center.z, c[0], c[1], c[2]);
        AddLine(out, center.x, center.y, center.z - cs, center.x, center.y, center.z + cs, c[0], c[1], c[2]);
        break;
      }
      case RenderLight::Type::Point: {
        const float* c = kColorSphere;
        float r = 0.05f;
        AddSphere(out, r, kCircleSegs, c[0], c[1], c[2], w);
        light3d::Vec3 center = XformPoint(w, {0, 0, 0});
        float cs = r * 3.0f;
        AddLine(out, center.x - cs, center.y, center.z, center.x + cs, center.y, center.z, c[0], c[1], c[2]);
        AddLine(out, center.x, center.y - cs, center.z, center.x, center.y + cs, center.z, c[0], c[1], c[2]);
        AddLine(out, center.x, center.y, center.z - cs, center.x, center.y, center.z + cs, c[0], c[1], c[2]);
        break;
      }
      case RenderLight::Type::Cylinder: {
        const float* c = kColorCylinder;
        float r = std::max(light.radius, 0.05f);
        float halfLen = std::max(light.length, 0.1f) * 0.5f;
        AddCylinder(out, r, halfLen, kCircleSegs, c[0], c[1], c[2], w);
        light3d::Vec3 top = XformPoint(w, {0, halfLen, 0});
        light3d::Vec3 bot = XformPoint(w, {0, -halfLen, 0});
        AddLineV(out, top, bot, c[0] * 0.6f, c[1] * 0.6f, c[2] * 0.6f);
        break;
      }
      case RenderLight::Type::Rect: {
        const float* c = kColorRect;
        float halfW = std::max(light.width, 0.1f) * 0.5f;
        float halfH = std::max(light.height, 0.1f) * 0.5f;
        AddRect(out, halfW, halfH, c[0], c[1], c[2], w);
        // Diagonal lines for visual clarity
        light3d::Vec3 tl = XformPoint(w, {-halfW, halfH, 0});
        light3d::Vec3 br = XformPoint(w, { halfW,-halfH, 0});
        AddLineV(out, tl, br, c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f);
        light3d::Vec3 tr = XformPoint(w, { halfW, halfH, 0});
        light3d::Vec3 bl = XformPoint(w, {-halfW,-halfH, 0});
        AddLineV(out, tr, bl, c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f);
        break;
      }
      case RenderLight::Type::Disk: {
        const float* c = kColorDisk;
        float r = std::max(light.radius, 0.05f);
        AddCircle(out, r, kCircleSegs, c[0], c[1], c[2], w);
        light3d::Vec3 center = XformPoint(w, {0, 0, 0});
        AddLineV(out, center, XformPoint(w, {0, r, 0}), c[0], c[1], c[2]);
        AddLineV(out, center, XformPoint(w, {0, -r, 0}), c[0], c[1], c[2]);
        AddLineV(out, center, XformPoint(w, {r, 0, 0}), c[0], c[1], c[2]);
        AddLineV(out, center, XformPoint(w, {-r, 0, 0}), c[0], c[1], c[2]);
        break;
      }
      case RenderLight::Type::Distant: {
        const float* c = kColorDistant;
        float arrowLen = 1.0f;
        float headSize = 0.15f;
        AddDirectionalArrow(out, arrowLen, headSize, c[0], c[1], c[2], w);
        AddSunCircle(out, 0.3f, kCircleSegs, c[0], c[1], c[2], w);
        break;
      }
      case RenderLight::Type::Dome: {
        const float* c = kColorDome;
        float r = std::max(light.guideRadius * 0.001f, 1.0f);
        AddSphere(out, r, kCircleSegs, c[0], c[1], c[2], w);
        break;
      }
      case RenderLight::Type::Geometry:
      case RenderLight::Type::Portal: {
        const float* c = kColorGeometry;
        AddAxisCross(out, 0.1f, w);
        light3d::Vec3 p = XformPoint(w, {0, 0, 0});
        float s = 0.05f;
        AddLine(out, p.x - s, p.y, p.z, p.x, p.y + s, p.z, c[0], c[1], c[2]);
        AddLine(out, p.x, p.y + s, p.z, p.x + s, p.y, p.z, c[0], c[1], c[2]);
        AddLine(out, p.x + s, p.y, p.z, p.x, p.y - s, p.z, c[0], c[1], c[2]);
        AddLine(out, p.x, p.y - s, p.z, p.x - s, p.y, p.z, c[0], c[1], c[2]);
        break;
      }
    }
  }
}

void BuildCameraGizmos(const lightusd::legacy::tydra::RenderScene& rs,
                       const std::unordered_map<int, std::array<float, 16>>& camXforms,
                       std::vector<HelperVertex>& out) {
  using lightusd::legacy::tydra::RenderCamera;
  using lightusd::legacy::GeomCamera;

  const float* c = kCameraColor;

  for (size_t i = 0; i < rs.cameras.size(); ++i) {
    auto it = camXforms.find(static_cast<int>(i));
    if (it == camXforms.end()) continue;
    const float* w = it->second.data();
    const RenderCamera& cam = rs.cameras[i];

    // Camera body: small box at origin
    const float bodyHalf = 0.04f;
    {
      const float xs[2] = {-bodyHalf, bodyHalf};
      const float ys[2] = {-bodyHalf, bodyHalf};
      const float zs[2] = {-bodyHalf * 1.5f, bodyHalf * 0.5f};
      for (int yi = 0; yi < 2; ++yi)
        for (int zi = 0; zi < 2; ++zi)
          AddLineV(out, XformPoint(w, {xs[0], ys[yi], zs[zi]}),
                      XformPoint(w, {xs[1], ys[yi], zs[zi]}), c[0], c[1], c[2]);
      for (int xi = 0; xi < 2; ++xi)
        for (int zi = 0; zi < 2; ++zi)
          AddLineV(out, XformPoint(w, {xs[xi], ys[0], zs[zi]}),
                      XformPoint(w, {xs[xi], ys[1], zs[zi]}), c[0], c[1], c[2]);
      for (int xi = 0; xi < 2; ++xi)
        for (int yi = 0; yi < 2; ++yi)
          AddLineV(out, XformPoint(w, {xs[xi], ys[yi], zs[0]}),
                      XformPoint(w, {xs[xi], ys[yi], zs[1]}), c[0], c[1], c[2]);
    }

    // Frustum: camera looks along +Z in local space.
    float znear = std::max(cam.znear, 0.01f);
    float zfar = std::min(cam.zfar, 100.0f);

    // Compute FOV from aperture/focalLength (safe for const ref)
    float halfVFov = 0.5f * std::atan2(cam.verticalAperture * 0.5f, cam.focalLength);
    float aspect = (cam.verticalAperture > 0.0f)
                       ? (cam.horizontalAperture / cam.verticalAperture)
                       : 1.5f;

    if (cam.projection == GeomCamera::Projection::Perspective) {
      float nearHalfH = znear * std::tan(halfVFov);
      float nearHalfW = nearHalfH * aspect;
      float farHalfH = zfar * std::tan(halfVFov);
      float farHalfW = farHalfH * aspect;

      light3d::Vec3 ntl = XformPoint(w, {-nearHalfW,  nearHalfH, znear});
      light3d::Vec3 ntr = XformPoint(w, { nearHalfW,  nearHalfH, znear});
      light3d::Vec3 nbr = XformPoint(w, { nearHalfW, -nearHalfH, znear});
      light3d::Vec3 nbl = XformPoint(w, {-nearHalfW, -nearHalfH, znear});

      light3d::Vec3 ftl = XformPoint(w, {-farHalfW,  farHalfH, zfar});
      light3d::Vec3 ftr = XformPoint(w, { farHalfW,  farHalfH, zfar});
      light3d::Vec3 fbr = XformPoint(w, { farHalfW, -farHalfH, zfar});
      light3d::Vec3 fbl = XformPoint(w, {-farHalfW, -farHalfH, zfar});

      AddLineV(out, ntl, ntr, c[0], c[1], c[2]);
      AddLineV(out, ntr, nbr, c[0], c[1], c[2]);
      AddLineV(out, nbr, nbl, c[0], c[1], c[2]);
      AddLineV(out, nbl, ntl, c[0], c[1], c[2]);

      float dim = 0.5f;
      AddLineV(out, ftl, ftr, c[0]*dim, c[1]*dim, c[2]*dim);
      AddLineV(out, ftr, fbr, c[0]*dim, c[1]*dim, c[2]*dim);
      AddLineV(out, fbr, fbl, c[0]*dim, c[1]*dim, c[2]*dim);
      AddLineV(out, fbl, ftl, c[0]*dim, c[1]*dim, c[2]*dim);

      AddLineV(out, ntl, ftl, c[0], c[1], c[2]);
      AddLineV(out, ntr, ftr, c[0], c[1], c[2]);
      AddLineV(out, nbr, fbr, c[0], c[1], c[2]);
      AddLineV(out, nbl, fbl, c[0], c[1], c[2]);
    } else {
      // Orthographic
      float halfW = cam.xmag * 0.5f;
      float halfH = cam.ymag * 0.5f;

      light3d::Vec3 ntl = XformPoint(w, {-halfW,  halfH, znear});
      light3d::Vec3 ntr = XformPoint(w, { halfW,  halfH, znear});
      light3d::Vec3 nbr = XformPoint(w, { halfW, -halfH, znear});
      light3d::Vec3 nbl = XformPoint(w, {-halfW, -halfH, znear});

      light3d::Vec3 ftl = XformPoint(w, {-halfW,  halfH, zfar});
      light3d::Vec3 ftr = XformPoint(w, { halfW,  halfH, zfar});
      light3d::Vec3 fbr = XformPoint(w, { halfW, -halfH, zfar});
      light3d::Vec3 fbl = XformPoint(w, {-halfW, -halfH, zfar});

      AddLineV(out, ntl, ntr, c[0], c[1], c[2]);
      AddLineV(out, ntr, nbr, c[0], c[1], c[2]);
      AddLineV(out, nbr, nbl, c[0], c[1], c[2]);
      AddLineV(out, nbl, ntl, c[0], c[1], c[2]);

      float dim = 0.5f;
      AddLineV(out, ftl, ftr, c[0]*dim, c[1]*dim, c[2]*dim);
      AddLineV(out, ftr, fbr, c[0]*dim, c[1]*dim, c[2]*dim);
      AddLineV(out, fbr, fbl, c[0]*dim, c[1]*dim, c[2]*dim);
      AddLineV(out, fbl, ftl, c[0]*dim, c[1]*dim, c[2]*dim);

      AddLineV(out, ntl, ftl, c[0], c[1], c[2]);
      AddLineV(out, ntr, ftr, c[0], c[1], c[2]);
      AddLineV(out, nbr, fbr, c[0], c[1], c[2]);
      AddLineV(out, nbl, fbl, c[0], c[1], c[2]);
    }

    // Center cross at camera position
    light3d::Vec3 eye = XformPoint(w, {0, 0, 0});
    float cs = 0.03f;
    AddLine(out, eye.x - cs, eye.y, eye.z, eye.x + cs, eye.y, eye.z, c[0], c[1], c[2]);
    AddLine(out, eye.x, eye.y - cs, eye.z, eye.x, eye.y + cs, eye.z, c[0], c[1], c[2]);
  }
}

}  // namespace lusdview
