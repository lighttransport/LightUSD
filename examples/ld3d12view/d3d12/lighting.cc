#include "lighting.hh"

#include "mathutil.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

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

namespace {

uint16_t FloatToHalf(float f) {
  f = std::min(std::max(f, 0.0f), 65504.0f);  // radiance: non-negative, finite
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
  uint32_t m = x & 0x7fffffu;
  if (e <= 0) {  // subnormal half (or zero)
    if (e < -10) return 0;
    m |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - e);
    return static_cast<uint16_t>((m + (1u << (shift - 1))) >> shift);
  }
  return static_cast<uint16_t>((static_cast<uint32_t>(e) << 10) +
                               ((m + 0x1000u) >> 13));
}

// One RGB float image with trilinear equirect sampling over a box pyramid.
struct EquirectPyramid {
  struct Level {
    int w = 0, h = 0;
    std::vector<float> rgb;
  };
  std::vector<Level> levels;

  void Sample(const Level& l, float u, float v, float out[3]) const {
    u -= std::floor(u);
    const float fx = u * l.w - 0.5f;
    const float fy = std::min(std::max(v, 0.0f), 1.0f) * l.h - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0, ty = fy - y0;
    float acc[3] = {0, 0, 0};
    for (int dy = 0; dy < 2; ++dy) {
      const int y = std::min(std::max(y0 + dy, 0), l.h - 1);
      for (int dx = 0; dx < 2; ++dx) {
        const int x = ((x0 + dx) % l.w + l.w) % l.w;  // wrap in longitude
        const float wgt = (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty);
        const float* p = &l.rgb[(static_cast<size_t>(y) * l.w + x) * 3];
        for (int c = 0; c < 3; ++c) acc[c] += p[c] * wgt;
      }
    }
    for (int c = 0; c < 3; ++c) out[c] = acc[c];
  }

  void Trilinear(float u, float v, float lod, float out[3]) const {
    lod = std::min(std::max(lod, 0.0f), static_cast<float>(levels.size() - 1));
    const size_t l0 = static_cast<size_t>(lod);
    const size_t l1 = std::min(l0 + 1, levels.size() - 1);
    const float t = lod - static_cast<float>(l0);
    float a[3], b[3];
    Sample(levels[l0], u, v, a);
    if (t <= 0.0f || l1 == l0) {
      for (int c = 0; c < 3; ++c) out[c] = a[c];
      return;
    }
    Sample(levels[l1], u, v, b);
    for (int c = 0; c < 3; ++c) out[c] = a[c] + (b[c] - a[c]) * t;
  }
};

float RadicalInverse(uint32_t bits) {
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

}  // namespace

bool BuildPrefilteredEnvironment(const lmv_scene* scene, int32_t image_id,
                                 PrefilteredEnv* out) {
  constexpr int kBaseW = 512, kBaseH = 256, kMips = 6, kSamples = 96;
  lmv_image_view iv = {};
  if (lmv_scene_image(scene, static_cast<uint32_t>(image_id), &iv) != LMV_OK ||
      !iv.pixels || iv.width < 2 || iv.height < 2)
    return false;
  const int w = static_cast<int>(iv.width), h = static_cast<int>(iv.height);
  auto texel = [&](int x, int y, float o[3]) {
    const size_t i = static_cast<size_t>(y) * w + x;
    if (iv.is_float && (iv.channels == 3 || iv.channels == 4)) {
      const float* p = static_cast<const float*>(iv.pixels) + i * iv.channels;
      for (int c = 0; c < 3; ++c) o[c] = std::max(p[c], 0.0f);
    } else if (!iv.is_float && iv.channels == 4) {
      const uint8_t* p = static_cast<const uint8_t*>(iv.pixels) + i * 4;
      for (int c = 0; c < 3; ++c) {
        float v = p[c] / 255.0f;
        if (iv.srgb)
          v = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        o[c] = v;
      }
    } else {
      o[0] = o[1] = o[2] = 0.0f;
    }
  };

  // Base level: area-resample the source to kBaseW x kBaseH (<= 4x4 taps).
  EquirectPyramid pyr;
  pyr.levels.resize(7);  // 512x256 down to 8x4
  {
    auto& b = pyr.levels[0];
    b.w = kBaseW;
    b.h = kBaseH;
    b.rgb.assign(static_cast<size_t>(kBaseW) * kBaseH * 3, 0.0f);
    const int tx = std::max(1, std::min(4, w / kBaseW));
    const int ty = std::max(1, std::min(4, h / kBaseH));
    for (int y = 0; y < kBaseH; ++y) {
      for (int x = 0; x < kBaseW; ++x) {
        float acc[3] = {0, 0, 0};
        for (int sy = 0; sy < ty; ++sy) {
          const int py = std::min(
              h - 1, static_cast<int>((y + (sy + 0.5f) / ty) * h / kBaseH));
          for (int sx = 0; sx < tx; ++sx) {
            const int px = std::min(
                w - 1, static_cast<int>((x + (sx + 0.5f) / tx) * w / kBaseW));
            float t[3];
            texel(px, py, t);
            for (int c = 0; c < 3; ++c) acc[c] += t[c];
          }
        }
        float* d = &b.rgb[(static_cast<size_t>(y) * kBaseW + x) * 3];
        for (int c = 0; c < 3; ++c) d[c] = acc[c] / float(tx * ty);
      }
    }
  }
  for (size_t k = 1; k < pyr.levels.size(); ++k) {
    const auto& s = pyr.levels[k - 1];
    auto& d = pyr.levels[k];
    d.w = s.w / 2;
    d.h = s.h / 2;
    d.rgb.resize(static_cast<size_t>(d.w) * d.h * 3);
    for (int y = 0; y < d.h; ++y)
      for (int x = 0; x < d.w; ++x)
        for (int c = 0; c < 3; ++c) {
          float a = 0;
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
              a += s.rgb[(static_cast<size_t>(2 * y + dy) * s.w + 2 * x + dx) * 3 + c];
          d.rgb[(static_cast<size_t>(y) * d.w + x) * 3 + c] = a * 0.25f;
        }
  }

  out->width = kBaseW;
  out->height = kBaseH;
  out->levels.assign(kMips, {});
  const float omega_p = 4.0f * kPi / (float(kBaseW) * float(kBaseH));

  for (int m = 0; m < kMips; ++m) {
    const int lw = kBaseW >> m, lh = kBaseH >> m;
    std::vector<uint16_t>& dst = out->levels[static_cast<size_t>(m)];
    dst.assign(static_cast<size_t>(lw) * lh * 4, 0);
    const float rough = static_cast<float>(m) / float(kMips - 1);
    const float a = std::max(rough * rough, 1e-4f);
    const float a2 = a * a;

    auto do_rows = [&](int y_begin, int y_end) {
      for (int y = y_begin; y < y_end; ++y) {
        const float v = (y + 0.5f) / lh;
        const float theta = (0.5f - v) * kPi;
        const float cos_t = std::cos(theta), sin_t = std::sin(theta);
        for (int x = 0; x < lw; ++x) {
          const float u = (x + 0.5f) / lw;
          const float phi = (u - 0.5f) * 2 * kPi;
          float n[3] = {cos_t * std::cos(phi), sin_t, cos_t * std::sin(phi)};
          float rgb[3] = {0, 0, 0};
          if (m == 0) {
            pyr.Trilinear(u, v, 0.0f, rgb);  // mirror: the resampled base
          } else {
            // Tangent frame around N (= V = R).
            float up[3] = {std::fabs(n[1]) < 0.999f ? 0.0f : 1.0f,
                           std::fabs(n[1]) < 0.999f ? 1.0f : 0.0f, 0.0f};
            float tx[3], ty[3];
            Cross(up, n, tx);
            Normalize(tx);
            Cross(n, tx, ty);
            float wsum = 0.0f;
            for (int s = 0; s < kSamples; ++s) {
              const float xi1 = (s + 0.5f) / kSamples;
              const float xi2 = RadicalInverse(static_cast<uint32_t>(s));
              const float ph = 2 * kPi * xi1;
              const float ct = std::sqrt((1 - xi2) / (1 + (a2 - 1) * xi2));
              const float st = std::sqrt(std::max(0.0f, 1 - ct * ct));
              float hv[3];
              for (int c = 0; c < 3; ++c)
                hv[c] = tx[c] * (st * std::cos(ph)) + ty[c] * (st * std::sin(ph)) +
                        n[c] * ct;
              const float vh = Dot(n, hv);
              float l[3];
              for (int c = 0; c < 3; ++c) l[c] = 2 * vh * hv[c] - n[c];
              const float nl = Dot(n, l);
              if (nl <= 0.0f) continue;
              const float denom = ct * ct * (a2 - 1) + 1;
              const float d_ggx = a2 / (kPi * denom * denom);
              const float pdf = d_ggx * 0.25f;  // D * NdotH / (4 VdotH), N = V
              const float omega_s = 1.0f / (kSamples * pdf);
              const float lod = std::max(0.0f, 0.5f * std::log2(omega_s / omega_p) + 1.0f);
              const float su = std::atan2(l[2], l[0]) / (2 * kPi) + 0.5f;
              const float sv = 0.5f - std::asin(std::min(std::max(l[1], -1.0f), 1.0f)) / kPi;
              float c3[3];
              pyr.Trilinear(su, sv, lod, c3);
              for (int c = 0; c < 3; ++c) rgb[c] += c3[c] * nl;
              wsum += nl;
            }
            if (wsum > 0.0f)
              for (int c = 0; c < 3; ++c) rgb[c] /= wsum;
          }
          uint16_t* d = &dst[(static_cast<size_t>(y) * lw + x) * 4];
          d[0] = FloatToHalf(rgb[0]);
          d[1] = FloatToHalf(rgb[1]);
          d[2] = FloatToHalf(rgb[2]);
          d[3] = 0x3C00;  // 1.0
        }
      }
    };

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const int threads = (m == 0 || lh < 16) ? 1 : static_cast<int>(std::min<unsigned>(hw, 16u));
    if (threads <= 1) {
      do_rows(0, lh);
    } else {
      std::vector<std::thread> pool;
      for (int t = 0; t < threads; ++t)
        pool.emplace_back(do_rows, lh * t / threads, lh * (t + 1) / threads);
      for (auto& th : pool) th.join();
    }
  }
  return true;
}

namespace {

void Unproject(const float inv[16], float x, float y, float z, float out[3]) {
  float p[4];
  const float in[4] = {x, y, z, 1.0f};
  for (int r = 0; r < 4; ++r) {
    p[r] = 0.0f;
    for (int c = 0; c < 4; ++c) p[r] += inv[c * 4 + r] * in[c];
  }
  const float w = std::fabs(p[3]) > 1e-20f ? p[3] : 1e-20f;
  for (int i = 0; i < 3; ++i) out[i] = p[i] / w;
}

}  // namespace

bool ComputeShadowCascades(const float view_proj[16], const float light_dir[3],
                           const float bmin[3], const float bmax[3], int count,
                           int map_size, ShadowCascades* out) {
  count = std::max(1, std::min(count, kMaxShadowCascades));
  *out = ShadowCascades();
  float inv[16];
  if (!Invert4x4(view_proj, inv)) return false;

  // View frustum corners (NDC x,y in {-1,1}; D3D depth 0 = near, 1 = far).
  const float xs[4] = {-1, 1, -1, 1}, ys[4] = {-1, -1, 1, 1};
  float nc[4][3], fc[4][3];
  float near_c[3] = {0, 0, 0}, far_c[3] = {0, 0, 0};
  for (int k = 0; k < 4; ++k) {
    Unproject(inv, xs[k], ys[k], 0.0f, nc[k]);
    Unproject(inv, xs[k], ys[k], 1.0f, fc[k]);
    for (int i = 0; i < 3; ++i) {
      near_c[i] += nc[k][i] * 0.25f;
      far_c[i] += fc[k][i] * 0.25f;
    }
  }
  float fwd[3] = {far_c[0] - near_c[0], far_c[1] - near_c[1], far_c[2] - near_c[2]};
  const float depth_len = std::sqrt(Dot(fwd, fwd));
  if (!(depth_len > 1e-8f)) return false;
  for (int i = 0; i < 3; ++i) fwd[i] /= depth_len;

  // Depth (along the view axis, from the near plane) covered by scene bounds.
  float d_min = 1e30f, d_max = -1e30f;
  for (int c = 0; c < 8; ++c) {
    const float p[3] = {(c & 1) ? bmax[0] : bmin[0], (c & 2) ? bmax[1] : bmin[1],
                        (c & 4) ? bmax[2] : bmin[2]};
    const float d = (p[0] - near_c[0]) * fwd[0] + (p[1] - near_c[1]) * fwd[1] +
                    (p[2] - near_c[2]) * fwd[2];
    d_min = std::min(d_min, d);
    d_max = std::max(d_max, d);
  }
  const float range_a = std::max(d_min, 0.0f);
  const float range_b = std::min(d_max, depth_len);
  for (int i = 0; i < 3; ++i) {
    out->origin[i] = near_c[i];
    out->dir[i] = fwd[i];
  }
  if (!(range_b > range_a + 1e-6f)) return false;  // scene is not in view

  // Practical split scheme: blend of uniform and logarithmic splits.
  const float lambda = 0.6f;
  const float log_a = std::max(range_a, range_b * 1e-3f);
  float ends[kMaxShadowCascades + 1];
  ends[0] = range_a;
  for (int i = 1; i <= count; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(count);
    const float uni = range_a + (range_b - range_a) * t;
    const float lg = log_a * std::pow(range_b / log_a, t);
    ends[i] = lambda * lg + (1.0f - lambda) * uni;
  }
  ends[count] = range_b;

  // Light basis (towards the light is +dir).
  float dir[3] = {light_dir[0], light_dir[1], light_dir[2]};
  Normalize(dir);
  float up[3] = {0, 1, 0};
  if (std::fabs(Dot(dir, up)) > 0.99f) {
    up[1] = 0;
    up[2] = 1;
  }
  const float f[3] = {-dir[0], -dir[1], -dir[2]};
  float s[3], u[3];
  Cross(f, up, s);
  Normalize(s);
  Cross(s, f, u);

  float bcorner[8][3];
  for (int c = 0; c < 8; ++c) {
    bcorner[c][0] = (c & 1) ? bmax[0] : bmin[0];
    bcorner[c][1] = (c & 2) ? bmax[1] : bmin[1];
    bcorner[c][2] = (c & 4) ? bmax[2] : bmin[2];
  }

  out->count = count;
  for (int ci = 0; ci < count; ++ci) {
    const float t0 = ends[ci] / depth_len, t1 = ends[ci + 1] / depth_len;
    // Slice corners and their bounding sphere (rotation-stable fit).
    float corners[8][3];
    float center[3] = {0, 0, 0};
    for (int k = 0; k < 4; ++k)
      for (int i = 0; i < 3; ++i) {
        corners[k][i] = nc[k][i] + (fc[k][i] - nc[k][i]) * t0;
        corners[4 + k][i] = nc[k][i] + (fc[k][i] - nc[k][i]) * t1;
      }
    for (int k = 0; k < 8; ++k)
      for (int i = 0; i < 3; ++i) center[i] += corners[k][i] * 0.125f;
    float radius = 0.0f;
    for (int k = 0; k < 8; ++k) {
      const float d[3] = {corners[k][0] - center[0], corners[k][1] - center[1],
                          corners[k][2] - center[2]};
      radius = std::max(radius, std::sqrt(Dot(d, d)));
    }
    radius = std::max(std::ceil(radius * 16.0f) / 16.0f, 0.01f);

    // Snap the centre to whole shadow texels in light space (no shimmering).
    const float texel = 2.0f * radius / static_cast<float>(map_size);
    const float cs = Dot(center, s), cu = Dot(center, u);
    const float ds = std::floor(cs / texel) * texel - cs;
    const float du = std::floor(cu / texel) * texel - cu;
    for (int i = 0; i < 3; ++i) center[i] += s[i] * ds + u[i] * du;

    // Depth extent along the light: casters anywhere up to the scene bounds.
    float t_max = radius, t_min = -radius;
    for (int c = 0; c < 8; ++c) {
      const float d[3] = {bcorner[c][0] - center[0], bcorner[c][1] - center[1],
                          bcorner[c][2] - center[2]};
      const float t = Dot(d, dir);
      t_max = std::max(t_max, t);
      t_min = std::min(t_min, t);
    }
    const float pad = 0.02f * (t_max - t_min);
    const float eye_dist = t_max + pad;
    const float far_z = eye_dist - t_min + pad;
    float eye[3];
    for (int i = 0; i < 3; ++i) eye[i] = center[i] + dir[i] * eye_dist;

    const float view[16] = {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0,
                            s[2], u[2], -f[2], 0, -Dot(s, eye), -Dot(u, eye),
                            Dot(f, eye), 1};
    float proj[16] = {};
    proj[0] = 1.0f / radius;
    proj[5] = 1.0f / radius;
    proj[10] = -1.0f / far_z;  // near = 0
    proj[15] = 1.0f;
    float* dst = out->view_proj[ci];
    for (int col = 0; col < 4; ++col)
      for (int row = 0; row < 4; ++row) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) sum += proj[k * 4 + row] * view[col * 4 + k];
        dst[col * 4 + row] = sum;
      }
    out->cascade_end[ci] = ends[ci + 1];
    out->texel_world[ci] = texel;
    out->depth_range[ci] = far_z;
  }
  return true;
}

}  // namespace ld3d12
