// SPDX-License-Identifier: Apache-2.0
//
// lmetalview - preview scene construction.
//
// Loading is deliberately built on the public next C API
// (lightusd_stage_load + lightusd_render_convert), so the example consumes the
// same composed stage / tydra-next conversion surface as every other language
// binding. Pixel decoding is the one thing the C API does not do (tydra records
// texture metadata only), so it runs through tydra::next::TextureDecoder here -
// the same decoder lusdview and lusdrender use.
//
// This file is C++17 and builds with -fno-exceptions like the rest of lightusd;
// every allocation failure is reported as lmv_status instead of unwinding.

#include "lmv.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lightusd-c.h"
#include "lightusd-render-c.h"

#include "next/reader/usdz-reader.hh"
#include "tydra/next/render-data-enums.hh"
#include "tydra/next/usdz-entry-match.hh"
#include "tydra/openpbr-params.hh"
#include "imageio/exr-codec.h"

#include "lmv-ptex.hh"

extern "C" {
#include "texpipe.h"
#include "zstd.h"
}

// Vendored single-header decoder (PNG/JPEG/BMP/TGA/PSD/GIF/PNM + Radiance HDR).
// tydra-next's TextureDecoder would pull the legacy image-loader/imageio stack
// into this example, so a prefix-only viewer decodes its own preview textures.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "external/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static_assert(LMV_PBR_FLOATS == lightusd::tydra::kLightRtOpenPBRFloats,
              "lmv_material_view::pbr must match the canonical OpenPBR block");

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

#if defined(__clang__)
// The message is always a caller-supplied literal; only this wrapper sees it as
// a variable format.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
void SetError(char* err, size_t cap, const char* fmt, ...) {
  if (!err || cap == 0) return;
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(err, cap, fmt, args);
  va_end(args);
  err[cap - 1] = '\0';
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

std::string Sv(lightusd_sv s) {
  return s.data ? std::string(s.data, s.len) : std::string();
}

std::string DirName(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return std::string();
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

lmv_status MapStatus(lightusd_status status) {
  switch (status) {
    case LIGHTUSD_OK: return LMV_OK;
    case LIGHTUSD_ERR_INVALID_ARG: return LMV_ERR_INVALID_ARG;
    case LIGHTUSD_ERR_IO: return LMV_ERR_IO;
    case LIGHTUSD_ERR_PARSE:
    case LIGHTUSD_ERR_TYPE_MISMATCH:
    case LIGHTUSD_ERR_COMPOSITION: return LMV_ERR_PARSE;
    case LIGHTUSD_ERR_NOT_FOUND: return LMV_ERR_NOT_FOUND;
    case LIGHTUSD_ERR_OUT_OF_MEMORY: return LMV_ERR_LIMIT;
    case LIGHTUSD_ERR_UNSUPPORTED: return LMV_ERR_UNSUPPORTED;
    case LIGHTUSD_ERR_RESOURCE_LIMIT: return LMV_ERR_LIMIT;
    case LIGHTUSD_ERR_CANCELLED: return LMV_ERR_CANCELLED;
    default: return LMV_ERR_INTERNAL;
  }
}

struct VertexAttr {
  const float* data = nullptr;
  size_t element_count = 0;
  uint32_t components = 0;
  int interpolation = 2;  // tydra Interpolation::Vertex
  // Maps a triangulated corner to the authored element index. `face` is the
  // authored face the triangle came from (-1 when unknown).
  size_t Index(uint32_t corner, uint32_t point, int32_t face) const {
    switch (interpolation) {
      case 0: return 0;                                       // Constant
      case 1: return face >= 0 ? static_cast<size_t>(face) : 0;  // Uniform
      case 3: return corner;                                  // FaceVarying
      default: return point;                                  // Vertex/Varying
    }
  }
  const float* At(uint32_t corner, uint32_t point, int32_t face) const {
    if (!data || components == 0) return nullptr;
    const size_t index = Index(corner, point, face);
    if (index >= element_count) return nullptr;
    return data + index * components;
  }
};

struct MeshData {
  std::string name;
  std::string prim_path;
  std::vector<lmv_vertex> vertices;
  std::vector<uint32_t> face_ids;
  std::vector<uint32_t> indices;
  std::vector<lmv_subset> subsets;
  int32_t material_id = -1;
  bool double_sided = false;
  bool valid = false;
  float bbox_min[3] = {0.0f, 0.0f, 0.0f};
  float bbox_max[3] = {0.0f, 0.0f, 0.0f};

  // UsdSkel playback: geometry stays in the bind pose and the GPU applies the
  // per-joint palette below. `geom_bind` is row-major (USD row-vector
  // convention) exactly as returned by the C API.
  bool skinned = false;
  std::string mesh_prim_path;
  std::string skeleton_prim_path;
  std::string animation_prim_path;
  double geom_bind[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                          0, 0, 1, 0, 0, 0, 0, 1};
  double mesh_world[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                           0, 0, 1, 0, 0, 0, 0, 1};
  uint32_t influences = 0;
  std::vector<uint16_t> joint_indices;  // 4 per vertex
  std::vector<float> joint_weights;     // 4 per vertex
  // World-space skin palette per joint, row-major (16 floats each). The vertex
  // shader treats it as a column-major float4x4, which transposes it back.
  std::vector<float> skin_matrices;
  // Blend-shape playback: deltas are in bind space, 3 floats per vertex.
  uint32_t morph_targets = 0;
  std::vector<std::string> morph_names;
  std::vector<float> morph_base_weights;
  std::vector<float> morph_weights;
  std::vector<std::vector<float>> morph_deltas;
};

struct DrawData {
  int32_t mesh_id = -1;
  int32_t material_override = -1;
  bool visible = true;
  // Prim path used to re-evaluate the node transform during playback. Point
  // instances keep their baked transform and set dynamic = false.
  std::string prim_path;
  bool dynamic = false;
  // PointInstancer instance: keep the instancer path/index and the source
  // instance TRS so position/orientation/scale animation can be applied
  // relative to the baked prototype transform.
  bool point_instance = false;
  bool point_instance_animated = false;
  bool instance_visibility_animated = false;
  bool proto_index_animated = false;
  bool placeholder_draw = false;
  uint32_t proto_index = 0;
  uint32_t proto_mesh_ref = 0;
  int64_t instance_id = 0;
  // Re-evaluate `visibility` on this prim/ancestors during playback.
  bool visibility_animated = false;
  std::string instancer_prim_path;
  uint32_t instance_index = 0;
  double instance_transform[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                   0, 0, 1, 0, 0, 0, 0, 1};
  float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct MaterialData {
  std::string name;
  std::string prim_path;
  uint32_t shader_type = 0;
  uint32_t alpha_mode = 0;
  uint32_t double_sided = 0;
  uint32_t default_fallback = 0;
  float alpha_cutoff = 0.5f;
  float pbr[LMV_PBR_FLOATS] = {};
  int32_t tex[LMV_TEX_SLOT_COUNT] = {-1, -1, -1, -1, -1, -1, -1};
  float normal_scale = 1.0f;
  float coat_normal_scale = 1.0f;
};

struct UdimTileData {
  uint32_t udim = 1001;
  int32_t image_id = -1;
};

struct TextureData {
  std::string asset_path;
  std::string ktx2_hint;
  int32_t image_id = -1;
  std::vector<UdimTileData> udim_tiles;
  bool is_ptex = false;
  uint32_t ptex_face_count = 0;
  std::vector<lmv_ptex_rect> ptex_rects;
  uint32_t wrap_s = LMV_WRAP_CLAMP;
  uint32_t wrap_t = LMV_WRAP_CLAMP;
  uint32_t channel = 5;  // RGBA
  float uv_offset[2] = {0.0f, 0.0f};
  float uv_scale[2] = {1.0f, 1.0f};
  float uv_rotation = 0.0f;
  float bias[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  uint32_t uv_set = 0;
};

struct ImageLevelData {
  std::vector<uint8_t> data;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct ImageData {
  std::string resolved_path;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;
  bool is_float = false;
  bool srgb = false;
  uint32_t compressed_format = 0;  // lmv_compressed_format
  std::shared_ptr<std::vector<ImageLevelData>> levels;
  std::shared_ptr<std::vector<lmv_image_level>> level_views;
  std::shared_ptr<const std::vector<uint8_t>> rgba;
  std::shared_ptr<const std::vector<float>> rgb;

  size_t byte_size() const {
    if (levels) {
      size_t n = 0;
      for (const ImageLevelData& level : *levels) n += level.data.size();
      return n;
    }
    const size_t n = rgba ? rgba->size() : (rgb ? rgb->size() * sizeof(float) : 0);
    return n;
  }
  bool valid() const {
    if (compressed_format != 0) return levels && !levels->empty();
    return width > 0 && height > 0 &&
           ((rgba && !rgba->empty()) || (rgb && !rgb->empty()));
  }
};

struct LightData {
  std::string name;
  std::string prim_path;
  uint32_t type = LMV_LIGHT_POINT;
  float color[3] = {1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  float exposure = 0.0f;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float param0 = 0.0f;
  float param1 = 0.0f;
  int32_t dome_texture_id = -1;
  // tydra stores an *image* id in the dome light params (despite the C API
  // comment that says texture index); keep it so the environment image is
  // decoded and viewers can find it.
  int32_t dome_image_id = -1;
};

struct CameraData {
  std::string name;
  std::string prim_path;
  uint32_t orthographic = 0;
  float focal_length = 50.0f;
  float horizontal_aperture = 36.0f;
  float vertical_aperture = 24.0f;
  float ortho_width = 10.0f;
  float near_clip = 0.1f;
  float far_clip = 10000.0f;
  float focus_distance = 5.0f;
  float exposure = 0.0f;
  float fstop = 0.0f;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct SceneData {
  struct UdimImageSource {
    std::string path;
    bool srgb = true;
  };
  std::vector<MeshData> meshes;
  std::vector<DrawData> draws;
  std::vector<MaterialData> materials;
  std::vector<TextureData> textures;
  std::vector<std::shared_ptr<const ImageData>> images;
  // Images discovered by UDIM probing (they have no lightusd render-image record).
  std::unordered_map<int32_t, UdimImageSource> udim_image_sources;
  std::vector<LightData> lights;
  std::vector<CameraData> cameras;
  std::vector<lmv_splat> splats;
  std::vector<lmv_line_segment> lines;
  std::vector<std::string> warnings;
  struct InstancerPlayback {
    std::vector<uint32_t> proto_mesh_offsets;
    std::vector<int32_t> proto_mesh_ids;
    std::vector<float> proto_mesh_transforms;
    size_t instance_count = 0;
    size_t max_instance_count = 0;
    bool proto_indices_animated = false;
    bool count_animated = false;
  };
  std::unordered_map<std::string, InstancerPlayback> instancers;
  // Set when animation lives in data the fast path does not own (animated
  // material inputs, geometry attributes, light/camera parameters, instance
  // masks). Playback then falls back to a full tydra re-conversion.
  bool force_reconversion = false;
  float working_to_display[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool bounds_valid = false;
  float bbox_min[3] = {-1.0f, -1.0f, -1.0f};
  float bbox_max[3] = {1.0f, 1.0f, 1.0f};
};

// Decoded-image cache shared across re-conversions (animation scrubbing) so a
// time change never re-decodes a texture.
struct ImageCache {
  std::unordered_map<std::string, std::shared_ptr<const ImageData>> by_key;
  std::unique_ptr<lightusd::next::USDZReader> usdz;
  std::string base_dir;
  uint32_t max_edge = 2048;
  uint64_t max_bytes = 512ull << 20;
  uint64_t resident_bytes = 0;
};

void CopyMatrix4(const float* src, float* dst) {
  // USD matrices transform row vectors (p' = p * M) with translation in the
  // last row. Metal transforms column vectors (p' = M * p) and stores
  // column-major, so the geometric conversion is a transpose that exactly
  // cancels the storage-order difference: an element-wise copy.
  std::memcpy(dst, src, 16 * sizeof(float));
}

bool ProgressCancelled(const lmv_load_options& opts) {
  if (!opts.progress) return false;
  lmv_progress p{};
  p.phase = LMV_PHASE_CONVERT;
  p.fraction = 0.0f;
  p.message = "converting";
  return opts.progress(opts.progress_user, &p) == 0;
}

// ---------------------------------------------------------------------------
// Buffer / attribute access
// ---------------------------------------------------------------------------

template <typename T>
bool ReadMeshBuffer(lightusd_render_scene* scene, int32_t mesh_id, uint8_t kind,
                    std::vector<T>* out) {
  out->clear();
  lightusd_buffer_view view{};
  if (lightusd_render_mesh_buffer(scene, mesh_id, kind, &view) != LIGHTUSD_OK) {
    return false;
  }
  if (!view.data || view.nbytes == 0) return true;
  if (view.nbytes % sizeof(T) != 0) return false;
  const T* data = static_cast<const T*>(view.data);
  const size_t count = view.nbytes / sizeof(T);
  out->assign(data, data + count);
  return true;
}

VertexAttr MakeAttr(const std::vector<float>& data, uint32_t components,
                    int interpolation) {
  VertexAttr a;
  if (data.empty() || components == 0) return a;
  a.data = data.data();
  a.element_count = data.size() / components;
  a.components = components;
  a.interpolation = interpolation;
  return a;
}

// Builds a triangle -> authored-face map from the face prefix sums. Returns an
// empty vector when the mapping is unavailable.
std::vector<int32_t> BuildTriangleFaces(const std::vector<uint32_t>& offsets,
                                       uint32_t triangle_count) {
  std::vector<int32_t> faces;
  if (offsets.size() < 2) return faces;
  const size_t face_count = offsets.size() - 1;
  faces.assign(triangle_count, -1);
  for (size_t face = 0; face < face_count; ++face) {
    const uint32_t begin = offsets[face];
    const uint32_t end = std::min<uint32_t>(offsets[face + 1], triangle_count);
    for (uint32_t t = begin; t < end; ++t) faces[t] = static_cast<int32_t>(face);
  }
  return faces;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct CornerKey {
  uint32_t point;
  uint32_t normal;
  uint32_t uv;
  uint32_t uv1;
  uint32_t color;
  uint32_t opacity;
  uint32_t face;  // Ptex face index; 0 when faces are not welded separately

  bool operator==(const CornerKey& other) const {
    return point == other.point && normal == other.normal && uv == other.uv &&
           uv1 == other.uv1 && color == other.color &&
           opacity == other.opacity && face == other.face;
  }
};

struct CornerKeyHash {
  size_t operator()(const CornerKey& k) const {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) {
      h ^= v;
      h *= 1099511628211ull;
    };
    mix(k.point);
    mix(k.normal);
    mix(k.uv);
    mix(k.uv1);
    mix(k.color);
    mix(k.opacity);
    mix(k.face);
    return static_cast<size_t>(h);
  }
};

size_t AttrElement(const VertexAttr& a, uint32_t corner, uint32_t point,
                   int32_t face) {
  return a.data ? a.Index(corner, point, face) : 0;
}

// ---------------------------------------------------------------------------
// UsdSkel skinning + blend shapes (GPU playback)
//
// Geometry stays in the bind pose. Per-vertex joint influences, the per-joint
// pose palette and blend-shape deltas are retained here and re-evaluated when
// the time changes, so playback never re-converts or re-uploads geometry. The
// math follows examples/lusdview/next_scene_loader.cc (PoseNextSkeleton) but
// uses only the public next C API: lightusd_skel_sample_*, lightusd_attr_eval
// and lightusd_prim_world_transform.
// ---------------------------------------------------------------------------

struct Mat4d {
  double m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

struct Quatd {
  double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

Mat4d Identity4d() { return Mat4d{}; }

Mat4d Mul4d(const Mat4d& a, const Mat4d& b) {
  Mat4d out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += a.m[r][k] * b.m[k][c];
      out.m[r][c] = sum;
    }
  }
  return out;
}

bool Invert4d(const Mat4d& in, Mat4d* out) {
  if (!out) return false;
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = in.m[r][c];
      a[r][c + 4] = (r == c) ? 1.0 : 0.0;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
    }
    if (std::fabs(a[pivot][col]) < 1e-12) return false;
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
    }
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double factor = a[r][col];
      if (factor == 0.0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= factor * a[col][c];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out->m[r][c] = a[r][c + 4];
  }
  return true;
}

Mat4d LoadRowMajor16d(const double* v) {
  Mat4d out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out.m[r][c] = v[r * 4 + c];
  }
  return out;
}

Mat4d LoadRowMajor16f(const float* v) {
  Mat4d out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out.m[r][c] = static_cast<double>(v[r * 4 + c]);
  }
  return out;
}

void ToColumnMatrix(const Quatd& q, double m[3][3]) {
  const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  m[0][0] = 1.0 - 2.0 * (yy + zz);
  m[0][1] = 2.0 * (xy - wz);
  m[0][2] = 2.0 * (xz + wy);
  m[1][0] = 2.0 * (xy + wz);
  m[1][1] = 1.0 - 2.0 * (xx + zz);
  m[1][2] = 2.0 * (yz - wx);
  m[2][0] = 2.0 * (xz - wy);
  m[2][1] = 2.0 * (yz + wx);
  m[2][2] = 1.0 - 2.0 * (xx + yy);
}

Quatd QuatFromColumnMatrix(const double m[3][3]) {
  Quatd q;
  const double trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    q.w = 0.25 * s;
    q.x = (m[2][1] - m[1][2]) / s;
    q.y = (m[0][2] - m[2][0]) / s;
    q.z = (m[1][0] - m[0][1]) / s;
  } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
    q.w = (m[2][1] - m[1][2]) / s;
    q.x = 0.25 * s;
    q.y = (m[0][1] + m[1][0]) / s;
    q.z = (m[0][2] + m[2][0]) / s;
  } else if (m[1][1] > m[2][2]) {
    const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
    q.w = (m[0][2] - m[2][0]) / s;
    q.x = (m[0][1] + m[1][0]) / s;
    q.y = 0.25 * s;
    q.z = (m[1][2] + m[2][1]) / s;
  } else {
    const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
    q.w = (m[1][0] - m[0][1]) / s;
    q.x = (m[0][2] + m[2][0]) / s;
    q.y = (m[1][2] + m[2][1]) / s;
    q.z = 0.25 * s;
  }
  const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len > 1e-12) {
    q.x /= len;
    q.y /= len;
    q.z /= len;
    q.w /= len;
  } else {
    q = Quatd{};
  }
  return q;
}

// Row-vector affine matrix -> translation, rotation, scale.
bool Decompose4d(const Mat4d& m, double t[3], Quatd* q, double s[3]) {
  if (!q) return false;
  for (int i = 0; i < 3; ++i) t[i] = m.m[3][i];
  for (int r = 0; r < 3; ++r) {
    s[r] = std::sqrt(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] +
                     m.m[r][2] * m.m[r][2]);
    if (s[r] < 1e-12) s[r] = 1.0;
  }
  double col[3][3];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) col[c][r] = m.m[r][c] / s[r];
  }
  *q = QuatFromColumnMatrix(col);
  return true;
}

Mat4d Compose4d(const double t[3], const Quatd& q, const double s[3]) {
  double rot[3][3];
  ToColumnMatrix(q, rot);
  Mat4d out;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) out.m[r][c] = rot[c][r] * s[r];
    out.m[r][3] = 0.0;
  }
  out.m[3][0] = t[0];
  out.m[3][1] = t[1];
  out.m[3][2] = t[2];
  out.m[3][3] = 1.0;
  return out;
}

// Row-vector translation concatenation: world[j] = local[j] * world[parent].
void ConcatJointWorlds(const std::vector<int32_t>& parents,
                       const std::vector<Mat4d>& local,
                       std::vector<Mat4d>* world) {
  const size_t n = local.size();
  world->assign(n, Identity4d());
  std::vector<char> done(n, 0);
  for (size_t pass = 0; pass <= n; ++pass) {
    bool progress = false;
    for (size_t j = 0; j < n; ++j) {
      if (done[j]) continue;
      const int32_t p = j < parents.size() ? parents[j] : -1;
      if (p < 0) {
        (*world)[j] = local[j];
        done[j] = 1;
        progress = true;
      } else if (static_cast<size_t>(p) < n && done[static_cast<size_t>(p)]) {
        (*world)[j] = Mul4d(local[j], (*world)[static_cast<size_t>(p)]);
        done[j] = 1;
        progress = true;
      }
    }
    if (!progress) break;
  }
}

lightusd_prim PrimAtPath(lightusd_stage* stage, const std::string& path) {
  if (!stage || path.empty()) return lightusd_prim{};
  return lightusd_stage_prim_at_path(stage, path.c_str());
}

bool PrimWorld(lightusd_stage* stage, lightusd_prim prim, double time,
               Mat4d* out) {
  double m[16] = {0};
  if (!lightusd_prim_is_valid(prim) ||
      lightusd_prim_world_transform(stage, prim, time, m) != LIGHTUSD_OK) {
    return false;
  }
  *out = LoadRowMajor16d(m);
  return true;
}

// Bound animation prim for a skeleton (mesh-level skel:animationSource wins,
// then the skeleton's own animation source).
lightusd_prim ResolveAnimationPrim(lightusd_stage* stage,
                                   lightusd_render_scene* render,
                                   int32_t skeleton_id) {
  lightusd_render_skeleton_info info{};
  if (!render ||
      lightusd_render_skeleton_get_info(render, skeleton_id, &info) !=
          LIGHTUSD_OK) {
    return lightusd_prim{};
  }
  const std::string source = Sv(info.animation_source_path);
  if (!source.empty()) {
    lightusd_prim prim = PrimAtPath(stage, source);
    if (lightusd_prim_is_valid(prim)) return prim;
  }
  if (info.animation_id >= 0) {
    lightusd_render_animation_info anim{};
    if (lightusd_render_animation_get_info(render, info.animation_id, &anim) ==
        LIGHTUSD_OK) {
      lightusd_prim prim = PrimAtPath(stage, Sv(anim.prim_path));
      if (lightusd_prim_is_valid(prim)) return prim;
    }
  }
  return lightusd_prim{};
}

// Ports PoseNextSkeleton from lusdview. Returns the per-joint skin matrix that
// takes a geomBind-space point into the posed skeleton space.
bool PoseSkeleton(lightusd_stage* stage, lightusd_prim skeleton,
                  lightusd_prim animation, double time,
                  std::vector<Mat4d>* skin_matrices) {
  if (!stage || !skin_matrices || !lightusd_prim_is_valid(skeleton)) return false;
  lightusd_skel_sample* sample = nullptr;
  if (lightusd_skel_sample_create(stage, skeleton, animation, time, &sample) !=
      LIGHTUSD_OK) {
    return false;
  }
  lightusd_skel_sample_info info{};
  const bool ok =
      lightusd_skel_sample_get_info(sample, &info) == LIGHTUSD_OK && info.joint_count > 0 &&
      info.parent_indices != nullptr;
  if (!ok) {
    lightusd_skel_sample_destroy(sample);
    return false;
  }
  const size_t nj = info.joint_count;
  std::vector<int32_t> parents(info.parent_indices, info.parent_indices + nj);

  bool translation_only = false;
  if (nj >= 512) {
    size_t roots = 0;
    size_t root_children = 0;
    for (int32_t parent : parents) {
      if (parent < 0) ++roots;
      else if (parent == 0) ++root_children;
    }
    translation_only = nj >= 1024 || root_children + 1 == nj ||
                       (roots == 1 && root_children + 1 == nj);
  }

  const bool have_rest = info.rest_transform_count == nj * 16;
  const bool have_bind = info.bind_transform_count == nj * 16;
  std::vector<Mat4d> rest_local(nj), bind_world(nj);
  for (size_t j = 0; j < nj; ++j) {
    rest_local[j] = have_rest ? LoadRowMajor16d(&info.rest_transforms[j * 16])
                              : Identity4d();
    bind_world[j] = have_bind ? LoadRowMajor16d(&info.bind_transforms[j * 16])
                              : Identity4d();
  }

  std::vector<Mat4d> local = rest_local;
  if (info.animation_joint_count != 0) {
    std::unordered_map<std::string, int> joint_index;
    for (size_t j = 0; j < nj; ++j) {
      lightusd_sv name{};
      if (lightusd_skel_sample_joint_name(sample, j, &name) == LIGHTUSD_OK) {
        joint_index[std::string(name.data, name.len)] = static_cast<int>(j);
      }
    }
    for (size_t a = 0; a < info.animation_joint_count; ++a) {
      lightusd_sv name{};
      if (lightusd_skel_sample_animation_joint_name(sample, a, &name) !=
          LIGHTUSD_OK) {
        continue;
      }
      auto it = joint_index.find(std::string(name.data, name.len));
      if (it == joint_index.end()) continue;
      const size_t j = static_cast<size_t>(it->second);
      double t[3] = {0, 0, 0};
      double s[3] = {1, 1, 1};
      Quatd q;
      Decompose4d(rest_local[j], t, &q, s);
      if (info.has_translations && (a + 1) * 3 <= info.translation_count) {
        t[0] = info.translations[a * 3 + 0];
        t[1] = info.translations[a * 3 + 1];
        t[2] = info.translations[a * 3 + 2];
      }
      if (!translation_only && info.has_rotations &&
          (a + 1) * 4 <= info.rotation_count) {
        q.w = info.rotations[a * 4 + 0];
        q.x = info.rotations[a * 4 + 1];
        q.y = info.rotations[a * 4 + 2];
        q.z = info.rotations[a * 4 + 3];
        const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z +
                                     q.w * q.w);
        if (len < 1e-12) {
          q = Quatd{};
        } else {
          q.x /= len;
          q.y /= len;
          q.z /= len;
          q.w /= len;
        }
      }
      if (!translation_only && info.has_scales &&
          (a + 1) * 3 <= info.scale_count) {
        s[0] = info.scales[a * 3 + 0];
        s[1] = info.scales[a * 3 + 1];
        s[2] = info.scales[a * 3 + 2];
      }
      local[j] = Compose4d(t, q, s);
    }
  }

  std::vector<Mat4d> world;
  ConcatJointWorlds(parents, local, &world);
  if (!have_bind || translation_only) {
    std::vector<Mat4d> rest_world;
    ConcatJointWorlds(parents, rest_local, &rest_world);
    if (rest_world.size() == nj) bind_world = std::move(rest_world);
  }

  skin_matrices->assign(nj, Identity4d());
  for (size_t j = 0; j < nj; ++j) {
    Mat4d inv_bind;
    if (!Invert4d(bind_world[j], &inv_bind)) {
      lightusd_skel_sample_destroy(sample);
      return false;
    }
    (*skin_matrices)[j] = Mul4d(inv_bind, world[j]);
  }
  lightusd_skel_sample_destroy(sample);
  return true;
}

struct SkinBindingData {
  std::vector<Mat4d> matrices;  // geomBind * meshLocalSkin (per joint)
  Mat4d geom_bind;
  Mat4d inv_geom_bind;
  Mat4d mesh_world;
  std::string mesh_prim_path;
  std::string skeleton_prim_path;
  std::string animation_prim_path;
  std::vector<uint16_t> joint_indices;  // per point * influences
  std::vector<float> joint_weights;     // per point * influences
  uint32_t influences = 0;
  bool valid = false;
};

bool ResolveSkinBinding(lightusd_stage* stage, lightusd_render_scene* render,
                        int32_t mesh_id, const std::string& mesh_prim_path,
                        double time, SkinBindingData* out) {
  if (!stage || !render || !out) return false;
  lightusd_render_mesh_info info{};
  if (lightusd_render_mesh_get_info(render, mesh_id, &info) != LIGHTUSD_OK ||
      !info.has_skin) {
    return false;
  }
  lightusd_render_mesh_skin_info skin{};
  if (lightusd_render_mesh_skin_binding(render, mesh_id, &skin) != LIGHTUSD_OK ||
      skin.skeleton_id < 0 || skin.influences_per_vertex == 0) {
    return false;
  }
  lightusd_render_skeleton_info skeleton_info{};
  if (lightusd_render_skeleton_get_info(render, skin.skeleton_id,
                                        &skeleton_info) != LIGHTUSD_OK) {
    return false;
  }
  lightusd_prim mesh_prim = PrimAtPath(stage, mesh_prim_path);
  lightusd_prim skeleton_prim = PrimAtPath(stage, Sv(skeleton_info.prim_path));
  if (!lightusd_prim_is_valid(mesh_prim) ||
      !lightusd_prim_is_valid(skeleton_prim)) {
    return false;
  }
  const lightusd_prim animation = ResolveAnimationPrim(stage, render,
                                                       skin.skeleton_id);

  std::vector<Mat4d> skin_matrices;
  if (!PoseSkeleton(stage, skeleton_prim, animation, time, &skin_matrices)) {
    return false;
  }

  Mat4d mesh_world, skeleton_world;
  if (!PrimWorld(stage, mesh_prim, time, &mesh_world) ||
      !PrimWorld(stage, skeleton_prim, time, &skeleton_world)) {
    return false;
  }
  const Mat4d geom_bind = LoadRowMajor16d(skin.geom_bind_transform);
  Mat4d inv_geom_bind;
  if (!Invert4d(geom_bind, &inv_geom_bind)) return false;
  out->geom_bind = geom_bind;
  out->mesh_prim_path = mesh_prim_path;
  out->skeleton_prim_path = Sv(skeleton_info.prim_path);
  if (lightusd_prim_is_valid(animation)) {
    out->animation_prim_path = Sv(lightusd_prim_path(animation));
  }
  out->mesh_world = mesh_world;
  Mat4d inv_mesh_world;
  if (!Invert4d(mesh_world, &inv_mesh_world)) return false;
  const Mat4d skeleton_to_mesh =
      Mul4d(Mul4d(skeleton_world, inv_mesh_world), geom_bind);

  out->matrices.resize(skin_matrices.size());
  for (size_t j = 0; j < skin_matrices.size(); ++j) {
    const Mat4d mesh_local_skin = Mul4d(skin_matrices[j], skeleton_to_mesh);
    out->matrices[j] = Mul4d(geom_bind, mesh_local_skin);
  }
  out->inv_geom_bind = inv_geom_bind;

  // Influence arrays (per POINT, already remapped to skeleton joint order).
  lightusd_buffer_view indices_view{};
  lightusd_buffer_view weights_view{};
  if (lightusd_render_mesh_buffer(render, mesh_id,
                                  LIGHTUSD_MESH_BUF_JOINT_INDICES,
                                  &indices_view) != LIGHTUSD_OK ||
      lightusd_render_mesh_buffer(render, mesh_id,
                                  LIGHTUSD_MESH_BUF_JOINT_WEIGHTS,
                                  &weights_view) != LIGHTUSD_OK ||
      !indices_view.data || !weights_view.data || indices_view.nbytes == 0 ||
      indices_view.nbytes % sizeof(uint16_t) != 0 ||
      weights_view.nbytes % sizeof(float) != 0) {
    return false;
  }
  out->influences = skin.influences_per_vertex;
  if (indices_view.components != out->influences ||
      weights_view.components != out->influences) {
    return false;
  }
  const size_t index_count = indices_view.nbytes / sizeof(uint16_t);
  const size_t weight_count = weights_view.nbytes / sizeof(float);
  if (index_count != weight_count || index_count % out->influences != 0) {
    return false;
  }
  const uint16_t* indices =
      static_cast<const uint16_t*>(indices_view.data);
  const float* weights = static_cast<const float*>(weights_view.data);
  out->joint_indices.assign(indices, indices + index_count);
  out->joint_weights.assign(weights, weights + weight_count);
  out->valid = true;
  return true;
}

// Blend-shape weights at `time`, keyed by target name.
void EvaluateBlendShapeWeights(lightusd_stage* stage, lightusd_prim animation,
                               double time,
                               std::unordered_map<std::string, float>* out) {
  if (!out || !lightusd_prim_is_valid(animation)) return;
  lightusd_value* shapes = nullptr;
  lightusd_value* weights = nullptr;
  std::vector<std::string> names;
  std::vector<float> values;
  if (lightusd_attr_eval(stage, animation, "blendShapes", time, &shapes) ==
          LIGHTUSD_OK &&
      shapes) {
    lightusd_strlist* list = nullptr;
    if (lightusd_value_get_token_array(shapes, &list) == LIGHTUSD_OK && list) {
      for (size_t i = 0; i < lightusd_strlist_size(list); ++i) {
        const lightusd_sv sv = lightusd_strlist_get(list, i);
        names.emplace_back(sv.data ? sv.data : "", sv.len);
      }
      lightusd_strlist_destroy(list);
    }
  }
  if (lightusd_attr_eval(stage, animation, "blendShapeWeights", time,
                         &weights) == LIGHTUSD_OK &&
      weights) {
    lightusd_value_view view{};
    if (lightusd_value_get_view(weights, &view) == LIGHTUSD_OK && view.data &&
        view.storage == LIGHTUSD_COMP_FLOAT32 && view.components == 1) {
      const float* src = static_cast<const float*>(view.data);
      values.assign(src, src + view.count);
    }
  }
  for (size_t i = 0; i < names.size() && i < values.size(); ++i) {
    (*out)[names[i]] = values[i];
  }
  if (weights) lightusd_value_destroy(weights);
  if (shapes) lightusd_value_destroy(shapes);
}

void ExtractBlendShapes(lightusd_stage* stage, lightusd_render_scene* render,
                        int32_t mesh_id, const std::string& mesh_prim_path,
                        double time, const std::vector<uint32_t>& vertex_to_point,
                        MeshData* out) {
  if (!stage || !render || !out) return;
  lightusd_render_mesh_info info{};
  if (lightusd_render_mesh_get_info(render, mesh_id, &info) != LIGHTUSD_OK ||
      info.blend_shape_count == 0) {
    return;
  }
  std::unordered_map<std::string, float> animated_weights;
  lightusd_render_mesh_skin_info skin{};
  lightusd_prim animation{};
  if (lightusd_render_mesh_skin_binding(render, mesh_id, &skin) == LIGHTUSD_OK &&
      skin.skeleton_id >= 0) {
    animation = ResolveAnimationPrim(stage, render, skin.skeleton_id);
  }
  if (!lightusd_prim_is_valid(animation)) {
    animation = PrimAtPath(stage, mesh_prim_path);
  }
  EvaluateBlendShapeWeights(stage, animation, time, &animated_weights);
  if (!out->animation_prim_path.empty()) {
    // Keep the skin's animation source; otherwise animate from the mesh prim.
  } else if (lightusd_prim_is_valid(animation)) {
    out->animation_prim_path = Sv(lightusd_prim_path(animation));
  }

  const size_t vertex_count = out->vertices.size();
  if (vertex_to_point.size() != vertex_count) return;
  for (size_t bs = 0; bs < info.blend_shape_count; ++bs) {
    lightusd_sv name{};
    float static_weight = 0.0f;
    lightusd_buffer_view offsets{};
    if (lightusd_render_mesh_blendshape(render, mesh_id, bs, 0, &name,
                                        &static_weight, &offsets) !=
            LIGHTUSD_OK ||
        !offsets.data || offsets.components != 3) {
      continue;
    }
    const std::string target_name(name.data ? name.data : "", name.len);
    lightusd_buffer_view indices{};
    const bool sparse =
        lightusd_render_mesh_blendshape_indices(render, mesh_id, bs, &indices) ==
            LIGHTUSD_OK &&
        indices.data && indices.nbytes > 0;
    const float* offset = static_cast<const float*>(offsets.data);
    const size_t offset_count = offsets.nbytes / sizeof(float) / 3;
    std::vector<uint32_t> point_to_offset;
    if (sparse) {
      if (indices.nbytes % sizeof(uint32_t) != 0) continue;
      const uint32_t* index = static_cast<const uint32_t*>(indices.data);
      const size_t count = indices.nbytes / sizeof(uint32_t);
      uint32_t max_point = 0;
      for (size_t k = 0; k < count; ++k) {
        max_point = std::max(max_point, index[k]);
      }
      point_to_offset.assign(static_cast<size_t>(max_point) + 1,
                             std::numeric_limits<uint32_t>::max());
      for (size_t k = 0; k < count; ++k) {
        point_to_offset[index[k]] = static_cast<uint32_t>(k);
      }
    }
    std::vector<float> deltas(vertex_count * 3, 0.0f);
    for (size_t v = 0; v < vertex_count; ++v) {
      const uint32_t point = vertex_to_point[v];
      size_t offset_index = point;
      if (sparse) {
        if (point >= point_to_offset.size()) continue;
        offset_index = point_to_offset[point];
        if (offset_index == std::numeric_limits<uint32_t>::max()) continue;
      }
      if (offset_index >= offset_count) continue;
      deltas[v * 3 + 0] = offset[offset_index * 3 + 0];
      deltas[v * 3 + 1] = offset[offset_index * 3 + 1];
      deltas[v * 3 + 2] = offset[offset_index * 3 + 2];
    }
    out->morph_names.push_back(target_name);
    out->morph_base_weights.push_back(static_weight);
    auto it = animated_weights.find(target_name);
    out->morph_weights.push_back(it != animated_weights.end() ? it->second
                                                              : static_weight);
    out->morph_deltas.push_back(std::move(deltas));
  }
  out->morph_targets = static_cast<uint32_t>(out->morph_names.size());
}

bool LocalVisible(lightusd_stage* stage, lightusd_prim prim, double time) {
  lightusd_value* value = nullptr;
  if (lightusd_attr_eval(stage, prim, "visibility", time, &value) !=
          LIGHTUSD_OK ||
      !value) {
    return true;
  }
  bool visible = true;
  lightusd_sv text{};
  if (lightusd_value_get_string(value, &text) == LIGHTUSD_OK && text.data &&
      text.len == 9 && std::memcmp(text.data, "invisible", 9) == 0) {
    visible = false;
  }
  lightusd_value_destroy(value);
  return visible;
}

bool PrimVisibleAt(lightusd_stage* stage, lightusd_prim prim, double time) {
  for (int depth = 0; depth < 256 && lightusd_prim_is_valid(prim); ++depth) {
    if (!LocalVisible(stage, prim, time)) return false;
    prim = lightusd_prim_parent(prim);
  }
  return true;
}

bool HasAnimatedVisibility(lightusd_stage* stage, lightusd_prim prim) {
  for (int depth = 0; depth < 256 && lightusd_prim_is_valid(prim); ++depth) {
    if (lightusd_attr_has_timesamples(prim, "visibility")) return true;
    prim = lightusd_prim_parent(prim);
  }
  (void)stage;
  return false;
}

bool SkipAnimationProperty(const std::string& name,
                           const std::vector<std::string>& prefixes,
                           const std::vector<std::string>& exact = {}) {
  for (const std::string& value : exact) {
    if (name == value) return true;
  }
  for (const std::string& prefix : prefixes) {
    if (name == prefix || name.compare(0, prefix.size(), prefix) == 0) {
      return true;
    }
  }
  return false;
}

bool PrimOrAncestorHasTimeSamples(
    lightusd_stage* stage, lightusd_prim prim,
    const std::vector<std::string>& prefixes) {
  for (int depth = 0; depth < 256 && lightusd_prim_is_valid(prim); ++depth) {
    lightusd_strlist* names = nullptr;
    if (lightusd_prim_property_names(prim, &names) == LIGHTUSD_OK && names) {
      const size_t count = lightusd_strlist_size(names);
      for (size_t i = 0; i < count; ++i) {
        const lightusd_sv text = lightusd_strlist_get(names, i);
        const std::string name(text.data ? text.data : "", text.len);
        if (!SkipAnimationProperty(name, prefixes)) continue;
        if (lightusd_attr_has_timesamples(prim, name.c_str())) {
          lightusd_strlist_destroy(names);
          return true;
        }
      }
      lightusd_strlist_destroy(names);
    }
    prim = lightusd_prim_parent(prim);
  }
  (void)stage;
  return false;
}

bool SubtreeHasUnsupportedTimeSamples(
    lightusd_stage* stage, lightusd_prim prim,
    const std::vector<std::string>& skip,
    const std::vector<std::string>& exact = {}) {
  if (!stage || !lightusd_prim_is_valid(prim)) return false;
  std::vector<lightusd_prim> stack;
  stack.push_back(prim);
  while (!stack.empty()) {
    const lightusd_prim current = stack.back();
    stack.pop_back();
    lightusd_strlist* names = nullptr;
    if (lightusd_prim_property_names(current, &names) == LIGHTUSD_OK && names) {
      const size_t count = lightusd_strlist_size(names);
      for (size_t i = 0; i < count; ++i) {
        const lightusd_sv text = lightusd_strlist_get(names, i);
        const std::string name(text.data ? text.data : "", text.len);
        if (SkipAnimationProperty(name, skip, exact)) continue;
        if (lightusd_attr_has_timesamples(current, name.c_str())) {
          lightusd_strlist_destroy(names);
          return true;
        }
      }
      lightusd_strlist_destroy(names);
    }
    const size_t child_count = lightusd_prim_child_count(current);
    for (size_t i = 0; i < child_count; ++i) {
      const lightusd_prim child = lightusd_prim_child(current, i);
      if (lightusd_prim_is_valid(child)) stack.push_back(child);
    }
  }
  return false;
}

// Returns true when a time-sampled channel exists that the in-place path does
// not own. Such scenes are re-converted at each time change (correctness over
// speed) instead of silently ignoring the animation.
bool DetectUnsupportedAnimation(lightusd_stage* stage, double time) {
  if (!stage) return false;
  lightusd_render_prim_catalog* catalog = nullptr;
  if (lightusd_render_prim_catalog_create(stage, time, 0, 0, 0, &catalog) !=
          LIGHTUSD_OK ||
      !catalog) {
    return false;
  }
  bool found = false;
  const auto scan = [&](uint8_t kind, const std::vector<std::string>& skip,
                        const std::vector<std::string>& exact = {},
                        const std::vector<std::string>& ancestor_prefixes = {}) {
    if (found) return;
    const size_t count = lightusd_render_prim_catalog_count(catalog, kind);
    for (size_t i = 0; i < count && !found; ++i) {
      lightusd_render_prim_info info{};
      info.struct_size = sizeof(info);
      if (lightusd_render_prim_catalog_get(catalog, kind, i, &info) !=
          LIGHTUSD_OK) {
        continue;
      }
      const std::string path(info.path.data ? info.path.data : "", info.path.len);
      const lightusd_prim prim = PrimAtPath(stage, path);
      if (SubtreeHasUnsupportedTimeSamples(stage, prim, skip, exact)) {
        found = true;
      } else if (!ancestor_prefixes.empty() &&
                 PrimOrAncestorHasTimeSamples(stage, prim, ancestor_prefixes)) {
        found = true;
      }
    }
  };

  const std::vector<std::string> geometry_skip = {
      "xformOp", "visibility", "skel:", "primvars:skel", "blendShape",
      "extent"};
  const std::vector<std::string> nonmesh_geometry_skip = {
      "skel:", "primvars:skel", "blendShape", "extent"};
  const std::vector<std::string> nonmesh_ancestor = {
      "xformOp", "visibility", "purpose"};
  const std::vector<std::string> instancer_skip = {
      "xformOp", "visibility", "positions", "orientations", "scales",
      "velocities", "angularVelocities"};
  const std::vector<std::string> instancer_exact = {
      "ids", "invisibleIds", "inactiveIds", "protoIndices"};
  const std::vector<std::string> transform_skip = {"xformOp", "visibility"};
  const std::vector<std::string> light_transform_skip = {"xformOp", "visibility"};
  const std::vector<std::string> light_scalar_exact = {
      "inputs:intensity", "inputs:color", "inputs:exposure"};
  const std::vector<std::string> camera_exact = {
      "focalLength", "horizontalAperture", "verticalAperture", "orthoWidth",
      "focusDistance", "exposure", "fStop", "clippingRange"};
  const std::vector<std::string> material_skip = {"visibility"};
  scan(LIGHTUSD_RENDER_PRIM_MESH, geometry_skip);
  scan(LIGHTUSD_RENDER_PRIM_POINTS, nonmesh_geometry_skip, {},
       nonmesh_ancestor);
  scan(LIGHTUSD_RENDER_PRIM_CURVE, nonmesh_geometry_skip, {},
       nonmesh_ancestor);
  scan(LIGHTUSD_RENDER_PRIM_POINT_INSTANCER, instancer_skip, instancer_exact,
       nonmesh_ancestor);
  scan(LIGHTUSD_RENDER_PRIM_MATERIAL, material_skip);
  scan(LIGHTUSD_RENDER_PRIM_LIGHT, light_transform_skip, light_scalar_exact);
  scan(LIGHTUSD_RENDER_PRIM_CAMERA, transform_skip, camera_exact);
  lightusd_render_prim_catalog_destroy(catalog);
  return found;
}

struct InstanceArrays {
  std::vector<float> positions;
  std::vector<float> orientations;
  std::vector<float> scales;
  std::vector<int64_t> ids;
  std::vector<int64_t> invisible_ids;
  std::vector<int64_t> inactive_ids;
  std::vector<int32_t> proto_indices;
  bool animated = false;
  bool visibility_animated = false;
  bool proto_animated = false;
};

// Reads the time-varying PointInstancer source arrays. `positions` is required;
// orientations/scales are optional and default to identity/unit scale.
bool ReadInstanceArrays(lightusd_stage* stage, const std::string& path,
                        double time, InstanceArrays* out) {
  if (!stage || !out || path.empty()) return false;
  lightusd_prim prim = PrimAtPath(stage, path);
  if (!lightusd_prim_is_valid(prim)) return false;
  *out = InstanceArrays();
  const auto read = [&](const char* name, std::vector<float>* dst) {
    lightusd_render_float_array* array = nullptr;
    if (lightusd_render_float_array_create(stage, prim, name, time, &array) !=
            LIGHTUSD_OK ||
        !array) {
      return false;
    }
    const float* data = nullptr;
    size_t count = 0;
    const bool ok =
        lightusd_render_float_array_data(array, &data, &count) == LIGHTUSD_OK &&
        data && count > 0;
    if (ok) dst->assign(data, data + count);
    lightusd_render_float_array_destroy(array);
    return ok;
  };
  const auto read_i64 = [&](const char* name, std::vector<int64_t>* dst) {
    lightusd_value* value = nullptr;
    if (lightusd_attr_eval(stage, prim, name, time, &value) != LIGHTUSD_OK ||
        !value) {
      return false;
    }
    bool ok = false;
    lightusd_value_view view{};
    if (lightusd_value_get_view(value, &view) == LIGHTUSD_OK && view.data &&
        view.storage == LIGHTUSD_COMP_INT64 && view.components >= 1 &&
        view.count > 0) {
      const int64_t* src = static_cast<const int64_t*>(view.data);
      dst->assign(src, src + view.count * view.components);
      ok = true;
    }
    lightusd_value_destroy(value);
    return ok;
  };
  const bool have_positions = read("positions", &out->positions);
  read("orientations", &out->orientations);
  read("scales", &out->scales);
  const auto read_i32 = [&](const char* name, std::vector<int32_t>* dst) {
    lightusd_value* value = nullptr;
    if (lightusd_attr_eval(stage, prim, name, time, &value) != LIGHTUSD_OK ||
        !value) {
      return false;
    }
    bool ok = false;
    lightusd_value_view view{};
    if (lightusd_value_get_view(value, &view) == LIGHTUSD_OK && view.data &&
        view.storage == LIGHTUSD_COMP_INT32 && view.components >= 1 &&
        view.count > 0) {
      const int32_t* src = static_cast<const int32_t*>(view.data);
      dst->assign(src, src + view.count * view.components);
      ok = true;
    }
    lightusd_value_destroy(value);
    return ok;
  };
  read_i64("ids", &out->ids);
  read_i64("invisibleIds", &out->invisible_ids);
  read_i64("inactiveIds", &out->inactive_ids);
  read_i32("protoIndices", &out->proto_indices);
  out->animated = lightusd_attr_has_timesamples(prim, "positions") ||
                  lightusd_attr_has_timesamples(prim, "orientations") ||
                  lightusd_attr_has_timesamples(prim, "scales");
  out->visibility_animated =
      lightusd_attr_has_timesamples(prim, "ids") ||
      lightusd_attr_has_timesamples(prim, "invisibleIds") ||
      lightusd_attr_has_timesamples(prim, "inactiveIds");
  out->proto_animated = lightusd_attr_has_timesamples(prim, "protoIndices");
  return have_positions;
}

// Mirrors next::UsdGeomPointInstancer's ComposeTRS (row-vector, row-major).
void ComposeInstanceTRS(const InstanceArrays& arrays, uint32_t index,
                        Mat4d* out) {
  if (!out) return;
  *out = Identity4d();
  const float* t = (static_cast<size_t>(index) * 3 + 3 <= arrays.positions.size())
                       ? arrays.positions.data() + index * 3
                       : nullptr;
  const float* q =
      (static_cast<size_t>(index) * 4 + 4 <= arrays.orientations.size())
          ? arrays.orientations.data() + index * 4
          : nullptr;
  const float* s =
      (static_cast<size_t>(index) * 3 + 3 <= arrays.scales.size())
          ? arrays.scales.data() + index * 3
          : nullptr;
  const double tx = t ? static_cast<double>(t[0]) : 0.0;
  const double ty = t ? static_cast<double>(t[1]) : 0.0;
  const double tz = t ? static_cast<double>(t[2]) : 0.0;
  const double sx = s ? static_cast<double>(s[0]) : 1.0;
  const double sy = s ? static_cast<double>(s[1]) : 1.0;
  const double sz = s ? static_cast<double>(s[2]) : 1.0;

  double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
  if (q) {
    w = static_cast<double>(q[0]);
    x = static_cast<double>(q[1]);
    y = static_cast<double>(q[2]);
    z = static_cast<double>(q[3]);
    const double len2 = w * w + x * x + y * y + z * z;
    if (len2 > 0.0) {
      const double inv_len = 1.0 / std::sqrt(len2);
      w *= inv_len;
      x *= inv_len;
      y *= inv_len;
      z *= inv_len;
    } else {
      w = 1.0;
      x = y = z = 0.0;
    }
  }

  const double xx = x * x, yy = y * y, zz = z * z;
  const double xy = x * y, xz = x * z, yz = y * z;
  const double wx = w * x, wy = w * y, wz = w * z;
  out->m[0][0] = (1.0 - 2.0 * (yy + zz)) * sx;
  out->m[0][1] = (2.0 * (xy - wz)) * sy;
  out->m[0][2] = (2.0 * (xz + wy)) * sz;
  out->m[1][0] = (2.0 * (xy + wz)) * sx;
  out->m[1][1] = (1.0 - 2.0 * (xx + zz)) * sy;
  out->m[1][2] = (2.0 * (yz - wx)) * sz;
  out->m[2][0] = (2.0 * (xz - wy)) * sx;
  out->m[2][1] = (2.0 * (yz + wx)) * sy;
  out->m[2][2] = (1.0 - 2.0 * (xx + yy)) * sz;
  out->m[3][0] = tx;
  out->m[3][1] = ty;
  out->m[3][2] = tz;
}

// Converts the world-space palette back to the mesh-local space used by the
// old CPU bake (and by the draw's world matrix for bounds/framing).
bool BuildLocalSkinPalette(const MeshData& mesh, std::vector<Mat4d>* local) {
  if (!local || !mesh.skinned || mesh.skin_matrices.empty() ||
      mesh.skin_matrices.size() % 16 != 0) {
    return false;
  }
  Mat4d inv_mesh_world;
  if (!Invert4d(LoadRowMajor16d(mesh.mesh_world), &inv_mesh_world)) {
    return false;
  }
  const size_t joint_count = mesh.skin_matrices.size() / 16;
  local->resize(joint_count);
  for (size_t j = 0; j < joint_count; ++j) {
    double raw[16];
    for (int i = 0; i < 16; ++i) {
      raw[i] = static_cast<double>(mesh.skin_matrices[j * 16 + i]);
    }
    (*local)[j] = Mul4d(LoadRowMajor16d(raw), inv_mesh_world);
  }
  return true;
}

// Local posed bounds at the current palette/weights, so the viewer frames the
// rigged pose rather than the bind pose.
void ComputeLocalPosedBounds(MeshData* mesh) {
  if (!mesh || !mesh->skinned) return;
  std::vector<Mat4d> local;
  if (!BuildLocalSkinPalette(*mesh, &local)) return;
  const size_t vertex_count = mesh->vertices.size();
  float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
  float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  const size_t morph_targets = mesh->morph_deltas.size();
  for (size_t v = 0; v < vertex_count; ++v) {
    float p[3] = {mesh->vertices[v].px, mesh->vertices[v].py,
                  mesh->vertices[v].pz};
    for (size_t t = 0; t < morph_targets && t < mesh->morph_weights.size();
         ++t) {
      const std::vector<float>& deltas = mesh->morph_deltas[t];
      if (deltas.size() < vertex_count * 3) continue;
      const float weight = mesh->morph_weights[t];
      p[0] += weight * deltas[v * 3 + 0];
      p[1] += weight * deltas[v * 3 + 1];
      p[2] += weight * deltas[v * 3 + 2];
    }
    double acc[3] = {0.0, 0.0, 0.0};
    double weight_sum = 0.0;
    for (int k = 0; k < 4; ++k) {
      const float w = mesh->joint_weights[v * 4 + k];
      if (!(w > 0.0f)) continue;
      const uint32_t j = mesh->joint_indices[v * 4 + k];
      if (j >= local.size()) continue;
      const Mat4d& m = local[j];
      for (int c = 0; c < 3; ++c) {
        acc[c] += double(w) * (double(p[0]) * m.m[0][c] +
                               double(p[1]) * m.m[1][c] +
                               double(p[2]) * m.m[2][c] + m.m[3][c]);
      }
      weight_sum += double(w);
    }
    if (weight_sum <= 0.0) continue;
    for (int c = 0; c < 3; ++c) {
      const float value = static_cast<float>(acc[c] / weight_sum);
      lo[c] = std::min(lo[c], value);
      hi[c] = std::max(hi[c], value);
    }
  }
  if (lo[0] <= hi[0]) {
    for (int c = 0; c < 3; ++c) {
      mesh->bbox_min[c] = lo[c];
      mesh->bbox_max[c] = hi[c];
    }
  }
}

bool BuildMesh(lightusd_stage* stage, lightusd_render_scene* scene,
               int32_t mesh_id, double time, bool weld_face_ids,
               uint64_t max_triangles, uint64_t* triangle_budget,
               MeshData* out) {
  lightusd_render_mesh_info info{};
  lightusd_render_mesh_extra_info extra{};
  if (lightusd_render_mesh_get_info(scene, mesh_id, &info) != LIGHTUSD_OK ||
      lightusd_render_mesh_get_extra_info(scene, mesh_id, &extra) !=
          LIGHTUSD_OK) {
    return false;
  }
  out->name = Sv(info.name);
  out->prim_path = Sv(info.prim_path);
  out->material_id = info.material_id;
  out->double_sided = extra.double_sided != 0;
  out->bbox_min[0] = info.bbox_min[0];
  out->bbox_min[1] = info.bbox_min[1];
  out->bbox_min[2] = info.bbox_min[2];
  out->bbox_max[0] = info.bbox_max[0];
  out->bbox_max[1] = info.bbox_max[1];
  out->bbox_max[2] = info.bbox_max[2];

  std::vector<float> points;
  std::vector<uint32_t> tri_indices;
  std::vector<uint32_t> tri_corners;
  std::vector<float> normals;
  std::vector<float> uvs;
  std::vector<float> uvs1;
  std::vector<float> colors;
  std::vector<float> opacities;
  std::vector<uint32_t> face_offsets;
  std::vector<uint32_t> subdivision_face_source;
  if (!ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_POINTS, &points) ||
      !ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_TRI_INDICES,
                      &tri_indices)) {
    return false;
  }
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_TRI_FACEVARYING_INDICES,
                 &tri_corners);
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_NORMALS, &normals);
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_TEXCOORDS0, &uvs);
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_TEXCOORDS1, &uvs1);
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_COLORS, &colors);
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_OPACITIES, &opacities);
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_FACE_TRIANGLE_OFFSETS,
                 &face_offsets);
  ReadMeshBuffer(scene, mesh_id, LIGHTUSD_MESH_BUF_SUBDIVISION_FACE_SOURCE,
                 &subdivision_face_source);

  // Optional authored Ptex face ids (`faceIndex` / `ptexIndex` primvar).
  std::vector<int64_t> ptex_face_ids;
  std::vector<uint32_t> ptex_face_remap;
  int ptex_face_interp = 2;
  bool have_ptex_face = false;
  for (uint32_t primvar = 0; primvar < info.primvar_count; ++primvar) {
    lightusd_render_primvar_info primvar_info{};
    if (lightusd_render_mesh_primvar_info(scene, mesh_id, primvar,
                                          &primvar_info) != LIGHTUSD_OK) {
      continue;
    }
    std::string name = Sv(primvar_info.name);
    for (char& c : name) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (name != "faceindex" && name != "ptexindex") continue;
    if (primvar_info.format != 4 /* Int */ &&
        primvar_info.format != 8 /* UInt */) {
      continue;
    }
    lightusd_buffer_view data{};
    lightusd_buffer_view remap{};
    if (lightusd_render_mesh_primvar_buffer(scene, mesh_id, primvar, 0,
                                            &data) != LIGHTUSD_OK ||
        !data.data) {
      continue;
    }
    if (primvar_info.has_indices) {
      lightusd_render_mesh_primvar_buffer(scene, mesh_id, primvar, 1, &remap);
    }
    const size_t count = data.nbytes / sizeof(uint32_t);
    ptex_face_ids.resize(count);
    if (primvar_info.format == 4) {
      const int32_t* src = static_cast<const int32_t*>(data.data);
      for (size_t i = 0; i < count; ++i) ptex_face_ids[i] = src[i];
    } else {
      const uint32_t* src = static_cast<const uint32_t*>(data.data);
      for (size_t i = 0; i < count; ++i) ptex_face_ids[i] = src[i];
    }
    if (remap.data) {
      const size_t n = remap.nbytes / sizeof(uint32_t);
      const uint32_t* src = static_cast<const uint32_t*>(remap.data);
      ptex_face_remap.assign(src, src + n);
    }
    ptex_face_interp = primvar_info.interpolation;
    have_ptex_face = true;
    break;
  }

  if (points.size() < 3 || tri_indices.size() < 3 ||
      tri_indices.size() % 3 != 0) {
    return false;
  }

  // UsdSkel data is retained for GPU playback; points stay in the bind pose.
  SkinBindingData skin;
  const bool skinned =
      ResolveSkinBinding(stage, scene, mesh_id, out->prim_path, time, &skin);

  const size_t triangle_count = tri_indices.size() / 3;
  if (max_triangles > 0 && *triangle_budget + triangle_count > max_triangles) {
    return false;
  }
  *triangle_budget += triangle_count;

  const size_t point_count = points.size() / 3;
  const bool have_corner_map = tri_corners.size() == tri_indices.size();
  const std::vector<int32_t> triangle_faces =
      BuildTriangleFaces(face_offsets, static_cast<uint32_t>(triangle_count));

  VertexAttr normal_attr =
      MakeAttr(normals, 3, info.normals_interp);
  VertexAttr uv_attr = MakeAttr(uvs, 2, info.texcoords0_interp);
  VertexAttr uv1_attr = MakeAttr(uvs1, 2, info.texcoords1_interp);
  const uint32_t color_components =
      extra.colors_components != 0
          ? static_cast<uint32_t>(extra.colors_components)
          : (colors.empty() ? 0u
                            : (colors.size() % (point_count * 4) == 0 ? 4u
                                                                      : 3u));
  VertexAttr color_attr = MakeAttr(colors, color_components, info.colors_interp);
  VertexAttr opacity_attr = MakeAttr(opacities, 1, extra.opacities_interp);

  // FaceVarying attributes need the corner remap; treat them as absent rather
  // than indexing garbage when the converter did not emit it.
  if (!have_corner_map) {
    if (normal_attr.interpolation == 3) normal_attr = VertexAttr{};
    if (uv_attr.interpolation == 3) uv_attr = VertexAttr{};
    if (uv1_attr.interpolation == 3) uv1_attr = VertexAttr{};
    if (color_attr.interpolation == 3) color_attr = VertexAttr{};
    if (opacity_attr.interpolation == 3) opacity_attr = VertexAttr{};
  }

  std::unordered_map<CornerKey, uint32_t, CornerKeyHash> weld;
  weld.reserve(point_count);
  out->vertices.reserve(point_count);
  out->indices.reserve(tri_indices.size());
  std::vector<uint32_t> vertex_to_point;
  vertex_to_point.reserve(point_count);

  for (size_t c = 0; c < tri_indices.size(); ++c) {
    const uint32_t point = tri_indices[c];
    if (point >= point_count) continue;
    const uint32_t corner = tri_corners.empty()
                                ? static_cast<uint32_t>(c)
                                : tri_corners[c];
    const int32_t face = triangle_faces.empty()
                             ? static_cast<int32_t>(c / 3)
                             : triangle_faces[c / 3];

    const int32_t triangle = static_cast<int32_t>(c / 3);
    const int32_t authored_face =
        triangle_faces.empty() ? triangle : triangle_faces[c / 3];
    uint32_t source_face =
        weld_face_ids && authored_face >= 0 &&
                static_cast<size_t>(authored_face) <
                    subdivision_face_source.size()
            ? subdivision_face_source[static_cast<size_t>(authored_face)]
            : (authored_face >= 0 ? static_cast<uint32_t>(authored_face) : 0u);
    if (have_ptex_face) {
      size_t index = 0;
      switch (ptex_face_interp) {
        case 0: index = 0; break;
        case 1:
          index = authored_face >= 0 ? static_cast<size_t>(authored_face) : 0;
          break;
        case 3: index = corner; break;
        default: index = point; break;
      }
      if (!ptex_face_remap.empty()) {
        if (index < ptex_face_remap.size()) {
          index = ptex_face_remap[index];
        } else {
          index = 0;
        }
      }
      if (index < ptex_face_ids.size() && ptex_face_ids[index] >= 0) {
        source_face = static_cast<uint32_t>(ptex_face_ids[index]);
      }
    }

    CornerKey key{};
    key.face = weld_face_ids ? source_face : 0u;
    key.point = point;
    key.normal = static_cast<uint32_t>(
        normal_attr.data ? AttrElement(normal_attr, corner, point, face) : 0);
    key.uv = static_cast<uint32_t>(
        uv_attr.data ? AttrElement(uv_attr, corner, point, face) : 0);
    key.uv1 = static_cast<uint32_t>(
        uv1_attr.data ? AttrElement(uv1_attr, corner, point, face) : 0);
    key.color = static_cast<uint32_t>(
        color_attr.data ? AttrElement(color_attr, corner, point, face) : 0);
    key.opacity = static_cast<uint32_t>(
        opacity_attr.data ? AttrElement(opacity_attr, corner, point, face) : 0);

    auto it = weld.find(key);
    if (it != weld.end()) {
      out->indices.push_back(it->second);
      continue;
    }

    lmv_vertex v{};
    v.px = points[point * 3 + 0];
    v.py = points[point * 3 + 1];
    v.pz = points[point * 3 + 2];
    if (const float* n = normal_attr.At(corner, point, face)) {
      v.nx = n[0];
      v.ny = n[1];
      v.nz = n[2];
    }
    if (const float* uv = uv_attr.At(corner, point, face)) {
      v.u = uv[0];
      v.v = uv[1];
    }
    if (const float* uv = uv1_attr.At(corner, point, face)) {
      v.u1 = uv[0];
      v.v1 = uv[1];
    }
    float opacity = 1.0f;
    if (const float* o = opacity_attr.At(corner, point, face)) opacity = o[0];
    v.r = v.g = v.b = v.a = 1.0f;
    if (const float* col = color_attr.At(corner, point, face)) {
      v.r = col[0];
      v.g = col[1];
      v.b = col[2];
      if (color_attr.components == 4) v.a = col[3];
    }
    v.a *= opacity;

    const uint32_t index = static_cast<uint32_t>(out->vertices.size());
    out->vertices.push_back(v);
    vertex_to_point.push_back(point);
    out->face_ids.push_back(weld_face_ids ? source_face : 0u);
    weld.emplace(key, index);
    out->indices.push_back(index);
  }

  if (out->indices.empty() || out->indices.size() % 3 != 0) return false;

  // Retain four skinning influences per welded vertex for GPU playback.
  out->mesh_prim_path = out->prim_path;
  if (skinned && skin.valid && vertex_to_point.size() == out->vertices.size() &&
      skin.influences > 0) {
    out->skinned = true;
    out->influences = 4;
    out->skeleton_prim_path = skin.skeleton_prim_path;
    out->animation_prim_path = skin.animation_prim_path;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        out->geom_bind[r * 4 + c] = skin.geom_bind.m[r][c];
        out->mesh_world[r * 4 + c] = skin.mesh_world.m[r][c];
      }
    }
    out->joint_indices.resize(out->vertices.size() * 4, 0);
    out->joint_weights.resize(out->vertices.size() * 4, 0.0f);
    const size_t src_influences = skin.influences;
    const size_t point_count_now = points.size() / 3;
    for (size_t i = 0; i < out->vertices.size(); ++i) {
      const size_t p = vertex_to_point[i];
      if (p >= point_count_now) continue;
      struct Influence {
        float weight;
        uint16_t joint;
      };
      Influence list[8];
      size_t count = 0;
      for (size_t k = 0; k < src_influences && k < 8; ++k) {
        const float w = skin.joint_weights[p * src_influences + k];
        if (!(w > 0.0f)) continue;
        list[count++] = {w, skin.joint_indices[p * src_influences + k]};
      }
      std::sort(list, list + count,
                [](const Influence& a, const Influence& b) {
                  return a.weight > b.weight;
                });
      float sum = 0.0f;
      for (size_t k = 0; k < count && k < 4; ++k) sum += list[k].weight;
      if (!(sum > 0.0f)) sum = 1.0f;
      for (size_t k = 0; k < 4; ++k) {
        const size_t dst = i * 4 + k;
        if (k < count) {
          out->joint_indices[dst] = list[k].joint;
          out->joint_weights[dst] = list[k].weight / sum;
        } else {
          out->joint_indices[dst] = 0;
          out->joint_weights[dst] = 0.0f;
        }
      }
    }
    // Initial palette: geomBind * skinPose * skeletonWorld (row-major).
    out->skin_matrices.resize(skin.matrices.size() * 16);
    for (size_t j = 0; j < skin.matrices.size(); ++j) {
      const Mat4d m = Mul4d(Mul4d(skin.matrices[j], skin.inv_geom_bind),
                            skin.mesh_world);
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          out->skin_matrices[j * 16 + r * 4 + c] = static_cast<float>(m.m[r][c]);
        }
      }
    }
    if (out->animation_prim_path.empty()) {
      out->animation_prim_path = out->mesh_prim_path;
    }
  }

  // Retain blend-shape deltas for GPU morphing.
  ExtractBlendShapes(stage, scene, mesh_id, out->prim_path, time,
                     vertex_to_point, out);
  ComputeLocalPosedBounds(out);

  // Fall back to generated normals when the source had none.
  if (!normal_attr.data) {
    std::vector<float> accum(out->vertices.size() * 3, 0.0f);
    for (size_t i = 0; i + 2 < out->indices.size(); i += 3) {
      const uint32_t ia = out->indices[i + 0];
      const uint32_t ib = out->indices[i + 1];
      const uint32_t ic = out->indices[i + 2];
      const lmv_vertex& a = out->vertices[ia];
      const lmv_vertex& b = out->vertices[ib];
      const lmv_vertex& c = out->vertices[ic];
      const float e1[3] = {b.px - a.px, b.py - a.py, b.pz - a.pz};
      const float e2[3] = {c.px - a.px, c.py - a.py, c.pz - a.pz};
      const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                          e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0]};
      for (uint32_t index : {ia, ib, ic}) {
        accum[index * 3 + 0] += n[0];
        accum[index * 3 + 1] += n[1];
        accum[index * 3 + 2] += n[2];
      }
    }
    for (size_t i = 0; i < out->vertices.size(); ++i) {
      float n[3] = {accum[i * 3 + 0], accum[i * 3 + 1], accum[i * 3 + 2]};
      float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (len < 1e-12f) {
        n[0] = 0.0f;
        n[1] = 1.0f;
        n[2] = 0.0f;
        len = 1.0f;
      }
      out->vertices[i].nx = n[0] / len;
      out->vertices[i].ny = n[1] / len;
      out->vertices[i].nz = n[2] / len;
    }
  }

  // Material ranges. With triangulation on, tydra reports subset ranges in
  // triangle space.
  const uint32_t total_triangles = static_cast<uint32_t>(out->indices.size() / 3);
  for (uint32_t i = 0; i < info.subset_count; ++i) {
    uint32_t face_start = 0;
    uint32_t face_count = 0;
    int32_t material_id = -1;
    if (lightusd_render_mesh_subset(scene, mesh_id, i, &face_start, &face_count,
                                    &material_id) != LIGHTUSD_OK) {
      continue;
    }
    const uint32_t first = std::min(face_start, total_triangles) * 3;
    const uint32_t count =
        std::min(face_count, total_triangles - first / 3) * 3;
    if (count == 0) continue;
    lmv_subset subset{};
    subset.first_index = first;
    subset.index_count = count;
    subset.material_id = material_id;
    out->subsets.push_back(subset);
  }
  if (out->subsets.empty()) {
    lmv_subset subset{};
    subset.first_index = 0;
    subset.index_count = static_cast<uint32_t>(out->indices.size());
    subset.material_id = out->material_id;
    out->subsets.push_back(subset);
  }
  // Use the actual point range (bind pose) rather than the renderer's bbox.
  {
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i + 2 < points.size(); i += 3) {
      for (int c = 0; c < 3; ++c) {
        lo[c] = std::min(lo[c], points[i + c]);
        hi[c] = std::max(hi[c], points[i + c]);
      }
    }
    if (lo[0] <= hi[0]) {
      for (int c = 0; c < 3; ++c) {
        out->bbox_min[c] = lo[c];
        out->bbox_max[c] = hi[c];
      }
    }
  }
  out->valid = true;
  return true;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

bool EndsWithNoCase(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    const char a = static_cast<char>(std::tolower(
        static_cast<unsigned char>(s[s.size() - n + i])));
    const char b = static_cast<char>(std::tolower(
        static_cast<unsigned char>(suffix[i])));
    if (a != b) return false;
  }
  return true;
}

uint32_t UvSetFromPrimvarName(const std::string& name) {
  if (name.empty()) return 0;
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  static const char* kSecond[] = {"st1", "uv1", "uv2", "uvmap1", "map2",
                                  "texcoord1", "texcoord2", "uvset1",
                                  "uvset2", "uv2set"};
  for (const char* candidate : kSecond) {
    if (lower == candidate) return 1;
  }
  return 0;
}

bool TextureIsPtex(lightusd_render_scene* render, int32_t texture_id) {
  if (texture_id < 0) return false;
  lightusd_render_texture_info info{};
  return lightusd_render_texture_get_info(render, texture_id, &info) ==
             LIGHTUSD_OK &&
         EndsWithNoCase(Sv(info.asset_path), ".ptx");
}

bool MaterialUsesPtex(lightusd_render_scene* render, int32_t material_id) {
  static const char* kPreview[] = {"diffuse_color", "metallic", "roughness",
                                   "normal", "emissive_color", "opacity"};
  static const char* kOpenPbr[] = {"base_color", "base_metalness",
                                   "base_roughness", "specular_roughness",
                                   "normal", "emission_color", "opacity"};
  lightusd_render_material_info info{};
  if (lightusd_render_material_get_info(render, material_id, &info) !=
      LIGHTUSD_OK) {
    return false;
  }
  const char* const* names = info.shader_type == 2 ? kOpenPbr : kPreview;
  const size_t count = info.shader_type == 2
                           ? sizeof(kOpenPbr) / sizeof(kOpenPbr[0])
                           : sizeof(kPreview) / sizeof(kPreview[0]);
  for (size_t i = 0; i < count; ++i) {
    int32_t texture_id = -1;
    float value[4] = {0, 0, 0, 0};
    if (lightusd_render_material_param(render, material_id, names[i],
                                       &texture_id, value) == LIGHTUSD_OK &&
        TextureIsPtex(render, texture_id)) {
      return true;
    }
  }
  return false;
}

bool MaterialParam(lightusd_render_scene* scene, int32_t material_id,
                   const char* name, int32_t* texture_id, float value[4]) {
  if (!texture_id || !value) return false;
  if (lightusd_render_material_param(scene, material_id, name, texture_id,
                                     value) != LIGHTUSD_OK) {
    return false;
  }
  return true;
}

void Copy3(const float in[4], float out[3]) {
  out[0] = in[0];
  out[1] = in[1];
  out[2] = in[2];
}

int32_t TextureSlot(int32_t texture_id) {
  return texture_id >= 0 ? texture_id : -1;
}

void ApplyColorMatrix(const float m[9], float* rgb) {
  const float x = m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2];
  const float y = m[3] * rgb[0] + m[4] * rgb[1] + m[5] * rgb[2];
  const float z = m[6] * rgb[0] + m[7] * rgb[1] + m[8] * rgb[2];
  rgb[0] = x;
  rgb[1] = y;
  rgb[2] = z;
}

bool ColorMatrixIsIdentity(const float m[9]) {
  const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (int i = 0; i < 9; ++i) {
    if (std::fabs(m[i] - identity[i]) > 1e-6f) return false;
  }
  return true;
}

void BuildMaterialData(lightusd_render_scene* scene, int32_t material_id,
                       const float color_matrix[9], MaterialData* out) {
  lightusd_render_material_info info{};
  if (lightusd_render_material_get_info(scene, material_id, &info) !=
      LIGHTUSD_OK) {
    return;
  }
  out->name = Sv(info.name);
  out->prim_path = Sv(info.prim_path);
  out->shader_type = info.shader_type;
  out->alpha_mode = info.alpha_mode;
  out->double_sided = info.double_sided;
  out->default_fallback = info.default_fallback;
  out->alpha_cutoff = info.alpha_cutoff;
  float normal_scale = 1.0f;
  float coat_normal_scale = 1.0f;
  lightusd_render_material_normal_scales(scene, material_id, &normal_scale,
                                         &coat_normal_scale);
  out->normal_scale = normal_scale;
  out->coat_normal_scale = coat_normal_scale;

  lightusd::tydra::RealtimePbrMaterial pbr;
  float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  int32_t tex = -1;
  bool has_textures = false;
  bool has_normal = false;

  if (info.shader_type == 1) {  // UsdPreviewSurface
    if (MaterialParam(scene, material_id, "diffuse_color", &tex, value)) {
      Copy3(value, pbr.baseColor);
      out->tex[LMV_TEX_BASE_COLOR] = TextureSlot(tex);
      has_textures = has_textures || tex >= 0;
    }
    // UsdPreviewSurface's specularColor defaults to black and is only meaningful
    // in the specular workflow; copying that default would zero F0 (black
    // metals). Treat black as unauthored and keep the neutral default.
    if (MaterialParam(scene, material_id, "specular_color", &tex, value) &&
        (value[0] > 0.0f || value[1] > 0.0f || value[2] > 0.0f)) {
      Copy3(value, pbr.specularColor);
    }
    if (MaterialParam(scene, material_id, "metallic", &tex, value)) {
      pbr.metalness = value[0];
      if (tex >= 0) {
        out->tex[LMV_TEX_METALLIC] = tex;
        has_textures = true;
      }
    }
    if (MaterialParam(scene, material_id, "roughness", &tex, value)) {
      pbr.specularRoughness = value[0];
      if (tex >= 0) {
        out->tex[LMV_TEX_ROUGHNESS] = tex;
        has_textures = true;
      }
    }
    if (MaterialParam(scene, material_id, "ior", &tex, value)) {
      pbr.specularIor = value[0] > 0.0f ? value[0] : 1.5f;
    }
    if (MaterialParam(scene, material_id, "clearcoat", &tex, value)) {
      pbr.coatWeight = value[0];
    }
    if (MaterialParam(scene, material_id, "clearcoat_roughness", &tex, value)) {
      pbr.coatRoughness = value[0];
    }
    if (MaterialParam(scene, material_id, "opacity", &tex, value)) {
      pbr.opacity = value[0];
      if (tex >= 0) {
        out->tex[LMV_TEX_OPACITY] = tex;
        has_textures = true;
      }
    }
    if (MaterialParam(scene, material_id, "normal", &tex, value)) {
      pbr.normal[0] = value[0];
      pbr.normal[1] = value[1];
      pbr.normal[2] = value[2];
      if (tex >= 0) {
        out->tex[LMV_TEX_NORMAL] = tex;
        has_textures = true;
        has_normal = true;
      }
    }
    if (MaterialParam(scene, material_id, "emissive_color", &tex, value)) {
      Copy3(value, pbr.emissionColor);
      pbr.emission = (value[0] > 0.0f || value[1] > 0.0f || value[2] > 0.0f)
                         ? 1.0f
                         : 0.0f;
      if (tex >= 0) {
        out->tex[LMV_TEX_EMISSION] = tex;
        has_textures = true;
      }
    }
  } else if (info.shader_type == 2) {  // OpenPBR / MaterialX surface
    if (MaterialParam(scene, material_id, "base_weight", &tex, value)) {
      pbr.baseWeight = value[0];
    }
    if (MaterialParam(scene, material_id, "base_color", &tex, value)) {
      Copy3(value, pbr.baseColor);
      out->tex[LMV_TEX_BASE_COLOR] = TextureSlot(tex);
      has_textures = has_textures || tex >= 0;
    }
    if (MaterialParam(scene, material_id, "base_roughness", &tex, value)) {
      pbr.diffuseRoughness = value[0];
    }
    if (MaterialParam(scene, material_id, "base_metalness", &tex, value)) {
      pbr.metalness = value[0];
      if (tex >= 0) {
        out->tex[LMV_TEX_METALLIC] = tex;
        has_textures = true;
      }
    }
    if (MaterialParam(scene, material_id, "specular_weight", &tex, value)) {
      pbr.specularWeight = value[0];
    }
    if (MaterialParam(scene, material_id, "specular_color", &tex, value)) {
      Copy3(value, pbr.specularColor);
    }
    if (MaterialParam(scene, material_id, "specular_roughness", &tex, value)) {
      pbr.specularRoughness = value[0];
      if (tex >= 0) {
        out->tex[LMV_TEX_ROUGHNESS] = tex;
        has_textures = true;
      }
    }
    if (MaterialParam(scene, material_id, "specular_ior", &tex, value)) {
      pbr.specularIor = value[0] > 0.0f ? value[0] : 1.5f;
    }
    if (MaterialParam(scene, material_id, "transmission_weight", &tex,
                      value)) {
      pbr.transmission = value[0];
    }
    if (MaterialParam(scene, material_id, "transmission_color", &tex, value)) {
      Copy3(value, pbr.transmissionColor);
    }
    if (MaterialParam(scene, material_id, "transmission_depth", &tex, value)) {
      pbr.transmissionDepth = value[0];
    }
    if (MaterialParam(scene, material_id, "subsurface_weight", &tex, value)) {
      pbr.subsurface = value[0];
    }
    if (MaterialParam(scene, material_id, "subsurface_color", &tex, value)) {
      Copy3(value, pbr.subsurfaceColor);
    }
    if (MaterialParam(scene, material_id, "subsurface_radius", &tex, value)) {
      Copy3(value, pbr.subsurfaceRadius);
    }
    if (MaterialParam(scene, material_id, "subsurface_scale", &tex, value)) {
      pbr.subsurfaceScale = value[0];
    }
    if (MaterialParam(scene, material_id, "coat_weight", &tex, value)) {
      pbr.coatWeight = value[0];
    }
    if (MaterialParam(scene, material_id, "coat_color", &tex, value)) {
      Copy3(value, pbr.coatColor);
    }
    if (MaterialParam(scene, material_id, "coat_roughness", &tex, value)) {
      pbr.coatRoughness = value[0];
    }
    if (MaterialParam(scene, material_id, "coat_ior", &tex, value)) {
      pbr.coatIor = value[0] > 0.0f ? value[0] : 1.5f;
    }
    if (MaterialParam(scene, material_id, "sheen_weight", &tex, value)) {
      pbr.sheenWeight = value[0];
    }
    if (MaterialParam(scene, material_id, "sheen_color", &tex, value)) {
      Copy3(value, pbr.sheenColor);
    }
    if (MaterialParam(scene, material_id, "sheen_roughness", &tex, value)) {
      pbr.sheenRoughness = value[0];
    }
    if (MaterialParam(scene, material_id, "emission_luminance", &tex, value)) {
      pbr.emission = value[0];
    }
    if (MaterialParam(scene, material_id, "emission_color", &tex, value)) {
      Copy3(value, pbr.emissionColor);
      if (tex >= 0) {
        out->tex[LMV_TEX_EMISSION] = tex;
        has_textures = true;
      }
    }
    if (MaterialParam(scene, material_id, "opacity", &tex, value)) {
      pbr.opacity = value[0];
      if (tex >= 0) {
        out->tex[LMV_TEX_OPACITY] = tex;
        has_textures = true;
      }
    }
    if (MaterialParam(scene, material_id, "thin_walled", &tex, value)) {
      pbr.geometryThinWalled = value[0] >= 0.5f;
    }
    if (MaterialParam(scene, material_id, "normal", &tex, value)) {
      pbr.normal[0] = value[0];
      pbr.normal[1] = value[1];
      pbr.normal[2] = value[2];
      if (tex >= 0) {
        out->tex[LMV_TEX_NORMAL] = tex;
        has_textures = true;
        has_normal = true;
      }
    }
    if (MaterialParam(scene, material_id, "coat_normal", &tex, value)) {
      if (tex >= 0) {
        out->tex[LMV_TEX_COAT_NORMAL] = tex;
        has_textures = true;
      }
    }
  }

  pbr.hasTextureInputs = has_textures;
  pbr.hasNormalInput = has_normal;
  lightusd::tydra::ClampRealtimePbrMaterial(&pbr);
  lightusd::tydra::PackRealtimePbrMaterial(
      pbr, /*valid=*/info.shader_type != 0,
      static_cast<float>(info.alpha_mode), info.alpha_cutoff, out->pbr);

  if (!ColorMatrixIsIdentity(color_matrix)) {
    static const int kRgbOffsets[] = {LMV_PBR_BASE_COLOR, LMV_PBR_SPECULAR_COLOR,
                                      LMV_PBR_TRANSMISSION_COLOR,
                                      LMV_PBR_SUBSURFACE_COLOR,
                                      LMV_PBR_COAT_COLOR, LMV_PBR_SHEEN_COLOR,
                                      LMV_PBR_EMISSION_COLOR, 72, 76};
    for (int offset : kRgbOffsets) {
      ApplyColorMatrix(color_matrix, &out->pbr[offset]);
    }
  }
}

// ---------------------------------------------------------------------------
// Texture / image decoding
// ---------------------------------------------------------------------------

std::string UsdzEntryName(const std::string& resolved) {
  const size_t open = resolved.find(".usdz[");
  if (open == std::string::npos) return resolved;
  const size_t begin = open + std::strlen(".usdz[");
  const size_t end = resolved.find(']', begin);
  if (end == std::string::npos || end <= begin) return resolved;
  return resolved.substr(begin, end - begin);
}

// Interactive preview assets are bounded: a corrupt or adversarial header must
// not make the viewer allocate a multi-gigabyte decode buffer.
constexpr long kMaxSourceImageBytes = 512L * 1024L * 1024L;

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>* out) {
  if (!out) return false;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > kMaxSourceImageBytes) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(size));
  const size_t read = std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  if (read != out->size()) {
    out->clear();
    return false;
  }
  return true;
}

bool ReadAssetBytes(ImageCache* cache, const std::string& resolved,
                    std::vector<uint8_t>* out) {
  if (!cache || !out) return false;
  const std::string asset = UsdzEntryName(resolved);
  if (cache->usdz) {
    for (size_t i = 0; i < cache->usdz->NumEntries(); ++i) {
      const std::string& entry = cache->usdz->EntryName(i);
      if (!lightusd::tydra::next::UsdzEntryMatches(entry, asset)) continue;
      const uint8_t* data = cache->usdz->EntryData(i);
      const size_t size = cache->usdz->EntrySize(i);
      if (!data || size == 0) break;
      out->assign(data, data + size);
      return true;
    }
  }
  std::string path = asset;
  // Authored Windows separators are common in DCC exports.
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  if (!path.empty() && path[0] != '/') {
    // The converter resolves image paths against the stage directory, so the
    // path is usually already usable relative to the process CWD. Fall back to
    // the stage directory for authors that left it relative to the root layer.
    if (ReadFileBytes(path, out)) return true;
    if (!cache->base_dir.empty()) path = cache->base_dir + "/" + path;
  }
  return ReadFileBytes(path, out);
}

void BoxDownsampleRGBA8(const uint8_t* src, int width, int height,
                        int dst_width, int dst_height,
                        std::vector<uint8_t>* out) {
  out->resize(static_cast<size_t>(dst_width) * dst_height * 4);
  const float sx = static_cast<float>(width) / dst_width;
  const float sy = static_cast<float>(height) / dst_height;
  for (int y = 0; y < dst_height; ++y) {
    const int y0 = std::min(height - 1, static_cast<int>(y * sy));
    const int y1 = std::min(height, std::max(y0 + 1, static_cast<int>((y + 1) * sy)));
    for (int x = 0; x < dst_width; ++x) {
      const int x0 = std::min(width - 1, static_cast<int>(x * sx));
      const int x1 = std::min(width, std::max(x0 + 1, static_cast<int>((x + 1) * sx)));
      uint32_t acc[4] = {0, 0, 0, 0};
      uint32_t count = 0;
      for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
          const uint8_t* p = src + (static_cast<size_t>(yy) * width + xx) * 4;
          for (int c = 0; c < 4; ++c) acc[c] += p[c];
          ++count;
        }
      }
      uint8_t* dst = out->data() + (static_cast<size_t>(y) * dst_width + x) * 4;
      for (int c = 0; c < 4; ++c) {
        dst[c] = count ? static_cast<uint8_t>((acc[c] + count / 2) / count) : 0;
      }
    }
  }
}

void BoxDownsampleRGBFloat(const float* src, int width, int height,
                           int dst_width, int dst_height,
                           std::vector<float>* out) {
  out->resize(static_cast<size_t>(dst_width) * dst_height * 3);
  const float sx = static_cast<float>(width) / dst_width;
  const float sy = static_cast<float>(height) / dst_height;
  for (int y = 0; y < dst_height; ++y) {
    const int y0 = std::min(height - 1, static_cast<int>(y * sy));
    const int y1 = std::min(height, std::max(y0 + 1, static_cast<int>((y + 1) * sy)));
    for (int x = 0; x < dst_width; ++x) {
      const int x0 = std::min(width - 1, static_cast<int>(x * sx));
      const int x1 = std::min(width, std::max(x0 + 1, static_cast<int>((x + 1) * sx)));
      double acc[3] = {0.0, 0.0, 0.0};
      uint32_t count = 0;
      for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
          const float* p = src + (static_cast<size_t>(yy) * width + xx) * 3;
          for (int c = 0; c < 3; ++c) acc[c] += p[c];
          ++count;
        }
      }
      float* dst = out->data() + (static_cast<size_t>(y) * dst_width + x) * 3;
      for (int c = 0; c < 3; ++c) {
        dst[c] = count ? static_cast<float>(acc[c] / count) : 0.0f;
      }
    }
  }
}

bool IsKtx2Bytes(const std::vector<uint8_t>& bytes) {
  static const uint8_t kKtx2Magic[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20,
                                         0x32, 0x30, 0xBB, 0x0D, 0x0A,
                                         0x1A, 0x0A};
  return bytes.size() >= sizeof(kKtx2Magic) &&
         std::memcmp(bytes.data(), kKtx2Magic, sizeof(kKtx2Magic)) == 0;
}

size_t Ktx2ZstdDecompress(void*, uint8_t* dst, size_t dst_cap,
                          const uint8_t* src, size_t src_size) {
  const size_t written = ZSTD_decompress(dst, dst_cap, src, src_size);
  return ZSTD_isError(written) ? 0u : written;
}

bool IsExrBytes(const std::vector<uint8_t>& bytes) {
  return bytes.size() >= 4 && bytes[0] == 0x76 && bytes[1] == 0x2f &&
         bytes[2] == 0x31 && bytes[3] == 0x01;
}

// Decode EXR to linear float RGB through the vendored tinyexr C backend.
bool DecodeExrToFloat(const std::vector<uint8_t>& bytes, uint32_t max_edge,
                      std::vector<float>* rgb, int* out_width, int* out_height) {
  if (!rgb || !out_width || !out_height) return false;
  const size_t budget = 512ull << 20;
  lu_exr_result result{};
  if (lu_exr_decode(bytes.data(), bytes.size(), budget, &result) != 0 ||
      !result.data || result.width == 0 || result.height == 0) {
    free(result.data);
    return false;
  }
  if (result.bits != 32) {
    if (lu_exr_convert(&result, 32, budget) != 0) {
      free(result.data);
      return false;
    }
  }
  const size_t pixels = static_cast<size_t>(result.width) * result.height;
  const float* rgba = reinterpret_cast<const float*>(result.data);
  int width = static_cast<int>(result.width);
  int height = static_cast<int>(result.height);
  int dst_width = width;
  int dst_height = height;
  if (max_edge > 0 && static_cast<uint32_t>(std::max(width, height)) > max_edge) {
    const float scale = static_cast<float>(max_edge) /
                        static_cast<float>(std::max(width, height));
    dst_width = std::max(1, static_cast<int>(width * scale));
    dst_height = std::max(1, static_cast<int>(height * scale));
  }
  std::vector<float> source(pixels * 3);
  for (size_t i = 0; i < pixels; ++i) {
    source[i * 3 + 0] = rgba[i * 4 + 0];
    source[i * 3 + 1] = rgba[i * 4 + 1];
    source[i * 3 + 2] = rgba[i * 4 + 2];
  }
  free(result.data);
  if (dst_width != width || dst_height != height) {
    BoxDownsampleRGBFloat(source.data(), width, height, dst_width, dst_height,
                          rgb);
  } else {
    *rgb = std::move(source);
  }
  *out_width = dst_width;
  *out_height = dst_height;
  return true;
}

bool DecodeKtx2ToFloat(const std::vector<uint8_t>& bytes, uint32_t max_edge,
                       std::vector<float>* rgb, int* out_width,
                       int* out_height) {
  if (!rgb || !out_width || !out_height) return false;
  tp_ktx2_image image{};
  if (!TP_OK(tp_ktx2_read_zstd(bytes.data(), bytes.size(), nullptr,
                               Ktx2ZstdDecompress, nullptr, &image))) {
    return false;
  }
  const int width = static_cast<int>(image.width);
  const int height = static_cast<int>(image.height);
  if (width <= 0 || height <= 0 || !image.is_hdr) {
    tp_ktx2_image_free(nullptr, &image);
    return false;
  }
  std::vector<float> rgba(static_cast<size_t>(width) * height * 4);
  const bool ok = TP_OK(tp_ktx2_decode_level_rgbaf(
      &image, 0, rgba.data(), rgba.size() * sizeof(float)));
  tp_ktx2_image_free(nullptr, &image);
  if (!ok) return false;

  int dst_width = width;
  int dst_height = height;
  if (max_edge > 0 && static_cast<uint32_t>(std::max(width, height)) > max_edge) {
    const float scale = static_cast<float>(max_edge) /
                        static_cast<float>(std::max(width, height));
    dst_width = std::max(1, static_cast<int>(width * scale));
    dst_height = std::max(1, static_cast<int>(height * scale));
  }
  std::vector<float> source(static_cast<size_t>(width) * height * 3);
  for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
    source[i * 3 + 0] = rgba[i * 4 + 0];
    source[i * 3 + 1] = rgba[i * 4 + 1];
    source[i * 3 + 2] = rgba[i * 4 + 2];
  }
  if (dst_width != width || dst_height != height) {
    BoxDownsampleRGBFloat(source.data(), width, height, dst_width, dst_height,
                          rgb);
  } else {
    *rgb = std::move(source);
  }
  *out_width = dst_width;
  *out_height = dst_height;
  return true;
}

uint32_t Ktx2CompressedFormat(const tp_ktx2_image& image) {
  if (image.is_uni) return 0;
  switch (image.codec) {
    case TP_CODEC_BC1: return LMV_COMPRESSED_BC1;
    case TP_CODEC_BC3: return LMV_COMPRESSED_BC3;
    case TP_CODEC_BC5: return LMV_COMPRESSED_BC5;
    case TP_CODEC_BC6H: return LMV_COMPRESSED_BC6H;
    case TP_CODEC_BC7: return LMV_COMPRESSED_BC7;
    default: return 0;
  }
}

bool DecodeKtx2Compressed(const std::vector<uint8_t>& bytes, ImageData* image) {
  if (!image) return false;
  tp_ktx2_image ktx{};
  if (!TP_OK(tp_ktx2_read_zstd(bytes.data(), bytes.size(), nullptr,
                               Ktx2ZstdDecompress, nullptr, &ktx))) {
    return false;
  }
  const uint32_t format = Ktx2CompressedFormat(ktx);
  const bool usable = format != 0 && ktx.num_faces == 1 && ktx.num_layers <= 1 &&
                      ktx.num_levels > 0;
  if (!usable) {
    tp_ktx2_image_free(nullptr, &ktx);
    return false;
  }
  auto levels = std::make_shared<std::vector<ImageLevelData>>();
  levels->resize(static_cast<size_t>(ktx.num_levels));
  for (int l = 0; l < ktx.num_levels; ++l) {
    ImageLevelData& level = (*levels)[static_cast<size_t>(l)];
    level.width = ktx.levels[l].width;
    level.height = ktx.levels[l].height;
    const uint8_t* data = ktx.levels[l].data;
    if (!data || ktx.levels[l].size == 0) {
      tp_ktx2_image_free(nullptr, &ktx);
      return false;
    }
    level.data.assign(data, data + ktx.levels[l].size);
  }
  image->width = ktx.width;
  image->height = ktx.height;
  image->channels = 4;
  image->is_float = ktx.is_hdr != 0;
  image->srgb = ktx.srgb != 0;
  image->compressed_format = format;
  auto views = std::make_shared<std::vector<lmv_image_level>>();
  views->resize(levels->size());
  for (size_t i = 0; i < levels->size(); ++i) {
    (*views)[i].data = (*levels)[i].data.data();
    (*views)[i].nbytes = (*levels)[i].data.size();
    (*views)[i].width = (*levels)[i].width;
    (*views)[i].height = (*levels)[i].height;
  }
  image->levels = std::move(levels);
  image->level_views = std::move(views);
  tp_ktx2_image_free(nullptr, &ktx);
  return image->valid();
}

bool DecodeKtx2ToRgba8(const std::vector<uint8_t>& bytes, uint32_t max_edge,
                       std::vector<uint8_t>* rgba, int* out_width,
                       int* out_height) {
  if (!rgba || !out_width || !out_height) return false;
  tp_ktx2_image image{};
  if (!TP_OK(tp_ktx2_read_zstd(bytes.data(), bytes.size(), nullptr,
                               Ktx2ZstdDecompress, nullptr, &image))) {
    return false;
  }
  const int width = static_cast<int>(image.width);
  const int height = static_cast<int>(image.height);
  if (width <= 0 || height <= 0 || image.is_hdr) {
    tp_ktx2_image_free(nullptr, &image);
    return false;
  }
  std::vector<uint8_t> source(static_cast<size_t>(width) * height * 4);
  const bool ok = TP_OK(tp_ktx2_decode_level_rgba8(&image, 0, source.data(),
                                                   source.size()));
  tp_ktx2_image_free(nullptr, &image);
  if (!ok) return false;

  int dst_width = width;
  int dst_height = height;
  if (max_edge > 0 && static_cast<uint32_t>(std::max(width, height)) > max_edge) {
    const float scale = static_cast<float>(max_edge) /
                        static_cast<float>(std::max(width, height));
    dst_width = std::max(1, static_cast<int>(width * scale));
    dst_height = std::max(1, static_cast<int>(height * scale));
  }
  if (dst_width != width || dst_height != height) {
    BoxDownsampleRGBA8(source.data(), width, height, dst_width, dst_height, rgba);
  } else {
    *rgba = std::move(source);
  }
  *out_width = dst_width;
  *out_height = dst_height;
  return true;
}

std::shared_ptr<const ImageData> DecodeImage(ImageCache* cache,
                                             const std::string& resolved,
                                             bool srgb,
                                             const std::string& hint,
                                             bool allow_compressed = true) {
  if (!cache || resolved.empty()) return nullptr;
  const std::string key =
      resolved + "|" + hint + (srgb ? "|srgb" : "|linear") + "|" +
      std::to_string(cache->max_edge);
  auto it = cache->by_key.find(key);
  if (it != cache->by_key.end()) return it->second;

  std::vector<uint8_t> bytes;
  bool have_bytes = false;
  if (!hint.empty()) {
    std::string hint_path = hint;
    for (char& c : hint_path) {
      if (c == '\\') c = '/';
    }
    if (hint_path[0] != '/') {
      hint_path = DirName(resolved) + "/" + hint_path;
    }
    have_bytes = ReadAssetBytes(cache, hint_path, &bytes);
    if (!have_bytes) bytes.clear();
  }
  if (!have_bytes) {
    if (!ReadAssetBytes(cache, resolved, &bytes) || bytes.empty()) {
      cache->by_key.emplace(key, nullptr);
      return nullptr;
    }
  }
  if (bytes.empty() ||
      bytes.size() > 0x7fffffff) {
    cache->by_key.emplace(key, nullptr);
    return nullptr;
  }

  if (IsKtx2Bytes(bytes)) {
    auto image = std::make_shared<ImageData>();
    image->resolved_path = resolved;
    image->srgb = srgb;
    int width = 0;
    int height = 0;
    std::vector<float> rgb;
    std::vector<uint8_t> rgba;
    if (allow_compressed && DecodeKtx2Compressed(bytes, image.get())) {
      image->resolved_path = resolved;
      image->srgb = srgb;
      if (cache->max_bytes > 0 &&
          cache->resident_bytes + image->byte_size() > cache->max_bytes) {
        cache->by_key.emplace(key, nullptr);
        return nullptr;
      }
      cache->resident_bytes += image->byte_size();
      cache->by_key.emplace(key, image);
      return image;
    }
    if (DecodeKtx2ToFloat(bytes, cache->max_edge, &rgb, &width, &height)) {
      image->is_float = true;
      image->channels = 3;
      image->rgb = std::make_shared<std::vector<float>>(std::move(rgb));
    } else if (DecodeKtx2ToRgba8(bytes, cache->max_edge, &rgba, &width,
                                  &height)) {
      image->is_float = false;
      image->channels = 4;
      image->rgba = std::make_shared<std::vector<uint8_t>>(std::move(rgba));
    } else {
      cache->by_key.emplace(key, nullptr);
      return nullptr;
    }
    image->width = static_cast<uint32_t>(width);
    image->height = static_cast<uint32_t>(height);
    if (!image->valid() ||
        (cache->max_bytes > 0 &&
         cache->resident_bytes + image->byte_size() > cache->max_bytes)) {
      cache->by_key.emplace(key, nullptr);
      return nullptr;
    }
    cache->resident_bytes += image->byte_size();
    cache->by_key.emplace(key, image);
    return image;
  }

  if (IsExrBytes(bytes)) {
    auto image = std::make_shared<ImageData>();
    image->resolved_path = resolved;
    image->srgb = srgb;
    image->is_float = true;
    image->channels = 3;
    int width = 0;
    int height = 0;
    std::vector<float> rgb;
    if (!DecodeExrToFloat(bytes, cache->max_edge, &rgb, &width, &height)) {
      cache->by_key.emplace(key, nullptr);
      return nullptr;
    }
    image->width = static_cast<uint32_t>(width);
    image->height = static_cast<uint32_t>(height);
    image->rgb = std::make_shared<std::vector<float>>(std::move(rgb));
    if (!image->valid() ||
        (cache->max_bytes > 0 &&
         cache->resident_bytes + image->byte_size() > cache->max_bytes)) {
      cache->by_key.emplace(key, nullptr);
      return nullptr;
    }
    cache->resident_bytes += image->byte_size();
    cache->by_key.emplace(key, image);
    return image;
  }

  int probe_width = 0;
  int probe_height = 0;
  int probe_components = 0;
  if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                             &probe_width, &probe_height, &probe_components) ||
      probe_width <= 0 || probe_height <= 0) {
    cache->by_key.emplace(key, nullptr);
    return nullptr;
  }
  const uint64_t decoded_bytes = static_cast<uint64_t>(probe_width) *
                                 static_cast<uint64_t>(probe_height) * 4u;
  const uint64_t ceiling =
      cache->max_bytes > 0 ? cache->max_bytes * 2u : (2ull << 30);
  if (decoded_bytes > ceiling) {
    cache->by_key.emplace(key, nullptr);
    return nullptr;
  }

  auto image = std::make_shared<ImageData>();
  image->resolved_path = resolved;
  image->srgb = srgb;

  int width = 0;
  int height = 0;
  int components = 0;
  if (stbi_is_hdr_from_memory(bytes.data(), static_cast<int>(bytes.size()))) {
    float* pixels = stbi_loadf_from_memory(bytes.data(),
                                           static_cast<int>(bytes.size()),
                                           &width, &height, &components, 3);
    if (!pixels || width <= 0 || height <= 0) {
      stbi_image_free(pixels);
      cache->by_key.emplace(key, nullptr);
      return nullptr;
    }
    int dst_width = width;
    int dst_height = height;
    const uint32_t max_edge = cache->max_edge;
    if (max_edge > 0 &&
        static_cast<uint32_t>(std::max(width, height)) > max_edge) {
      const float scale = static_cast<float>(max_edge) /
                          static_cast<float>(std::max(width, height));
      dst_width = std::max(1, static_cast<int>(width * scale));
      dst_height = std::max(1, static_cast<int>(height * scale));
    }
    auto out = std::make_shared<std::vector<float>>();
    if (dst_width != width || dst_height != height) {
      BoxDownsampleRGBFloat(pixels, width, height, dst_width, dst_height, out.get());
    } else {
      out->assign(pixels, pixels + static_cast<size_t>(width) * height * 3);
    }
    stbi_image_free(pixels);
    image->is_float = true;
    image->channels = 3;
    image->width = static_cast<uint32_t>(dst_width);
    image->height = static_cast<uint32_t>(dst_height);
    image->rgb = out;
  } else {
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(),
                                            static_cast<int>(bytes.size()),
                                            &width, &height, &components, 4);
    if (!pixels || width <= 0 || height <= 0) {
      stbi_image_free(pixels);
      cache->by_key.emplace(key, nullptr);
      return nullptr;
    }
    int dst_width = width;
    int dst_height = height;
    const uint32_t max_edge = cache->max_edge;
    if (max_edge > 0 &&
        static_cast<uint32_t>(std::max(width, height)) > max_edge) {
      const float scale = static_cast<float>(max_edge) /
                          static_cast<float>(std::max(width, height));
      dst_width = std::max(1, static_cast<int>(width * scale));
      dst_height = std::max(1, static_cast<int>(height * scale));
    }
    auto out = std::make_shared<std::vector<uint8_t>>();
    if (dst_width != width || dst_height != height) {
      BoxDownsampleRGBA8(pixels, width, height, dst_width, dst_height, out.get());
    } else {
      out->assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    }
    stbi_image_free(pixels);
    image->is_float = false;
    image->channels = 4;
    image->width = static_cast<uint32_t>(dst_width);
    image->height = static_cast<uint32_t>(dst_height);
    image->rgba = out;
  }

  if (!image->valid()) {
    cache->by_key.emplace(key, nullptr);
    return nullptr;
  }
  if (cache->max_bytes > 0 &&
      cache->resident_bytes + image->byte_size() > cache->max_bytes) {
    cache->by_key.emplace(key, nullptr);
    return nullptr;
  }
  cache->resident_bytes += image->byte_size();
  cache->by_key.emplace(key, image);
  return image;
}

void TransformPointColumn(const float m[16], const float p[3], float out[3]) {
  out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
  out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
  out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

float ColumnScale(const float m[16]) {
  const auto len = [](float x, float y, float z) {
    return std::sqrt(x * x + y * y + z * z);
  };
  return (len(m[0], m[1], m[2]) + len(m[4], m[5], m[6]) +
          len(m[8], m[9], m[10])) /
         3.0f;
}

void TransformPointColumnD(const double m[16], const float p[3], float out[3]) {
  out[0] = static_cast<float>(m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]);
  out[1] = static_cast<float>(m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]);
  out[2] = static_cast<float>(m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]);
}

bool AssetExists(ImageCache* cache, const std::string& resolved) {
  if (!cache || resolved.empty()) return false;
  const std::string asset = UsdzEntryName(resolved);
  if (cache->usdz) {
    for (size_t i = 0; i < cache->usdz->NumEntries(); ++i) {
      if (lightusd::tydra::next::UsdzEntryMatches(cache->usdz->EntryName(i),
                                                  asset)) {
        return true;
      }
    }
  }
  std::string path = asset;
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  if (!path.empty() && path[0] != '/') {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) {
      std::fclose(f);
      return true;
    }
    if (!cache->base_dir.empty()) path = cache->base_dir + "/" + path;
  }
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f) std::fclose(f);
  return f != nullptr;
}

std::string ReplaceUdimToken(const std::string& pattern, uint32_t udim) {
  const size_t token = pattern.find("<UDIM>");
  if (token == std::string::npos) return pattern;
  std::string out = pattern;
  out.replace(token, 6, std::to_string(udim));
  return out;
}

std::string BaseName(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void GatherNonMeshGeometry(lightusd_render_scene* render, SceneData* out,
                           const float color_matrix[9]) {
  const uint32_t node_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_NODE));
  const bool color_transform = !ColorMatrixIsIdentity(color_matrix);
  for (uint32_t i = 0; i < node_count; ++i) {
    lightusd_render_node_info info{};
    if (lightusd_render_node_get_info(render, static_cast<int32_t>(i), &info) !=
        LIGHTUSD_OK) {
      continue;
    }
    const bool is_points =
        info.type == static_cast<uint8_t>(lightusd::tydra::next::NodeType::Points);
    const bool is_curves =
        info.type == static_cast<uint8_t>(lightusd::tydra::next::NodeType::Curves);
    if ((!is_points && !is_curves) || info.data_id < 0) continue;
    const float* world = info.world_transform;
    const float scale = ColumnScale(world);

    if (is_points) {
      lightusd_buffer_view pos{};
      lightusd_buffer_view widths{};
      lightusd_buffer_view colors{};
      if (lightusd_render_points_buffer(render, info.data_id,
                                        LIGHTUSD_POINTS_BUF_POSITIONS,
                                        &pos) != LIGHTUSD_OK ||
          !pos.data || pos.nbytes < 3 * sizeof(float)) {
        continue;
      }
      lightusd_render_points_buffer(render, info.data_id,
                                    LIGHTUSD_POINTS_BUF_WIDTHS, &widths);
      lightusd_render_points_buffer(render, info.data_id,
                                    LIGHTUSD_POINTS_BUF_COLORS, &colors);
      const size_t count = pos.nbytes / (3 * sizeof(float));
      const float* positions = static_cast<const float*>(pos.data);
      const float* width_data =
          widths.data ? static_cast<const float*>(widths.data) : nullptr;
      const size_t width_count = widths.nbytes / sizeof(float);
      const float* color_data =
          colors.data ? static_cast<const float*>(colors.data) : nullptr;
      const size_t color_count = colors.nbytes / sizeof(float) / 3;
      for (size_t p = 0; p < count; ++p) {
        lmv_splat splat{};
        TransformPointColumn(world, &positions[p * 3], &splat.x);
        const float width =
            width_data && p < width_count ? width_data[p] : 0.01f;
        splat.radius = std::max(width * 0.5f * scale, 1e-5f);
        splat.r = splat.g = splat.b = 0.8f;
        if (color_data && p < color_count) {
          splat.r = color_data[p * 3 + 0];
          splat.g = color_data[p * 3 + 1];
          splat.b = color_data[p * 3 + 2];
        }
        if (color_transform) {
          ApplyColorMatrix(color_matrix, &splat.r);
        }
        splat.a = 1.0f;
        out->splats.push_back(splat);
      }
      continue;
    }

    lightusd_buffer_view points{};
    lightusd_buffer_view counts{};
    lightusd_buffer_view widths{};
    lightusd_buffer_view colors{};
    lightusd_buffer_view opacities{};
    lightusd_render_curves_buffer(render, info.data_id,
                                  LIGHTUSD_CURVES_BUF_TESSELLATED_POINTS,
                                  &points);
    if (!points.data) {
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_POINTS, &points);
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_VERTEX_COUNTS, &counts);
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_WIDTHS, &widths);
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_COLORS, &colors);
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_OPACITIES, &opacities);
    } else {
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_TESSELLATED_COUNTS,
                                    &counts);
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_TESSELLATED_WIDTHS,
                                    &widths);
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_TESSELLATED_COLORS,
                                    &colors);
      lightusd_render_curves_buffer(render, info.data_id,
                                    LIGHTUSD_CURVES_BUF_TESSELLATED_OPACITIES,
                                    &opacities);
    }
    if (!points.data || !counts.data || points.nbytes < 3 * sizeof(float)) {
      continue;
    }
    const float* positions = static_cast<const float*>(points.data);
    const uint32_t* curve_counts = static_cast<const uint32_t*>(counts.data);
    const size_t curve_count = counts.nbytes / sizeof(uint32_t);
    const size_t position_count = points.nbytes / (3 * sizeof(float));
    const float* width_data =
        widths.data ? static_cast<const float*>(widths.data) : nullptr;
    const size_t width_count = widths.nbytes / sizeof(float);
    const float* color_data =
        colors.data ? static_cast<const float*>(colors.data) : nullptr;
    const size_t color_count = colors.nbytes / sizeof(float) / 3;
    const float* opacity_data =
        opacities.data ? static_cast<const float*>(opacities.data) : nullptr;
    const size_t opacity_count = opacities.nbytes / sizeof(float);
    size_t cursor = 0;
    for (size_t c = 0; c < curve_count; ++c) {
      const size_t n = curve_counts[c];
      if (n < 2 || cursor + n > position_count) {
        cursor += n;
        continue;
      }
      for (size_t s = 0; s + 1 < n; ++s) {
        const size_t i0 = cursor + s;
        const size_t i1 = i0 + 1;
        lmv_line_segment seg{};
        TransformPointColumn(world, &positions[i0 * 3], seg.p0);
        TransformPointColumn(world, &positions[i1 * 3], seg.p1);
        const float w0 = width_data && i0 < width_count ? width_data[i0]
                                                        : 0.004f;
        const float w1 = width_data && i1 < width_count ? width_data[i1]
                                                        : w0;
        seg.width = std::max((w0 + w1) * 0.5f * scale, 1e-5f);
        seg.r = seg.g = seg.b = 0.8f;
        seg.a = 1.0f;
        if (color_data && i0 < color_count) {
          seg.r = color_data[i0 * 3 + 0];
          seg.g = color_data[i0 * 3 + 1];
          seg.b = color_data[i0 * 3 + 2];
        }
        if (opacity_data && i0 < opacity_count) seg.a = opacity_data[i0];
        if (color_transform) ApplyColorMatrix(color_matrix, &seg.r);
        out->lines.push_back(seg);
      }
      cursor += n;
    }
  }

  // UsdVol ParticleFields (3D Gaussian splats) as camera-facing splats.
  const size_t particle_count = lightusd_render_particle_data_count(render);
  for (size_t i = 0; i < particle_count; ++i) {
    lightusd_render_particle_data data{};
    if (lightusd_render_particle_data_get(render, static_cast<int32_t>(i),
                                          &data) != LIGHTUSD_OK ||
        !data.positions || data.count == 0) {
      continue;
    }
    const float* opacities = data.opacities;
    const float* scales = data.scales;
    const float* sh = data.sh_coefficients;
    const size_t coefficients =
        data.sh_degree >= 0
            ? static_cast<size_t>(data.sh_degree + 1) *
                  static_cast<size_t>(data.sh_degree + 1)
            : 0;
    for (size_t p = 0; p < data.count; ++p) {
      lmv_splat splat{};
      TransformPointColumnD(data.world_matrix, &data.positions[p * 3],
                            &splat.x);
      const float sx = scales ? scales[p * 3 + 0] : 0.01f;
      const float sy = scales ? scales[p * 3 + 1] : sx;
      const float sz = scales ? scales[p * 3 + 2] : sx;
      splat.radius = std::max(0.5f * std::max(sx, std::max(sy, sz)), 1e-5f);
      splat.r = splat.g = splat.b = 0.8f;
      if (sh && coefficients > 0) {
        const size_t base = p * coefficients * 3;
        splat.r = sh[base + 0];
        splat.g = sh[base + 1];
        splat.b = sh[base + 2];
      }
      if (color_transform) ApplyColorMatrix(color_matrix, &splat.r);
      splat.a = opacities ? std::max(0.0f, std::min(1.0f, opacities[p])) : 1.0f;
      out->splats.push_back(splat);
    }
  }
}

// ---------------------------------------------------------------------------
// Scene construction
// ---------------------------------------------------------------------------

void ExpandBounds(const float world[16], const float local_min[3],
                  const float local_max[3], float out_min[3],
                  float out_max[3], bool* valid) {
  for (int i = 0; i < 8; ++i) {
    const float x = (i & 1) ? local_max[0] : local_min[0];
    const float y = (i & 2) ? local_max[1] : local_min[1];
    const float z = (i & 4) ? local_max[2] : local_min[2];
    const float p[3] = {
        world[0] * x + world[4] * y + world[8] * z + world[12],
        world[1] * x + world[5] * y + world[9] * z + world[13],
        world[2] * x + world[6] * y + world[10] * z + world[14]};
    if (!*valid) {
      out_min[0] = out_max[0] = p[0];
      out_min[1] = out_max[1] = p[1];
      out_min[2] = out_max[2] = p[2];
      *valid = true;
    } else {
      for (int k = 0; k < 3; ++k) {
        out_min[k] = std::min(out_min[k], p[k]);
        out_max[k] = std::max(out_max[k], p[k]);
      }
    }
  }
}

std::string CopyInstancerPrototypePath(lightusd_render_scene* render,
                                       int32_t instancer_id,
                                       uint32_t prototype_index) {
  size_t required = 0;
  if (lightusd_render_instancer_prototype_path_copy(
          render, instancer_id, prototype_index, nullptr, 0, &required) !=
      LIGHTUSD_OK) {
    return std::string();
  }
  std::string path(required, '\0');
  if (required > 0 &&
      lightusd_render_instancer_prototype_path_copy(
          render, instancer_id, prototype_index, path.data(), required + 1,
          &required) != LIGHTUSD_OK) {
    return std::string();
  }
  return path;
}

bool PathIsUnder(const std::string& path, const std::string& root) {
  if (root.empty() || path.size() <= root.size()) return false;
  return path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
}

uint32_t MapLightType(uint8_t type) {
  switch (type) {
    case 0: return LMV_LIGHT_POINT;
    case 1: return LMV_LIGHT_DIRECTIONAL;
    case 2: return LMV_LIGHT_SPOT;
    case 3: return LMV_LIGHT_RECT;
    case 4: return LMV_LIGHT_DISK;
    case 5: return LMV_LIGHT_DOME;
    case 6: return LMV_LIGHT_SPHERE;
    default: return LMV_LIGHT_OTHER;
  }
}

// CPU fallback for skinned meshes that cannot use the world-space GPU palette:
// multiple draws of the same mesh (native instances) or a baked PointInstancer
// transform. Produces the same local posed geometry the old CPU bake did, then
// drops the skin/morph playback data so the shader does not apply it twice.
void BakeMeshSkinInPlace(MeshData* mesh) {
  if (!mesh || !mesh->skinned) return;
  const size_t vertex_count = mesh->vertices.size();
  if (vertex_count == 0 || mesh->joint_indices.size() != vertex_count * 4 ||
      mesh->joint_weights.size() != vertex_count * 4) {
    return;
  }
  std::vector<Mat4d> local;
  if (!BuildLocalSkinPalette(*mesh, &local)) return;

  const size_t morph_targets = mesh->morph_deltas.size();
  for (size_t v = 0; v < vertex_count; ++v) {
    float p[3] = {mesh->vertices[v].px, mesh->vertices[v].py,
                  mesh->vertices[v].pz};
    for (size_t t = 0; t < morph_targets && t < mesh->morph_weights.size();
         ++t) {
      const std::vector<float>& deltas = mesh->morph_deltas[t];
      if (deltas.size() < vertex_count * 3) continue;
      const float weight = mesh->morph_weights[t];
      p[0] += weight * deltas[v * 3 + 0];
      p[1] += weight * deltas[v * 3 + 1];
      p[2] += weight * deltas[v * 3 + 2];
    }
    const float n[3] = {mesh->vertices[v].nx, mesh->vertices[v].ny,
                        mesh->vertices[v].nz};
    double acc[3] = {0.0, 0.0, 0.0};
    double acc_n[3] = {0.0, 0.0, 0.0};
    double weight_sum = 0.0;
    for (int k = 0; k < 4; ++k) {
      const float w = mesh->joint_weights[v * 4 + k];
      if (!(w > 0.0f)) continue;
      const uint32_t j = mesh->joint_indices[v * 4 + k];
      if (j >= local.size()) continue;
      const Mat4d& m = local[j];
      for (int c = 0; c < 3; ++c) {
        acc[c] += double(w) * (double(p[0]) * m.m[0][c] +
                               double(p[1]) * m.m[1][c] +
                               double(p[2]) * m.m[2][c] + m.m[3][c]);
        acc_n[c] += double(w) * (double(n[0]) * m.m[0][c] +
                                 double(n[1]) * m.m[1][c] +
                                 double(n[2]) * m.m[2][c]);
      }
      weight_sum += double(w);
    }
    if (weight_sum <= 0.0) continue;
    mesh->vertices[v].px = static_cast<float>(acc[0] / weight_sum);
    mesh->vertices[v].py = static_cast<float>(acc[1] / weight_sum);
    mesh->vertices[v].pz = static_cast<float>(acc[2] / weight_sum);
    const double len =
        std::sqrt(acc_n[0] * acc_n[0] + acc_n[1] * acc_n[1] +
                  acc_n[2] * acc_n[2]);
    if (len > 1e-12) {
      mesh->vertices[v].nx = static_cast<float>(acc_n[0] / len);
      mesh->vertices[v].ny = static_cast<float>(acc_n[1] / len);
      mesh->vertices[v].nz = static_cast<float>(acc_n[2] / len);
    }
  }

  mesh->skinned = false;
  mesh->influences = 0;
  mesh->joint_indices.clear();
  mesh->joint_weights.clear();
  mesh->skin_matrices.clear();
  mesh->morph_targets = 0;
  mesh->morph_names.clear();
  mesh->morph_base_weights.clear();
  mesh->morph_weights.clear();
  mesh->morph_deltas.clear();
}

bool NearlyEqual(float a, float b) {
  const float scale = std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
  return std::fabs(a - b) <= 1e-6f * scale;
}

bool EvalFloatScalar(lightusd_stage* stage, lightusd_prim prim,
                     const char* name, double time, float* out) {
  if (!stage || !out) return false;
  lightusd_value* value = nullptr;
  if (lightusd_attr_eval(stage, prim, name, time, &value) != LIGHTUSD_OK ||
      !value) {
    return false;
  }
  bool ok = false;
  lightusd_value_view view{};
  if (lightusd_value_get_view(value, &view) == LIGHTUSD_OK && view.data &&
      view.storage == LIGHTUSD_COMP_FLOAT32 && view.components >= 1 &&
      view.count >= 1) {
    *out = static_cast<const float*>(view.data)[0];
    ok = true;
  }
  lightusd_value_destroy(value);
  return ok;
}

bool EvalFloat2(lightusd_stage* stage, lightusd_prim prim, const char* name,
                double time, float out[2]) {
  if (!stage || !out) return false;
  lightusd_value* value = nullptr;
  if (lightusd_attr_eval(stage, prim, name, time, &value) != LIGHTUSD_OK ||
      !value) {
    return false;
  }
  bool ok = false;
  lightusd_value_view view{};
  if (lightusd_value_get_view(value, &view) == LIGHTUSD_OK && view.data &&
      view.storage == LIGHTUSD_COMP_FLOAT32 && view.components >= 2 &&
      view.count >= 1) {
    const float* src = static_cast<const float*>(view.data);
    out[0] = src[0];
    out[1] = src[1];
    ok = true;
  }
  lightusd_value_destroy(value);
  return ok;
}

bool EvalColor3(lightusd_stage* stage, lightusd_prim prim, const char* name,
                double time, float out[3]) {
  if (!stage || !out) return false;
  lightusd_value* value = nullptr;
  if (lightusd_attr_eval(stage, prim, name, time, &value) != LIGHTUSD_OK ||
      !value) {
    return false;
  }
  bool ok = false;
  lightusd_value_view view{};
  if (lightusd_value_get_view(value, &view) == LIGHTUSD_OK && view.data &&
      view.storage == LIGHTUSD_COMP_FLOAT32 && view.components >= 3 &&
      view.count >= 1) {
    const float* src = static_cast<const float*>(view.data);
    out[0] = src[0];
    out[1] = src[1];
    out[2] = src[2];
    ok = true;
  }
  lightusd_value_destroy(value);
  return ok;
}

bool RefreshCameraParams(lightusd_stage* stage, lightusd_prim prim,
                         double time, CameraData* camera) {
  if (!stage || !camera || !lightusd_prim_is_valid(prim)) return false;
  bool changed = false;
  struct Param {
    const char* name;
    float* value;
  };
  const Param params[] = {
      {"focalLength", &camera->focal_length},
      {"horizontalAperture", &camera->horizontal_aperture},
      {"verticalAperture", &camera->vertical_aperture},
      {"orthoWidth", &camera->ortho_width},
      {"focusDistance", &camera->focus_distance},
      {"exposure", &camera->exposure},
      {"fStop", &camera->fstop},
  };
  float scalar = 0.0f;
  for (const Param& param : params) {
    if (!EvalFloatScalar(stage, prim, param.name, time, &scalar)) continue;
    if (!NearlyEqual(*param.value, scalar)) {
      *param.value = scalar;
      changed = true;
    }
  }
  float clip[2] = {0.0f, 0.0f};
  if (EvalFloat2(stage, prim, "clippingRange", time, clip)) {
    if (!NearlyEqual(camera->near_clip, clip[0])) {
      camera->near_clip = clip[0];
      changed = true;
    }
    if (!NearlyEqual(camera->far_clip, clip[1])) {
      camera->far_clip = clip[1];
      changed = true;
    }
  }
  return changed;
}

size_t ArrayElementCount(lightusd_stage* stage, lightusd_prim prim,
                         const char* name, double time, uint32_t components) {
  if (!stage || components == 0) return 0;
  lightusd_value* value = nullptr;
  if (lightusd_attr_eval(stage, prim, name, time, &value) != LIGHTUSD_OK ||
      !value) {
    return 0;
  }
  size_t count = 0;
  lightusd_value_view view{};
  if (lightusd_value_get_view(value, &view) == LIGHTUSD_OK && view.data &&
      view.count > 0 && view.components > 0) {
    const size_t total = view.count * static_cast<size_t>(view.components);
    count = total / components;
  }
  lightusd_value_destroy(value);
  return count;
}

size_t MaxInstanceCount(lightusd_stage* stage, lightusd_prim prim,
                        double time_code) {
  if (!lightusd_prim_is_valid(prim)) return 0;
  size_t max_count = ArrayElementCount(stage, prim, "positions", time_code, 3);
  max_count = std::max(
      max_count,
      ArrayElementCount(stage, prim, "protoIndices", time_code, 1));
  const char* names[] = {"positions", "protoIndices"};
  for (const char* name : names) {
    const size_t samples = lightusd_attr_timesample_count(prim, name);
    if (samples == 0) continue;
    std::vector<double> times(samples);
    lightusd_attr_timesample_times(prim, name, times.data(), samples);
    for (double sample : times) {
      max_count = std::max(
          max_count,
          ArrayElementCount(stage, prim, name, sample, name[0] == 'p' ? 3 : 1));
    }
  }
  return max_count;
}

uint32_t FindProtoMeshRef(const SceneData::InstancerPlayback& playback,
                          uint32_t proto_index, int32_t mesh_id) {
  if (proto_index + 1 >= playback.proto_mesh_offsets.size()) return 0;
  const uint32_t begin = playback.proto_mesh_offsets[proto_index];
  const uint32_t end = playback.proto_mesh_offsets[proto_index + 1];
  for (uint32_t ref = begin; ref < end && ref < playback.proto_mesh_ids.size();
       ++ref) {
    if (playback.proto_mesh_ids[ref] == mesh_id) return ref;
  }
  return begin;
}

bool BuildSceneData(lightusd_stage* stage, lightusd_render_scene* render,
                    ImageCache* cache, const lmv_load_options& opts,
                    double time_code, const float color_matrix[9],
                    SceneData* out, char* err, size_t err_cap) {
  uint64_t triangle_budget = 0;
  const uint32_t mesh_count =
      static_cast<uint32_t>(lightusd_render_count(render, LIGHTUSD_RENDER_MESH));
  // Images are declared before textures so UDIM tiles and Ptex atlases can
  // append their own entries.
  const uint32_t image_count =
      static_cast<uint32_t>(lightusd_render_count(render, LIGHTUSD_RENDER_IMAGE));
  out->images.assign(image_count, nullptr);
  std::vector<bool> mesh_valid(mesh_count, false);
  out->meshes.resize(mesh_count);
  const uint32_t material_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_MATERIAL));
  std::vector<bool> ptex_material(material_count, false);
  for (uint32_t i = 0; i < material_count; ++i) {
    ptex_material[i] = MaterialUsesPtex(render, static_cast<int32_t>(i));
  }
  uint32_t skipped_meshes = 0;
  for (uint32_t i = 0; i < mesh_count; ++i) {
    if (ProgressCancelled(opts)) {
      SetError(err, err_cap, "cancelled");
      return false;
    }
    lightusd_render_mesh_info mesh_info{};
    lightusd_render_mesh_get_info(render, static_cast<int32_t>(i), &mesh_info);
    const bool weld_face_ids =
        mesh_info.material_id >= 0 &&
        static_cast<size_t>(mesh_info.material_id) < ptex_material.size() &&
        ptex_material[static_cast<size_t>(mesh_info.material_id)];
    BuildMesh(stage, render, static_cast<int32_t>(i), time_code,
              weld_face_ids,
              opts.max_triangles ? opts.max_triangles : (20u << 20),
              &triangle_budget, &out->meshes[i]);
    mesh_valid[i] = out->meshes[i].valid;
    if (!mesh_valid[i]) ++skipped_meshes;
  }

  // PointInstancer prototype roots are drawn through their instance records,
  // not as ordinary nodes.
  std::vector<std::string> instancer_prototypes;
  const uint32_t instancer_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_INSTANCER));
  for (uint32_t i = 0; i < instancer_count; ++i) {
    lightusd_render_instancer_info info{};
    if (lightusd_render_instancer_get_info(render, static_cast<int32_t>(i),
                                           &info) != LIGHTUSD_OK) {
      continue;
    }
    for (uint32_t p = 0; p < info.prototype_count; ++p) {
      const std::string path =
          CopyInstancerPrototypePath(render, static_cast<int32_t>(i), p);
      if (!path.empty()) instancer_prototypes.push_back(path);
    }
  }

  const auto is_instancer_prototype = [&](const std::string& path) {
    for (const std::string& root : instancer_prototypes) {
      if (path == root || PathIsUnder(path, root)) return true;
    }
    return false;
  };

  // Draws: one per visible Mesh node. Native instances are already expanded to
  // per-instance mesh nodes by the loader (preserve_native_instances).
  const uint32_t node_count =
      static_cast<uint32_t>(lightusd_render_count(render, LIGHTUSD_RENDER_NODE));
  for (uint32_t i = 0; i < node_count; ++i) {
    lightusd_render_node_info info{};
    if (lightusd_render_node_get_info(render, static_cast<int32_t>(i), &info) !=
        LIGHTUSD_OK) {
      continue;
    }
    if (info.type != static_cast<uint8_t>(
                           lightusd::tydra::next::NodeType::Mesh)) {
      continue;
    }
    if (info.data_id < 0 || static_cast<uint32_t>(info.data_id) >= mesh_count) {
      continue;
    }
    if (!mesh_valid[static_cast<size_t>(info.data_id)]) continue;
    if (is_instancer_prototype(Sv(info.prim_path))) continue;
    DrawData draw;
    draw.mesh_id = info.data_id;
    draw.visible = info.visible != 0;
    draw.prim_path = Sv(info.prim_path);
    draw.dynamic = true;
    draw.visibility_animated =
        HasAnimatedVisibility(stage, PrimAtPath(stage, draw.prim_path));
    if (draw.visibility_animated) {
      // tydra's node visibility is the default value, so sample time-varying
      // visibility here (including inherited invisibility).
      draw.visible = PrimVisibleAt(stage, PrimAtPath(stage, draw.prim_path),
                                   time_code);
    }
    CopyMatrix4(info.world_transform, draw.world);
    out->draws.push_back(draw);
    if (draw.visible) {
      const MeshData& mesh = out->meshes[static_cast<size_t>(info.data_id)];
      ExpandBounds(draw.world, mesh.bbox_min, mesh.bbox_max, out->bbox_min,
                   out->bbox_max, &out->bounds_valid);
    }
  }

  // Instancer playback tables are shared by visible, hidden and placeholder
  // draws. Read the prototype mesh/transform tables once per instancer.
  const uint32_t playback_instancer_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_INSTANCER));
  for (uint32_t i = 0; i < playback_instancer_count; ++i) {
    lightusd_render_instancer_info info{};
    if (lightusd_render_instancer_get_info(render, static_cast<int32_t>(i),
                                           &info) != LIGHTUSD_OK ||
        !info.valid) {
      continue;
    }
    const std::string path = Sv(info.prim_path);
    SceneData::InstancerPlayback playback;
    playback.instance_count = static_cast<size_t>(info.instance_count);
    lightusd_buffer_view offsets{};
    lightusd_buffer_view ids{};
    lightusd_buffer_view transforms{};
    if (lightusd_render_instancer_buffer(render, static_cast<int32_t>(i),
                                         LIGHTUSD_INST_BUF_PROTO_MESH_OFFSETS,
                                         &offsets) == LIGHTUSD_OK &&
        offsets.data && offsets.nbytes > 0 &&
        lightusd_render_instancer_buffer(render, static_cast<int32_t>(i),
                                         LIGHTUSD_INST_BUF_PROTO_MESH_IDS,
                                         &ids) == LIGHTUSD_OK &&
        ids.data && ids.nbytes > 0 &&
        lightusd_render_instancer_buffer(render, static_cast<int32_t>(i),
                                         LIGHTUSD_INST_BUF_PROTO_TRANSFORMS,
                                         &transforms) == LIGHTUSD_OK &&
        transforms.data && transforms.nbytes > 0) {
      const uint32_t* offset_data =
          static_cast<const uint32_t*>(offsets.data);
      playback.proto_mesh_offsets.assign(
          offset_data, offset_data + offsets.nbytes / sizeof(uint32_t));
      const int32_t* id_data = static_cast<const int32_t*>(ids.data);
      playback.proto_mesh_ids.assign(id_data,
                                     id_data + ids.nbytes / sizeof(int32_t));
      const float* transform_data = static_cast<const float*>(transforms.data);
      playback.proto_mesh_transforms.assign(
          transform_data,
          transform_data + transforms.nbytes / sizeof(float));
    }
    const lightusd_prim prim = PrimAtPath(stage, path);
    playback.max_instance_count = MaxInstanceCount(stage, prim, time_code);
    playback.proto_indices_animated =
        lightusd_attr_has_timesamples(prim, "protoIndices");
    playback.count_animated =
        playback.max_instance_count > playback.instance_count ||
        lightusd_attr_has_timesamples(prim, "positions");
    out->instancers[path] = std::move(playback);
  }

  // PointInstancer draws: one per visible instance, reusing the prototype mesh.
  std::unordered_map<std::string, InstanceArrays> instance_array_cache;
  const uint32_t instance_draw_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_POINT_INSTANCE_DRAW));
  for (uint32_t i = 0; i < instance_draw_count; ++i) {
    lightusd_render_point_instance_draw_info info{};
    if (lightusd_render_point_instance_draw_get_info(
            render, static_cast<int32_t>(i), &info) != LIGHTUSD_OK) {
      continue;
    }
    int32_t mesh_id = info.expanded_mesh_id >= 0 ? info.expanded_mesh_id
                                                 : info.mesh_id;
    if (mesh_id < 0 ||
        static_cast<uint32_t>(mesh_id) >= mesh_count ||
        !mesh_valid[static_cast<size_t>(mesh_id)]) {
      continue;
    }
    DrawData draw;
    draw.mesh_id = mesh_id;
    draw.material_override = info.material_id;
    draw.dynamic = false;
    CopyMatrix4(info.transform, draw.world);
    lightusd_render_instancer_info instancer{};
    if (lightusd_render_instancer_get_info(
            render, info.point_instancer_id, &instancer) == LIGHTUSD_OK) {
      const std::string instancer_path = Sv(instancer.prim_path);
      auto it = instance_array_cache.find(instancer_path);
      if (it == instance_array_cache.end()) {
        InstanceArrays arrays;
        ReadInstanceArrays(stage, instancer_path, time_code, &arrays);
        it = instance_array_cache.emplace(instancer_path, std::move(arrays)).first;
      }
      if (!it->second.positions.empty()) {
        Mat4d instance_transform;
        ComposeInstanceTRS(it->second, info.instance_index, &instance_transform);
        draw.point_instance = true;
        draw.point_instance_animated = it->second.animated;
        draw.instance_visibility_animated = it->second.visibility_animated;
        draw.instance_id =
            info.instance_index < it->second.ids.size()
                ? it->second.ids[info.instance_index]
                : static_cast<int64_t>(info.instance_index);
        draw.instancer_prim_path = instancer_path;
        draw.instance_index = info.instance_index;
        const auto playback_it = out->instancers.find(instancer_path);
        if (playback_it != out->instancers.end()) {
          draw.proto_index = info.prototype_index;
          draw.proto_mesh_ref = FindProtoMeshRef(
              playback_it->second, info.prototype_index, mesh_id);
          draw.proto_index_animated =
              playback_it->second.proto_indices_animated;
        }
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            draw.instance_transform[r * 4 + c] =
                instance_transform.m[r][c];
          }
        }
      }
    }
    out->draws.push_back(draw);
    const MeshData& mesh = out->meshes[static_cast<size_t>(mesh_id)];
    ExpandBounds(draw.world, mesh.bbox_min, mesh.bbox_max, out->bbox_min,
                 out->bbox_max, &out->bounds_valid);
  }

  // Skinned meshes with more than one draw (native instances) or a baked
  // PointInstancer transform use the CPU fallback, matching the previous bake.
  {
    std::vector<uint32_t> draw_count(mesh_count, 0);
    std::vector<bool> baked_transform(mesh_count, false);
    for (const DrawData& draw : out->draws) {
      if (draw.mesh_id < 0 ||
          static_cast<uint32_t>(draw.mesh_id) >= mesh_count) {
        continue;
      }
      const size_t id = static_cast<size_t>(draw.mesh_id);
      ++draw_count[id];
      if (!draw.dynamic) baked_transform[id] = true;
    }
    for (uint32_t i = 0; i < mesh_count; ++i) {
      if (!out->meshes[i].skinned) continue;
      if (draw_count[i] > 1 || baked_transform[i]) {
        BakeMeshSkinInPlace(&out->meshes[i]);
      }
    }
  }

  // Instances that were invisible at conversion time have no entries in the
  // render scene's draw list. Add kept-but-hidden draws so animated
  // invisibleIds can reveal them without a re-conversion.
  const uint32_t hidden_instancer_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_INSTANCER));
  for (uint32_t i = 0; i < hidden_instancer_count; ++i) {
    lightusd_render_instancer_info instancer{};
    if (lightusd_render_instancer_get_info(render, static_cast<int32_t>(i),
                                           &instancer) != LIGHTUSD_OK ||
        !instancer.valid) {
      continue;
    }
    const auto buffer = [&](uint8_t kind, lightusd_buffer_view* view) {
      return lightusd_render_instancer_buffer(render, static_cast<int32_t>(i),
                                              kind, view) == LIGHTUSD_OK &&
             view->data && view->nbytes > 0;
    };
    lightusd_buffer_view visible{};
    lightusd_buffer_view transforms{};
    lightusd_buffer_view proto_indices{};
    lightusd_buffer_view proto_offsets{};
    lightusd_buffer_view proto_mesh_ids{};
    lightusd_buffer_view proto_transforms{};
    if (!buffer(LIGHTUSD_INST_BUF_VISIBLE, &visible) ||
        !buffer(LIGHTUSD_INST_BUF_TRANSFORMS, &transforms) ||
        !buffer(LIGHTUSD_INST_BUF_PROTO_INDICES, &proto_indices) ||
        !buffer(LIGHTUSD_INST_BUF_PROTO_MESH_OFFSETS, &proto_offsets) ||
        !buffer(LIGHTUSD_INST_BUF_PROTO_MESH_IDS, &proto_mesh_ids) ||
        !buffer(LIGHTUSD_INST_BUF_PROTO_TRANSFORMS, &proto_transforms)) {
      continue;
    }
    const size_t instance_count =
        std::min<size_t>(visible.nbytes, instancer.instance_count);
    const size_t transform_count = transforms.nbytes / (sizeof(float) * 16);
    const size_t proto_index_count = proto_indices.nbytes / sizeof(int32_t);
    const size_t offset_count = proto_offsets.nbytes / sizeof(uint32_t);
    const size_t mesh_ref_count = proto_mesh_ids.nbytes / sizeof(int32_t);
    const size_t proto_transform_count =
        proto_transforms.nbytes / (sizeof(float) * 16);
    const uint8_t* vis = static_cast<const uint8_t*>(visible.data);
    const int32_t* indices = static_cast<const int32_t*>(proto_indices.data);
    const uint32_t* offsets = static_cast<const uint32_t*>(proto_offsets.data);
    const int32_t* mesh_ids = static_cast<const int32_t*>(proto_mesh_ids.data);
    const std::string instancer_path = Sv(instancer.prim_path);
    auto cache_it = instance_array_cache.find(instancer_path);
    if (cache_it == instance_array_cache.end()) {
      InstanceArrays arrays;
      ReadInstanceArrays(stage, instancer_path, time_code, &arrays);
      cache_it =
          instance_array_cache.emplace(instancer_path, std::move(arrays)).first;
    }
    for (size_t inst = 0; inst < instance_count; ++inst) {
      if (vis[inst] != 0 || inst >= transform_count ||
          inst >= proto_index_count) {
        continue;
      }
      const int32_t proto = indices[inst];
      if (proto < 0 || static_cast<size_t>(proto) + 1 >= offset_count) continue;
      const uint32_t begin = offsets[static_cast<size_t>(proto)];
      const uint32_t end = offsets[static_cast<size_t>(proto) + 1];
      const Mat4d instance_transform =
          LoadRowMajor16f(static_cast<const float*>(transforms.data) + inst * 16);
      for (uint32_t mesh_ref = begin; mesh_ref < end; ++mesh_ref) {
        if (mesh_ref >= mesh_ref_count || mesh_ref >= proto_transform_count) {
          continue;
        }
        const int32_t mesh_id = mesh_ids[mesh_ref];
        if (mesh_id < 0 || static_cast<uint32_t>(mesh_id) >= mesh_count ||
            !mesh_valid[static_cast<size_t>(mesh_id)]) {
          continue;
        }
        const Mat4d proto_transform = LoadRowMajor16f(
            static_cast<const float*>(proto_transforms.data) +
            static_cast<size_t>(mesh_ref) * 16);
        const Mat4d world = Mul4d(proto_transform, instance_transform);
        DrawData draw;
        draw.mesh_id = mesh_id;
        draw.visible = false;
        draw.dynamic = false;
        draw.point_instance = true;
        draw.point_instance_animated = cache_it->second.animated;
        draw.instance_visibility_animated =
            cache_it->second.visibility_animated;
        draw.instance_id =
            inst < cache_it->second.ids.size()
                ? cache_it->second.ids[inst]
                : static_cast<int64_t>(inst);
        draw.instancer_prim_path = instancer_path;
        draw.instance_index = static_cast<uint32_t>(inst);
        draw.proto_index = static_cast<uint32_t>(proto);
        draw.proto_mesh_ref = mesh_ref;
        const auto playback_it = out->instancers.find(instancer_path);
        if (playback_it != out->instancers.end()) {
          draw.proto_index_animated =
              playback_it->second.proto_indices_animated;
        }
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            draw.world[r * 4 + c] =
                static_cast<float>(world.m[r][c]);
            draw.instance_transform[r * 4 + c] = instance_transform.m[r][c];
          }
        }
        out->draws.push_back(draw);
      }
    }
  }

  // Grow-capable placeholders: an instancer whose positions/protoIndices array
  // grows over time needs draw slots before those instances exist at load.
  for (const auto& entry : out->instancers) {
    const SceneData::InstancerPlayback& playback = entry.second;
    if (playback.max_instance_count <= playback.instance_count ||
        playback.proto_mesh_ids.empty() ||
        playback.proto_mesh_offsets.size() < 2) {
      continue;
    }
    uint32_t proto = 0;
    uint32_t mesh_ref = 0;
    bool found = false;
    for (size_t p = 0; p + 1 < playback.proto_mesh_offsets.size(); ++p) {
      const uint32_t begin = playback.proto_mesh_offsets[p];
      const uint32_t end = playback.proto_mesh_offsets[p + 1];
      if (begin < end && begin < playback.proto_mesh_ids.size()) {
        proto = static_cast<uint32_t>(p);
        mesh_ref = begin;
        found = true;
        break;
      }
    }
    if (!found) continue;
    const int32_t prototype_mesh_id = playback.proto_mesh_ids[mesh_ref];
    if (prototype_mesh_id < 0 ||
        static_cast<uint32_t>(prototype_mesh_id) >= mesh_count ||
        !mesh_valid[static_cast<size_t>(prototype_mesh_id)]) {
      continue;
    }
    for (size_t inst = playback.instance_count;
         inst < playback.max_instance_count; ++inst) {
      DrawData draw;
      draw.mesh_id = prototype_mesh_id;
      draw.visible = false;
      draw.dynamic = false;
      draw.point_instance = true;
      draw.point_instance_animated = true;
      draw.instance_visibility_animated = true;
      draw.proto_index_animated = playback.proto_indices_animated;
      draw.placeholder_draw = true;
      draw.proto_index = proto;
      draw.proto_mesh_ref = mesh_ref;
      draw.instance_id = static_cast<int64_t>(inst);
      draw.instancer_prim_path = entry.first;
      draw.instance_index = static_cast<uint32_t>(inst);
      out->draws.push_back(draw);
    }
  }

  GatherNonMeshGeometry(render, out, color_matrix);

  // Materials.
  out->materials.resize(material_count);
  for (uint32_t i = 0; i < material_count; ++i) {
    BuildMaterialData(render, static_cast<int32_t>(i), color_matrix,
                      &out->materials[i]);
  }

  // Textures.
  const uint32_t texture_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_TEXTURE));
  out->textures.resize(texture_count);
  for (uint32_t i = 0; i < texture_count; ++i) {
    lightusd_render_texture_info info{};
    if (lightusd_render_texture_get_info(render, static_cast<int32_t>(i),
                                         &info) != LIGHTUSD_OK) {
      continue;
    }
    TextureData& tex = out->textures[i];
    tex.asset_path = Sv(info.asset_path);
    tex.ktx2_hint = Sv(info.ktx2_hint);
    tex.image_id = info.image_id;
    tex.uv_set = UvSetFromPrimvarName(Sv(info.uv_primvar));
    if (tex.asset_path.find("<UDIM>") != std::string::npos) {
      // Resolve the authored <UDIM> pattern beside the lowest resolved tile.
      lightusd_render_image_info tile_info{};
      std::string resolved_tile;
      if (info.image_id >= 0 && lightusd_render_image_get_info(
              render, info.image_id, &tile_info) == LIGHTUSD_OK) {
        resolved_tile = Sv(tile_info.resolved_path);
      }
      std::string pattern;
      if (resolved_tile.find("<UDIM>") != std::string::npos) {
        pattern = resolved_tile;
      } else if (!resolved_tile.empty()) {
        pattern = DirName(resolved_tile) + "/" + BaseName(tex.asset_path);
      } else {
        pattern = tex.asset_path;
      }
      const bool tile_srgb =
          info.image_id < 0 ||
          tile_info.color_space ==
              static_cast<uint8_t>(lightusd::tydra::next::ColorSpace::sRGB);
      for (uint32_t udim = 1001; udim <= 1100; ++udim) {
        const std::string tile_path = ReplaceUdimToken(pattern, udim);
        if (!AssetExists(cache, tile_path)) continue;
        UdimTileData tile;
        tile.udim = udim;
        tile.image_id = static_cast<int32_t>(out->images.size());
        out->images.push_back(nullptr);
        out->udim_image_sources[tile.image_id] = {tile_path, tile_srgb};
        tex.udim_tiles.push_back(tile);
      }
      if (!tex.udim_tiles.empty()) tex.image_id = tex.udim_tiles.front().image_id;
    } else if (EndsWithNoCase(tex.asset_path, ".ptx")) {
      std::string resolved = tex.asset_path;
      lightusd_render_image_info ptex_info{};
      if (info.image_id >= 0 &&
          lightusd_render_image_get_info(render, info.image_id, &ptex_info) ==
              LIGHTUSD_OK) {
        const std::string candidate = Sv(ptex_info.resolved_path);
        if (!candidate.empty()) resolved = candidate;
      }
      std::vector<uint8_t> bytes;
      ReadAssetBytes(cache, resolved, &bytes);
      lmv::PtexAtlasOptions ptex_options;
      lmv::PtexAtlas atlas;
      std::string ptex_error;
      if (lmv::BuildPtexAtlas(resolved, bytes.empty() ? nullptr : bytes.data(),
                              bytes.size(), ptex_options, &atlas, &ptex_error) &&
          atlas.valid()) {
        auto image = std::make_shared<ImageData>();
        image->resolved_path = resolved;
        image->width = atlas.width;
        image->height = atlas.height;
        image->channels = 4;
        image->is_float = false;
        image->srgb = true;
        image->rgba = std::make_shared<const std::vector<uint8_t>>(
            std::move(atlas.rgba));
        const int32_t atlas_image_id =
            static_cast<int32_t>(out->images.size());
        out->images.push_back(image);
        tex.image_id = atlas_image_id;
        tex.is_ptex = true;
        tex.ptex_face_count = atlas.face_count;
        tex.ptex_rects.resize(atlas.rects.size());
        for (size_t r = 0; r < atlas.rects.size(); ++r) {
          tex.ptex_rects[r].x = atlas.rects[r].x;
          tex.ptex_rects[r].y = atlas.rects[r].y;
          tex.ptex_rects[r].w = atlas.rects[r].w;
          tex.ptex_rects[r].h = atlas.rects[r].h;
        }
      } else {
        out->warnings.push_back("Ptex decode failed: " + resolved + " (" +
                                ptex_error + ")");
      }
    }
    tex.wrap_s = info.wrap_s;
    tex.wrap_t = info.wrap_t;
    tex.channel = info.output_channel;
    tex.uv_offset[0] = info.uv_offset[0];
    tex.uv_offset[1] = info.uv_offset[1];
    tex.uv_scale[0] = info.uv_scale[0];
    tex.uv_scale[1] = info.uv_scale[1];
    tex.uv_rotation = info.uv_rotation;
    for (int k = 0; k < 4; ++k) {
      tex.bias[k] = info.bias[k];
      tex.scale[k] = info.scale[k];
    }
  }

  // Images: only decode the ones actually bound by a texture or dome light.
  std::unordered_set<int32_t> used_images;
  for (const TextureData& tex : out->textures) {
    if (tex.image_id >= 0) used_images.insert(tex.image_id);
    for (const UdimTileData& tile : tex.udim_tiles) {
      used_images.insert(tile.image_id);
    }
  }

  // Lights first, so dome textures are in the used set.
  std::unordered_set<int32_t> env_images;
  const uint32_t light_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_LIGHT));
  out->lights.resize(light_count);
  for (uint32_t i = 0; i < light_count; ++i) {
    lightusd_render_light_info info{};
    if (lightusd_render_light_get_info(render, static_cast<int32_t>(i), &info) !=
        LIGHTUSD_OK) {
      continue;
    }
    LightData& light = out->lights[i];
    light.name = Sv(info.name);
    light.prim_path = Sv(info.prim_path);
    light.type = MapLightType(info.type);
    light.color[0] = info.color[0];
    light.color[1] = info.color[1];
    light.color[2] = info.color[2];
    if (!ColorMatrixIsIdentity(color_matrix)) {
      ApplyColorMatrix(color_matrix, light.color);
    }
    light.intensity = info.intensity;
    light.exposure = info.exposure;
    light.param0 = info.param0;
    light.param1 = info.param1;
    CopyMatrix4(info.transform, light.transform);
    int32_t dome_texture = -1;
    if (lightusd_render_light_dome_texture_id(render, static_cast<int32_t>(i),
                                              &dome_texture) == LIGHTUSD_OK) {
      light.dome_texture_id = dome_texture;
      if (dome_texture >= 0 &&
          static_cast<size_t>(dome_texture) < out->images.size()) {
        light.dome_image_id = dome_texture;
        used_images.insert(dome_texture);
        env_images.insert(dome_texture);
      }
    }
  }

  // KTX2 companions declared on the texture (`customData = { asset ktx2 }`).
  std::unordered_map<int32_t, std::string> ktx2_by_image;
  for (const TextureData& texture : out->textures) {
    if (texture.image_id >= 0 && !texture.ktx2_hint.empty()) {
      ktx2_by_image.emplace(texture.image_id, texture.ktx2_hint);
    }
  }

  for (int32_t image_id : used_images) {
    if (image_id < 0) continue;
    if (static_cast<uint32_t>(image_id) >= out->images.size()) continue;
    if (ProgressCancelled(opts)) {
      SetError(err, err_cap, "cancelled");
      return false;
    }
    auto udim_source = out->udim_image_sources.find(image_id);
    if (udim_source != out->udim_image_sources.end()) {
      out->images[static_cast<size_t>(image_id)] =
          DecodeImage(cache, udim_source->second.path, udim_source->second.srgb,
                      std::string());
      continue;
    }
    lightusd_render_image_info info{};
    if (lightusd_render_image_get_info(render, image_id, &info) != LIGHTUSD_OK) {
      continue;
    }
    std::shared_ptr<const ImageData> image;
    if (info.is_loaded) {
      // The converter decoded pixels (custom loader build); copy them.
      lightusd_buffer_view view{};
      if (lightusd_render_image_buffer(render, image_id, &view) == LIGHTUSD_OK &&
          view.data && view.nbytes > 0) {
        auto data = std::make_shared<ImageData>();
        data->resolved_path = Sv(info.resolved_path);
        data->width = info.width;
        data->height = info.height;
        data->is_float =
            info.component_type == LIGHTUSD_COMP_FLOAT32 ||
            info.component_type == LIGHTUSD_COMP_FLOAT16;
        data->srgb = info.color_space ==
                     static_cast<uint8_t>(lightusd::tydra::next::ColorSpace::sRGB);
        data->channels = data->is_float ? 3u : 4u;
        if (data->is_float) {
          const float* src = static_cast<const float*>(view.data);
          auto pixels = std::make_shared<std::vector<float>>(
              src, src + view.nbytes / sizeof(float));
          data->rgb = pixels;
        } else {
          const uint8_t* src = static_cast<const uint8_t*>(view.data);
          auto pixels = std::make_shared<std::vector<uint8_t>>(
              src, src + view.nbytes);
          data->rgba = pixels;
        }
        if (data->valid()) image = data;
      }
    } else {
      // tydra records texture metadata only by design; decode here through the
      // shared next TextureDecoder (same code path as lusdview/lusdrender).
      const bool srgb =
          info.color_space ==
          static_cast<uint8_t>(lightusd::tydra::next::ColorSpace::sRGB);
      const std::string image_path = Sv(info.resolved_path);
      std::string hint;
      auto hint_it = ktx2_by_image.find(image_id);
      if (hint_it != ktx2_by_image.end()) hint = hint_it->second;
      image = DecodeImage(cache, image_path, srgb, hint,
                          env_images.count(image_id) == 0);
    }
    if (!image && EndsWithNoCase(Sv(info.resolved_path), ".exr")) {
      out->warnings.push_back(
          "EXR could not be decoded (unsupported compression, e.g. DWA): " +
          Sv(info.resolved_path));
    }
    out->images[static_cast<size_t>(image_id)] = image;
  }

  // Cameras.
  const uint32_t camera_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_CAMERA));
  out->cameras.resize(camera_count);
  for (uint32_t i = 0; i < camera_count; ++i) {
    lightusd_render_camera_info info{};
    if (lightusd_render_camera_get_info(render, static_cast<int32_t>(i), &info) !=
        LIGHTUSD_OK) {
      continue;
    }
    CameraData& camera = out->cameras[i];
    camera.name = Sv(info.name);
    camera.prim_path = Sv(info.prim_path);
    camera.orthographic = info.type == 1 ? 1u : 0u;
    camera.focal_length = info.focal_length;
    camera.horizontal_aperture = info.horizontal_aperture;
    camera.vertical_aperture = info.vertical_aperture;
    camera.ortho_width = info.ortho_width;
    camera.near_clip = info.near_clip;
    camera.far_clip = info.far_clip;
    camera.focus_distance = info.focus_distance;
    camera.exposure = info.exposure;
    camera.fstop = info.fstop;
    CopyMatrix4(info.transform, camera.transform);
    RefreshCameraParams(stage, PrimAtPath(stage, camera.prim_path), time_code,
                        &camera);
  }

  if (!out->bounds_valid) {
    out->bbox_min[0] = out->bbox_min[1] = out->bbox_min[2] = -1.0f;
    out->bbox_max[0] = out->bbox_max[1] = out->bbox_max[2] = 1.0f;
  }
  if (skipped_meshes > 0) {
    out->warnings.push_back(std::to_string(skipped_meshes) +
                            " mesh(es) skipped (empty, unsupported or over the "
                            "triangle budget)");
  }
  return true;
}

// Re-evaluates node transforms, skeleton palettes and blend-shape weights
// against the already-converted scene. Returns true when anything moved.
bool UpdateAnimationState(lightusd_stage* stage, SceneData* data, double time) {
  if (!stage || !data || data->force_reconversion) return false;
  bool changed = false;
  // Scenes whose animatable state is owned by the in-place path stay there
  // even when this exact time equals the previous one (loop seams/steps),
  // avoiding a re-conversion hitch. Node-only scenes still fall back when
  // nothing moved so other channels (textures, topology) can refresh.
  bool keep_in_place = false;

  std::unordered_map<std::string, InstanceArrays> instance_cache;
  for (DrawData& draw : data->draws) {
    if (draw.point_instance) {
      if (!draw.point_instance_animated &&
          !draw.instance_visibility_animated &&
          !draw.proto_index_animated && !draw.placeholder_draw) {
        continue;
      }
      keep_in_place = true;
      auto playback_it = data->instancers.find(draw.instancer_prim_path);
      const SceneData::InstancerPlayback* playback =
          playback_it != data->instancers.end() ? &playback_it->second
                                                : nullptr;
      auto it = instance_cache.find(draw.instancer_prim_path);
      if (it == instance_cache.end()) {
        InstanceArrays arrays;
        ReadInstanceArrays(stage, draw.instancer_prim_path, time, &arrays);
        it = instance_cache
                 .emplace(draw.instancer_prim_path, std::move(arrays))
                 .first;
      }
      const InstanceArrays& arrays = it->second;
      const size_t active_count = std::max(
          arrays.positions.size() / 3, arrays.proto_indices.size());
      if (draw.instance_index >= active_count) {
        if (draw.visible) {
          draw.visible = false;
          changed = true;
        }
        continue;
      }

      bool needs_mesh = draw.placeholder_draw || draw.proto_index_animated;
      uint32_t proto = draw.proto_index;
      if (arrays.proto_animated &&
          draw.instance_index < arrays.proto_indices.size()) {
        const int32_t value = arrays.proto_indices[draw.instance_index];
        if (value >= 0) proto = static_cast<uint32_t>(value);
      }
      if (needs_mesh && playback &&
          !playback->proto_mesh_offsets.empty() &&
          !playback->proto_mesh_ids.empty() &&
          !playback->proto_mesh_transforms.empty()) {
        if (proto + 1 >= playback->proto_mesh_offsets.size()) {
          return false;
        }
        const uint32_t begin = playback->proto_mesh_offsets[proto];
        const uint32_t end = playback->proto_mesh_offsets[proto + 1];
        if (end - begin != 1 || begin >= playback->proto_mesh_ids.size() ||
            begin * 16 + 16 > playback->proto_mesh_transforms.size()) {
          // Multi-mesh prototype changes would require adding/removing draws.
          return false;
        }
        const int32_t new_mesh_id = playback->proto_mesh_ids[begin];
        if (new_mesh_id < 0) return false;
        Mat4d new_instance;
        ComposeInstanceTRS(arrays, draw.instance_index, &new_instance);
        const Mat4d proto_transform = LoadRowMajor16f(
            playback->proto_mesh_transforms.data() + begin * 16);
        const Mat4d new_world = Mul4d(proto_transform, new_instance);
        if (draw.mesh_id != new_mesh_id) {
          draw.mesh_id = new_mesh_id;
          changed = true;
        }
        if (draw.proto_index != proto) {
          draw.proto_index = proto;
          changed = true;
        }
        draw.proto_mesh_ref = begin;
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            const float value = static_cast<float>(new_world.m[r][c]);
            float& dst = draw.world[r * 4 + c];
            if (!NearlyEqual(dst, value)) {
              dst = value;
              changed = true;
            }
            draw.instance_transform[r * 4 + c] = new_instance.m[r][c];
          }
        }
      } else if (draw.point_instance_animated && !arrays.positions.empty()) {
        const Mat4d old_instance =
            LoadRowMajor16d(draw.instance_transform);
        Mat4d inv_old_instance;
        if (Invert4d(old_instance, &inv_old_instance)) {
          Mat4d new_instance;
          ComposeInstanceTRS(arrays, draw.instance_index, &new_instance);
          double world_raw[16];
          for (int i = 0; i < 16; ++i) world_raw[i] = draw.world[i];
          const Mat4d new_world =
              Mul4d(Mul4d(LoadRowMajor16d(world_raw), inv_old_instance),
                    new_instance);
          for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
              const float value = static_cast<float>(new_world.m[r][c]);
              float& dst = draw.world[r * 4 + c];
              if (!NearlyEqual(dst, value)) {
                dst = value;
                changed = true;
              }
              draw.instance_transform[r * 4 + c] = new_instance.m[r][c];
            }
          }
        }
      }
      if (draw.instance_visibility_animated || draw.placeholder_draw ||
          (playback && playback->count_animated)) {
        const int64_t id =
            draw.instance_index < arrays.ids.size()
                ? arrays.ids[draw.instance_index]
                : draw.instance_id;
        bool hidden = false;
        for (int64_t hidden_id : arrays.inactive_ids) {
          if (hidden_id == id) hidden = true;
        }
        for (int64_t hidden_id : arrays.invisible_ids) {
          if (hidden_id == id) hidden = true;
        }
        const bool visible = !hidden;
        if (draw.visible != visible) {
          draw.visible = visible;
          changed = true;
        }
      }
      continue;
    }
    if (draw.prim_path.empty()) continue;
    lightusd_prim prim = PrimAtPath(stage, draw.prim_path);
    if (!lightusd_prim_is_valid(prim)) continue;
    if (draw.visibility_animated) {
      keep_in_place = true;
      const bool visible = PrimVisibleAt(stage, prim, time);
      if (draw.visible != visible) {
        draw.visible = visible;
        changed = true;
      }
    }
    if (!draw.dynamic) continue;
    double m[16] = {0};
    if (lightusd_prim_world_transform(stage, prim, time, m) != LIGHTUSD_OK) {
      continue;
    }
    for (int i = 0; i < 16; ++i) {
      const float value = static_cast<float>(m[i]);
      if (!NearlyEqual(draw.world[i], value)) {
        draw.world[i] = value;
        changed = true;
      }
    }
  }

  for (LightData& light : data->lights) {
    lightusd_prim prim = PrimAtPath(stage, light.prim_path);
    double m[16] = {0};
    if (!lightusd_prim_is_valid(prim) ||
        lightusd_prim_world_transform(stage, prim, time, m) != LIGHTUSD_OK) {
      continue;
    }
    for (int i = 0; i < 16; ++i) {
      const float value = static_cast<float>(m[i]);
      if (!NearlyEqual(light.transform[i], value)) {
        light.transform[i] = value;
        changed = true;
      }
    }
    float scalar = 0.0f;
    if (EvalFloatScalar(stage, prim, "inputs:intensity", time, &scalar) &&
        !NearlyEqual(light.intensity, scalar)) {
      light.intensity = scalar;
      changed = true;
    }
    if (EvalFloatScalar(stage, prim, "inputs:exposure", time, &scalar) &&
        !NearlyEqual(light.exposure, scalar)) {
      light.exposure = scalar;
      changed = true;
    }
    float rgb[3] = {0, 0, 0};
    if (EvalColor3(stage, prim, "inputs:color", time, rgb)) {
      ApplyColorMatrix(data->working_to_display, rgb);
      for (int c = 0; c < 3; ++c) {
        if (!NearlyEqual(light.color[c], rgb[c])) {
          light.color[c] = rgb[c];
          changed = true;
        }
      }
    }
  }

  for (CameraData& camera : data->cameras) {
    lightusd_prim prim = PrimAtPath(stage, camera.prim_path);
    double m[16] = {0};
    if (!lightusd_prim_is_valid(prim) ||
        lightusd_prim_world_transform(stage, prim, time, m) != LIGHTUSD_OK) {
      continue;
    }
    for (int i = 0; i < 16; ++i) {
      const float value = static_cast<float>(m[i]);
      if (!NearlyEqual(camera.transform[i], value)) {
        camera.transform[i] = value;
        changed = true;
      }
    }
    if (RefreshCameraParams(stage, prim, time, &camera)) changed = true;
  }

  for (MeshData& mesh : data->meshes) {
    if (mesh.skinned || mesh.morph_targets > 0) keep_in_place = true;
    if (mesh.skinned) {
      lightusd_prim skeleton = PrimAtPath(stage, mesh.skeleton_prim_path);
      lightusd_prim animation = PrimAtPath(stage, mesh.animation_prim_path);
      std::vector<Mat4d> pose;
      Mat4d skeleton_world;
      if (lightusd_prim_is_valid(skeleton) &&
          PrimWorld(stage, skeleton, time, &skeleton_world) &&
          PoseSkeleton(stage, skeleton, animation, time, &pose)) {
        const Mat4d geom_bind = LoadRowMajor16d(mesh.geom_bind);
        if (mesh.skin_matrices.size() != pose.size() * 16) {
          mesh.skin_matrices.assign(pose.size() * 16, 0.0f);
          changed = true;
        }
        for (size_t j = 0; j < pose.size(); ++j) {
          const Mat4d world =
              Mul4d(geom_bind, Mul4d(pose[j], skeleton_world));
          for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
              const float value = static_cast<float>(world.m[r][c]);
              float& dst = mesh.skin_matrices[j * 16 + r * 4 + c];
              if (!NearlyEqual(dst, value)) {
                dst = value;
                changed = true;
              }
            }
          }
        }
      }
    }

    if (mesh.morph_targets > 0) {
      std::vector<float> weights = mesh.morph_base_weights;
      lightusd_prim animation = PrimAtPath(stage, mesh.animation_prim_path);
      std::unordered_map<std::string, float> animated;
      EvaluateBlendShapeWeights(stage, animation, time, &animated);
      for (size_t i = 0; i < mesh.morph_names.size() && i < weights.size();
           ++i) {
        auto it = animated.find(mesh.morph_names[i]);
        if (it != animated.end()) weights[i] = it->second;
      }
      if (mesh.morph_weights.size() != weights.size()) {
        mesh.morph_weights = weights;
        changed = true;
      } else {
        for (size_t i = 0; i < weights.size(); ++i) {
          if (!NearlyEqual(mesh.morph_weights[i], weights[i])) {
            mesh.morph_weights[i] = weights[i];
            changed = true;
          }
        }
      }
    }
  }
  return changed || keep_in_place;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public ABI
// ---------------------------------------------------------------------------

struct lmv_scene {
  std::string path;
  lightusd_stage* stage = nullptr;
  lmv_load_options opts{};
  std::shared_ptr<ImageCache> cache;
  SceneData data;
  // Load-time diagnostics kept across time re-conversions.
  std::vector<std::string> base_warnings;
  float working_to_display[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  double time_code = 0.0;
  double start_time = 0.0;
  double end_time = 0.0;
  double fps = 24.0;
  int up_axis = 0;
  float meters_per_unit = 1.0f;
  uint32_t animation_count = 0;
  bool force_reconversion = false;

  // Variant sets of the composed stage, collected on first request.
  struct VariantSetInfo {
    std::string prim_path, set_name, selection;
    std::vector<std::string> variants;
    std::vector<const char*> variant_ptrs;  // into `variants`
  };
  mutable std::vector<VariantSetInfo> variant_sets;
  mutable bool variant_sets_built = false;
};

namespace {

lmv_status Convert(lightusd_render_scene* render, const lmv_load_options& opts,
                   double time_code, lmv_scene* scene, char* err,
                   size_t err_cap) {
  lightusd_render_scene_info info{};
  if (lightusd_render_scene_get_info(render, &info) == LIGHTUSD_OK) {
    for (int i = 0; i < 9; ++i) {
      scene->working_to_display[i] = info.working_to_display_linear[i];
    }
  }
  SceneData data;
  data.force_reconversion = scene->force_reconversion;
  for (int i = 0; i < 9; ++i) {
    data.working_to_display[i] = scene->working_to_display[i];
  }
  if (!BuildSceneData(scene->stage, render, scene->cache.get(), opts, time_code,
                      scene->working_to_display, &data, err, err_cap)) {
    if (ProgressCancelled(opts)) {
      SetError(err, err_cap, "cancelled");
      return LMV_ERR_CANCELLED;
    }
    if (err && err[0] == '\0') SetError(err, err_cap, "scene conversion failed");
    return LMV_ERR_INTERNAL;
  }
  if (lightusd_render_scene_get_info(render, &info) == LIGHTUSD_OK) {
    scene->start_time = info.start_time;
    scene->end_time = info.end_time;
    scene->fps = info.frames_per_second > 0.0 ? info.frames_per_second : 24.0;
    scene->up_axis = info.up_axis != 0 ? 1 : 0;
    scene->meters_per_unit =
        info.meters_per_unit > 0.0f ? info.meters_per_unit : 1.0f;
  }
  scene->animation_count = static_cast<uint32_t>(
      lightusd_render_count(render, LIGHTUSD_RENDER_ANIMATION));
  scene->data = std::move(data);
  if (!scene->base_warnings.empty()) {
    scene->data.warnings.insert(scene->data.warnings.begin(),
                                scene->base_warnings.begin(),
                                scene->base_warnings.end());
  }
  scene->time_code = time_code;
  return LMV_OK;
}

lightusd_status CreateRenderScene(lightusd_stage* stage,
                                  const lmv_load_options& opts,
                                  double time_code,
                                  lightusd_render_scene** out) {
  lightusd_render_config cfg;
  lightusd_render_config_init(&cfg);
  cfg.triangulate = 1;
  cfg.compute_normals = 1;
  cfg.compute_tangents = 0;
  cfg.build_vertex_indices = 1;
  // Textures are decoded by this loader (tydra records metadata only).
  cfg.load_textures = 0;
  cfg.allow_missing_textures = 1;
  cfg.target_color_space = 1;  // linear working space
  cfg.material_binding_purpose = LIGHTUSD_MATERIAL_BINDING_PREVIEW;
  cfg.time_code = time_code;
  cfg.max_threads = 0;
  cfg.use_default_asset_resolver = 1;
  if (opts.max_memory_bytes > 0) {
    cfg.max_resident_bytes = opts.max_memory_bytes;
  }
  return lightusd_render_convert(stage, &cfg, out);
}

}  // namespace

extern "C" {

void lmv_load_options_init(lmv_load_options* opts) {
  if (!opts) return;
  std::memset(opts, 0, sizeof(*opts));
  opts->struct_size = sizeof(*opts);
  opts->max_memory_bytes = 1ull << 30;
  opts->max_texture_bytes = 512ull << 20;
  opts->max_texture_edge = 2048;
  opts->max_triangles = 20u << 20;
  opts->load_payloads = 1;
}

lmv_status lmv_load(const char* path, const lmv_load_options* options,
                    lmv_scene** out, char* err, size_t err_cap) {
  if (err && err_cap) err[0] = '\0';
  if (out) *out = nullptr;
  if (!path || !out) return LMV_ERR_INVALID_ARG;

  lmv_load_options opts;
  if (options && options->struct_size >= sizeof(lmv_load_options)) {
    opts = *options;
  } else {
    lmv_load_options_init(&opts);
  }

  std::unique_ptr<lmv_scene> scene(new (std::nothrow) lmv_scene());
  if (!scene) return LMV_ERR_LIMIT;
  scene->path = path;
  scene->opts = opts;

  // Persistent decoded-image cache (survives time re-conversions).
  scene->cache = std::make_shared<ImageCache>();
  scene->cache->base_dir = DirName(scene->path);
  scene->cache->max_edge =
      opts.max_texture_edge ? opts.max_texture_edge : 2048u;
  scene->cache->max_bytes =
      opts.max_texture_bytes ? opts.max_texture_bytes : (512ull << 20);
  if (EndsWith(scene->path, ".usdz")) {
    lightusd::next::USDZReader* reader = new (std::nothrow) lightusd::next::USDZReader();
    if (reader && reader->OpenFile(scene->path)) {
      scene->cache->usdz.reset(reader);
    } else {
      delete reader;
    }
  }

  if (opts.progress) {
    lmv_progress p{};
    p.phase = LMV_PHASE_OPEN;
    p.fraction = 0.0f;
    p.message = "opening";
    if (opts.progress(opts.progress_user, &p) == 0) return LMV_ERR_CANCELLED;
  }

  lightusd_load_options load;
  lightusd_load_options_init(&load);
  load.composed = 1;
  load.load_payloads = opts.load_payloads ? 1 : 0;
  // Keep native instance prototypes in the composed stage so every instance
  // becomes a drawable node (the radar preview does not share prototype data
  // across instances, which is fine for a preview).
  load.preserve_native_instances = 1;
  if (opts.max_memory_bytes > 0) {
    load.max_resident_bytes = opts.max_memory_bytes;
  }
  // Variant overrides: the single legacy pair plus any extras. Borrowed for the
  // duration of the load call.
  std::vector<const char*> override_sets, override_names;
  if (opts.variant_set && opts.variant_name) {
    override_sets.push_back(opts.variant_set);
    override_names.push_back(opts.variant_name);
  }
  for (uint32_t i = 0; i < opts.extra_variant_count; ++i) {
    if (opts.extra_variant_sets && opts.extra_variant_names &&
        opts.extra_variant_sets[i] && opts.extra_variant_names[i]) {
      override_sets.push_back(opts.extra_variant_sets[i]);
      override_names.push_back(opts.extra_variant_names[i]);
    }
  }
  if (!override_sets.empty()) {
    load.variant_sets = override_sets.data();
    load.variant_names = override_names.data();
    load.variant_override_count = override_sets.size();
  }
  // The retained options must not point at the caller's temporary arrays.
  for (lmv_load_options* o : {&opts, &scene->opts}) {
    o->extra_variant_sets = nullptr;
    o->extra_variant_names = nullptr;
    o->extra_variant_count = 0;
    o->variant_set = nullptr;
    o->variant_name = nullptr;
  }
  load.progress_callback =
      [](void* userdata, const char* phase, size_t current,
         size_t total) -> int {
    const lmv_load_options* o = static_cast<const lmv_load_options*>(userdata);
    if (!o || !o->progress) return 1;
    lmv_progress p{};
    p.phase = (phase && (std::strstr(phase, "compose") ||
                         std::strstr(phase, "Compose")))
                  ? LMV_PHASE_COMPOSE
                  : LMV_PHASE_OPEN;
    p.fraction = total > 0 ? static_cast<float>(current) /
                                 static_cast<float>(total)
                           : 0.0f;
    p.message = phase ? phase : "";
    return o->progress(o->progress_user, &p);
  };
  load.progress_userdata = &opts;

  lightusd_stage* stage = nullptr;
  const lightusd_status status = lightusd_stage_load(path, &load, &stage);
  if (status != LIGHTUSD_OK || !stage) {
    SetError(err, err_cap, "failed to load %s (status %d)", path,
             static_cast<int>(status));
    return MapStatus(status);
  }
  scene->stage = stage;
  scene->force_reconversion = DetectUnsupportedAnimation(stage, opts.time_code);

  lightusd_string* stage_warnings = nullptr;
  if (lightusd_stage_take_warnings(stage, &stage_warnings) == LIGHTUSD_OK &&
      stage_warnings) {
    const lightusd_sv text = lightusd_string_view(stage_warnings);
    if (text.data && text.len) {
      scene->base_warnings.emplace_back(text.data, text.len);
    }
    lightusd_string_destroy(stage_warnings);
  }

  if (opts.progress) {
    lmv_progress p{};
    p.phase = LMV_PHASE_CONVERT;
    p.fraction = 0.0f;
    p.message = "converting";
    if (opts.progress(opts.progress_user, &p) == 0) {
      lmv_scene_free(scene.release());
      return LMV_ERR_CANCELLED;
    }
  }

  lightusd_render_scene* render = nullptr;
  const lightusd_status convert_status =
      CreateRenderScene(stage, opts, opts.time_code, &render);
  if (convert_status != LIGHTUSD_OK || !render) {
    SetError(err, err_cap, "failed to convert %s (status %d)", path,
             static_cast<int>(convert_status));
    lmv_scene_free(scene.release());
    return MapStatus(convert_status);
  }

  lightusd_strlist* warnings = nullptr;
  if (lightusd_render_scene_warnings(render, &warnings) == LIGHTUSD_OK &&
      warnings) {
    const size_t n = lightusd_strlist_size(warnings);
    for (size_t i = 0; i < n; ++i) {
      const lightusd_sv text = lightusd_strlist_get(warnings, i);
      if (text.data && text.len) {
        scene->base_warnings.emplace_back(text.data, text.len);
      }
    }
    lightusd_strlist_destroy(warnings);
  }

  if (opts.progress) {
    lmv_progress p{};
    p.phase = LMV_PHASE_TEXTURES;
    p.fraction = 0.0f;
    p.message = "decoding textures";
    if (opts.progress(opts.progress_user, &p) == 0) {
      lightusd_render_scene_destroy(render);
      lmv_scene_free(scene.release());
      return LMV_ERR_CANCELLED;
    }
  }

  const lmv_status result = Convert(render, opts, opts.time_code, scene.get(),
                                    err, err_cap);
  lightusd_render_scene_destroy(render);
  if (result != LMV_OK) {
    lmv_scene_free(scene.release());
    return result;
  }

  if (opts.progress) {
    lmv_progress p{};
    p.phase = LMV_PHASE_DONE;
    p.fraction = 1.0f;
    p.message = "ready";
    opts.progress(opts.progress_user, &p);
  }

  *out = scene.release();
  return LMV_OK;
}

void lmv_scene_free(lmv_scene* scene) {
  if (!scene) return;
  if (scene->stage) lightusd_stage_destroy(scene->stage);
  delete scene;
}

lmv_status lmv_scene_set_time(lmv_scene* scene, double time_code, char* err,
                              size_t err_cap) {
  if (err && err_cap) err[0] = '\0';
  if (!scene || !scene->stage) return LMV_ERR_INVALID_ARG;

  if (scene->opts.progress) {
    lmv_progress p{};
    p.phase = LMV_PHASE_CONVERT;
    p.fraction = 0.0f;
    p.message = "re-converting at time";
    if (scene->opts.progress(scene->opts.progress_user, &p) == 0) {
      return LMV_ERR_CANCELLED;
    }
  }

  lightusd_render_scene* render = nullptr;
  const lightusd_status status =
      CreateRenderScene(scene->stage, scene->opts, time_code, &render);
  if (status != LIGHTUSD_OK || !render) {
    SetError(err, err_cap, "re-conversion failed (status %d)",
             static_cast<int>(status));
    return MapStatus(status);
  }
  const lmv_status result = Convert(render, scene->opts, time_code, scene, err,
                                    err_cap);
  lightusd_render_scene_destroy(render);
  return result;
}

lmv_status lmv_scene_update_animation(lmv_scene* scene, double time, char* err,
                                      size_t err_cap) {
  if (err && err_cap) err[0] = '\0';
  if (!scene || !scene->stage) return LMV_ERR_INVALID_ARG;
  if (!UpdateAnimationState(scene->stage, &scene->data, time)) {
    SetError(err, err_cap, "no animatable node, rig or blend-shape data");
    return LMV_ERR_UNSUPPORTED;
  }
  scene->time_code = time;
  return LMV_OK;
}

double lmv_scene_time(const lmv_scene* scene) {
  return scene ? scene->time_code : 0.0;
}
double lmv_scene_start_time(const lmv_scene* scene) {
  return scene ? scene->start_time : 0.0;
}
double lmv_scene_end_time(const lmv_scene* scene) {
  return scene ? scene->end_time : 0.0;
}
double lmv_scene_frames_per_second(const lmv_scene* scene) {
  return scene && scene->fps > 0.0 ? scene->fps : 24.0;
}
int lmv_scene_has_animation(const lmv_scene* scene) {
  if (!scene) return 0;
  return scene->animation_count > 0 || scene->end_time > scene->start_time;
}
void lmv_scene_working_to_display_linear(const lmv_scene* scene, float out[9]) {
  if (!out) return;
  for (int i = 0; i < 9; ++i) {
    out[i] = scene ? scene->working_to_display[i]
                   : (i % 4 == 0 ? 1.0f : 0.0f);
  }
}

int lmv_scene_up_axis(const lmv_scene* scene) {
  return scene ? scene->up_axis : 0;
}
float lmv_scene_meters_per_unit(const lmv_scene* scene) {
  return scene ? scene->meters_per_unit : 1.0f;
}

void lmv_scene_bounds(const lmv_scene* scene, float out_min[3],
                      float out_max[3]) {
  if (!out_min || !out_max) return;
  if (!scene) {
    for (int i = 0; i < 3; ++i) {
      out_min[i] = -1.0f;
      out_max[i] = 1.0f;
    }
    return;
  }
  for (int i = 0; i < 3; ++i) {
    out_min[i] = scene->data.bbox_min[i];
    out_max[i] = scene->data.bbox_max[i];
  }
}

uint32_t lmv_scene_mesh_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.meshes.size()) : 0;
}
uint32_t lmv_scene_draw_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.draws.size()) : 0;
}
uint32_t lmv_scene_material_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.materials.size()) : 0;
}
uint32_t lmv_scene_texture_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.textures.size()) : 0;
}
uint32_t lmv_scene_image_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.images.size()) : 0;
}
uint32_t lmv_scene_splat_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.splats.size()) : 0;
}
const lmv_splat* lmv_scene_splats(const lmv_scene* scene) {
  return scene && !scene->data.splats.empty() ? scene->data.splats.data()
                                              : nullptr;
}
uint32_t lmv_scene_line_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.lines.size()) : 0;
}
const lmv_line_segment* lmv_scene_lines(const lmv_scene* scene) {
  return scene && !scene->data.lines.empty() ? scene->data.lines.data()
                                             : nullptr;
}

uint32_t lmv_scene_light_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.lights.size()) : 0;
}
uint32_t lmv_scene_camera_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.cameras.size()) : 0;
}

uint32_t lmv_scene_warning_count(const lmv_scene* scene) {
  return scene ? static_cast<uint32_t>(scene->data.warnings.size()) : 0;
}
const char* lmv_scene_warning(const lmv_scene* scene, uint32_t index) {
  if (!scene || index >= scene->data.warnings.size()) return "";
  return scene->data.warnings[index].c_str();
}

static void CollectVariantSets(const lmv_scene* scene) {
  if (scene->variant_sets_built) return;
  scene->variant_sets_built = true;
  if (scene->path.empty()) return;
  // Composition resolves variant selections and drops the set definitions, so
  // read them from the root layer on its own (uncomposed). Sets that only exist
  // in referenced/payloaded layers are therefore not listed.
  lightusd_load_options lo;
  lightusd_load_options_init(&lo);
  lo.composed = 0;
  lo.load_payloads = 0;
  lightusd_stage* layer = nullptr;
  if (lightusd_stage_load(scene->path.c_str(), &lo, &layer) != LIGHTUSD_OK || !layer)
    return;
  // Start from the root prims (in reverse, so the traversal follows stage
  // order); the pseudo-root handle is not usable here.
  std::vector<lightusd_prim> stack;
  for (size_t r = lightusd_stage_root_prim_count(layer); r > 0; --r)
    stack.push_back(lightusd_stage_root_prim(layer, r - 1));
  while (!stack.empty()) {
    const lightusd_prim prim = stack.back();
    stack.pop_back();
    if (!lightusd_prim_is_valid(prim)) continue;
    const size_t set_count = lightusd_prim_variant_set_count(prim);
    for (size_t s = 0; s < set_count; ++s) {
      lmv_scene::VariantSetInfo info;
      info.set_name = Sv(lightusd_prim_variant_set_name(prim, s));
      if (info.set_name.empty()) continue;
      info.prim_path = Sv(lightusd_prim_path(prim));
      info.selection = Sv(lightusd_variant_selection(prim, info.set_name.c_str()));
      const size_t n = lightusd_variant_count(prim, info.set_name.c_str());
      for (size_t v = 0; v < n; ++v)
        info.variants.push_back(
            Sv(lightusd_variant_name(prim, info.set_name.c_str(), v)));
      for (const std::string& name : info.variants)
        info.variant_ptrs.push_back(name.c_str());
      scene->variant_sets.push_back(std::move(info));
    }
    // Children in reverse so the traversal (and the list) follows stage order.
    for (size_t c = lightusd_prim_child_count(prim); c > 0; --c)
      stack.push_back(lightusd_prim_child(prim, c - 1));
  }
  lightusd_stage_destroy(layer);  // all strings were copied out above
}

uint32_t lmv_scene_variant_set_count(const lmv_scene* scene) {
  if (!scene) return 0;
  CollectVariantSets(scene);
  return static_cast<uint32_t>(scene->variant_sets.size());
}

lmv_status lmv_scene_variant_set(const lmv_scene* scene, uint32_t index,
                                 lmv_variant_set_view* out) {
  if (!scene || !out) return LMV_ERR_INVALID_ARG;
  CollectVariantSets(scene);
  if (index >= scene->variant_sets.size()) return LMV_ERR_INVALID_ARG;
  const lmv_scene::VariantSetInfo& v = scene->variant_sets[index];
  std::memset(out, 0, sizeof(*out));
  out->prim_path = v.prim_path.c_str();
  out->set_name = v.set_name.c_str();
  out->selection = v.selection.c_str();
  out->variant_count = static_cast<uint32_t>(v.variants.size());
  out->variants = v.variant_ptrs.data();
  return LMV_OK;
}

lmv_status lmv_scene_mesh(const lmv_scene* scene, uint32_t index,
                          lmv_mesh_view* out) {
  if (!scene || !out || index >= scene->data.meshes.size()) {
    return LMV_ERR_INVALID_ARG;
  }
  const MeshData& mesh = scene->data.meshes[index];
  if (!mesh.valid) return LMV_ERR_NOT_FOUND;
  std::memset(out, 0, sizeof(*out));
  out->name = mesh.name.c_str();
  out->prim_path = mesh.prim_path.c_str();
  out->vertices = mesh.vertices.data();
  out->face_ids = mesh.face_ids.empty() ? nullptr : mesh.face_ids.data();
  out->vertex_count = static_cast<uint32_t>(mesh.vertices.size());
  out->indices = mesh.indices.data();
  out->index_count = static_cast<uint32_t>(mesh.indices.size());
  out->subsets = mesh.subsets.data();
  out->subset_count = static_cast<uint32_t>(mesh.subsets.size());
  out->material_id = mesh.material_id;
  out->double_sided = mesh.double_sided ? 1u : 0u;
  out->skinned = mesh.skinned ? 1u : 0u;
  out->influences = mesh.skinned ? 4u : 0u;
  out->morph_targets = mesh.morph_targets;
  for (int i = 0; i < 3; ++i) {
    out->bbox_min[i] = mesh.bbox_min[i];
    out->bbox_max[i] = mesh.bbox_max[i];
  }
  return LMV_OK;
}

const uint16_t* lmv_scene_mesh_joint_indices(const lmv_scene* scene,
                                             uint32_t index, uint32_t* count) {
  if (count) *count = 0;
  if (!scene || index >= scene->data.meshes.size()) return nullptr;
  const MeshData& mesh = scene->data.meshes[index];
  if (mesh.joint_indices.empty()) return nullptr;
  if (count) *count = static_cast<uint32_t>(mesh.joint_indices.size());
  return mesh.joint_indices.data();
}

const float* lmv_scene_mesh_joint_weights(const lmv_scene* scene,
                                          uint32_t index, uint32_t* count) {
  if (count) *count = 0;
  if (!scene || index >= scene->data.meshes.size()) return nullptr;
  const MeshData& mesh = scene->data.meshes[index];
  if (mesh.joint_weights.empty()) return nullptr;
  if (count) *count = static_cast<uint32_t>(mesh.joint_weights.size());
  return mesh.joint_weights.data();
}

const float* lmv_scene_mesh_skin_matrices(const lmv_scene* scene, uint32_t index,
                                          uint32_t* matrix_count) {
  if (matrix_count) *matrix_count = 0;
  if (!scene || index >= scene->data.meshes.size()) return nullptr;
  const MeshData& mesh = scene->data.meshes[index];
  if (mesh.skin_matrices.empty() ||
      mesh.skin_matrices.size() % 16 != 0) {
    return nullptr;
  }
  if (matrix_count) {
    *matrix_count =
        static_cast<uint32_t>(mesh.skin_matrices.size() / 16);
  }
  return mesh.skin_matrices.data();
}

const float* lmv_scene_mesh_morph_deltas(const lmv_scene* scene, uint32_t index,
                                         uint32_t target, uint32_t* float_count) {
  if (float_count) *float_count = 0;
  if (!scene || index >= scene->data.meshes.size()) return nullptr;
  const MeshData& mesh = scene->data.meshes[index];
  if (target >= mesh.morph_deltas.size()) return nullptr;
  const std::vector<float>& deltas = mesh.morph_deltas[target];
  if (deltas.empty()) return nullptr;
  if (float_count) *float_count = static_cast<uint32_t>(deltas.size());
  return deltas.data();
}

const float* lmv_scene_mesh_morph_weights(const lmv_scene* scene, uint32_t index,
                                          uint32_t* count) {
  if (count) *count = 0;
  if (!scene || index >= scene->data.meshes.size()) return nullptr;
  const MeshData& mesh = scene->data.meshes[index];
  if (mesh.morph_weights.empty()) return nullptr;
  if (count) *count = static_cast<uint32_t>(mesh.morph_weights.size());
  return mesh.morph_weights.data();
}

lmv_status lmv_scene_draw(const lmv_scene* scene, uint32_t index,
                          lmv_draw_view* out) {
  if (!scene || !out || index >= scene->data.draws.size()) {
    return LMV_ERR_INVALID_ARG;
  }
  const DrawData& draw = scene->data.draws[index];
  out->mesh_id = draw.mesh_id;
  out->material_override = draw.material_override;
  out->visible = draw.visible ? 1u : 0u;
  out->world = draw.world;
  return LMV_OK;
}

lmv_status lmv_scene_material(const lmv_scene* scene, uint32_t index,
                              lmv_material_view* out) {
  if (!scene || !out || index >= scene->data.materials.size()) {
    return LMV_ERR_INVALID_ARG;
  }
  const MaterialData& material = scene->data.materials[index];
  std::memset(out, 0, sizeof(*out));
  out->name = material.name.c_str();
  out->prim_path = material.prim_path.c_str();
  out->shader_type = material.shader_type;
  out->alpha_mode = material.alpha_mode;
  out->double_sided = material.double_sided;
  out->default_fallback = material.default_fallback;
  out->alpha_cutoff = material.alpha_cutoff;
  out->pbr = material.pbr;
  out->tex_base_color = material.tex[LMV_TEX_BASE_COLOR];
  out->tex_metallic = material.tex[LMV_TEX_METALLIC];
  out->tex_roughness = material.tex[LMV_TEX_ROUGHNESS];
  out->tex_normal = material.tex[LMV_TEX_NORMAL];
  out->tex_emission = material.tex[LMV_TEX_EMISSION];
  out->tex_opacity = material.tex[LMV_TEX_OPACITY];
  out->tex_coat_normal = material.tex[LMV_TEX_COAT_NORMAL];
  out->normal_scale = material.normal_scale;
  out->coat_normal_scale = material.coat_normal_scale;
  return LMV_OK;
}

lmv_status lmv_scene_texture(const lmv_scene* scene, uint32_t index,
                             lmv_texture_view* out) {
  if (!scene || !out || index >= scene->data.textures.size()) {
    return LMV_ERR_INVALID_ARG;
  }
  const TextureData& texture = scene->data.textures[index];
  std::memset(out, 0, sizeof(*out));
  out->asset_path = texture.asset_path.c_str();
  out->ktx2_hint = texture.ktx2_hint.c_str();
  out->image_id = texture.image_id;
  out->wrap_s = texture.wrap_s;
  out->wrap_t = texture.wrap_t;
  out->channel = texture.channel;
  out->uv_set = texture.uv_set;
  out->uv_offset[0] = texture.uv_offset[0];
  out->uv_offset[1] = texture.uv_offset[1];
  out->uv_scale[0] = texture.uv_scale[0];
  out->uv_scale[1] = texture.uv_scale[1];
  out->uv_rotation = texture.uv_rotation;
  for (int i = 0; i < 4; ++i) {
    out->bias[i] = texture.bias[i];
    out->scale[i] = texture.scale[i];
  }
  out->udim_tile_count = static_cast<uint32_t>(texture.udim_tiles.size());
  static_assert(sizeof(UdimTileData) == sizeof(lmv_udim_tile),
                "UDIM tile ABI mismatch");
  out->udim_tiles =
      texture.udim_tiles.empty()
          ? nullptr
          : reinterpret_cast<const lmv_udim_tile*>(texture.udim_tiles.data());
  out->is_ptex = texture.is_ptex ? 1u : 0u;
  out->ptex_face_count = texture.ptex_face_count;
  out->ptex_rects =
      texture.ptex_rects.empty()
          ? nullptr
          : reinterpret_cast<const lmv_ptex_rect*>(texture.ptex_rects.data());
  return LMV_OK;
}

lmv_status lmv_scene_image(const lmv_scene* scene, uint32_t index,
                           lmv_image_view* out) {
  if (!scene || !out || index >= scene->data.images.size()) {
    return LMV_ERR_INVALID_ARG;
  }
  const std::shared_ptr<const ImageData>& image = scene->data.images[index];
  std::memset(out, 0, sizeof(*out));
  if (!image || !image->valid()) return LMV_ERR_NOT_FOUND;
  out->resolved_path = image->resolved_path.c_str();
  out->width = image->width;
  out->height = image->height;
  out->channels = image->channels;
  out->is_float = image->is_float ? 1u : 0u;
  out->srgb = image->srgb ? 1u : 0u;
  if (image->compressed_format != 0 && image->levels) {
    out->compressed_format = image->compressed_format;
    out->mip_levels = static_cast<uint32_t>(image->levels->size());
    out->levels = image->level_views ? image->level_views->data() : nullptr;
    return LMV_OK;
  }
  if (image->is_float) {
    out->pixels = image->rgb->data();
    out->nbytes = image->rgb->size() * sizeof(float);
  } else {
    out->pixels = image->rgba->data();
    out->nbytes = image->rgba->size();
  }
  return LMV_OK;
}

lmv_status lmv_scene_light(const lmv_scene* scene, uint32_t index,
                           lmv_light_view* out) {
  if (!scene || !out || index >= scene->data.lights.size()) {
    return LMV_ERR_INVALID_ARG;
  }
  const LightData& light = scene->data.lights[index];
  std::memset(out, 0, sizeof(*out));
  out->name = light.name.c_str();
  out->prim_path = light.prim_path.c_str();
  out->type = light.type;
  for (int i = 0; i < 3; ++i) out->color[i] = light.color[i];
  out->intensity = light.intensity;
  out->exposure = light.exposure;
  out->transform = light.transform;
  out->param0 = light.param0;
  out->param1 = light.param1;
  out->dome_texture_id = light.dome_texture_id;
  out->dome_image_id = light.dome_image_id;
  return LMV_OK;
}

lmv_status lmv_scene_camera(const lmv_scene* scene, uint32_t index,
                            lmv_camera_view* out) {
  if (!scene || !out || index >= scene->data.cameras.size()) {
    return LMV_ERR_INVALID_ARG;
  }
  const CameraData& camera = scene->data.cameras[index];
  std::memset(out, 0, sizeof(*out));
  out->name = camera.name.c_str();
  out->prim_path = camera.prim_path.c_str();
  out->orthographic = camera.orthographic;
  out->focal_length = camera.focal_length;
  out->horizontal_aperture = camera.horizontal_aperture;
  out->vertical_aperture = camera.vertical_aperture;
  out->ortho_width = camera.ortho_width;
  out->near_clip = camera.near_clip;
  out->far_clip = camera.far_clip;
  out->focus_distance = camera.focus_distance;
  out->exposure = camera.exposure;
  out->fstop = camera.fstop;
  out->transform = camera.transform;
  return LMV_OK;
}

}  // extern "C"
