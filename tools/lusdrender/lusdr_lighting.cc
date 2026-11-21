// SPDX-License-Identifier: Apache-2.0
// lusdrender — environment / IBL precompute (diffuse irradiance, prefiltered
// specular, BRDF LUT, lat-long sampling) and legacy light collection.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(LUSDR_WITH_TEXTOOLS)
#include "envmap.h"
#endif

#include "image-loader.hh"
#include "lusdr_context.hh"

namespace lusdr {

void AppendPowerCdf(std::vector<PreviewLight> *lights, std::vector<float> *cdf) {
  if (!lights || !cdf) return;
  cdf->clear();
  float sum = 0.0f;
  for (PreviewLight &light : *lights) {
    sum += std::max(0.0f, light.power);
    light.cdf = sum;
    cdf->push_back(sum);
  }
  if (sum > 0.0f) {
    for (float &v : *cdf) {
      v /= sum;
    }
    for (PreviewLight &light : *lights) {
      light.cdf /= sum;
    }
  }
}

Vec3 DirectionFromLatlong(float u, float v) {
  constexpr float kPi = 3.14159265358979323846f;
  float phi = (u - 0.5f) * 2.0f * kPi;
  float theta = v * kPi;
  float st = std::sin(theta);
  return Normalize(Vec3{st * std::sin(phi), std::cos(theta), st * std::cos(phi)});
}

void LatlongUV(const Vec3 &dir, float *u, float *v) {
  constexpr float kPi = 3.14159265358979323846f;
  Vec3 d = Normalize(dir);
  float phi = std::atan2(d.x, d.z);
  float theta = std::acos(ClampFloat(d.y, -1.0f, 1.0f));
  if (u) *u = phi / (2.0f * kPi) + 0.5f;
  if (v) *v = theta / kPi;
}

Vec3 SampleEnvNearest(const EnvImage &img, float u, float v) {
  if (img.width <= 0 || img.height <= 0 || img.pixels.empty()) {
    return Vec3{0.0f, 0.0f, 0.0f};
  }
  if (!std::isfinite(u) || !std::isfinite(v)) return Vec3{0, 0, 0};
  u = u - std::floor(u);
  v = ClampFloat(v, 0.0f, 1.0f);
  int x = int(std::floor(u * float(img.width))) % img.width;
  int y = std::min(img.height - 1, int(std::floor(v * float(img.height))));
  const size_t index = size_t(y) * size_t(img.width) + size_t(x);
  return index < img.pixels.size() ? img.pixels[index] : Vec3{0, 0, 0};
}

Vec3 SampleEnv(const EnvImage &img, const Vec3 &dir) {
  float u = 0.0f;
  float v = 0.0f;
  LatlongUV(dir, &u, &v);
  return SampleEnvNearest(img, u, v);
}

// Resample a light-probe image (mirroredBall = 2 / angular = 3) into latlong.
// For every latlong texel's direction, invert the probe projection -- both are
// expressed in the dome's local frame, photographed along -Z with +Y up, the
// same formulas lusdview's TexToolsProbeToEquirect uses -- and bilinearly
// sample the probe there. Any other format value passes through untouched.
EnvImage RemapProbeToLatlong(EnvImage &&env, int format) {
  if ((format != 2 && format != 3) || env.width <= 0 || env.height <= 0 ||
      env.pixels.empty()) {
    return std::move(env);
  }
  constexpr float kPi = 3.14159265358979323846f;
  const EnvImage src = std::move(env);
  auto bilinear = [&src](float u, float v) -> Vec3 {
    const float fx = u * float(src.width) - 0.5f;
    const float fy = v * float(src.height) - 0.5f;
    const int x0 = int(std::floor(fx));
    const int y0 = int(std::floor(fy));
    const float tx = fx - float(x0);
    const float ty = fy - float(y0);
    auto at = [&src](int x, int y) -> const Vec3 & {
      x = x < 0 ? 0 : (x >= src.width ? src.width - 1 : x);
      y = y < 0 ? 0 : (y >= src.height ? src.height - 1 : y);
      return src.pixels[size_t(y) * size_t(src.width) + size_t(x)];
    };
    const Vec3 a = Lerp(at(x0, y0), at(x0 + 1, y0), tx);
    const Vec3 b = Lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx);
    return Lerp(a, b, ty);
  };
  EnvImage out;
  out.width = std::min(2048, std::max(256, 2 * src.width));
  out.height = out.width / 2;
  out.pixels.resize(size_t(out.width) * size_t(out.height));
  for (int y = 0; y < out.height; y++) {
    for (int x = 0; x < out.width; x++) {
      const Vec3 d = DirectionFromLatlong((float(x) + 0.5f) / float(out.width),
                                          (float(y) + 0.5f) / float(out.height));
      float u = 0.5f, v = 0.5f;
      if (format == 2) {
        // Mirrored ball: the ball normal reflecting `d` back to the camera is
        // normalize(d + (0,0,1)); its xy IS the position in the unit disc.
        const Vec3 n{d.x, d.y, d.z + 1.0f};
        const float len = Length(n);
        if (len > 1.0e-8f) {
          u = 0.5f + 0.5f * (n.x / len);
          v = 0.5f - 0.5f * (n.y / len);
        }
      } else {
        // Angular / light-probe (Debevec): image radius = angle from the
        // forward axis (-Z) / pi, the full sphere at the disc edge.
        const float z = ClampFloat(d.z, -1.0f, 1.0f);
        const float theta = std::acos(-z);
        const float lxy = std::sqrt(d.x * d.x + d.y * d.y);
        if (lxy > 1.0e-8f) {
          const float r = theta / kPi;
          u = 0.5f + 0.5f * r * (d.x / lxy);
          v = 0.5f - 0.5f * r * (d.y / lxy);
        }
      }
      out.pixels[size_t(y) * size_t(out.width) + size_t(x)] = bilinear(u, v);
    }
  }
  return out;
}


EnvImage ConvolveDiffuseEnv(const EnvImage &env, int width, int height) {
  constexpr float kPi = 3.14159265358979323846f;
  EnvImage out;
  out.width = width;
  out.height = height;
  out.pixels.resize(size_t(width) * size_t(height));
  const int theta_steps = 8;
  const int phi_steps = 16;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      Vec3 n = DirectionFromLatlong((float(x) + 0.5f) / float(width),
                                    (float(y) + 0.5f) / float(height));
      Vec3 up = (std::abs(n.y) < 0.999f) ? Vec3{0.0f, 1.0f, 0.0f}
                                         : Vec3{1.0f, 0.0f, 0.0f};
      Vec3 tangent = Normalize(Cross(up, n));
      Vec3 bitangent = Cross(n, tangent);
      Vec3 sum{0.0f, 0.0f, 0.0f};
      float weight_sum = 0.0f;
      for (int ti = 0; ti < theta_steps; ti++) {
        float theta = (float(ti) + 0.5f) / float(theta_steps) * 0.5f * kPi;
        float st = std::sin(theta);
        float ct = std::cos(theta);
        for (int pi = 0; pi < phi_steps; pi++) {
          float phi = (float(pi) + 0.5f) / float(phi_steps) * 2.0f * kPi;
          Vec3 h = Add(Add(Mul(tangent, std::cos(phi) * st),
                           Mul(bitangent, std::sin(phi) * st)),
                       Mul(n, ct));
          float w = ct * st;
          sum = Add(sum, Mul(SampleEnv(env, h), w));
          weight_sum += w;
        }
      }
      out.pixels[size_t(y) * size_t(width) + size_t(x)] = Div(sum, weight_sum);
    }
  }
  return out;
}

EnvImage PrefilterEnvMip(const EnvImage &env, int width, int height,
                         float roughness) {
  EnvImage out;
  out.width = width;
  out.height = height;
  out.pixels.resize(size_t(width) * size_t(height));
  int radius = std::max(0, int(std::round(roughness * 8.0f)));
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float u = (float(x) + 0.5f) / float(width);
      float v = (float(y) + 0.5f) / float(height);
      Vec3 sum{0.0f, 0.0f, 0.0f};
      float weight_sum = 0.0f;
      for (int yy = -radius; yy <= radius; yy++) {
        for (int xx = -radius; xx <= radius; xx++) {
          float du = float(xx) / float(std::max(1, width));
          float dv = float(yy) / float(std::max(1, height));
          float d2 = float(xx * xx + yy * yy);
          float sigma = std::max(1.0f, float(radius) * 0.5f);
          float w = radius == 0 ? 1.0f : std::exp(-d2 / (2.0f * sigma * sigma));
          sum = Add(sum, Mul(SampleEnvNearest(env, u + du, v + dv), w));
          weight_sum += w;
        }
      }
      out.pixels[size_t(y) * size_t(width) + size_t(x)] = Div(sum, weight_sum);
    }
  }
  return out;
}

float RadicalInverseVdc(uint32_t bits) {
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return float(bits) * 2.3283064365386963e-10f;
}

Vec3 ImportanceSampleGGX(float xi0, float xi1, float roughness, const Vec3 &n) {
  constexpr float kPi = 3.14159265358979323846f;
  float a = roughness * roughness;
  float phi = 2.0f * kPi * xi0;
  float cos_theta = std::sqrt((1.0f - xi1) / (1.0f + (a * a - 1.0f) * xi1));
  float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
  Vec3 h{std::cos(phi) * sin_theta, std::sin(phi) * sin_theta, cos_theta};
  Vec3 up = (std::abs(n.z) < 0.999f) ? Vec3{0.0f, 0.0f, 1.0f}
                                     : Vec3{1.0f, 0.0f, 0.0f};
  Vec3 tangent = Normalize(Cross(up, n));
  Vec3 bitangent = Cross(n, tangent);
  return Normalize(Add(Add(Mul(tangent, h.x), Mul(bitangent, h.y)), Mul(n, h.z)));
}

float GeometrySchlickGGX(float ndotv, float roughness) {
  float a = roughness;
  float k = (a * a) / 2.0f;
  return ndotv / std::max(1.0e-6f, ndotv * (1.0f - k) + k);
}

float GeometrySmith(float ndotv, float ndotl, float roughness) {
  return GeometrySchlickGGX(ndotv, roughness) *
         GeometrySchlickGGX(ndotl, roughness);
}

void BuildBrdfLut(int size, IblCache *ibl) {
  if (!ibl || size <= 0) return;
  ibl->brdf_size = size;
  ibl->brdf_lut.assign(size_t(size) * size_t(size) * 2, 0.0f);
  const uint32_t sample_count = 64;
  for (int y = 0; y < size; y++) {
    float roughness = (float(y) + 0.5f) / float(size);
    for (int x = 0; x < size; x++) {
      float ndotv = (float(x) + 0.5f) / float(size);
      Vec3 v{std::sqrt(std::max(0.0f, 1.0f - ndotv * ndotv)), 0.0f, ndotv};
      float a = 0.0f;
      float b = 0.0f;
      Vec3 n{0.0f, 0.0f, 1.0f};
      for (uint32_t i = 0; i < sample_count; i++) {
        Vec3 h = ImportanceSampleGGX(float(i) / float(sample_count),
                                     RadicalInverseVdc(i), roughness, n);
        Vec3 l = Normalize(Sub(Mul(h, 2.0f * Dot(v, h)), v));
        float ndotl = std::max(0.0f, l.z);
        float ndoth = std::max(0.0f, h.z);
        float vdoth = std::max(0.0f, Dot(v, h));
        if (ndotl > 0.0f) {
          float g = GeometrySmith(ndotv, ndotl, roughness);
          float g_vis = (g * vdoth) / std::max(1.0e-6f, ndoth * ndotv);
          float fc = std::pow(1.0f - vdoth, 5.0f);
          a += (1.0f - fc) * g_vis;
          b += fc * g_vis;
        }
      }
      size_t ofs = (size_t(y) * size_t(size) + size_t(x)) * 2;
      ibl->brdf_lut[ofs + 0] = a / float(sample_count);
      ibl->brdf_lut[ofs + 1] = b / float(sample_count);
    }
  }
}

// Build the IBL pyramids (diffuse irradiance + prefiltered specular mips + BRDF
// LUT) from a ready environment map. Shared by the legacy (tydra texture) and
// next (file / dome light) paths.
namespace {
bool g_ibl_envmap_backend = false;
}  // namespace

void SetIblBackendEnvmap(bool enabled) { g_ibl_envmap_backend = enabled; }

#if defined(LUSDR_WITH_TEXTOOLS)
// Vendored envmap-library IBL precompute (-ibl envmap): GGX prefilter,
// irradiance and BRDF LUT computed on cubes, resampled back to the lat-long
// EnvImages the integrator consumes. Same chain shape as the built-in path.
static bool BuildIblFromEnvTextools(EnvImage &&env, IblCache *ibl) {
  em_image src{};
  src.proj = EM_PROJ_EQUIRECT;
  src.width = env.width;
  src.height = env.height;
  src.channels = 3;
  src.faces = 1;
  src.data = &env.pixels[0].x;  // Vec3 = 3 contiguous floats

  auto cube_to_latlong = [](const em_image &cube, int dstW, EnvImage *out) {
    em_image eq{};
    if (!EM_OK(em_convert(nullptr, &cube, EM_PROJ_EQUIRECT, dstW, &eq))) {
      return false;
    }
    out->width = eq.width;
    out->height = eq.height;
    out->pixels.resize(size_t(eq.width) * size_t(eq.height));
    std::memcpy(&out->pixels[0].x, eq.data,
                out->pixels.size() * 3 * sizeof(float));
    em_image_free(nullptr, &eq);
    return true;
  };

  const int levels = 5;
  em_image spec[5];
  for (auto &l : spec) l = em_image{};
  bool ok = EM_OK(em_prefilter_specular(nullptr, &src, 64, levels, 64, spec));
  em_image irr{};
  if (ok) ok = EM_OK(em_irradiance_cube(nullptr, &src, 16, 256, &irr));
  if (ok) {
    ibl->prefiltered.assign(size_t(levels), EnvImage{});
    for (int l = 0; ok && l < levels; ++l) {
      ok = cube_to_latlong(spec[l], std::max(4, 64 >> l),
                           &ibl->prefiltered[size_t(l)]);
    }
  }
  if (ok) ok = cube_to_latlong(irr, 32, &ibl->diffuse);
  for (auto &l : spec) em_image_free(nullptr, &l);
  em_image_free(nullptr, &irr);
  if (!ok) return false;
  // Same [roughness][NdotV] row-major (scale, bias) layout as BuildBrdfLut.
  ibl->brdf_size = 64;
  ibl->brdf_lut.assign(size_t(64) * 64 * 2, 0.0f);
  em_brdf_lut(64, 1024, ibl->brdf_lut.data());
  ibl->env = std::move(env);
  ibl->valid = true;
  return true;
}
#endif  // LUSDR_WITH_TEXTOOLS

bool BuildIblFromEnv(EnvImage &&env, IblCache *ibl) {
  if (!ibl || env.width <= 0 || env.height <= 0 || env.pixels.empty()) {
    return false;
  }
#if defined(LUSDR_WITH_TEXTOOLS)
  if (g_ibl_envmap_backend) {
    return BuildIblFromEnvTextools(std::move(env), ibl);
  }
#else
  (void)g_ibl_envmap_backend;
#endif
  ibl->env = std::move(env);
  ibl->diffuse = ConvolveDiffuseEnv(ibl->env, 32, 16);
  ibl->prefiltered.clear();
  const int levels = 5;
  for (int level = 0; level < levels; level++) {
    int w = std::max(4, 64 >> level);
    int h = std::max(2, 32 >> level);
    float roughness = float(level) / float(levels - 1);
    ibl->prefiltered.push_back(PrefilterEnvMip(ibl->env, w, h, roughness));
  }
  BuildBrdfLut(64, ibl);
  ibl->valid = true;
  return true;
}


// Load a lat-long environment map (HDR float, or 8-bit) from a file into an
// EnvImage, scaled by `scale` (dome intensity * color). 8-bit is treated as
// already-linear (matching DecodeTextureToEnvImage).
bool LoadEnvImageFromFile(const std::string &path, const Vec3 &scale,
                          EnvImage *out) {
  auto res = lightusd::image::LoadImageFromFile(path);
  if (!res) {
    std::cerr << "WARN: failed to load environment map: " << path << "\n";
    return false;
  }
  const lightusd::Image &img = res.value().image;
  if (img.width <= 0 || img.height <= 0 || img.channels <= 0 ||
      img.data.empty()) {
    return false;
  }
  const size_t n = size_t(img.width) * size_t(img.height);
  const size_t ch = size_t(img.channels);
  EnvImage e;
  e.width = img.width;
  e.height = img.height;
  e.pixels.resize(n);
  if (img.format == lightusd::Image::PixelFormat::Float && img.bpp == 32) {
    if (img.data.size() < n * ch * sizeof(float)) return false;
    const float *src = reinterpret_cast<const float *>(img.data.data());
    for (size_t i = 0; i < n; i++) {
      const float *p = src + i * ch;
      e.pixels[i] = Vec3{p[0] * scale.x, p[std::min<size_t>(1, ch - 1)] * scale.y,
                         p[std::min<size_t>(2, ch - 1)] * scale.z};
    }
  } else if (img.bpp == 8) {
    if (img.data.size() < n * ch) return false;
    for (size_t i = 0; i < n; i++) {
      const uint8_t *p = img.data.data() + i * ch;
      e.pixels[i] =
          Vec3{float(p[0]) / 255.0f * scale.x,
               float(p[std::min<size_t>(1, ch - 1)]) / 255.0f * scale.y,
               float(p[std::min<size_t>(2, ch - 1)]) / 255.0f * scale.z};
    }
  } else {
    std::cerr << "WARN: unsupported environment-map format: " << path << "\n";
    return false;
  }
  *out = std::move(e);
  return true;
}

bool LoadBackPlateImage(const std::string &color_path,
                        const std::string &alpha_path,
                        const std::string &depth_path,
                        BackPlateImage *out) {
  if (!out || color_path.empty()) return false;
  auto load = [](const std::string &path, lightusd::Image *image) {
    if (path.empty()) return false;
    auto result = lightusd::image::LoadImageFromFile(path);
    if (!result) return false;
    *image = std::move(result.value().image);
    return image->width > 0 && image->height > 0 && image->channels > 0;
  };
  auto channel = [](const lightusd::Image &image, size_t pixel,
                    size_t component) {
    const size_t ch = size_t(image.channels);
    component = std::min(component, ch - 1);
    if (image.format == lightusd::Image::PixelFormat::Float &&
        image.bpp == 32) {
      const float *data = reinterpret_cast<const float *>(image.data.data());
      return data[pixel * ch + component];
    }
    return float(image.data[pixel * ch + component]) / 255.0f;
  };
  lightusd::Image color;
  if (!load(color_path, &color) ||
      !((color.format == lightusd::Image::PixelFormat::Float &&
         color.bpp == 32) || color.bpp == 8)) {
    std::cerr << "WARN: failed to load BackPlate image: " << color_path << "\n";
    return false;
  }
  BackPlateImage plate;
  plate.width = color.width;
  plate.height = color.height;
  const size_t count = size_t(plate.width) * size_t(plate.height);
  const size_t color_stride = size_t(color.channels) *
      ((color.format == lightusd::Image::PixelFormat::Float &&
        color.bpp == 32) ? sizeof(float) : sizeof(uint8_t));
  if (count > color.data.size() / color_stride) return false;
  plate.color.resize(count);
  const bool srgb = color.format != lightusd::Image::PixelFormat::Float;
  auto linear = [srgb](float value) {
    if (!srgb) return value;
    return value <= 0.04045f ? value / 12.92f
                            : std::pow((value + 0.055f) / 1.055f, 2.4f);
  };
  for (size_t i = 0; i < count; ++i) {
    plate.color[i] = {linear(channel(color, i, 0)),
                      linear(channel(color, i, 1)),
                      linear(channel(color, i, 2))};
    if (color.channels >= 4) {
      if (plate.alpha.empty()) plate.alpha.resize(count);
      plate.alpha[i] = channel(color, i, 3);
    }
  }
  auto load_scalar = [&](const std::string &path, std::vector<float> *dst) {
    if (path.empty()) return true;
    lightusd::Image image;
    if (!load(path, &image) || image.width != plate.width ||
        image.height != plate.height ||
        !((image.format == lightusd::Image::PixelFormat::Float &&
           image.bpp == 32) || image.bpp == 8)) {
      std::cerr << "WARN: BackPlate auxiliary image is invalid or has a "
                   "different resolution: " << path << "\n";
      return false;
    }
    const size_t stride = size_t(image.channels) *
        ((image.format == lightusd::Image::PixelFormat::Float &&
          image.bpp == 32) ? sizeof(float) : sizeof(uint8_t));
    if (count > image.data.size() / stride) return false;
    dst->resize(count);
    for (size_t i = 0; i < count; ++i) (*dst)[i] = channel(image, i, 0);
    return true;
  };
  if (!alpha_path.empty() && !load_scalar(alpha_path, &plate.alpha)) return false;
  if (!load_scalar(depth_path, &plate.depth)) return false;
  *out = std::move(plate);
  return true;
}

Vec3 SampleIblMip(const std::vector<EnvImage> &mips, const Vec3 &dir,
                  float roughness) {
  if (mips.empty()) return Vec3{0.0f, 0.0f, 0.0f};
  float f = ClampFloat(roughness, 0.0f, 1.0f) * float(mips.size() - 1);
  size_t l0 = size_t(std::floor(f));
  size_t l1 = std::min(mips.size() - 1, l0 + 1);
  float t = f - float(l0);
  return Lerp(SampleEnv(mips[l0], dir), SampleEnv(mips[l1], dir), t);
}

void SampleBrdfLut(const IblCache &ibl, float ndotv, float roughness, float *a,
                   float *b) {
  if (!a || !b || ibl.brdf_size <= 0 || ibl.brdf_lut.empty()) {
    if (a) *a = 1.0f;
    if (b) *b = 0.0f;
    return;
  }
  int x = std::min(ibl.brdf_size - 1,
                   std::max(0, int(ClampFloat(ndotv, 0.0f, 1.0f) *
                                   float(ibl.brdf_size))));
  int y = std::min(ibl.brdf_size - 1,
                   std::max(0, int(ClampFloat(roughness, 0.0f, 1.0f) *
                                   float(ibl.brdf_size))));
  size_t ofs = (size_t(y) * size_t(ibl.brdf_size) + size_t(x)) * 2;
  *a = ibl.brdf_lut[ofs + 0];
  *b = ibl.brdf_lut[ofs + 1];
}








}  // namespace lusdr
