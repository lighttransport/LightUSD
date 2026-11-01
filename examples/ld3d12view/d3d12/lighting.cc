#include "lighting.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ld3d12 {
namespace {

constexpr float kPi = 3.14159265358979f;

void Normalize(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-8f)
    for (int i = 0; i < 3; ++i) v[i] /= l;
}

float Dot(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

LightGPU Directional(float x, float y, float z, float r, float g, float b) {
  LightGPU l;
  float d[3] = {x, y, z};
  Normalize(d);
  for (int i = 0; i < 3; ++i) {
    l.position_type[i] = d[i] * 10.0f;
    l.direction[i] = d[i];
  }
  l.position_type[3] = static_cast<float>(LMV_LIGHT_DIRECTIONAL);
  l.color_intensity[0] = r;
  l.color_intensity[1] = g;
  l.color_intensity[2] = b;
  return l;
}

std::vector<LightGPU> StudioRig() {
  return {Directional(-0.45f, 0.60f, 0.50f, 2.6f, 2.5f, 2.35f),
          Directional(0.65f, 0.18f, 0.35f, 0.65f, 0.7f, 0.8f),
          Directional(0.15f, -0.25f, -0.9f, 0.5f, 0.55f, 0.7f)};
}

}  // namespace

std::vector<LightGPU> BuildLights(const lmv_scene* scene, bool* authored) {
  std::vector<LightGPU> lights;
  const uint32_t count = lmv_scene_light_count(scene);
  for (uint32_t i = 0; i < count; ++i) {
    lmv_light_view lv = {};
    if (lmv_scene_light(scene, i, &lv) != LMV_OK) continue;
    if (lv.type == LMV_LIGHT_DOME || !lv.transform) continue;  // dome = IBL
    const float* m = lv.transform;  // column-major
    LightGPU g;
    g.position_type[0] = m[12];
    g.position_type[1] = m[13];
    g.position_type[2] = m[14];
    // Light-space -Z is the emitted direction; store the way towards the light.
    g.direction[0] = m[8];
    g.direction[1] = m[9];
    g.direction[2] = m[10];
    Normalize(g.direction);
    const float intensity = lv.intensity * std::exp2(lv.exposure);
    for (int c = 0; c < 3; ++c) g.color_intensity[c] = lv.color[c] * intensity;
    g.color_intensity[3] = 1.0f;
    switch (lv.type) {
      case LMV_LIGHT_DIRECTIONAL:
        g.position_type[3] = static_cast<float>(LMV_LIGHT_DIRECTIONAL);
        break;
      case LMV_LIGHT_SPOT: {
        const float half = std::max(lv.param0, 0.001f) * 0.5f;
        g.color_intensity[3] = std::cos(half);
        g.params[1] = std::cos(half * 0.8f);
        g.position_type[3] = static_cast<float>(LMV_LIGHT_SPOT);
        break;
      }
      default:  // point, rect, disk, sphere, other: treated as point lights
        g.params[0] = std::max(lv.param0, 0.001f);
        g.params[1] = std::max(lv.param1, 0.001f);
        g.position_type[3] = static_cast<float>(LMV_LIGHT_POINT);
        break;
    }
    lights.push_back(g);
  }
  if (authored) *authored = !lights.empty();
  return lights.empty() ? StudioRig() : lights;
}

int32_t FindDomeImage(const lmv_scene* scene) {
  const uint32_t count = lmv_scene_light_count(scene);
  for (uint32_t i = 0; i < count; ++i) {
    lmv_light_view lv = {};
    if (lmv_scene_light(scene, i, &lv) != LMV_OK || lv.type != LMV_LIGHT_DOME ||
        lv.dome_image_id < 0)
      continue;
    return lv.dome_image_id;
  }
  return -1;
}

bool ComputeEnvironmentSH(const lmv_scene* scene, int32_t image_id,
                          const float color_matrix[9], float sh[9][4]) {
  lmv_image_view iv = {};
  if (lmv_scene_image(scene, static_cast<uint32_t>(image_id), &iv) != LMV_OK ||
      !iv.pixels || iv.width < 2 || iv.height < 2)
    return false;
  const int w = static_cast<int>(iv.width), h = static_cast<int>(iv.height);
  auto texel = [&](int x, int y, float out[3]) {
    const size_t i = static_cast<size_t>(y) * w + x;
    if (iv.is_float && (iv.channels == 3 || iv.channels == 4)) {
      const float* p = static_cast<const float*>(iv.pixels) + i * iv.channels;
      for (int c = 0; c < 3; ++c) out[c] = std::max(p[c], 0.0f);
    } else if (!iv.is_float && iv.channels == 4) {
      const uint8_t* p = static_cast<const uint8_t*>(iv.pixels) + i * 4;
      for (int c = 0; c < 3; ++c) {
        float v = p[c] / 255.0f;
        if (iv.srgb)
          v = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        out[c] = v;
      }
    } else {
      out[0] = out[1] = out[2] = 0.0f;
    }
  };

  const int step = std::max(1, std::min(w, h) / 128);
  const float inv_w = 1.0f / w, inv_h = 1.0f / h;
  float acc[9][3] = {};
  for (int y = 0; y < h; y += step) {
    const float v = (y + 0.5f) * inv_h;
    const float theta = (0.5f - v) * kPi;  // elevation
    const float cos_t = std::cos(theta), sin_t = std::sin(theta);
    const float d_omega =
        (2 * kPi * inv_w) * (kPi * inv_h) * std::max(cos_t, 0.001f);
    for (int x = 0; x < w; x += step) {
      const float u = (x + 0.5f) * inv_w;
      const float phi = (u - 0.5f) * 2 * kPi;
      const float dx = cos_t * std::cos(phi), dy = sin_t,
                  dz = cos_t * std::sin(phi);
      float rgb[3];
      texel(x, y, rgb);
      const float basis[9] = {0.282095f,
                              0.488603f * dy,
                              0.488603f * dz,
                              0.488603f * dx,
                              1.092548f * dx * dy,
                              1.092548f * dy * dz,
                              0.315392f * (3 * dz * dz - 1),
                              1.092548f * dx * dz,
                              0.546274f * (dx * dx - dy * dy)};
      for (int k = 0; k < 9; ++k)
        for (int c = 0; c < 3; ++c) acc[k][c] += rgb[c] * d_omega * basis[k];
    }
  }
  const float factors[9] = {kPi,         2 * kPi / 3, 2 * kPi / 3,
                            2 * kPi / 3, kPi / 4,     kPi / 4,
                            kPi / 4,     kPi / 4,     kPi / 4};
  for (int k = 0; k < 9; ++k) {
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = acc[k][i] * factors[k];
    for (int i = 0; i < 3; ++i)
      sh[k][i] = color_matrix[i * 3 + 0] * c[0] + color_matrix[i * 3 + 1] * c[1] +
                 color_matrix[i * 3 + 2] * c[2];
    sh[k][3] = 0.0f;
  }
  return true;
}

void DirectionalShadowMatrix(const float light_dir[3], const float bmin[3],
                             const float bmax[3], float out[16]) {
  float center[3], ext[3];
  for (int i = 0; i < 3; ++i) {
    center[i] = 0.5f * (bmin[i] + bmax[i]);
    ext[i] = bmax[i] - bmin[i];
  }
  const float radius =
      std::max(std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]) *
                   0.5f,
               0.01f) *
      1.25f;
  float dir[3] = {light_dir[0], light_dir[1], light_dir[2]};
  Normalize(dir);
  float up[3] = {0, 1, 0};
  if (std::fabs(Dot(dir, up)) > 0.99f) {
    up[1] = 0;
    up[2] = 1;
  }
  float eye[3];
  for (int i = 0; i < 3; ++i) eye[i] = center[i] + dir[i] * radius * 2.0f;

  // Right-handed look-at (camera looks down -Z), column-major.
  float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
  Normalize(f);
  float s[3];
  Cross(f, up, s);
  Normalize(s);
  float u[3];
  Cross(s, f, u);
  float view[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0,
                    s[2], u[2], -f[2], 0, -Dot(s, eye), -Dot(u, eye),
                    Dot(f, eye), 1};

  // Orthographic, 0..1 depth: view z in [-near, -far] maps to [0, 1].
  const float near_z = 0.0f, far_z = radius * 4.0f;
  float proj[16] = {};
  proj[0] = 1.0f / radius;
  proj[5] = 1.0f / radius;
  proj[10] = 1.0f / (near_z - far_z);
  proj[14] = near_z / (near_z - far_z);
  proj[15] = 1.0f;

  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += proj[k * 4 + row] * view[col * 4 + k];
      out[col * 4 + row] = sum;
    }
}

}  // namespace ld3d12
