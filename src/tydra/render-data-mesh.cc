// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// TODO:
//   - [ ] Subdivision surface to polygon mesh conversion.
//     - [ ] Correctly handle primvar with 'vertex' interpolation(Use the basis
//     function of subd surface)
//   - [ ] Support material binding collection(Collection API)
//   - [ ] Support multiple skel animation
//   https://github.com/PixarAnimationStudios/OpenUSD/issues/2246
//   - [ ] Adjust normal vector computation with handness?
//   - [ ] Node xform animation
//   - [ ] Better build of index buffer
//     - [ ] Preserve the order of 'points' variable(mesh.points, Skin
//     indices/weights, BlendShape points, ...) as much as possible.
//     - Implement spatial hash
//
//
// Mesh conversion routines split from render-data.cc
//
#include <algorithm>
#include <limits>
#include <numeric>
#include <set>

#include "common-utils.hh"
#include "common-types.hh"
#include "../tiny-hashmap.hh"
#include "image-loader.hh"
#include "image-util.hh"
#include "image-types.hh"
#include "linear-algebra.hh"
#include "math-util.inc"
#include "pprint-enum.hh"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usdShade.hh"
#include "usdLux.hh"
#include "usdMtlx.hh"
#include "value-pprint.hh"
#include "logger.hh"
#include "bone-util.hh"
#include "shape-to-mesh.hh"
#include "materialx-to-json.hh"
#include "mmap-array-ref.hh"
#include "safe-arithmetic.hh"
#include "tsd/tsd-lightusd.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

// For tangent/binormal computation
// NOTE: HalfEdge is not used atm.
#include "external/half-edge.hh"

#if defined(LIGHTUSD_WITH_MESHOPT)
#include "external/meshoptimizer/meshoptimizer.h"
#endif

// MikkTSpace tangent computation
#include "mikktspace-tangent.hh"
// Optimized MikkTSpace reimplementation
#include "fast-mikktspace.hh"

// For triangulation.
// TODO: Use tinyobjloader's triangulation
#include "external/mapbox/earcut/earcut.hpp"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

//
#include "common-macros.inc"
#include "math-util.inc"


//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/render-data-internal.hh"
#include "tydra/tangent-quantize.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"

#include "tydra/render-data-mesh-internal.hh"

#include <cstdint>

namespace lightusd {

namespace tydra {

namespace {

#define PushError(msg) TYDRA_PUSH_ERROR(err, msg)

bool SizeToU32(size_t n, uint32_t *out) {
#if SIZE_MAX > UINT32_MAX
  if (n > size_t((std::numeric_limits<uint32_t>::max)())) {
    return false;
  }
#endif
  *out = uint32_t(n);
  return true;
}



//
// Convert vertex attribute with Uniform variability(interpolation) to
// facevarying variability, by replicating uniform value per face over face
// vertices.
//

//
// Convert vertex attribute with Uniform variability(interpolation) to vertex
// variability, by replicating uniform value for vertices of a face. For shared
// vertex, the value will be overwritten.
//

nonstd::expected<std::vector<uint8_t>, std::string> UniformToVertex(
    const std::vector<uint8_t> &inputs, const size_t stride_bytes,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices) {
  // NOTE: Uniform -> Vertex convertsion may give wrong result.
  std::vector<uint8_t> dst;

  if (stride_bytes == 0) {
    return nonstd::make_unexpected(fmt::format("stride_bytes is zero."));
  }

  if (faceVertexIndices.size() < 3) {
    return nonstd::make_unexpected(
        fmt::format("faceVertexIndices.size must be 3 or greater, but got {}.",
                    faceVertexCounts.size()));
  }

  if ((inputs.size() % stride_bytes) != 0) {
    return nonstd::make_unexpected(
        fmt::format("input bytes {} must be dividable by stride_bytes {}.",
                    inputs.size(), stride_bytes));
  }

  size_t num_uniforms = inputs.size() / stride_bytes;

  if (num_uniforms != faceVertexCounts.size()) {
    return nonstd::make_unexpected(fmt::format(
        "The number of input uniform attributes {} must be the same with "
        "faceVertexCounts.size() {}",
        num_uniforms, faceVertexCounts.size()));
  }

  // max_element on an empty range is UB, and `max + 1` on uint32 wraps to 0
  // for a 0xFFFFFFFF index (a source -1), making dst.resize(0) then OOB writes
  // below. The sole live caller validates indices first, but guard here too.
  if (faceVertexIndices.empty()) {
    return nonstd::make_unexpected("faceVertexIndices is empty.");
  }
  const uint32_t max_index =
      *std::max_element(faceVertexIndices.cbegin(), faceVertexIndices.cend());
  if (max_index == (std::numeric_limits<uint32_t>::max)()) {
    return nonstd::make_unexpected("faceVertexIndices contains an invalid (max-uint32) index.");
  }
  const size_t num_vertices = size_t(max_index) + 1;

  size_t resize_size;
  if (!safe::mul(num_vertices, stride_bytes, &resize_size)) {
    return nonstd::make_unexpected("Integer overflow: num_vertices * stride_bytes");
  }
  dst.resize(resize_size);

  size_t fvIndexOffset{0};

  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    size_t cnt = faceVertexCounts[i];

    if ((fvIndexOffset + cnt) > faceVertexIndices.size()) {
      return nonstd::make_unexpected(
          fmt::format("faceVertexCounts[{}] {} gives buffer-overrun to "
                      "faceVertexIndices.size {}.",
                      i, cnt, faceVertexIndices.size()));
    }

    for (size_t k = 0; k < cnt; k++) {
      uint32_t v_idx = faceVertexIndices[fvIndexOffset + k];

      // may overwrite the value when a vertex is referenced from multiple facet.
      memcpy(dst.data() + v_idx * stride_bytes,
             inputs.data() + i * stride_bytes, stride_bytes);
    }

    fvIndexOffset += cnt;
  }

  return dst;
}

// Generic uniform to facevarying conversion
nonstd::expected<std::vector<uint8_t>, std::string> UniformToFaceVarying(
    const std::vector<uint8_t> &src, const size_t stride_bytes,
    const std::vector<uint32_t> &faceVertexCounts) {
  std::vector<uint8_t> dst;

  if (stride_bytes == 0) {
    return nonstd::make_unexpected("stride_bytes is zero.");
  }

  if ((src.size() % stride_bytes) != 0) {
    return nonstd::make_unexpected(
        fmt::format("input bytes {} must be the multiple of stride_bytes {}",
                    src.size(), stride_bytes));
  }

  size_t num_uniforms = src.size() / stride_bytes;

  if (num_uniforms != faceVertexCounts.size()) {
    return nonstd::make_unexpected(fmt::format(
        "The number of input uniform attributes {} must be the same with "
        "faceVertexCounts.size() {}",
        num_uniforms, faceVertexCounts.size()));
  }

  // Pre-size the output to its exact byte count and write via offset memcpy.
  // (The previous code grew `dst` with a per-face-vertex insert(), causing
  // repeated reallocation.) Total face-vertices = sum(faceVertexCounts).
  size_t total_fv = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    if (!safe::add(total_fv, size_t(faceVertexCounts[i]), &total_fv)) {
      return nonstd::make_unexpected(
          "Overflow computing total face-vertex count.");
    }
  }
  size_t total_bytes;
  if (!safe::mul(total_fv, stride_bytes, &total_bytes)) {
    return nonstd::make_unexpected("Overflow computing output buffer size.");
  }
  dst.resize(total_bytes);

  size_t off = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    const size_t cnt = faceVertexCounts[i];
    const uint8_t *srcp = src.data() + i * stride_bytes;
    for (size_t k = 0; k < cnt; k++) {
      memcpy(dst.data() + off, srcp, stride_bytes);
      off += stride_bytes;
    }
  }

  return dst;
}

//
// Convert vertex attribute with Vertex variability(interpolation) to
// facevarying attribute, by expanding(flatten) the value per vertex per face.
//

// Generic vertex to facevarying conversion
nonstd::expected<std::vector<uint8_t>, std::string> VertexToFaceVarying(
    const std::vector<uint8_t> &src, const size_t stride_bytes,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices) {

  if (src.empty()) {
    return nonstd::make_unexpected("src data is empty.");
  }

  if (stride_bytes == 0) {
    return nonstd::make_unexpected("stride_bytes must be non-zero.");
  }

  if ((src.size() % stride_bytes) != 0) {
    return nonstd::make_unexpected(
        fmt::format("src size {} must be the multiple of stride_bytes {}",
                    src.size(), stride_bytes));
  }

  const size_t num_vertices = src.size() / stride_bytes;

  // Pre-allocate output buffer to exact size needed
  const size_t total_face_vertices = faceVertexIndices.size();
  std::vector<uint8_t> dst;
  size_t dst_size;
  if (!safe::mul(total_face_vertices, stride_bytes, &dst_size)) {
    return nonstd::make_unexpected("Integer overflow: total_face_vertices * stride_bytes");
  }
  dst.resize(dst_size);

  const uint8_t* src_data = src.data();
  uint8_t* dst_ptr = dst.data();

  size_t faceVertexIndexOffset{0};

  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    size_t cnt = faceVertexCounts[i];

    for (size_t k = 0; k < cnt; k++) {
      size_t fv_idx = k + faceVertexIndexOffset;

      if (fv_idx >= faceVertexIndices.size()) {
        return nonstd::make_unexpected(
            fmt::format("faeVertexIndex {} out-of-range at faceVertexCount[{}]",
                        fv_idx, i));
      }

      size_t v_idx = faceVertexIndices[fv_idx];

      if (v_idx >= num_vertices) {
        return nonstd::make_unexpected(fmt::format(
            "faeVertexIndices[{}] {} exceeds the number of vertices {}", fv_idx,
            v_idx, num_vertices));
      }

      // Direct memcpy to pre-allocated destination
      std::memcpy(dst_ptr, src_data + v_idx * stride_bytes, stride_bytes);
      dst_ptr += stride_bytes;
    }

    faceVertexIndexOffset += cnt;
  }

  return dst;
}


static nonstd::expected<std::vector<uint8_t>, std::string> ConstantToVertex(
    const std::vector<uint8_t> &src, const size_t stride_bytes,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices) {
  if (faceVertexCounts.empty()) {
    return nonstd::make_unexpected("faceVertexCounts is empty.");
  }

  if (faceVertexIndices.size() < 3) {
    return nonstd::make_unexpected(
        fmt::format("faceVertexIndices.size must be at least 3, but got {}.",
                    faceVertexIndices.size()));
  }

  if (faceVertexIndices.empty()) {
    return nonstd::make_unexpected("faceVertexIndices is empty.");
  }
  const uint32_t max_index =
      *std::max_element(faceVertexIndices.cbegin(), faceVertexIndices.cend());
  if (max_index == (std::numeric_limits<uint32_t>::max)()) {
    return nonstd::make_unexpected("faceVertexIndices contains an invalid (max-uint32) index.");
  }
  const size_t num_vertices = size_t(max_index) + 1;

  std::vector<uint8_t> dst;

  if (src.empty()) {
    return nonstd::make_unexpected("src data is empty.");
  }

  if (stride_bytes == 0) {
    return nonstd::make_unexpected("stride_bytes must be non-zero.");
  }

  if (src.size() != stride_bytes) {
    return nonstd::make_unexpected(
        fmt::format("src size {} must be equal to stride_bytes {}", src.size(),
                    stride_bytes));
  }

  size_t dst_size;
  if (!safe::mul(stride_bytes, num_vertices, &dst_size)) {
    return nonstd::make_unexpected("Integer overflow: stride_bytes * num_vertices");
  }
  dst.resize(dst_size);

  size_t faceVertexIndexOffset = 0;
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    uint32_t cnt = faceVertexCounts[i];
    if (cnt < 3) {
      return nonstd::make_unexpected(fmt::format(
          "faeVertexCounts[{}] must be equal to or greater than 3, but got {}",
          i, cnt));
    }

    for (size_t k = 0; k < cnt; k++) {
      size_t fv_idx = k + faceVertexIndexOffset;

      if (fv_idx >= faceVertexIndices.size()) {
        return nonstd::make_unexpected(
            fmt::format("faeVertexIndex {} out-of-range at faceVertexCount[{}]",
                        fv_idx, i));
      }

      size_t v_idx = faceVertexIndices[fv_idx];

      if (v_idx >= num_vertices) {  // this should not happen. just in case.
        return nonstd::make_unexpected(fmt::format(
            "faeVertexIndices[{}] {} exceeds the number of vertices {}", fv_idx,
            v_idx, num_vertices));
      }

      memcpy(dst.data() + v_idx * stride_bytes, src.data(), stride_bytes);
    }

    faceVertexIndexOffset += cnt;
  }

  return dst;
}


// T = int
template <typename T>
bool TryConvertFacevaryingToVertexInt(
    const std::vector<T> &src, std::vector<T> *dst,
    const std::vector<uint32_t> &faceVertexIndices) {
  if (!dst) {
    return false;
  }

  if (src.size() != faceVertexIndices.size()) {
    return false;
  }

  // size must be at least 1 triangle(3 verts).
  if (faceVertexIndices.size() < 3) {
    return false;
  }

  // vidx, value
  lightusd::HashMap<uint32_t, T> vdata;
  vdata.reserve(faceVertexIndices.size());

  uint32_t max_vidx = 0;
  for (size_t i = 0; i < faceVertexIndices.size(); i++) {
    uint32_t vidx = faceVertexIndices[i];
    max_vidx = (std::max)(vidx, max_vidx);

    auto it = vdata.find(vidx);
    if (it != vdata.end()) {
      if (!math::is_close(it->second, src[i])) {
        return false;
      }
    } else {
      vdata.emplace(vidx, src[i]);
    }
  }

  dst->resize(max_vidx + 1);
  {
    size_t byte_count;
    if (!safe::mul(size_t(max_vidx + 1), sizeof(T), &byte_count)) {
      return false;  // integer overflow
    }
    memset(dst->data(), 0, byte_count);
  }

  for (const auto &v : vdata) {
    (*dst)[v.first] = v.second;
  }

  return true;
}

// T = float, double, float2, ...
template <typename T, typename EpsTy>
bool TryConvertFacevaryingToVertexFloat(
    const std::vector<T> &src, std::vector<T> *dst,
    const std::vector<uint32_t> &faceVertexIndices, const EpsTy eps) {
  DCOUT("TryConvertFacevaryingToVertexFloat");
  if (!dst) {
    return false;
  }

  if (src.size() != faceVertexIndices.size()) {
    DCOUT("size mismatch.");
    return false;
  }

  // size must be at least 1 triangle(3 verts).
  if (faceVertexIndices.size() < 3) {
    return false;
  }

  // Direct-indexed seen + value arrays. Vertex indices are dense small ints,
  // so a HashMap<uint32_t, T> here was ~⅔ of conversion time on USDC inputs.
  // Grow dst lazily as vidx values come in to avoid a separate max-vidx pass.
  dst->clear();
  std::vector<uint8_t> seen;

  for (size_t i = 0; i < faceVertexIndices.size(); i++) {
    uint32_t vidx = faceVertexIndices[i];
    if (vidx >= dst->size()) {
      dst->resize(size_t(vidx) + 1, T{});
      seen.resize(size_t(vidx) + 1, uint8_t(0));
    }
    if (seen[vidx]) {
      if (!math::is_close((*dst)[vidx], src[i], eps)) {
        DCOUT("diff at faceVertexIndices[" << i << "]");
        return false;
      }
    } else {
      (*dst)[vidx] = src[i];
      seen[vidx] = 1;
    }
  }

  return true;
}

// T = matrix type.
template <typename T>
bool TryConvertFacevaryingToVertexMat(
    const std::vector<T> &src, std::vector<T> *dst,
    const std::vector<uint32_t> &faceVertexIndices) {
  if (!dst) {
    return false;
  }

  if (src.size() != faceVertexIndices.size()) {
    return false;
  }

  // size must be at least 1 triangle(3 verts).
  if (faceVertexIndices.size() < 3) {
    return false;
  }

  // Direct-indexed; see TryConvertFacevaryingToVertexFloat for rationale.
  dst->clear();
  std::vector<uint8_t> seen;

  for (size_t i = 0; i < faceVertexIndices.size(); i++) {
    uint32_t vidx = faceVertexIndices[i];
    if (vidx >= dst->size()) {
      dst->resize(size_t(vidx) + 1, T::identity());
      seen.resize(size_t(vidx) + 1, uint8_t(0));
    }
    if (seen[vidx]) {
      if (!is_close((*dst)[vidx], src[i])) {
        return false;
      }
    } else {
      (*dst)[vidx] = src[i];
      seen[vidx] = 1;
    }
  }

  return true;
}

///
/// Try to convert 'facevarying' vertex attribute to 'vertex' attribute.
/// Inspect each vertex value is the same(with given eps)
///
/// Current limitation:
/// - stride must be 0 or tightly packed.
/// - elementSize must be 1
///
/// @return true when 'facevarying' vertex attribute successfully converted to
/// 'vertex'
///
static bool TryConvertFacevaryingToVertex(
    const VertexAttribute &src, VertexAttribute *dst,
    const std::vector<uint32_t> &faceVertexIndices, std::string *err,
    const float eps) {
  DCOUT("TryConvertFacevaryingToVertex");
  if (!dst) {
    PUSH_ERROR_AND_RETURN("Output `dst` is nullptr.");
  }

  if (!src.is_facevarying()) {
    PUSH_ERROR_AND_RETURN("Input must be 'facevarying' attribute");
  }

  if (src.element_size() != 1) {
    PUSH_ERROR_AND_RETURN("Input's element_size must be 1.");
  }

  if ((src.stride != 0) && (src.stride_bytes() != src.format_size())) {
    PUSH_ERROR_AND_RETURN(
        "Input attribute must be tightly packed. stride_bytes = "
        << src.stride_bytes() << ", format_size = " << src.format_size());
  }

#define CONVERT_FUN_INT(__fmt, __ty)                                      \
  if (src.format == __fmt) {                                              \
    std::vector<__ty> vsrc;                                               \
    vsrc.resize(src.vertex_count());                                      \
    memcpy(vsrc.data(), src.get_data().data(), src.get_data().size());    \
    std::vector<__ty> vdst;                                               \
    bool ret = TryConvertFacevaryingToVertexInt<__ty>(vsrc, &vdst,        \
                                                       faceVertexIndices); \
    if (!ret) {                                                           \
      return false;                                                       \
    }                                                                     \
    dst->name = src.name;                                                 \
    dst->elementSize = 1;                                                 \
    dst->format = src.format;                                             \
    dst->variability = VertexVariability::Vertex;                         \
    size_t resize_size;                                                    \
    if (!safe::mul(vdst.size(), src.format_size(), &resize_size)) {       \
      return false;                                                       \
    }                                                                     \
    dst->data.resize(resize_size);                                         \
    size_t memcpy_size;                                                    \
    if (!safe::mul(vdst.size(), src.format_size(), &memcpy_size)) {       \
      return false;                                                       \
    }                                                                     \
    memcpy(dst->data.data(), vdst.data(), memcpy_size);                    \
    return true;                                                          \
  } else

#define CONVERT_FUN_FLOAT(__fmt, __ty, __epsty)                        \
  if (src.format == __fmt) {                                           \
    std::vector<__ty> vsrc;                                            \
    vsrc.resize(src.vertex_count());                                   \
    memcpy(vsrc.data(), src.get_data().data(), src.get_data().size()); \
    std::vector<__ty> vdst;                                            \
    bool ret = TryConvertFacevaryingToVertexFloat<__ty, __epsty>(      \
        vsrc, &vdst, faceVertexIndices, __epsty(eps));                 \
    if (!ret) {                                                        \
      return false;                                                    \
    }                                                                  \
    dst->name = src.name;                                                 \
    dst->elementSize = 1;                                              \
    dst->format = src.format;                                          \
    dst->variability = VertexVariability::Vertex;                      \
    size_t resize_size;                                                    \
    if (!safe::mul(vdst.size(), src.format_size(), &resize_size)) {       \
      return false;                                                       \
    }                                                                     \
    dst->data.resize(resize_size);                                         \
    size_t memcpy_size;                                                    \
    if (!safe::mul(vdst.size(), src.format_size(), &memcpy_size)) {       \
      return false;                                                       \
    }                                                                     \
    memcpy(dst->data.data(), vdst.data(), memcpy_size);                    \
    return true;                                                       \
  } else

#define CONVERT_FUN_MAT(__fmt, __ty)                                      \
  if (src.format == __fmt) {                                              \
    std::vector<__ty> vsrc;                                               \
    vsrc.resize(src.vertex_count());                                      \
    memcpy(vsrc.data(), src.get_data().data(), src.get_data().size());    \
    std::vector<__ty> vdst;                                               \
    bool ret = TryConvertFacevaryingToVertexMat<__ty>(vsrc, &vdst,        \
                                                       faceVertexIndices); \
    if (!ret) {                                                           \
      return false;                                                       \
    }                                                                     \
    dst->name = src.name;                                                 \
    dst->elementSize = 1;                                                 \
    dst->format = src.format;                                             \
    dst->variability = VertexVariability::Vertex;                         \
    size_t resize_size;                                                    \
    if (!safe::mul(vdst.size(), src.format_size(), &resize_size)) {       \
      return false;                                                       \
    }                                                                     \
    dst->data.resize(resize_size);                                         \
    size_t memcpy_size;                                                    \
    if (!safe::mul(vdst.size(), src.format_size(), &memcpy_size)) {       \
      return false;                                                       \
    }                                                                     \
    memcpy(dst->data.data(), vdst.data(), memcpy_size);                    \
    return true;                                                          \
  } else

  // NOTE: VertexAttributeFormat::Bool is preserved
  CONVERT_FUN_INT(VertexAttributeFormat::Bool, uint8_t)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Float, float, float)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Vec2, value::float2, float)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Vec3, value::float3, float)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Vec4, value::float4, float)
  CONVERT_FUN_INT(VertexAttributeFormat::Char, signed char)
  // CONVERT_FUN(VertexAttributeFormat::Char2, value::char2)
  // CONVERT_FUN(VertexAttributeFormat::Char3, value::char3)
  // CONVERT_FUN(VertexAttributeFormat::Char4,    // int8x4
  CONVERT_FUN_INT(VertexAttributeFormat::Byte, uint8_t)
  // CONVERT_FUN(VertexAttributeFormat::Byte2,    // uint8x2
  // CONVERT_FUN(VertexAttributeFormat::Byte3,    // uint8x3
  // CONVERT_FUN(VertexAttributeFormat::Byte4,    // uint8x4
  CONVERT_FUN_INT(VertexAttributeFormat::Short, int16_t)
  // CONVERT_FUN(VertexAttributeFormat::Short2, value::short2)
  // CONVERT_FUN(VertexAttributeFormat::Short3, value::short3)
  // CONVERT_FUN(VertexAttributeFormat::Short4, value::short4)
  CONVERT_FUN_INT(VertexAttributeFormat::Ushort, uint16_t)
  // CONVERT_FUN(VertexAttributeFormat::Ushort2, uint16_t)
  // CONVERT_FUN(VertexAttributeFormat::Ushort3, uint16_t)
  // CONVERT_FUN(VertexAttributeFormat::Ushort4, uint16_t)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Half, value::half, float)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Half2, value::half2, float)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Half3, value::half3, float)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Half4, value::half4, float)
  CONVERT_FUN_INT(VertexAttributeFormat::Int, int)
  CONVERT_FUN_INT(VertexAttributeFormat::Ivec2, value::int2)
  CONVERT_FUN_INT(VertexAttributeFormat::Ivec3, value::int3)
  CONVERT_FUN_INT(VertexAttributeFormat::Ivec4, value::int4)
  CONVERT_FUN_INT(VertexAttributeFormat::Uint, uint32_t)
  CONVERT_FUN_INT(VertexAttributeFormat::Uvec2, value::uint2)
  CONVERT_FUN_INT(VertexAttributeFormat::Uvec3, value::uint3)
  CONVERT_FUN_INT(VertexAttributeFormat::Uvec4, value::uint4)
  // NOTE: Use float precision eps is upcasted to double precision.
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Double, double, double)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Dvec2, value::double2, double)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Dvec3, value::double3, double)
  CONVERT_FUN_FLOAT(VertexAttributeFormat::Dvec4, value::double4, double)
  CONVERT_FUN_MAT(VertexAttributeFormat::Mat2, value::matrix2f)
  CONVERT_FUN_MAT(VertexAttributeFormat::Mat3, value::matrix3f)
  CONVERT_FUN_MAT(VertexAttributeFormat::Mat4, value::matrix4f)
  CONVERT_FUN_MAT(VertexAttributeFormat::Dmat2, value::matrix2d)
  CONVERT_FUN_MAT(VertexAttributeFormat::Dmat3, value::matrix3d)
  CONVERT_FUN_MAT(VertexAttributeFormat::Dmat4, value::matrix4d) {
    if (err) {
      (*err) +=
          fmt::format("Unsupported/Unimplemented VertexAttributeFormat: {}",
                      to_string(src.format));
    }
  }

#undef CONVERT_FUN_INT
#undef CONVERT_FUN_FLOAT
#undef CONVERT_FUN_MAT

  return false;
}




///
/// Triangulate VeretexAttribute data.
///
static bool TriangulateVertexAttribute(
    VertexAttribute &vattr, const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &triangulatedToOrigFaceVertexIndexMap,
    const std::vector<uint32_t> &triangulatedFaceCounts,
    const std::vector<uint32_t> &triangulatedFaceVertexIndices,
    std::string *err) {
  if (vattr.vertex_count() == 0) {
    return true;
  }

  if (triangulatedFaceCounts.empty()) {
    PUSH_ERROR_AND_RETURN("triangulatedFaceCounts is empty.");
  }

  if (faceVertexCounts.size() != triangulatedFaceCounts.size()) {
    PUSH_ERROR_AND_RETURN(
        "faceVertexCounts.size must be equal to triangulatedFaceCounts.size.");
  }

  if ((triangulatedFaceVertexIndices.size() % 3) != 0) {
    PUSH_ERROR_AND_RETURN("Invalid size for triangulatedFaceVertexIndices.");
  }

  if (vattr.is_facevarying()) {
    if (triangulatedToOrigFaceVertexIndexMap.size() !=
        triangulatedFaceVertexIndices.size()) {
      PUSH_ERROR_AND_RETURN(
          "triangulatedToOrigFaceVertexIndexMap.size must be equal to "
          "triangulatedFaceVertexIndices.");
    }

    size_t num_vs = vattr.vertex_count();
    const size_t stride = vattr.stride_bytes();
    size_t total_size;
    if (!safe::mul(triangulatedFaceVertexIndices.size(), stride, &total_size)) {
      PUSH_ERROR_AND_RETURN(
          "Integer overflow: triangulatedFaceVertexIndices.size * stride.");
    }

    std::vector<uint8_t> buf;
    buf.resize(total_size);  // Pre-allocate exact size

    const uint8_t* src_data = vattr.get_data().data();
    const size_t src_size = vattr.get_data().size();
    uint8_t* dst_ptr = buf.data();

    for (uint32_t f = 0; f < triangulatedFaceVertexIndices.size(); f++) {
      // Array index to faceVertexIndices(before triangulation).
      uint32_t src_fvIdx = triangulatedToOrigFaceVertexIndexMap[f];

      if (src_fvIdx >= num_vs) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("triangulatedToOrigFaceVertexIndexMap[{}] {} exceeds num_vs {}.", f, src_fvIdx, num_vs));
      }

      // Guard the source read against a malformed/short backing buffer.
      if (((size_t(src_fvIdx) * stride) + stride) > src_size) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Source read out of range in facevarying attribute remap at {}.",
            f));
      }

      // Use memcpy instead of insert for better performance
      std::memcpy(dst_ptr, src_data + src_fvIdx * stride, stride);
      dst_ptr += stride;
    }

    vattr.data = std::move(buf);
  } else if (vattr.is_vertex()) {
    // # of vertices does not change, so nothing is required.
    return true;
  } else if (vattr.is_indexed()) {
    PUSH_ERROR_AND_RETURN("Indexed VertexAttribute is not supported.");
  } else if (vattr.is_constant()) {
    const size_t stride = vattr.stride_bytes();

    // Pre-calculate total size to avoid reallocations
    size_t total_triangles = 0;
    for (size_t f = 0; f < triangulatedFaceCounts.size(); f++) {
      total_triangles += triangulatedFaceCounts[f];
    }
    // Each triangle has 3 vertices
    size_t total_verts;
    size_t total_size;
    if (!safe::mul(total_triangles, size_t(3), &total_verts) ||
        !safe::mul(total_verts, stride, &total_size)) {
      PUSH_ERROR_AND_RETURN("Integer overflow: total_triangles * 3 * stride.");
    }

    std::vector<uint8_t> buf;
    buf.resize(total_size);

    const uint8_t* src_data = vattr.get_data().data();
    const size_t src_size = vattr.get_data().size();
    uint8_t* dst_ptr = buf.data();

    // Constant variability stores a single value (one element) that applies to
    // every element, so the source is always read at offset 0. (The previous
    // `src_data + f * stride` indexing read out of bounds for any mesh with
    // more than one face, since the backing buffer holds just one element.)
    if (src_size < stride) {
      PUSH_ERROR_AND_RETURN(
          "Constant attribute backing buffer is smaller than one element.");
    }

    for (size_t f = 0; f < triangulatedFaceCounts.size(); f++) {
      uint32_t nf = triangulatedFaceCounts[f];

      // copy `nf` triangles (each with 3 vertices)
      for (size_t k = 0; k < size_t(nf) * 3; k++) {
        std::memcpy(dst_ptr, src_data, stride);
        dst_ptr += stride;
      }
    }

    vattr.data = std::move(buf);
  } else if (vattr.is_uniform()) {
    // nothing is required
    return true;
  }

  return true;
}

template <typename T>
static bool TryReadMMapArray(
    const Stage &stage,
    const std::string &prim_path,
    const std::string &attr_name,
    std::vector<T> *out) {
  if (!stage.has_mmap_zero_copy()) return false;
  const MMapArrayRef *ref = stage.mmap_table()->find(prim_path, attr_name);
  if (!ref) return false;
  const T *ptr = stage.mmap_source()->get_ptr<T>(*ref);
  if (!ptr) return false;
  if (ref->element_count >
      (uint64_t((std::numeric_limits<size_t>::max)()) / sizeof(T))) {
    return false;
  }
  out->resize(static_cast<size_t>(ref->element_count));
  const size_t byte_count = static_cast<size_t>(ref->element_count) * sizeof(T);
  memcpy(out->data(), ptr, byte_count);
  return true;
}

/// Try to read an indexed primvar from mmap: read raw data, then expand with indices.
/// Returns false if mmap read fails or primvar has no indices (caller should use other path).
template <typename T>
static bool TryReadMMapArrayWithIndices(
    const Stage &stage,
    const std::string &prim_path,
    const std::string &attr_name,
    const GeomPrimvar &primvar,
    double timecode,
    std::vector<T> *out) {
  if (!stage.has_mmap_zero_copy()) return false;
  if (!primvar.has_indices()) return false;

  // Read raw (un-expanded) data from mmap
  std::vector<T> raw_data;
  if (!TryReadMMapArray<T>(stage, prim_path, attr_name, &raw_data)) {
    return false;
  }

  // Get indices (int arrays are NOT deferred — they use LZ4 compression)
  std::vector<int32_t> indices = primvar.get_indices(timecode);
  if (indices.empty()) {
    // No indices at this timecode — use raw data directly
    *out = std::move(raw_data);
    return true;
  }

  // Expand: out[i] = raw_data[indices[i]]
  out->resize(indices.size());
  for (size_t i = 0; i < indices.size(); i++) {
    int32_t idx = indices[i];
    if (idx >= 0 && size_t(idx) < raw_data.size()) {
      (*out)[i] = raw_data[size_t(idx)];
    }
    // Out-of-bounds indices get zero-initialized (default T{})
  }
  return true;
}

//
// name does not include "primvars:" prefix.
//
nonstd::expected<VertexAttribute, std::string> GetTextureCoordinate(
    const Stage &stage, const GeomMesh &mesh, const std::string &name,
    const double t, const value::TimeSampleInterpolationType tinterp,
    const std::string &prim_path = std::string(),
    std::string *warn = nullptr) {
  VertexAttribute vattr;

  std::string err;
  GeomPrimvar primvar;
  if (!GetGeomPrimvar(stage, &mesh, name, &primvar, &err, warn)) {
    return nonstd::make_unexpected(err);
  }

  if (!primvar.has_value()) {
    return nonstd::make_unexpected("No value exist for primvars:" + name +
                                   "\n");
  }

  // Accept the texCoord2f[] role type and the layout-identical float2[] (some
  // exporters author `st` as float2[] — e.g. usd-wg TextureTransformTest).
  const bool is_texcoord2f =
      (primvar.get_type_id() ==
       value::TypeTraits<std::vector<value::texcoord2f>>::type_id());
  const bool is_float2 =
      (primvar.get_type_id() ==
       value::TypeTraits<std::vector<value::float2>>::type_id());
  if (!is_texcoord2f && !is_float2) {
    return nonstd::make_unexpected(
        "Texture coordinate primvar must be texCoord2f[] or float2[] type, but "
        "got " + primvar.get_type_name() + "\n");
  }

  std::vector<value::texcoord2f> uvs;

  // mmap zero-copy path: read texcoords directly from mmap (texcoord2f only).
  // V2: also handles indexed primvars via TryReadMMapArrayWithIndices.
  bool got_from_mmap = false;
  if (is_texcoord2f && !prim_path.empty()) {
    if (primvar.has_indices()) {
      got_from_mmap = TryReadMMapArrayWithIndices<value::texcoord2f>(
          stage, prim_path, "primvars:" + name, primvar, t, &uvs);
    } else {
      got_from_mmap = TryReadMMapArray<value::texcoord2f>(
          stage, prim_path, "primvars:" + name, &uvs);
    }
  }

  if (!got_from_mmap) {
    if (is_texcoord2f) {
      if (!primvar.flatten_with_indices(t, &uvs, tinterp)) {
        return nonstd::make_unexpected(
            "Failed to retrieve texture coordinate primvar with concrete "
            "type.\n");
      }
    } else {  // float2[] — flatten as float2 then copy to texcoord2f (same
              // memory layout: two floats).
      std::vector<value::float2> f2;
      if (!primvar.flatten_with_indices(t, &f2, tinterp)) {
        return nonstd::make_unexpected(
            "Failed to retrieve float2[] texture coordinate primvar.\n");
      }
      // float2 (std::array<float,2>) and texcoord2f are both two contiguous
      // floats with identical layout — copy in one shot.
      static_assert(sizeof(value::float2) == sizeof(value::texcoord2f),
                    "float2 and texcoord2f must share layout for memcpy");
      uvs.resize(f2.size());
      if (!f2.empty()) {
        size_t byte_size;
        if (!safe::mul(f2.size(), sizeof(value::float2), &byte_size)) {
          return nonstd::make_unexpected(
              "Integer overflow: f2.size() * sizeof(value::float2)");
        }
        std::memcpy(uvs.data(), f2.data(), byte_size);
      }
    }
  }

  if (primvar.get_interpolation() == Interpolation::Varying) {
    vattr.variability = VertexVariability::Varying;
  } else if (primvar.get_interpolation() == Interpolation::Constant) {
    vattr.variability = VertexVariability::Constant;
  } else if (primvar.get_interpolation() == Interpolation::Uniform) {
    vattr.variability = VertexVariability::Uniform;
  } else if (primvar.get_interpolation() == Interpolation::Vertex) {
    vattr.variability = VertexVariability::Vertex;
  } else if (primvar.get_interpolation() == Interpolation::FaceVarying) {
    vattr.variability = VertexVariability::FaceVarying;
  }


  //LIGHTUSD_LOG_I("texcoord. " << name << ", " << uvs.size());
  DCOUT("texcoord " << name << " : " << uvs);

  vattr.format = VertexAttributeFormat::Vec2;
  size_t resize_size;
  if (!safe::n_to_size<value::texcoord2f>(uvs.size(), &resize_size)) {
    return nonstd::make_unexpected("Integer overflow: uvs.size() * sizeof(value::texcoord2f)");
  }
  vattr.data.resize(resize_size);
  memcpy(vattr.data.data(), uvs.data(), vattr.data.size());
  vattr.indices.clear();  // just in case.

  vattr.name = name;  // TODO: add "primvars:" namespace?
  //LIGHTUSD_LOG_I("end");

  return std::move(vattr);
}


namespace {

template <typename UnderlyingTy>
bool ScalarValueToVertexAttribute(const value::Value &value,
                                  const std::string &name,
                                  const VertexAttributeFormat format,
                                  VertexAttribute &dst, std::string *err) {
  if (VertexAttributeFormatSize(format) != sizeof(UnderlyingTy)) {
    PUSH_ERROR_AND_RETURN("format size mismatch.");
    return false;
  }

  if (auto pv = value.as<UnderlyingTy>()) {
    dst.data.resize(sizeof(UnderlyingTy));
    memcpy(dst.data.data(), pv, sizeof(UnderlyingTy));

    dst.elementSize = 1;
    dst.stride = 0;
    dst.format = format;
    dst.variability = VertexVariability::Constant;
    dst.name = name;
    dst.indices.clear();
    return true;
  }

  PUSH_ERROR_AND_RETURN("[Internal error] value is not scalar-typed value.");
}

template <typename UnderlyingTy>
bool ArrayValueToVertexAttribute(
    const value::Value &value, const std::string &name,
    const uint32_t elementSize, const VertexVariability variability,
    const uint32_t num_vertices, const uint32_t num_face_counts,
    const uint32_t num_face_vertex_indices, const VertexAttributeFormat format,
    VertexAttribute &dst, std::string *err) {
  if (!value::TypeTraits<UnderlyingTy>::is_array()) {
    PUSH_ERROR_AND_RETURN(
        "[Internal error] UnderlyingTy template parameter must be array type.");
  }

  size_t baseTySize = value::TypeTraits<UnderlyingTy>::size();

  size_t value_counts = value.array_size();
  if (value_counts == 0) {
    PUSH_ERROR_AND_RETURN("Empty array size");
  }

  if (variability == VertexVariability::Indexed) {
    PUSH_ERROR_AND_RETURN("Indexed variability is not supported.");
  }

  if (VertexAttributeFormatSize(format) != baseTySize) {
    PUSH_ERROR_AND_RETURN("format size mismatch. expected "
                          << VertexAttributeFormatSize(format) << " but got "
                          << baseTySize);
    return false;
  }

  DCOUT("value.type = " << value.type_name());
  DCOUT("UnderlyingTy = " << value::TypeTraits<UnderlyingTy>::type_name());
  const auto p = value.as<UnderlyingTy>();
  if (!p) {
    DCOUT("p is nullptr");
  }

  if (auto pv = value.as<UnderlyingTy>()) {
    switch (variability) {
    case VertexVariability::Constant: {
      if (value_counts != elementSize) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "{} # of items {} expected, but got {}. Variability = Constant",
            name, elementSize, value_counts));
      }
      break;
    }
    case VertexVariability::Uniform: {
      // Compute the expected count in size_t. `elementSize * num_face_counts`
      // multiplies two uint32_t in 32-bit arithmetic and can wrap, letting a
      // crafted array whose size equals the wrapped value pass validation.
      size_t expected_uniform;
      if (!safe::mul(size_t(elementSize), size_t(num_face_counts),
                     &expected_uniform) ||
          (value_counts != expected_uniform)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "{} # of items {} expected, but got {}. Variability = Uniform",
            name, uint64_t(elementSize) * uint64_t(num_face_counts),
            value_counts));
      }
      break;
    }
    case VertexVariability::Vertex: {
      size_t expected_vertex;
      if (!safe::mul(size_t(elementSize), size_t(num_vertices),
                     &expected_vertex) ||
          (value_counts != expected_vertex)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "{} # of items {} expected, but got {}. Variability = Vertex",
            name, uint64_t(elementSize) * uint64_t(num_vertices), value_counts));
      }
      break;
    case VertexVariability::Varying: {
      size_t expected_varying;
      if (!safe::mul(size_t(elementSize), size_t(num_vertices),
                     &expected_varying) ||
          (value_counts != expected_varying)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "{} # of items {} expected, but got {}. Variability = Varying",
            name, uint64_t(elementSize) * uint64_t(num_vertices), value_counts));
      }
      break;
    }
    case VertexVariability::FaceVarying: {
      size_t expected_fv;
      if (!safe::mul(size_t(elementSize), size_t(num_face_vertex_indices),
                     &expected_fv) ||
          (value_counts != expected_fv)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "# of items {} expected, but got {}. Variability = FaceVarying",
            uint64_t(elementSize) * uint64_t(num_face_vertex_indices),
            value_counts));
      }
      break;
    }
    case VertexVariability::Indexed: {
      PUSH_ERROR_AND_RETURN(fmt::format(
            "{} Internal error. 'Indexed' variability is not supported."));
      }
      break;
    }
    }

    size_t resize_size;
    if (!safe::mul(value_counts, baseTySize, &resize_size)) {
      PUSH_ERROR_AND_RETURN("Integer overflow: value_counts * baseTySize");
    }
    dst.data.resize(resize_size);
    size_t memcpy_size;
    if (!safe::mul(value_counts, baseTySize, &memcpy_size)) {
      PUSH_ERROR_AND_RETURN("Integer overflow in memcpy: value_counts * baseTySize");
    }
    memcpy(dst.data.data(), pv->data(), memcpy_size);

    dst.elementSize = elementSize;
    dst.stride = 0;
    dst.format = format;
    dst.variability = variability;
    dst.name = name;
    dst.indices.clear();
    return true;
  }

  PUSH_ERROR_AND_RETURN(fmt::format(
      "Requested underlying type {} but input `value` has underlying type {}.",
      value::TypeTraits<UnderlyingTy>::type_name(),
      value.underlying_type_name()));
}

#if defined(LIGHTUSD_WITH_MESHOPT)
//
// Optimize RenderMesh indices using meshoptimizer
//
[[maybe_unused]] static void OptimizeRenderMeshIndices(RenderMesh& mesh) {
  // Only optimize triangulated meshes with valid indices
  if (!mesh.is_triangulated() || mesh.triangulatedFaceVertexIndices.empty() || mesh.points.empty()) {
    return;
  }

  const size_t index_count = mesh.triangulatedFaceVertexIndices.size();
  const size_t vertex_count = mesh.points.size();

  if (index_count == 0 || vertex_count == 0) {
    return;
  }

  // Create optimized index buffer
  std::vector<unsigned int> optimized_indices(index_count);

  // Convert indices to unsigned int for meshoptimizer
  std::vector<unsigned int> indices(index_count);
  for (size_t i = 0; i < index_count; i++) {
    indices[i] = static_cast<unsigned int>(mesh.triangulatedFaceVertexIndices[i]);
  }

  // Step 1: Optimize vertex cache
  meshopt_optimizeVertexCache(optimized_indices.data(), indices.data(),
                              index_count, vertex_count);

  // Step 2: Optimize overdraw (requires vertex positions)
  if (!mesh.points.empty()) {
    std::vector<unsigned int> overdraw_optimized(index_count);
    meshopt_optimizeOverdraw(overdraw_optimized.data(), optimized_indices.data(),
                             index_count,
                             reinterpret_cast<const float*>(mesh.points.data()),
                             vertex_count,
                             sizeof(vec3), // stride
                             1.05f); // threshold (allow up to 5% vertex cache degradation)

    optimized_indices = std::move(overdraw_optimized);
  }

  // Step 3: Optimize vertex fetch
  std::vector<unsigned int> fetch_remap(vertex_count);
  size_t unique_vertices = meshopt_optimizeVertexFetchRemap(fetch_remap.data(),
                                                            optimized_indices.data(),
                                                            index_count,
                                                            vertex_count);

  // Only apply vertex fetch optimization if it reduces vertex count
  if (unique_vertices < vertex_count && unique_vertices > 0) {
    // Remap indices
    meshopt_remapIndexBuffer(optimized_indices.data(), optimized_indices.data(),
                             index_count, fetch_remap.data());

    // Remap vertex positions
    std::vector<vec3> optimized_points(unique_vertices);
    meshopt_remapVertexBuffer(optimized_points.data(), mesh.points.data(),
                              vertex_count, sizeof(vec3), fetch_remap.data());

    mesh.points = std::move(optimized_points);

    // TODO: Remap other vertex attributes (normals, texcoords, etc.) as needed
    // This would require more complex logic to handle all vertex attributes
  }

  // Convert back to uint32_t and update mesh
  for (size_t i = 0; i < index_count; i++) {
    mesh.triangulatedFaceVertexIndices[i] = static_cast<uint32_t>(optimized_indices[i]);
  }
}
#endif

}  // namespace

bool ToVertexAttribute(const GeomPrimvar &primvar, const std::string &name,
                       const uint32_t num_vertices,
                       const uint32_t num_face_counts,
                       const uint32_t num_face_vertex_indices,
                       VertexAttribute &dst, std::string *err, const double t,
                       const value::TimeSampleInterpolationType tinterp,
                       std::string *warn = nullptr) {
  uint32_t elementSize = uint32_t(primvar.get_elementSize());
  if (elementSize == 0) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("elementSize is zero for primvar: {}", primvar.name()));
  }

  VertexAttribute vattr;

  const lightusd::Attribute &attr = primvar.get_attribute();

  // Check if primvar has timesamples and report detailed warning
  if (attr.has_timesamples()) {
    std::string msg = fmt::format(
        "Geometry primvar '{}' has timesamples (animated values). "
        "RenderMesh conversion uses value at specified time (timecode={}). "
        "To capture animation, you need to convert at multiple timesamples. "
        "Consider using ConvertMesh() at each timeframe or implementing "
        "per-frame conversion. This is particularly important for vertex "
        "attributes like normals, tangents, texture coordinates, colors, "
        "and opacities that may change over time.",
        name, t);
    if (warn) {
      (*warn) += msg + "\n";
    }
    DCOUT("WARN: " << msg);
  }

  value::Value value;
  std::string flatten_err;
  if (!primvar.flatten_with_indices(t, &value, tinterp, &flatten_err)) {
    // INVARIANT: flatten_with_indices() returns false with NO error message
    // ONLY for a primvar that carries no data — either no authored value and no
    // timesamples (usdGeom.cc, the value::Value overload) or an empty authored
    // array (usdGeom-primvar-impl.inc, the typed overload; intentional
    // OpenUSD-compat, e.g. `float4[] primvars:tangents = []` in usd-wg
    // TextureTransformTest). Every *real* failure sets a message. So an empty
    // `flatten_err` here means "no data": skip the primvar (leaving `dst`
    // empty) rather than failing the whole mesh conversion. If that invariant
    // ever changes, surface real errors here instead of silently skipping.
    if (flatten_err.empty()) {
      if (warn) {
        (*warn) += fmt::format(
            "Primvar `{}` has no authored value (or an empty array); skipped.\n",
            name);
      }
      return true;
    }
    PUSH_ERROR_AND_RETURN(fmt::format(
        "Failed to flatten primvar `{}` (interpolation {}, elementSize {}): {}",
        name, to_string(primvar.get_interpolation()), elementSize, flatten_err));
  }

  bool is_array = value.type_id() & value::TYPE_ID_1D_ARRAY_BIT;
  DCOUT("is_array " << (is_array ? "true" : "false"));

  VertexVariability variability;
  if (primvar.get_interpolation() == Interpolation::Varying) {
    variability = VertexVariability::Varying;
  } else if (primvar.get_interpolation() == Interpolation::Constant) {
    variability = VertexVariability::Constant;
  } else if (primvar.get_interpolation() == Interpolation::Uniform) {
    variability = VertexVariability::Uniform;
  } else if (primvar.get_interpolation() == Interpolation::Vertex) {
    variability = VertexVariability::Vertex;
  } else if (primvar.get_interpolation() == Interpolation::FaceVarying) {
    variability = VertexVariability::FaceVarying;
  } else {
    PUSH_ERROR_AND_RETURN("[Internal Error] Invalid `interpolation` type.");
  }

  uint32_t baseUnderlyingTypeId =
      value.underlying_type_id() & (~value::TYPE_ID_1D_ARRAY_BIT);
  DCOUT("flattened primvar type: " << value.type_name() << ", underlying type "
                                   << value::GetTypeName(baseUnderlyingTypeId));

  // Cast to underlying type

#define TO_TYPED_VALUE(__underlying_ty, __vfmt)                                \
  if (baseUnderlyingTypeId == value::TypeTraits<__underlying_ty>::type_id()) { \
    if (is_array) {                                                            \
      return ArrayValueToVertexAttribute<std::vector<__underlying_ty>>(        \
          value, name, elementSize, variability, num_vertices,                 \
          num_face_counts, num_face_vertex_indices, __vfmt, dst, err);         \
    } else {                                                                   \
      return ScalarValueToVertexAttribute<__underlying_ty>(value, name,        \
                                                           __vfmt, dst, err);  \
    }                                                                          \
  } else

  // specialization for bool type: bool is represented as uint8 in USD primvar
  if (baseUnderlyingTypeId == value::TypeTraits<bool>::type_id()) {
    if (is_array) {
      return ArrayValueToVertexAttribute<std::vector<uint8_t>>(
          value, name, elementSize, variability, num_vertices, num_face_counts,
          num_face_vertex_indices, VertexAttributeFormat::Bool, dst, err);
    } else {
      return ScalarValueToVertexAttribute<uint8_t>(
          value, name, VertexAttributeFormat::Bool, dst, err);
    }
  } else
    TO_TYPED_VALUE(uint8_t, VertexAttributeFormat::Byte)
  TO_TYPED_VALUE(value::uchar2, VertexAttributeFormat::Byte2)
  TO_TYPED_VALUE(value::uchar3, VertexAttributeFormat::Byte3)
  TO_TYPED_VALUE(value::uchar4, VertexAttributeFormat::Byte4)
  TO_TYPED_VALUE(char, VertexAttributeFormat::Char)
  TO_TYPED_VALUE(value::char2, VertexAttributeFormat::Char2)
  TO_TYPED_VALUE(value::char3, VertexAttributeFormat::Char3)
  TO_TYPED_VALUE(value::char4, VertexAttributeFormat::Char4)
  TO_TYPED_VALUE(short, VertexAttributeFormat::Short)
  TO_TYPED_VALUE(value::short2, VertexAttributeFormat::Short2)
  TO_TYPED_VALUE(value::short3, VertexAttributeFormat::Short3)
  TO_TYPED_VALUE(value::short4, VertexAttributeFormat::Short4)
  TO_TYPED_VALUE(uint16_t, VertexAttributeFormat::Ushort)
  TO_TYPED_VALUE(value::ushort2, VertexAttributeFormat::Ushort2)
  TO_TYPED_VALUE(value::ushort3, VertexAttributeFormat::Ushort3)
  TO_TYPED_VALUE(value::ushort4, VertexAttributeFormat::Ushort4)
  TO_TYPED_VALUE(int, VertexAttributeFormat::Int)
  TO_TYPED_VALUE(value::int2, VertexAttributeFormat::Ivec2)
  TO_TYPED_VALUE(value::int3, VertexAttributeFormat::Ivec3)
  TO_TYPED_VALUE(value::int4, VertexAttributeFormat::Ivec4)
  TO_TYPED_VALUE(uint32_t, VertexAttributeFormat::Uint)
  TO_TYPED_VALUE(value::uint2, VertexAttributeFormat::Uvec2)
  TO_TYPED_VALUE(value::uint3, VertexAttributeFormat::Uvec3)
  TO_TYPED_VALUE(value::uint4, VertexAttributeFormat::Uvec4)
  TO_TYPED_VALUE(float, VertexAttributeFormat::Float)
  TO_TYPED_VALUE(value::float2, VertexAttributeFormat::Vec2)
  TO_TYPED_VALUE(value::float3, VertexAttributeFormat::Vec3)
  TO_TYPED_VALUE(value::float4, VertexAttributeFormat::Vec4)
  TO_TYPED_VALUE(value::half, VertexAttributeFormat::Half)
  TO_TYPED_VALUE(value::half2, VertexAttributeFormat::Half2)
  TO_TYPED_VALUE(value::half3, VertexAttributeFormat::Half3)
  TO_TYPED_VALUE(value::half4, VertexAttributeFormat::Half4)
  TO_TYPED_VALUE(double, VertexAttributeFormat::Double)
  TO_TYPED_VALUE(value::double2, VertexAttributeFormat::Dvec2)
  TO_TYPED_VALUE(value::double3, VertexAttributeFormat::Dvec3)
  TO_TYPED_VALUE(value::double4, VertexAttributeFormat::Dvec4)
  TO_TYPED_VALUE(value::matrix2f, VertexAttributeFormat::Mat2)
  TO_TYPED_VALUE(value::matrix3f, VertexAttributeFormat::Mat3)
  TO_TYPED_VALUE(value::matrix4f, VertexAttributeFormat::Mat4)
  TO_TYPED_VALUE(value::matrix2d, VertexAttributeFormat::Dmat2)
  TO_TYPED_VALUE(value::matrix3d, VertexAttributeFormat::Dmat3)
  TO_TYPED_VALUE(value::matrix4d, VertexAttributeFormat::Dmat4) {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Unknown or unsupported data type for Geom PrimVar: {}",
                    attr.type_name()));
  }

#undef TO_TYPED_VALUE
}


#if 1
///
/// Input: points, faceVertexCounts, faceVertexIndices
/// Output: triangulated faceVertexCounts(all filled with 3), triangulated
/// faceVertexIndices, triangulatedToOrigFaceVertexIndexMap (length =
/// triangulated faceVertexIndices. triangulatedToOrigFaceVertexIndexMap[i]
/// stores an array index to original faceVertexIndices. For remapping
/// facevarying primvar attributes.)
///
/// triangulatedFaceVertexCounts: len = len(faceVertexCounts). Records the
/// number of triangle faces. 1 = triangle. 2 = quad, ... For remapping face
/// indices(e.g. GeomSubset::indices)
///
/// triangulated*** output is generated even when input mesh is fully composed
/// from triangles(`faceVertexCounts` are all filled with 3) Return false when a
/// polygon is degenerated. No overlap check at the moment
///
/// Example:
///   - faceVertexCounts = [4]
///   - faceVertexIndices = [0, 1, 3, 2]
///
///   - triangulatedFaceVertexCounts = [3, 3]
///   - triangulatedFaceVertexIndices = [0, 1, 3, 0, 3, 2]
///   - triangulatedToOrigFaceVertexIndexMap = [0, 1, 2, 0, 2, 3]
///
/// T = value::float3 or value::double3
/// BaseTy = float or double
template <typename T, typename BaseTy>
bool TriangulatePolygon(
    const std::vector<T> &points, const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices,
    std::vector<uint32_t> &triangulatedFaceVertexCounts,
    std::vector<uint32_t> &triangulatedFaceVertexIndices,
    std::vector<uint32_t> &triangulatedToOrigFaceVertexIndexMap,
    std::vector<uint32_t> &triangulatedFaceCounts,
    MeshConverterConfig::TriangulationMethod triangulation_method,
    std::string &warn, std::string &err,
    const std::set<uint32_t> &holeFaceIndices = std::set<uint32_t>()) {
  triangulatedFaceVertexCounts.clear();
  triangulatedFaceVertexIndices.clear();
  triangulatedToOrigFaceVertexIndexMap.clear();
  triangulatedFaceCounts.clear();

  // Pre-allocate based on estimated triangle count.
  // For each face with N vertices, we generate N-2 triangles, each with 3 indices.
  // Total triangles ≈ total_vertex_indices - 2*num_faces
  // This is an upper bound estimate.
  size_t numFaces = faceVertexCounts.size();
  size_t numFaceVertexIndices = faceVertexIndices.size();
  size_t estimatedTriangles = numFaceVertexIndices > 2 * numFaces
                             ? numFaceVertexIndices - 2 * numFaces
                             : numFaces;
  size_t estimatedIndices = estimatedTriangles * 3;

  triangulatedFaceVertexCounts.reserve(estimatedTriangles);
  triangulatedFaceVertexIndices.reserve(estimatedIndices);
  triangulatedToOrigFaceVertexIndexMap.reserve(estimatedIndices);
  triangulatedFaceCounts.reserve(numFaces);

  size_t faceIndexOffset = 0;

  // Reusable temporaries for earcut (avoid per-face heap allocation).
  using Point3D = std::array<BaseTy, 3>;
  using Point2D = std::array<BaseTy, 2>;
  std::vector<Point2D> polyline;
  std::vector<std::vector<Point2D>> polygon_2d(1);  // single ring, no holes

  // For each polygon(face)
  for (size_t i = 0; i < faceVertexCounts.size(); i++) {
    uint32_t npolys = faceVertexCounts[i];

    if (npolys < 3) {
      err = fmt::format(
          "faceVertex count must be 3(triangle) or "
          "more(polygon), but got faceVertexCounts[{}] = {}\n",
          i, npolys);
      return false;
    }

    if (faceIndexOffset + npolys > faceVertexIndices.size()) {
      err = fmt::format(
          "Invalid faceVertexIndices or faceVertexCounts. faceVertex index "
          "exceeds faceVertexIndices.size() at [{}]\n",
          i);
      return false;
    }

    // Skip hole faces (`holeIndices` in USD GeomMesh): emit no triangles for
    // them. Still record a 0 entry in triangulatedFaceCounts so per-original-
    // face bookkeeping(e.g. GeomSubset face index remapping) stays aligned.
    if (holeFaceIndices.count(uint32_t(i))) {
      triangulatedFaceCounts.push_back(0);
      faceIndexOffset += npolys;
      continue;
    }

    if (npolys == 3) {
      // No need for triangulation.
      triangulatedFaceVertexCounts.push_back(3);
      triangulatedFaceVertexIndices.push_back(
          faceVertexIndices[faceIndexOffset + 0]);
      triangulatedFaceVertexIndices.push_back(
          faceVertexIndices[faceIndexOffset + 1]);
      triangulatedFaceVertexIndices.push_back(
          faceVertexIndices[faceIndexOffset + 2]);
      triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 0));
      triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 1));
      triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 2));
      triangulatedFaceCounts.push_back(1);
    } else if (npolys == 4) {
      // Split quad along the shorter diagonal for better triangle quality.
      // Diagonal 0-2 vs diagonal 1-3: compare squared lengths.
      uint32_t idx0 = faceVertexIndices[faceIndexOffset + 0];
      uint32_t idx1 = faceVertexIndices[faceIndexOffset + 1];
      uint32_t idx2 = faceVertexIndices[faceIndexOffset + 2];
      uint32_t idx3 = faceVertexIndices[faceIndexOffset + 3];

      // Defense-in-depth: indices are expected to be pre-validated against
      // points.size(), but guard here to avoid OOB on malformed input.
      if ((idx0 >= points.size()) || (idx1 >= points.size()) ||
          (idx2 >= points.size()) || (idx3 >= points.size())) {
        err = fmt::format("Invalid vertex index at face {}.\n", i);
        return false;
      }

      const T &p0 = points[idx0];
      const T &p1 = points[idx1];
      const T &p2 = points[idx2];
      const T &p3 = points[idx3];

      BaseTy d02_sq = (p0[0]-p2[0])*(p0[0]-p2[0]) + (p0[1]-p2[1])*(p0[1]-p2[1]) + (p0[2]-p2[2])*(p0[2]-p2[2]);
      BaseTy d13_sq = (p1[0]-p3[0])*(p1[0]-p3[0]) + (p1[1]-p3[1])*(p1[1]-p3[1]) + (p1[2]-p3[2])*(p1[2]-p3[2]);

      triangulatedFaceVertexCounts.push_back(3);
      triangulatedFaceVertexCounts.push_back(3);

      if (d13_sq < d02_sq) {
        // Split along diagonal 1-3: triangles (0,1,3) and (1,2,3)
        triangulatedFaceVertexIndices.push_back(idx0);
        triangulatedFaceVertexIndices.push_back(idx1);
        triangulatedFaceVertexIndices.push_back(idx3);

        triangulatedFaceVertexIndices.push_back(idx1);
        triangulatedFaceVertexIndices.push_back(idx2);
        triangulatedFaceVertexIndices.push_back(idx3);

        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 0));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 1));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 3));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 1));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 2));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 3));
      } else {
        // Split along diagonal 0-2: triangles (0,1,2) and (0,2,3)
        triangulatedFaceVertexIndices.push_back(idx0);
        triangulatedFaceVertexIndices.push_back(idx1);
        triangulatedFaceVertexIndices.push_back(idx2);

        triangulatedFaceVertexIndices.push_back(idx0);
        triangulatedFaceVertexIndices.push_back(idx2);
        triangulatedFaceVertexIndices.push_back(idx3);

        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 0));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 1));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 2));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 0));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 2));
        triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 3));
      }
      triangulatedFaceCounts.push_back(2);
    } else {
      // Polygon with 5+ vertices
      if (triangulation_method == MeshConverterConfig::TriangulationMethod::TriangleFan) {
        // Simple triangle fan triangulation
        // This assumes the polygon is convex
        // Creates triangles: (0,1,2), (0,2,3), (0,3,4), ...

        size_t ntris = npolys - 2;

        for (size_t k = 0; k < ntris; k++) {
          triangulatedFaceVertexCounts.push_back(3);

          // First vertex is always the pivot (index 0)
          triangulatedFaceVertexIndices.push_back(
              faceVertexIndices[faceIndexOffset + 0]);
          triangulatedFaceVertexIndices.push_back(
              faceVertexIndices[faceIndexOffset + k + 1]);
          triangulatedFaceVertexIndices.push_back(
              faceVertexIndices[faceIndexOffset + k + 2]);

          triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + 0));
          triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + k + 1));
          triangulatedToOrigFaceVertexIndexMap.push_back(uint32_t(faceIndexOffset + k + 2));
        }

        triangulatedFaceCounts.push_back(uint32_t(ntris));

      } else {
        // Use earcut algorithm (default, handles complex polygons)
        // Use double for accuracy. `float` precision may classify small-are polygon as degenerated.
        // Find the normal axis of the polygon using Newell's method
        value::double3 n = {0, 0, 0};

        for (size_t k = 0; k < npolys; ++k) {
          size_t vi0 = faceVertexIndices[faceIndexOffset + k];
          size_t vi0_2 = faceVertexIndices[faceIndexOffset + (k + 1) % npolys];

          if (vi0 >= points.size() || vi0_2 >= points.size()) {
            err = fmt::format("Invalid vertex index at face {}.\n", i);
            return false;
          }

          // Newell's method: compute entirely in double to avoid
          // float cancellation in (p1 - p2) for nearly-coplanar vertices.
          const T &p0 = points[vi0];
          const T &p1 = points[vi0_2];
          double d0x = double(p0[0]), d0y = double(p0[1]), d0z = double(p0[2]);
          double d1x = double(p1[0]), d1y = double(p1[1]), d1z = double(p1[2]);

          n[0] += (d0y - d1y) * (d0z + d1z);
          n[1] += (d0z - d1z) * (d0x + d1x);
          n[2] += (d0x - d1x) * (d0y + d1y);
          DCOUT("p0 " << p0);
          DCOUT("p1 " << p1);
          DCOUT("n " << n);
        }
        //BaseTy length_n = vlength(n);
        double length_n = vlength(n);

        // Skip degenerate polygon (zero-area) instead of aborting the
        // entire mesh.  Production meshes often have a few collapsed faces.
        if (std::fabs(length_n) < std::numeric_limits<double>::epsilon()) {
          DCOUT("length_n " << length_n);
          warn += fmt::format("Skipping degenerate polygon at face {}.\n", i);
          triangulatedFaceCounts.push_back(0);
          faceIndexOffset += npolys;
          continue;
        }

        // Negative is to flip the normal to the correct direction
        n = vnormalize(n);

        T axis_w, axis_v, axis_u;
        axis_w[0] = BaseTy(n[0]);
        axis_w[1] = BaseTy(n[1]);
        axis_w[2] = BaseTy(n[2]);
        T a;
        if (std::fabs(axis_w[0]) > BaseTy(0.9999999)) {  // TODO: use 1.0 - eps?
          a = {BaseTy(0), BaseTy(1), BaseTy(0)};
        } else {
          a = {BaseTy(1), BaseTy(0), BaseTy(0)};
        }
        axis_v = vnormalize(vcross(axis_w, a));
        axis_u = vcross(axis_w, axis_v);

        // Project polygon vertices to 2D via the computed normal frame.
        // Reuse polyline/polygon_2d across faces to avoid per-face allocation.
        polyline.clear();
        for (size_t k = 0; k < npolys; k++) {
          size_t vidx = faceVertexIndices[faceIndexOffset + k];
          const T &v = points[vidx];

          // world to local
          Point3D loc = {vdot(v, axis_u), vdot(v, axis_v), vdot(v, axis_w)};
          polyline.push_back({loc[0], loc[1]});
        }

        polygon_2d[0] = polyline;  // single ring, no holes

        std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(polygon_2d);
        //  => result = 3 * faces, clockwise

        if (indices.empty() || (indices.size() % 3) != 0) {
          // Earcut failed — skip this face gracefully.
          warn += fmt::format(
              "Failed to triangulate polygon at face {} "
              "(not CCW, has holes, or invalid topology).\n", i);
          triangulatedFaceCounts.push_back(0);
          faceIndexOffset += npolys;
          continue;
        }

        size_t ntris = indices.size() / 3;

        // Up to 2GB tris.
        if (ntris > size_t((std::numeric_limits<int32_t>::max)())) {
          err = "Too many triangles are generated.\n";
          return false;
        }

        for (size_t k = 0; k < ntris; k++) {
          triangulatedFaceVertexCounts.push_back(3);
          // earcut returns clockwise triangles, but USD expects CCW
          // so we reverse the winding order by swapping indices 1 and 2
          triangulatedFaceVertexIndices.push_back(
              faceVertexIndices[faceIndexOffset + indices[3 * k + 0]]);
          triangulatedFaceVertexIndices.push_back(
              faceVertexIndices[faceIndexOffset + indices[3 * k + 2]]);
          triangulatedFaceVertexIndices.push_back(
              faceVertexIndices[faceIndexOffset + indices[3 * k + 1]]);

          triangulatedToOrigFaceVertexIndexMap.push_back(
              uint32_t(faceIndexOffset + indices[3 * k + 0]));
          triangulatedToOrigFaceVertexIndexMap.push_back(
              uint32_t(faceIndexOffset + indices[3 * k + 2]));
          triangulatedToOrigFaceVertexIndexMap.push_back(
              uint32_t(faceIndexOffset + indices[3 * k + 1]));
        }
        triangulatedFaceCounts.push_back(uint32_t(ntris));
      }
    }

    faceIndexOffset += npolys;
  }

  return true;
}
#endif


//
// Compute geometric normal in CCW(Counter Clock-Wise) manner
// Also computes the area of the input triangle.
//
// GeometricNormal() moved to render-data-mesh-tangent.cc (only used by
// ComputeNormals there).

//
// Compute a normal for vertices.
// Normal vector is computed as weighted(by the area of the triangle) vector.
//
// TODO: Implement better normal calculation. ref.
// http://www.bytehazard.com/articles/vertnorm.html
//

}  // namespace

// ---------------------------------------------------------------
// BuildIndices implementation
// (moved from render-data.hh to reduce header bloat)
// ---------------------------------------------------------------

template <class VertexInput, class VertexOutput, class PackedVert,
          class PackedVertHasher, class PackedVertEqual>
void BuildIndices(const VertexInput &input, VertexOutput &output,
                  std::vector<uint32_t> &out_indices, std::vector<uint32_t> &out_point_indices)
{
  lightusd::HashMap<PackedVert, uint32_t, PackedVertHasher, PackedVertEqual>
      vertexToIndexMap;

  auto GetSimilarVertex = [&](const PackedVert &v, uint32_t &out_idx) -> bool {
    auto it = vertexToIndexMap.find(v);
    if (it == vertexToIndexMap.end()) {
      return false;
    }

    out_idx = it->second;
    return true;
  };

  for (size_t i = 0; i < input.size(); i++) {
    PackedVert v;
    input.get(i, v);

    uint32_t index{0};
    bool found = GetSimilarVertex(v, index);
    if (found) {
      out_indices.push_back(index);
    } else {
      uint32_t new_index = uint32_t(output.size());
      out_indices.push_back(new_index);
      output.push_back(v);
      vertexToIndexMap[v] = new_index;
    }
    out_point_indices.push_back(v.point_index);
  }
}

// Explicit instantiation for BuildIndices
template void BuildIndices<
    DefaultVertexInput<DefaultPackedVertexData>,
    DefaultVertexOutput<DefaultPackedVertexData>,
    DefaultPackedVertexData,
    DefaultPackedVertexDataHasher,
    DefaultPackedVertexDataEqual>(
    const DefaultVertexInput<DefaultPackedVertexData> &,
    DefaultVertexOutput<DefaultPackedVertexData> &,
    std::vector<uint32_t> &, std::vector<uint32_t> &);


namespace {

// UDIM atlas UV remap for one primvar: uv' = uv * scale + offset.
struct UDIMUVRemap {
  vec2 scale{1.0f, 1.0f};
  vec2 offset{0.0f, 0.0f};
};

bool ListUVNames(const RenderMaterial &material,
                 const std::vector<UVTexture> &textures,
                 StringAndIdMap &si_map,
                 std::map<std::string, UDIMUVRemap> *udim_remaps = nullptr) {
  // Record the UV-set remap for combined-UDIM textures so the mesh UV set can
  // be rebaked into the atlas layout. Keep-as-is UDIM textures
  // (udim_texture_id >= 0) keep their tiles separate and are not remapped.
  auto record_udim = [&](const UVTexture &tex) {
    if (!udim_remaps) return;
    if (!tex.is_udim || tex.udim_texture_id >= 0) return;
    if (tex.varname_uv.empty()) return;
    UDIMUVRemap r;
    r.scale = tex.udim_uv_scale;
    r.offset = tex.udim_uv_offset;
    (*udim_remaps)[tex.varname_uv] = r;
  };

  // Helper lambdas to extract UV names from shader parameters
  auto fun_vec3 = [&](const ShaderParam<vec3> &param) {
    int32_t texId = param.texture_id;
    if ((texId >= 0) && (size_t(texId) < textures.size())) {
      const UVTexture &tex = textures[size_t(texId)];
      if (tex.varname_uv.size()) {
        if (si_map.find(tex.varname_uv) == si_map.s_end()) {
          uint64_t slotId = si_map.size();
          DCOUT("Add textureSlot: " << tex.varname_uv << ", " << slotId);
          si_map.add(tex.varname_uv, slotId);
        }
        record_udim(tex);
      }
    }
  };

  auto fun_float = [&](const ShaderParam<float> &param) {
    int32_t texId = param.texture_id;
    if ((texId >= 0) && (size_t(texId) < textures.size())) {
      const UVTexture &tex = textures[size_t(texId)];
      if (tex.varname_uv.size()) {
        if (si_map.find(tex.varname_uv) == si_map.s_end()) {
          uint64_t slotId = si_map.size();
          DCOUT("Add textureSlot: " << tex.varname_uv << ", " << slotId);
          si_map.add(tex.varname_uv, slotId);
        }
        record_udim(tex);
      }
    }
  };

  // Check UsdPreviewSurface shader
  if (material.surfaceShader.has_value()) {
    fun_vec3(material.surfaceShader->diffuseColor);
    fun_vec3(material.surfaceShader->normal);
    fun_float(material.surfaceShader->metallic);
    fun_float(material.surfaceShader->roughness);
    fun_float(material.surfaceShader->clearcoat);
    fun_float(material.surfaceShader->clearcoatRoughness);
    fun_float(material.surfaceShader->opacity);
    fun_float(material.surfaceShader->opacityThreshold);
    fun_float(material.surfaceShader->ior);
    fun_float(material.surfaceShader->displacement);
    fun_float(material.surfaceShader->occlusion);
  }

  // Check MaterialX OpenPBR shader
  if (material.openPBRShader.has_value()) {
    // Base layer
    fun_float(material.openPBRShader->base_weight);
    fun_vec3(material.openPBRShader->base_color);
    fun_float(material.openPBRShader->base_roughness);
    fun_float(material.openPBRShader->base_metalness);
    fun_float(material.openPBRShader->base_diffuse_roughness);

    // Specular layer
    fun_float(material.openPBRShader->specular_weight);
    fun_vec3(material.openPBRShader->specular_color);
    fun_float(material.openPBRShader->specular_roughness);
    fun_float(material.openPBRShader->specular_ior);
    fun_float(material.openPBRShader->specular_ior_level);
    fun_float(material.openPBRShader->specular_anisotropy);
    fun_float(material.openPBRShader->specular_rotation);

    // Transmission
    fun_float(material.openPBRShader->transmission_weight);
    fun_vec3(material.openPBRShader->transmission_color);
    fun_float(material.openPBRShader->transmission_depth);
    fun_vec3(material.openPBRShader->transmission_scatter);
    fun_float(material.openPBRShader->transmission_scatter_anisotropy);
    fun_float(material.openPBRShader->transmission_dispersion);

    // Subsurface
    fun_float(material.openPBRShader->subsurface_weight);
    fun_vec3(material.openPBRShader->subsurface_color);
    fun_float(material.openPBRShader->subsurface_radius);
    fun_vec3(material.openPBRShader->subsurface_radius_scale);
    fun_float(material.openPBRShader->subsurface_scale);
    fun_float(material.openPBRShader->subsurface_anisotropy);

    // Sheen
    fun_float(material.openPBRShader->sheen_weight);
    fun_vec3(material.openPBRShader->sheen_color);
    fun_float(material.openPBRShader->sheen_roughness);

    // Fuzz
    fun_float(material.openPBRShader->fuzz_weight);
    fun_vec3(material.openPBRShader->fuzz_color);
    fun_float(material.openPBRShader->fuzz_roughness);

    // Thin film
    fun_float(material.openPBRShader->thin_film_weight);
    fun_float(material.openPBRShader->thin_film_thickness);
    fun_float(material.openPBRShader->thin_film_ior);

    // Coat
    fun_float(material.openPBRShader->coat_weight);
    fun_vec3(material.openPBRShader->coat_color);
    fun_float(material.openPBRShader->coat_roughness);
    fun_float(material.openPBRShader->coat_anisotropy);
    fun_float(material.openPBRShader->coat_rotation);
    fun_float(material.openPBRShader->coat_ior);
    fun_float(material.openPBRShader->coat_affect_color);
    fun_float(material.openPBRShader->coat_affect_roughness);

    // Emission
    fun_float(material.openPBRShader->emission_luminance);
    fun_vec3(material.openPBRShader->emission_color);

    // Geometry
    fun_float(material.openPBRShader->opacity);
    fun_vec3(material.openPBRShader->normal);
    fun_vec3(material.openPBRShader->tangent);
  }

  return true;
}


#undef PushError

}  // namespace

///
/// Convert vertex variability either 'vertex' or 'facevarying'
///
/// @param[in] to_vertex_varying true: Convert to 'vertrex' varying. false:
/// Convert to 'facevarying'
///
bool RenderSceneConverter::ConvertVertexVariabilityImpl(
    VertexAttribute &vattr, const bool to_vertex_varying,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices) {
  if (vattr.data.empty()) {
    return true;
  }

  if (vattr.variability == VertexVariability::Uniform) {
    if (to_vertex_varying) {
      auto result = UniformToVertex(vattr.get_data(), vattr.stride_bytes(),
                                    faceVertexCounts, faceVertexIndices);

      if (!result) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Convert `{}` attribute with uniform-varying "
                        "to vertex-varying failed: {}",
                        vattr.name, result.error()));
      }

      vattr.data = result.value();
      vattr.variability = VertexVariability::Vertex;

    } else {
      auto result = UniformToFaceVarying(vattr.get_data(), vattr.stride_bytes(),
                                         faceVertexCounts);
      if (!result) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Convert uniform `{}` attribute to failed: {}",
                        vattr.name, result.error()));
      }

      vattr.data = result.value();
      vattr.variability = VertexVariability::FaceVarying;
    }
  } else if (vattr.variability == VertexVariability::Constant) {
    if (to_vertex_varying) {
      auto result = ConstantToVertex(vattr.get_data(), vattr.stride_bytes(),
                                     faceVertexCounts, faceVertexIndices);

      if (!result) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Convert `{}` attribute with uniform-varying "
                        "to vertex-varying failed: {}",
                        vattr.name, result.error()));
      }

      vattr.data = result.value();
      vattr.variability = VertexVariability::Vertex;

    } else {
      auto result = UniformToFaceVarying(vattr.get_data(), vattr.stride_bytes(),
                                         faceVertexCounts);
      if (!result) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Convert uniform `{}` attribute to failed: {}",
                        vattr.name, result.error()));
      }

      vattr.data = result.value();
      vattr.variability = VertexVariability::FaceVarying;
    }

  } else if ((vattr.variability == VertexVariability::Vertex) ||
             (vattr.variability == VertexVariability::Varying)) {
    if (!to_vertex_varying) {
      auto result = VertexToFaceVarying(vattr.get_data(), vattr.stride_bytes(),
                                        faceVertexCounts, faceVertexIndices);
      if (!result) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("Convert vertex/varying `{}` attribute to failed: {}",
                        vattr.name, result.error()));
      }

      vattr.data = result.value();
      vattr.variability = VertexVariability::FaceVarying;
    }

  } else if (vattr.variability == VertexVariability::FaceVarying) {
    if (to_vertex_varying) {
      PUSH_ERROR_AND_RETURN(
          "Internal error. `to_vertex_varying` should not be true when "
          "FaceVarying.");
    }

  } else {
    PUSH_ERROR_AND_RETURN(
        fmt::format("Unsupported/unimplemented interpolation: {} ",
                    to_string(vattr.variability)));
  }

  return true;
}

// ---------------------------------------------------------------------------
// Reorder vertex-varying attributes (points, joint/weights, BlendShape
// targets) so that they match a new vertex layout.
//
// `vert_to_point[v]` gives the original mesh.points index for output vertex v.
// The array size determines the number of output vertices.
// ---------------------------------------------------------------------------
namespace {
#define PushError(msg) TYDRA_PUSH_ERROR(err, msg)

static bool ReorderVertexVaryingAttributes(
    RenderMesh &mesh,
    const std::vector<uint32_t> &vert_to_point,
    std::string *err) {

  const size_t num_verts = vert_to_point.size();
  if (num_verts == 0) {
    PUSH_ERROR_AND_RETURN(
        "Internal error. vert_to_point is empty in ReorderVertexVaryingAttributes.");
  }

  // --- Points ---
  {
    std::vector<value::float3> tmp_points(num_verts);
    for (size_t v = 0; v < num_verts; v++) {
      if (vert_to_point[v] >= mesh.points.size()) {
        PUSH_ERROR_AND_RETURN("Internal error. point index out-of-range.");
      }
      tmp_points[v] = mesh.points[vert_to_point[v]];
    }
    mesh.points.swap(tmp_points);
  }

  // --- Joint indices / weights ---
  if (mesh.joint_and_weights.jointIndices.size()) {
    if (mesh.joint_and_weights.elementSize < 1) {
      PUSH_ERROR_AND_RETURN(
          "Internal error. Invalid elementSize in mesh.joint_and_weights.");
    }
    uint32_t elementSize = uint32_t(mesh.joint_and_weights.elementSize);
    // Overflow-safe allocation sizing. `elementSize` can be as large as
    // max_skin_elementSize and `num_verts` is mesh-controlled, so the product
    // can overflow size_t (notably on wasm32 where size_t is 32-bit).
    size_t tmp_count;
    if (!safe::mul(num_verts, size_t(elementSize), &tmp_count)) {
      PUSH_ERROR_AND_RETURN("Skin weights buffer size overflow.");
    }
    std::vector<int> tmp_indices(tmp_count);
    std::vector<float> tmp_weights(tmp_count);
    for (size_t v = 0; v < num_verts; v++) {
      // Compute the source base offset in size_t. Computing
      // `elementSize * vert_to_point[v]` directly multiplies two uint32_t in
      // 32-bit arithmetic and can wrap, letting an out-of-range index slip
      // past the bounds check while the access below uses the true (64-bit)
      // offset and reads OOB. Validate the whole [off, off+elementSize) span.
      size_t src_off;
      if (!safe::mul(size_t(elementSize), size_t(vert_to_point[v]), &src_off)) {
        PUSH_ERROR_AND_RETURN("Skin index offset overflow.");
      }
      const size_t dst_off = size_t(elementSize) * v;  // < tmp_count, no overflow

      if ((src_off >= mesh.joint_and_weights.jointIndices.size()) ||
          ((mesh.joint_and_weights.jointIndices.size() - src_off) <
           size_t(elementSize))) {
        PUSH_ERROR_AND_RETURN(
            "Internal error. point index exceeds jointIndices.size.");
      }
      for (size_t k = 0; k < elementSize; k++) {
        tmp_indices[dst_off + k] =
            mesh.joint_and_weights.jointIndices[src_off + k];
      }

      if ((src_off >= mesh.joint_and_weights.jointWeights.size()) ||
          ((mesh.joint_and_weights.jointWeights.size() - src_off) <
           size_t(elementSize))) {
        PUSH_ERROR_AND_RETURN(
            "Internal error. point index exceeds jointWeights.size.");
      }
      for (size_t k = 0; k < elementSize; k++) {
        tmp_weights[dst_off + k] =
            mesh.joint_and_weights.jointWeights[src_off + k];
      }
    }
    mesh.joint_and_weights.jointIndices.swap(tmp_indices);
    mesh.joint_and_weights.jointWeights.swap(tmp_weights);
  }

  // --- BlendShape targets ---
  if (mesh.targets.size()) {
    // For BlendShape, reordering pointIndices, pointOffsets and normalOffsets is not enough.
    // Some points could be duplicated, so we need to find a mapping of org pointIdx -> pointIdx list in reordered points,
    // Then splat point attributes accordingly.

    // org pointIdx -> List of pointIdx in reordered points.
    lightusd::HashMap<uint32_t, std::vector<uint32_t>> pointIdxRemap;
    pointIdxRemap.reserve(num_verts);

    for (size_t v = 0; v < num_verts; v++) {
      pointIdxRemap[vert_to_point[v]].push_back(uint32_t(v));
    }

    for (auto &target : mesh.targets) {

      std::vector<value::float3> tmpPointOffsets;
      std::vector<value::float3> tmpNormalOffsets;
      std::vector<uint32_t> tmpPointIndices;

      // In-between offsets are parallel to the primary `pointIndices`, so they
      // are splatted with the same remap to stay aligned after reordering. Only
      // pointOffsets are carried (the converter does not store in-between
      // normalOffsets -- morphed meshes shade with recomputed normals).
      std::vector<InbetweenShapeTarget *> ibTargets;
      std::vector<std::vector<value::float3>> ibTmpPointOffsets;
      for (auto &ib : target.second.inbetweens) {
        ibTargets.push_back(&ib.second);
        ibTmpPointOffsets.emplace_back();
      }

      for (size_t i = 0; i < target.second.pointIndices.size(); i++) {

        uint32_t orgPointIdx = target.second.pointIndices[i];
        auto remap_it = pointIdxRemap.find(orgPointIdx);
        if (remap_it == pointIdxRemap.end()) {
          PUSH_ERROR_AND_RETURN("Invalid pointIndices value.");
        }
        const std::vector<uint32_t> &dstPointIndices = remap_it->second;

        for (size_t k = 0; k < dstPointIndices.size(); k++) {
          if (target.second.pointOffsets.size()) {
            if (i >= target.second.pointOffsets.size()) {
              PUSH_ERROR_AND_RETURN("Invalid pointOffsets.size.");
            }
            tmpPointOffsets.push_back(target.second.pointOffsets[i]);
          }
          if (target.second.normalOffsets.size()) {
            if (i >= target.second.normalOffsets.size()) {
              PUSH_ERROR_AND_RETURN("Invalid normalOffsets.size.");
            }
            tmpNormalOffsets.push_back(target.second.normalOffsets[i]);
          }

          for (size_t b = 0; b < ibTargets.size(); b++) {
            if (ibTargets[b]->pointOffsets.size()) {
              if (i >= ibTargets[b]->pointOffsets.size()) {
                PUSH_ERROR_AND_RETURN("Invalid in-between pointOffsets.size.");
              }
              ibTmpPointOffsets[b].push_back(ibTargets[b]->pointOffsets[i]);
            }
          }

          tmpPointIndices.push_back(dstPointIndices[k]);
        }
      }

      target.second.pointIndices.swap(tmpPointIndices);
      target.second.pointOffsets.swap(tmpPointOffsets);
      target.second.normalOffsets.swap(tmpNormalOffsets);

      for (size_t b = 0; b < ibTargets.size(); b++) {
        ibTargets[b]->pointOffsets.swap(ibTmpPointOffsets[b]);
      }

    }

  }

  return true;
}

#undef PushError
}  // namespace

bool RenderSceneConverter::BuildVertexIndicesImpl(RenderMesh &mesh, uint32_t max_vertex_valence, float dedup_eps) {
  //
  // - If mesh is triangulated, use triangulatedFaceVertexIndices, otherwise use
  // faceVertxIndices.
  // - Make vertex attributes 'facevarying' variability
  // - Assign same id for similar(currently identitical) vertex attribute.
  // - Reorder vertex attributes to 'vertex' variability.
  //

  //LIGHTUSD_LOG_I("BuildVertexIndicesImpl");

  const std::vector<uint32_t> &fvIndices =
      mesh.triangulatedFaceVertexIndices.size()
          ? mesh.triangulatedFaceVertexIndices
          : mesh.usdFaceVertexIndices;

  // Empty mesh (e.g. Blender export with points=[], faceVertexIndices=[]):
  // nothing to build, just succeed.
  if (fvIndices.empty()) {
    return true;
  }

  //std::cout << "triangulatedFaceVertexIndices.max_value: " << *std::max_element(mesh.triangulatedFaceVertexIndices.begin(), mesh.triangulatedFaceVertexIndices.end() << "\n");

  //std::cout << "usdFaceVertexIndices.min_value: " << *std::min_element(mesh.usdFaceVertexIndices.begin(), mesh.usdFaceVertexIndices.end() << "\n");
  //std::cout << "usdFaceVertexIndices.max_value: " << *std::max_element(mesh.usdFaceVertexIndices.begin(), mesh.usdFaceVertexIndices.end() << "\n");

  size_t num_verts = mesh.points.size();
  size_t num_fvs = fvIndices.size();

  // Validate that a vertex attribute is facevarying with the expected count.
#define VALIDATE_FACEVARYING_ATTR(attr, name) \
  if (attr.vertex_count()) { \
    if (!attr.is_facevarying()) { \
      PUSH_ERROR_AND_RETURN( \
          "Internal error. " name " must be 'facevarying' variability."); \
    } \
    if (attr.vertex_count() != num_fvs) { \
      PUSH_ERROR_AND_RETURN( \
          "Internal error. The number of " name " items does not match " \
          "with the number of facevarying items."); \
    } \
  }

  VALIDATE_FACEVARYING_ATTR(mesh.normals, "normals")

  const value::float2 *texcoord0_ptr = nullptr;
  const value::float2 *texcoord1_ptr = nullptr;

  for (const auto &it : mesh.texcoords) {
    if (it.second.vertex_count() > 0) {
      if (!it.second.is_facevarying()) {
        PUSH_ERROR_AND_RETURN(
            "Internal error. texcoords must be 'facevarying' variability.");
      }
      if (it.second.vertex_count() != num_fvs) {
        PUSH_ERROR_AND_RETURN(
            "Internal error. The number of texcoord items does not match "
            "with the number of facevarying items.");
      }

      if (it.first == 0) {
        texcoord0_ptr = reinterpret_cast<const value::float2 *>(
            it.second.get_data().data());
      } else if (it.first == 1) {
        texcoord1_ptr = reinterpret_cast<const value::float2 *>(
            it.second.get_data().data());
      } else {
        // ignore.
      }
    }
  }

  const value::float3 *tangents_ptr = nullptr;
  const value::float3 *binormals_ptr = nullptr;

  if (texcoord0_ptr) {
    VALIDATE_FACEVARYING_ATTR(mesh.tangents, "tangents")
    if (mesh.tangents.vertex_count()) {
      tangents_ptr = reinterpret_cast<const value::float3 *>(
          mesh.tangents.get_data().data());
    }

    VALIDATE_FACEVARYING_ATTR(mesh.binormals, "binormals")
    if (mesh.binormals.vertex_count()) {
      binormals_ptr = reinterpret_cast<const value::float3 *>(
          mesh.binormals.get_data().data());
    }
  }

  VALIDATE_FACEVARYING_ATTR(mesh.vertex_colors, "vertex_colors")
  VALIDATE_FACEVARYING_ATTR(mesh.vertex_opacities, "vertex_opacities")

  const value::float3 *normals_ptr =
      (mesh.normals.vertex_count() > 0)
          ? reinterpret_cast<const value::float3 *>(
                mesh.normals.get_data().data())
          : nullptr;
  const value::float3 *colors_ptr =
      (mesh.vertex_colors.vertex_count() > 0)
          ? reinterpret_cast<const value::float3 *>(
                mesh.vertex_colors.get_data().data())
          : nullptr;
  const float *opacities_ptr =
      (mesh.vertex_opacities.vertex_count() > 0)
          ? reinterpret_cast<const float *>(
                mesh.vertex_opacities.get_data().data())
          : nullptr;

  //
  // Position-bucketed vertex deduplication.
  // Bucket by position index (faceVertexIndices[i]), then linear-scan within
  // each bucket comparing only the attributes that are present.
  // Typical vertex valence is 4-8, so each scan is very short.
  //
  // Safeguard: if any vertex's degree (number of face-vertex references)
  // exceeds max_vertex_valence, skip dedup entirely (flatten).
  //

  std::vector<uint32_t> out_indices(num_fvs);
  std::vector<uint32_t> out_point_indices(num_fvs);
  uint32_t next_vertex_id = 0;

  // Pre-count per-position degree to detect high-valence vertices.
  bool flatten = false;
  if (max_vertex_valence > 0 && num_verts > 0) {
    std::vector<uint32_t> degree(num_verts, 0);
    for (size_t i = 0; i < num_fvs; i++) {
      uint32_t pid = fvIndices[i];
      if (pid < num_verts) {
        degree[pid]++;
      }
    }
    uint32_t max_deg = *std::max_element(degree.begin(), degree.end());
    if (max_deg > max_vertex_valence) {
      DCOUT("Max vertex degree " << max_deg << " exceeds threshold "
            << max_vertex_valence << ", falling back to flatten (no dedup).");
      flatten = true;
    }
  }

  if (flatten) {
    // Flatten: each face-vertex becomes its own unique vertex (no dedup).
    for (size_t i = 0; i < num_fvs; i++) {
      uint32_t pid = fvIndices[i];
      if (pid >= num_verts) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Invalid faceVertexIndex {}. Must be less than {}(triangulated = {})", pid, num_verts, mesh.triangulatedFaceVertexIndices.size() ? "true" : "false"));
      }
      out_indices[i] = uint32_t(i);
      out_point_indices[i] = pid;
    }
    next_vertex_id = uint32_t(num_fvs);
  } else {
    // Normal position-bucketed dedup.
    struct BucketEntry {
      uint32_t fv_index;       // source face-vertex index (for attribute comparison)
      uint32_t out_vertex_id;  // assigned output vertex index
    };

    // When dedup_eps > 0, use is_close() for approximate matching (handles
    // DCC rounding, interpolation artifacts). Otherwise use exact memcmp.
    auto attribs_match = [&](size_t a, size_t b) -> bool {
      if (dedup_eps > 0.0f) {
        if (normals_ptr   && !math::is_close(normals_ptr[a],   normals_ptr[b],   dedup_eps)) return false;
        if (texcoord0_ptr && !math::is_close(texcoord0_ptr[a],  texcoord0_ptr[b], dedup_eps)) return false;
        if (texcoord1_ptr && !math::is_close(texcoord1_ptr[a],  texcoord1_ptr[b], dedup_eps)) return false;
        if (tangents_ptr  && !math::is_close(tangents_ptr[a],   tangents_ptr[b],  dedup_eps)) return false;
        if (binormals_ptr && !math::is_close(binormals_ptr[a],  binormals_ptr[b], dedup_eps)) return false;
        if (colors_ptr    && !math::is_close(colors_ptr[a],     colors_ptr[b],    dedup_eps)) return false;
        if (opacities_ptr && !math::is_close(opacities_ptr[a],  opacities_ptr[b], dedup_eps)) return false;
      } else {
        if (normals_ptr   && memcmp(&normals_ptr[a],   &normals_ptr[b],   sizeof(value::float3)) != 0) return false;
        if (texcoord0_ptr && memcmp(&texcoord0_ptr[a],  &texcoord0_ptr[b], sizeof(value::float2)) != 0) return false;
        if (texcoord1_ptr && memcmp(&texcoord1_ptr[a],  &texcoord1_ptr[b], sizeof(value::float2)) != 0) return false;
        if (tangents_ptr  && memcmp(&tangents_ptr[a],   &tangents_ptr[b],  sizeof(value::float3)) != 0) return false;
        if (binormals_ptr && memcmp(&binormals_ptr[a],  &binormals_ptr[b], sizeof(value::float3)) != 0) return false;
        if (colors_ptr    && memcmp(&colors_ptr[a],     &colors_ptr[b],    sizeof(value::float3)) != 0) return false;
        if (opacities_ptr && memcmp(&opacities_ptr[a],  &opacities_ptr[b], sizeof(float))         != 0) return false;
      }
      return true;
    };

    std::vector<std::vector<BucketEntry>> buckets(num_verts);

    for (size_t i = 0; i < num_fvs; i++) {
      uint32_t pid = fvIndices[i];
      if (pid >= num_verts) {
        PUSH_ERROR("usdFaceVertexIndices.min_value: " << *std::min_element(mesh.usdFaceVertexIndices.begin(), mesh.usdFaceVertexIndices.end()) << "\n");
        PUSH_ERROR("usdFaceVertexIndices.max_value: " << *std::max_element(mesh.usdFaceVertexIndices.begin(), mesh.usdFaceVertexIndices.end()) << "\n");
        PUSH_ERROR("triangulatedFaceVertexIndices.min_value: " << *std::min_element(mesh.triangulatedFaceVertexIndices.begin(), mesh.triangulatedFaceVertexIndices.end()) << "\n");
        PUSH_ERROR("triangulatedFaceVertexIndices.max_value: " << *std::max_element(mesh.triangulatedFaceVertexIndices.begin(), mesh.triangulatedFaceVertexIndices.end()) << "\n");
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Invalid faceVertexIndex {}. Must be less than {}(triangulated = {})", pid, num_verts, mesh.triangulatedFaceVertexIndices.size() ? "true" : "false"));
      }

      auto &bucket = buckets[pid];
      uint32_t matched_id = ~0u;
      for (const auto &entry : bucket) {
        if (attribs_match(i, entry.fv_index)) {
          matched_id = entry.out_vertex_id;
          break;
        }
      }
      if (matched_id == ~0u) {
        matched_id = next_vertex_id++;
        bucket.push_back({uint32_t(i), matched_id});
      }
      out_indices[i] = matched_id;
      out_point_indices[i] = pid;
    }

    // Free dedup buckets — no longer needed after vertex dedup.
    { std::vector<std::vector<BucketEntry>> tmp; buckets.swap(tmp); }
  }

  DCOUT("faceVertexIndices.size : " << fvIndices.size());
  DCOUT("vertex dedup: " << next_vertex_id << " unique vertices from "
        << num_fvs << " face-vertices." << (flatten ? " (flattened)" : ""));

  // Build reordered attribute arrays from dedup results.
  // Each unique vertex is represented by its canonical face-vertex index.
  uint32_t numUniqueVerts = next_vertex_id;
  DefaultVertexOutput<DefaultPackedVertexData> vertex_output;
  vertex_output.point_indices.resize(numUniqueVerts);
  if (normals_ptr)   vertex_output.normals.resize(numUniqueVerts);
  if (texcoord0_ptr) vertex_output.uv0s.resize(numUniqueVerts);
  if (texcoord1_ptr) vertex_output.uv1s.resize(numUniqueVerts);
  if (tangents_ptr)  vertex_output.tangents.resize(numUniqueVerts);
  if (binormals_ptr) vertex_output.binormals.resize(numUniqueVerts);
  if (colors_ptr)    vertex_output.colors.resize(numUniqueVerts);
  if (opacities_ptr) vertex_output.opacities.resize(numUniqueVerts);

  // Populate vertex_output from dedup results.
  // In flatten mode, out_indices[i] == i so this is a direct copy.
  // In dedup mode, each unique vertex appears once via out_indices mapping.
  for (size_t i = 0; i < num_fvs; i++) {
    uint32_t vid = out_indices[i];
    vertex_output.point_indices[vid] = fvIndices[i];
    if (normals_ptr)   vertex_output.normals[vid]   = normals_ptr[i];
    if (texcoord0_ptr) vertex_output.uv0s[vid]      = texcoord0_ptr[i];
    if (texcoord1_ptr) vertex_output.uv1s[vid]      = texcoord1_ptr[i];
    if (tangents_ptr)  vertex_output.tangents[vid]   = tangents_ptr[i];
    if (binormals_ptr) vertex_output.binormals[vid]  = binormals_ptr[i];
    if (colors_ptr)    vertex_output.colors[vid]     = colors_ptr[i];
    if (opacities_ptr) vertex_output.opacities[vid]  = opacities_ptr[i];
  }



  //
  // Reorder 'vertex' varying attributes(points, jointIndices/jointWeights,
  // BlendShape points, ...)
  //
  if (!ReorderVertexVaryingAttributes(mesh, vertex_output.point_indices,
                                      &_err)) {
    return false;
  }

  // Other 'facevarying' attributes are now 'vertex' variability.
  // Free each vertex_output field immediately after set_buffer() copies it,
  // to avoid simultaneous peak overlap of source + destination.
  {
    size_t byte_size;
    if (normals_ptr) {
      if (!safe::mul(vertex_output.normals.size(), sizeof(value::float3),
                     &byte_size)) {
        PUSH_ERROR_AND_RETURN("Normal buffer size overflow.");
      }
      mesh.normals.set_buffer(
          reinterpret_cast<const uint8_t *>(vertex_output.normals.data()),
          byte_size);
      mesh.normals.variability = VertexVariability::Vertex;
      { std::vector<value::float3> tmp; vertex_output.normals.swap(tmp); }
    }

    if (texcoord0_ptr) {
      if (!safe::mul(vertex_output.uv0s.size(), sizeof(value::float2),
                     &byte_size)) {
        PUSH_ERROR_AND_RETURN("Texcoord0 buffer size overflow.");
      }
      mesh.texcoords[0].set_buffer(
          reinterpret_cast<const uint8_t *>(vertex_output.uv0s.data()),
          byte_size);
      mesh.texcoords[0].variability = VertexVariability::Vertex;
      { std::vector<value::float2> tmp; vertex_output.uv0s.swap(tmp); }
    }

    if (texcoord1_ptr) {
      if (!safe::mul(vertex_output.uv1s.size(), sizeof(value::float2),
                     &byte_size)) {
        PUSH_ERROR_AND_RETURN("Texcoord1 buffer size overflow.");
      }
      mesh.texcoords[1].set_buffer(
          reinterpret_cast<const uint8_t *>(vertex_output.uv1s.data()),
          byte_size);
      mesh.texcoords[1].variability = VertexVariability::Vertex;
      { std::vector<value::float2> tmp; vertex_output.uv1s.swap(tmp); }
    }

    if (tangents_ptr) {
      if (!safe::mul(vertex_output.tangents.size(), sizeof(value::float3),
                     &byte_size)) {
        PUSH_ERROR_AND_RETURN("Tangent buffer size overflow.");
      }
      mesh.tangents.set_buffer(
          reinterpret_cast<const uint8_t *>(vertex_output.tangents.data()),
          byte_size);
      mesh.tangents.variability = VertexVariability::Vertex;
      { std::vector<value::float3> tmp; vertex_output.tangents.swap(tmp); }
    }

    if (binormals_ptr) {
      if (!safe::mul(vertex_output.binormals.size(), sizeof(value::float3),
                     &byte_size)) {
        PUSH_ERROR_AND_RETURN("Binormal buffer size overflow.");
      }
      mesh.binormals.set_buffer(
          reinterpret_cast<const uint8_t *>(vertex_output.binormals.data()),
          byte_size);
      mesh.binormals.variability = VertexVariability::Vertex;
      { std::vector<value::float3> tmp; vertex_output.binormals.swap(tmp); }
    }

    if (colors_ptr) {
      if (!safe::mul(vertex_output.colors.size(), sizeof(value::float3),
                     &byte_size)) {
        PUSH_ERROR_AND_RETURN("Color buffer size overflow.");
      }
      mesh.vertex_colors.set_buffer(
          reinterpret_cast<const uint8_t *>(vertex_output.colors.data()),
          byte_size);
      mesh.vertex_colors.variability = VertexVariability::Vertex;
      { std::vector<value::float3> tmp; vertex_output.colors.swap(tmp); }
    }

    if (opacities_ptr) {
      if (!safe::mul(vertex_output.opacities.size(), sizeof(float),
                     &byte_size)) {
        PUSH_ERROR_AND_RETURN("Opacity buffer size overflow.");
      }
      mesh.vertex_opacities.set_buffer(
          reinterpret_cast<const uint8_t *>(vertex_output.opacities.data()),
          byte_size);
      mesh.vertex_opacities.variability = VertexVariability::Vertex;
      { std::vector<float> tmp; vertex_output.opacities.swap(tmp); }
    }
  }

  if (mesh.is_triangulated()) {
    mesh.triangulatedFaceVertexIndices = std::move(out_indices);
  } else {
    mesh.usdFaceVertexIndices = std::move(out_indices);
  }


  return true;
}

bool RenderSceneConverter::BuildVertexIndicesFastImpl(RenderMesh &mesh) {
  //
  // - If mesh is triangulated, use triangulatedFaceVertexIndices, otherwise use
  // faceVertxIndices.
  // - Make vertex attributes 'facevarying' variability
  // - No similarity search.
  // - Reorder vertex attributes to 'vertex' variability.
  //

  //LIGHTUSD_LOG_I("BuildVertexIndicesFastImpl");

  const std::vector<uint32_t> &fvIndices =
      mesh.triangulatedFaceVertexIndices.size()
          ? mesh.triangulatedFaceVertexIndices
          : mesh.usdFaceVertexIndices;

  // Empty mesh (e.g. Blender export with points=[], faceVertexIndices=[]):
  // nothing to build, just succeed.
  if (fvIndices.empty()) {
    return true;
  }

  size_t num_verts = mesh.points.size();
  size_t num_fvs = fvIndices.size();

  VALIDATE_FACEVARYING_ATTR(mesh.normals, "normals")

  for (const auto &it : mesh.texcoords) {
    if (it.second.vertex_count() > 0) {
      if (!it.second.is_facevarying()) {
        PUSH_ERROR_AND_RETURN(
            "Internal error. texcoords must be 'facevarying' variability.");
      }
      if (it.second.vertex_count() != num_fvs) {
        PUSH_ERROR_AND_RETURN(
            "Internal error. The number of texcoord items does not match "
            "with the number of facevarying items.");
      }
    }
  }

  VALIDATE_FACEVARYING_ATTR(mesh.tangents, "tangents")
  VALIDATE_FACEVARYING_ATTR(mesh.binormals, "binormals")
  VALIDATE_FACEVARYING_ATTR(mesh.vertex_colors, "vertex_colors")
  VALIDATE_FACEVARYING_ATTR(mesh.vertex_opacities, "vertex_opacities")

  // range check
  for (size_t i = 0; i < num_fvs; i++) {
    size_t fvi = fvIndices[i];
    if (fvi >= num_verts) {
      PUSH_ERROR("usdFaceVertexIndices.min_value: " << *std::min_element(mesh.usdFaceVertexIndices.begin(), mesh.usdFaceVertexIndices.end()) << "\n");
      PUSH_ERROR("usdFaceVertexIndices.max_value: " << *std::max_element(mesh.usdFaceVertexIndices.begin(), mesh.usdFaceVertexIndices.end()) << "\n");
      PUSH_ERROR("triangulatedFaceVertexIndices.min_value: " << *std::min_element(mesh.triangulatedFaceVertexIndices.begin(), mesh.triangulatedFaceVertexIndices.end()) << "\n");
      PUSH_ERROR("triangulatedFaceVertexIndices.max_value: " << *std::max_element(mesh.triangulatedFaceVertexIndices.begin(), mesh.triangulatedFaceVertexIndices.end()) << "\n");
      PUSH_ERROR_AND_RETURN(fmt::format(
          "Invalid faceVertexIndex {}. Must be less than {}(triangulated = {})", fvi, num_fvs, mesh.triangulatedFaceVertexIndices.size() ? "true" : "faise"));
    }
  }

  //
  // Reorder 'vertex' varying attributes(points, jointIndices/jointWeights,
  // BlendShape points, ...)
  //
  if (!ReorderVertexVaryingAttributes(mesh, fvIndices, &_err)) {
    return false;
  }

  //LIGHTUSD_LOG_I("proc normal");

  // Just change variability
  if (mesh.normals.vertex_count() > 0) {
      mesh.normals.variability = VertexVariability::Vertex;
  }

  for (auto &it : mesh.texcoords) {
    if (it.second.vertex_count() > 0) {
      it.second.variability = VertexVariability::Vertex;
    }
  }

  if (mesh.tangents.vertex_count() > 0) {
      mesh.tangents.variability = VertexVariability::Vertex;
  }

  if (mesh.binormals.vertex_count() > 0) {
      mesh.binormals.variability = VertexVariability::Vertex;
  }

  if (mesh.vertex_colors.vertex_count() > 0) {
      mesh.vertex_colors.variability = VertexVariability::Vertex;
  }

  if (mesh.vertex_opacities.vertex_count() > 0) {
      mesh.vertex_opacities.variability = VertexVariability::Vertex;
  }


  //LIGHTUSD_LOG_I("build indices");

  // TODO: omit indices.
  std::vector<uint32_t> out_indices;
  out_indices.resize(fvIndices.size());
  for (size_t i = 0; i < out_indices.size(); i++) {
    out_indices[i] = uint32_t(i);
  }

  if (mesh.is_triangulated()) {
    mesh.triangulatedFaceVertexIndices = std::move(out_indices);
  } else {
    mesh.usdFaceVertexIndices = std::move(out_indices);
  }

  //LIGHTUSD_LOG_I("done build indices");

  return true;
}

// ---------------------------------------------------------------------------
// Quantize tangents in a RenderMesh from Vec3 float to a packed format.
// Replaces tangent VertexAttribute with packed data and clears binormals.
// Returns true on success (or if no quantization is needed).
// ---------------------------------------------------------------------------

///
/// Try to convert face-varying normals to vertex by quantizing to 10-bit
/// SNORM and comparing packed uint32 values.  This succeeds when normals at
/// a shared vertex differ only by floating-point noise (< ~0.1 degree),
/// as is typical for subdivision surface limit normals.
///
/// On success the VertexAttribute is replaced with vertex-varying float3
/// normals (one per vertex).  Returns false if any vertex has mismatching
/// packed normals across its face-vertices.
///


bool RenderSceneConverter::ConvertMesh(
    const RenderSceneConverterEnv &env, const Path &abs_prim_path,
    const GeomMesh &mesh, const MaterialPath &material_path,
    const std::map<std::string, MaterialPath> &subset_material_path_map,
    // const std::map<std::string, int64_t> &rmaterial_idMap,
    const StringAndIdMap &rmaterial_map,
    const std::vector<const lightusd::GeomSubset *> &material_subsets,
    const std::vector<std::pair<std::string, const lightusd::BlendShape *>>
        &blendshapes,
    RenderMesh *dstMesh) {
  //
  // Steps:
  //
  // 1. Get points, faceVertexIndices and faceVertexOffsets at specified time.
  //   - Validate GeomSubsets
  // 2. Assign Material and list up texcoord primvars
  // 3. convert texcoord, normals, vetexcolor(displaycolors)
  //   - First try to convert it to `vertex` varying(Can be drawn with single
  //   index buffer)
  //   - Otherwise convert to `facevarying` as the last resort.
  // 4. Triangulate indices  when `triangulate` is enabled.
  //   - Triangulate texcoord, normals, vertexcolor.
  // 5. Convert Skin weights
  // 6. Convert BlendShape
  // 7. Build indices(convert 'facevarying' to 'vertrex')
  // 8. Calcualte normals(if not present in the mesh)
  // 9. Build tangent frame(for normal mapping)
  //
  //

  if (!dstMesh) {
    PUSH_ERROR_AND_RETURN("`dst` mesh pointer is nullptr");
  }

  //
  // Thread-safety: when invoked on a TaskArena worker, diagnostics produced
  // by this call are captured into local buffers (via the TLS DiagCapture
  // installed below and the local strings passed to evaluation helpers) and
  // funneled back into the converter-wide _err/_warn when the call returns --
  // on ANY exit path. In serial mode this reproduces immediate-append
  // semantics (content identical; ordering within a single call unchanged).
  //
  std::string local_err, local_warn;

  // RAII funnel: appends captured diagnostics to the converter state no
  // matter how the function exits (success, early return, cancel).
  struct DiagFunnel {
    RenderSceneConverter *self;
    std::string &le;
    std::string &lw;
    DiagCapture *prev{nullptr};
    DiagCapture cap;
    ~DiagFunnel() {
      self->SetDiagCapture(prev);
      if (prev) {
        // Nested inside an outer capture (parallel worker): keep text in the
        // worker's slot buffer -- never touch shared converter strings here.
        if (lw.size() && prev->warn) (*prev->warn) += lw;
        if (le.size() && prev->err) (*prev->err) += le;
        return;
      }
      if (lw.size()) {
        self->_warn += lw;
      }
      if (le.size()) {
        self->_err += le;
      }
    }
  };
  DiagFunnel diag_funnel{this, local_err, local_warn, nullptr, {}};
  diag_funnel.cap.err = &local_err;
  diag_funnel.cap.warn = &local_warn;
  diag_funnel.prev = SetDiagCapture(&diag_funnel.cap);

  RenderMesh dst;

  dst.is_rightHanded =
      (mesh.orientation.get_value() == lightusd::Orientation::RightHanded);
  dst.doubleSided = mesh.doubleSided.get_value();

  //
  // 1. Mandatory attribute: points, faceVertexCounts and faceVertexIndices.
  //
  // TODO: Make error when Mesh's indices is empty?
  //

  // Prim path string for mmap zero-copy lookups
  const std::string prim_path_str = abs_prim_path.prim_part();

  {
    std::vector<value::point3f> points;
    bool got_points = TryReadMMapArray<value::point3f>(
        env.stage, prim_path_str, "points", &points);
    if (!got_points) {
      bool ret = EvaluateTypedAnimatableAttribute(
          env.stage, mesh.points, "points", &points, &local_err, env.timecode,
          value::TimeSampleInterpolationType::Linear);
      if (!ret) {
        return false;
      }
    }

    // V2 safety: deferred array but both mmap and Stage reads produced empty
    if (points.empty() && env.stage.has_mmap_zero_copy()) {
      const MMapArrayRef *ref = env.stage.mmap_table()->find(
          prim_path_str, "points");
      if (ref && ref->element_count > 0) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "mmap deferred 'points' for {} ({} elements) could not be read. "
            "mmap buffer may be invalid.", abs_prim_path, ref->element_count));
      }
    }

    if (points.empty()) {

      // maybe points is explicitly authored, but empty.
      // point3f points = []

      dst.points.clear();
      //PUSH_ERROR_AND_RETURN(
      //    fmt::format("`points` is empty. Prim {}", abs_prim_path));

    } else {
      dst.points.resize(points.size());
      memcpy(dst.points.data(), points.data(),
             sizeof(value::float3) * points.size());

      std::vector<value::point3f>().swap(points);
    }

  }

  {
    std::vector<int32_t> indices;
    bool ret = EvaluateTypedAnimatableAttribute(
        env.stage, mesh.faceVertexIndices, "faceVertexIndices", &indices, &local_err,
        env.timecode, value::TimeSampleInterpolationType::Held);
    if (!ret) {
      return false;
    }

    dst.usdFaceVertexIndices.reserve(indices.size());
    for (size_t i = 0; i < indices.size(); i++) {
      if (indices[i] < 0) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "faceVertexIndices[{}] contains negative index value {}.", i,
            indices[i]));
      }
      if (size_t(indices[i]) >= dst.points.size()) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("faceVertexIndices[{}] {} is out of range. Must be less "
                        "than points.size {}.",
                        i, indices[i], dst.points.size()));
      }
      dst.usdFaceVertexIndices.push_back(uint32_t(indices[i]));
    }
  }

  {
    std::vector<int> counts;
    bool ret = EvaluateTypedAnimatableAttribute(
        env.stage, mesh.faceVertexCounts, "faceVertexCounts", &counts, &local_err,
        env.timecode, value::TimeSampleInterpolationType::Held);
    if (!ret) {
      return false;
    }

    size_t sumCounts = 0;
    dst.usdFaceVertexCounts.clear();
    dst.usdFaceVertexCounts.reserve(counts.size());
    for (size_t i = 0; i < counts.size(); i++) {
      if (counts[i] < 3) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("faceVertexCounts[{}] contains invalid value {}. The "
                        "count value must be >= 3",
                        i, counts[i]));
      }

      if ((sumCounts + size_t(counts[i])) > dst.usdFaceVertexIndices.size()) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "faceVertexCounts[{}] exceeds faceVertexIndices.size {}.", i,
            dst.usdFaceVertexIndices.size()));
      }
      dst.usdFaceVertexCounts.push_back(uint32_t(counts[i]));
      sumCounts += size_t(counts[i]);
    }
  }

  uint32_t src_num_faces = 0;
  if (!SizeToU32(dst.usdFaceVertexCounts.size(), &src_num_faces)) {
    PUSH_ERROR_AND_RETURN("Mesh face count exceeds 32-bit index space.");
  }
  bool subdivision_applied{false};
  // Base-face id per refined face when subdivision is applied (used to
  // remap GeomSubset face indices).
  std::vector<uint32_t> subdiv_face_source;
  int32_t subdivision_level = env.mesh_config.subdivision_level;
  if (auto sit = env.mesh_config.subdivision_prim_levels.find(
          abs_prim_path.full_path_name());
      sit != env.mesh_config.subdivision_prim_levels.end() && sit->second >= 0) {
    subdivision_level = sit->second;
  }
  const bool want_subdivision =
      (subdivision_level > 0) &&
      (mesh.subdivisionScheme.get_value() !=
       GeomMesh::SubdivisionScheme::SubdivisionSchemeNone);

  // Authored normals cannot survive refinement meaningfully; they are
  // recomputed from the subdivided topology.
  if (want_subdivision &&
      (mesh.has_primvar("normals") || mesh.normals.authored())) {
    PUSH_WARN(
        "Subdivision currently ignores authored normals and recomputes "
        "normals from the subdivided topology.");
  }


  //
  // 2. bindMaterial GeoMesh and GeomSubset.
  //
  // Assume Material conversion is done before ConvertMesh.
  // Here we only assign RenderMaterial id and extract GeomSubset::indices
  // information.
  //

  DCOUT("rmaterial_ap.size " << rmaterial_map.size());
  if (auto it = rmaterial_map.find(material_path.material_path);
      it != rmaterial_map.s_end()) {
    dst.material_id = int(it->second);
  } else {
    dst.material_id = material_path.default_material_id;
  }

  if (auto it = rmaterial_map.find(material_path.backface_material_path);
      it != rmaterial_map.s_end()) {
    dst.backface_material_id = int(it->second);
  } else {
    dst.backface_material_id = material_path.default_backface_material_id;
  }

  if (env.mesh_config.validate_geomsubset) {
    size_t elementCount = src_num_faces;

    if (material_subsets.size()) {
      auto family_it =
          mesh.subsetFamilyTypeMap.find(value::token("materialBind"));
      if (family_it != mesh.subsetFamilyTypeMap.end()) {
        const GeomSubset::FamilyType familyType = family_it->second;
        std::string subset_err;
        if (!GeomSubset::ValidateSubsets(material_subsets, elementCount,
                                         familyType, &subset_err)) {
          // Warn but don't fail — many DCC tools (Blender, Maya) export
          // incomplete partitions where only some faces are assigned to
          // subsets.  pxrUSD also warns without failing here.
          // Unassigned faces inherit the parent mesh's material binding.
          PUSH_WARN("GeomSubset validation: " + subset_err);
        }
      }
    }
  }

  for (const auto &psubset : material_subsets) {
    MaterialSubset ms;
    ms.prim_name = psubset->name;
    // ms.prim_index = // TODO
    ms.abs_path = abs_prim_path.prim_part() + std::string("/") + psubset->name;
    ms.display_name = psubset->meta.has_displayName() ? psubset->meta.get_displayName() : "";

    // TODO: Raise error when indices is empty?
    if (psubset->indices.authored()) {
      std::vector<int> indices;  // index to faceVertexCounts
      bool ret = EvaluateTypedAnimatableAttribute(
          env.stage, psubset->indices, "indices", &indices, &local_err, env.timecode,
          value::TimeSampleInterpolationType::Held);
      if (!ret) {
        return false;
      }

      ms.usdIndices = indices;
    }

    if (auto subset_it = subset_material_path_map.find(psubset->name);
        subset_it != subset_material_path_map.end()) {
      const auto &mp = subset_it->second;
      if (auto mat_it = rmaterial_map.find(mp.material_path);
          mat_it != rmaterial_map.s_end()) {
        ms.material_id = int(mat_it->second);
        DCOUT("MaterialSubset " << psubset->name << " : material_id "
                                << ms.material_id);
      } else {
        ms.material_id = mp.default_material_id;
      }
      if (auto backface_it = rmaterial_map.find(mp.backface_material_path);
          backface_it != rmaterial_map.s_end()) {
        ms.backface_material_id = int(backface_it->second);
        DCOUT("MaterialSubset " << psubset->name << " : backface_material_id "
                                << ms.backface_material_id);
      } else {
        ms.backface_material_id = mp.default_backface_material_id;
      }
    }

    // TODO: Ensure prim_name is unique.
    dst.material_subsetMap[ms.prim_name] = ms;
  }

  uint32_t num_vertices = 0;
  uint32_t num_faces = 0;
  uint32_t num_face_vertex_indices = 0;
  if (!SizeToU32(dst.points.size(), &num_vertices) ||
      !SizeToU32(dst.usdFaceVertexCounts.size(), &num_faces) ||
      !SizeToU32(dst.usdFaceVertexIndices.size(), &num_face_vertex_indices)) {
    PUSH_ERROR_AND_RETURN("Mesh counts exceed 32-bit index space.");
  }

  //
  // List up texcoords in this mesh.
  // - If no material assigned to this mesh, look into
  // `default_texcoords_primvar_name`
  // - If materials are assigned, find all corresponding UV primvars in this
  // mesh.
  //

  // key:slotId, value:texcoord data
  lightusd::HashMap<uint32_t, VertexAttribute> uvAttrs;

  // UDIM atlas UV remaps for this mesh, keyed by texcoord primvar name.
  // {scale.x, scale.y, offset.x, offset.y}. Populated from combined-UDIM
  // textures bound to this mesh's material(s); consumed below to rebake UVs.
  std::map<std::string, std::array<float, 4>> mesh_udim_remaps;

  // We need Material info to get corresponding primvar name.
  if (rmaterial_map.empty()) {
    // No material assigned to the Mesh, but we may still want texcoords solely(
    // assign material after the conversion)
    // So find a primvar whose name matches default texcoord name.
    if (mesh.has_primvar(env.mesh_config.default_texcoords_primvar_name)) {
      DCOUT("uv primvar  with default_texcoords_primvar_name found.");
      auto ret = GetTextureCoordinate(
          env.stage, mesh, env.mesh_config.default_texcoords_primvar_name,
          env.timecode, env.tinterp, prim_path_str, &local_warn);
      if (ret) {
        //LIGHTUSD_LOG_I("uv attr");

        // Use slotId 0 - use move to avoid copy
        uvAttrs[0] = std::move(ret.value());
      } else {
        PUSH_WARN("Failed to get texture coordinate for `"
                  << env.mesh_config.default_texcoords_primvar_name
                  << "` : " << ret.error());
      }
    }
  } else {
    for (auto mit = rmaterial_map.i_begin(); mit != rmaterial_map.i_end();
         mit++) {
      int64_t rmaterial_id = int64_t(mit->first);

      if ((rmaterial_id > -1) && (size_t(rmaterial_id) < materials.size())) {
        const RenderMaterial &material = materials[size_t(rmaterial_id)];

        // Cache ListUVNames per material_id to avoid redundant shader walks.
        // Guarded: multiple workers may convert meshes sharing a material.
        // Element references stay valid across concurrent inserts, so the
        // map is only accessed (find/emplace/read) under the lock; the
        // potentially slow UV extraction below runs unlocked.
        const StringAndIdMap *uvname_map{nullptr};
        {
          std::lock_guard<std::mutex> cache_lk(_cache_mu);
          auto uv_cache_it = _uvNameCache.find(rmaterial_id);
          if (uv_cache_it == _uvNameCache.end()) {
            StringAndIdMap tmp;
            std::map<std::string, UDIMUVRemap> remaps;
            if (!ListUVNames(material, textures, tmp, &remaps)) {
              DCOUT("Failed to list UV names");
              return false;
            }
            uv_cache_it =
                _uvNameCache.emplace(rmaterial_id, std::move(tmp)).first;

            std::map<std::string, std::array<float, 4>> packed;
            for (const auto &kv : remaps) {
              packed[kv.first] = {kv.second.scale[0], kv.second.scale[1],
                                  kv.second.offset[0], kv.second.offset[1]};
            }
            _udimRemapCache.emplace(rmaterial_id, std::move(packed));
          }
          uvname_map = &uv_cache_it->second;

          // Accumulate UDIM remaps bound to this mesh's material(s).
          const auto udim_cache_it = _udimRemapCache.find(rmaterial_id);
          if (udim_cache_it != _udimRemapCache.end()) {
            for (const auto &kv : udim_cache_it->second) {
              mesh_udim_remaps[kv.first] = kv.second;
            }
          }
        }

        for (auto it = uvname_map->i_begin(); it != uvname_map->i_end(); it++) {
          uint64_t slotId = it->first;
          std::string uvname = it->second;

          if (uvAttrs.find(uint32_t(slotId)) == uvAttrs.end()) {
            // FIXME: Use GetGeomPrimvar() & ToVertexAttribute()
            auto ret = GetTextureCoordinate(env.stage, mesh, uvname,
                                            env.timecode, env.tinterp,
                                            prim_path_str, &local_warn);
            if (ret) {
              VertexAttribute &vattr = ret.value();
              // UV data is optional render metadata.  A malformed primvar
              // must not make otherwise valid geometry unusable for physics
              // conversion or offscreen rendering; skip only this UV slot.
              bool valid = true;

              if (vattr.is_vertex()) {
                if (vattr.vertex_count() != num_vertices) {
                  PUSH_WARN(fmt::format("Array length of texture coordinate `{}`(Prim path {}) must be {}, but got {}; skipping UV attribute", uvname, abs_prim_path.prim_part(), num_vertices, vattr.vertex_count()));
                  valid = false;
                }
              } else if (vattr.is_constant()) {
                if (vattr.vertex_count() != 1) {
                  PUSH_WARN(fmt::format("Array length of texture coordinate `{}`(Prim path {}) must be {}, but got {}; skipping UV attribute", uvname, abs_prim_path.prim_part(), 1, vattr.vertex_count()));
                  valid = false;
                }
              } else if (vattr.is_uniform()) {
                if (vattr.vertex_count() != num_faces) {
                  PUSH_WARN(fmt::format("Array length of texture coordinate `{}`(Prim path {}) must be {}, but got {}; skipping UV attribute", uvname, abs_prim_path.prim_part(), num_faces, vattr.vertex_count()));
                  valid = false;
                }
              } else if (vattr.is_facevarying()) {
                if (vattr.vertex_count() != num_face_vertex_indices) {
                  PUSH_WARN(fmt::format("Array length of texture coordinate `{}`(Prim path {}) must be {}, but got {}; skipping UV attribute", uvname, abs_prim_path.prim_part(), num_face_vertex_indices, vattr.vertex_count()));
                  valid = false;
                }
              } else {
                PUSH_WARN("Internal error. Unknown variability of texcoord attribute; skipping UV attribute.");
                valid = false;
              }

              if (valid) {
                // Use move to avoid copy
                uvAttrs.emplace(uint32_t(slotId), std::move(vattr));
              }
            } else {
              PUSH_WARN("Failed to get texture coordinate for `"
                        << uvname << "` : " << ret.error());
            }
          }
        }
      }
    }

    // Fallback: If no UV names were found via shader connections (e.g.
    // MaterialX materials without explicit texture nodes), try the default
    // texcoord primvar (usually "st") — similar to OpenUSD's implicit
    // defaultgeomprop="UV0" -> primvars:st mapping.
    if (uvAttrs.empty() &&
        mesh.has_primvar(env.mesh_config.default_texcoords_primvar_name)) {
      DCOUT("No UV names from material shader connections. "
            "Falling back to default texcoord primvar `"
            << env.mesh_config.default_texcoords_primvar_name << "`.");
      auto ret = GetTextureCoordinate(
          env.stage, mesh, env.mesh_config.default_texcoords_primvar_name,
          env.timecode, env.tinterp, prim_path_str, &local_warn);
      if (ret) {
        uvAttrs[0] = std::move(ret.value());
      } else {
        PUSH_WARN("Failed to get default texture coordinate `"
                  << env.mesh_config.default_texcoords_primvar_name
                  << "` : " << ret.error());
      }
    }
  }

  // Optionally expose EVERY authored texCoord2f[] primvar as a UV slot (secondary
  // UV sets that no shader references), for multi-UV tools. Off by default so the
  // standard slot assignment is untouched.
  if (env.mesh_config.extract_all_texcoords) {
    std::set<std::string> have_uv_names;
    uint32_t next_uv_slot = 0;
    for (const auto &kv : uvAttrs) {
      have_uv_names.insert(kv.second.name);
      next_uv_slot = (std::max)(next_uv_slot, kv.first + 1u);
    }
    for (const GeomPrimvar &pv : mesh.get_primvars()) {
      if (pv.type_name() != "texCoord2f[]") continue;  // strict UV role only
      const std::string pv_name = pv.name();
      if (have_uv_names.count(pv_name)) continue;
      auto ret = GetTextureCoordinate(env.stage, mesh, pv_name, env.timecode,
                                      env.tinterp, prim_path_str, &local_warn);
      if (ret) {
        have_uv_names.insert(pv_name);
        uvAttrs.emplace(next_uv_slot++, std::move(ret.value()));
      }
    }
  }

  //
  // Gather displayColor/displayOpacity primvars (validated against the base
  // topology) so they can refine through subdivision together with the
  // geometry and UVs. Without subdivision they convert exactly as before.
  //
  constexpr auto kDisplayColor = "displayColor";
  constexpr auto kDisplayOpacity = "displayOpacity";
  VertexAttribute vcolor;
  VertexAttribute vopacity;
  if (mesh.has_primvar(kDisplayColor)) {
    GeomPrimvar pvar;
    if (!GetGeomPrimvar(env.stage, &mesh, kDisplayColor, &pvar, &local_err,
                        &local_warn)) {
      return false;
    }
    std::string warn_msg;
    if (!ToVertexAttribute(pvar, kDisplayColor, num_vertices, num_faces,
                           num_face_vertex_indices, vcolor, &local_err,
                           env.timecode, env.tinterp, &warn_msg)) {
      return false;
    }
    if (!warn_msg.empty()) {
      local_warn += warn_msg;
    }
  }
  if (mesh.has_primvar(kDisplayOpacity)) {
    GeomPrimvar pvar;
    if (!GetGeomPrimvar(env.stage, &mesh, kDisplayOpacity, &pvar, &local_err,
                        &local_warn)) {
      return false;
    }
    std::string warn_msg;
    if (!ToVertexAttribute(pvar, kDisplayOpacity, num_vertices, num_faces,
                           num_face_vertex_indices, vopacity, &local_err,
                           env.timecode, env.tinterp, &warn_msg)) {
      return false;
    }
    if (!warn_msg.empty()) {
      local_warn += warn_msg;
    }
  }

  //
  // Gather tangent/binormal primvars (validated against the base topology)
  // so they refine through subdivision with the geometry.
  //
  if (mesh.has_primvar(env.mesh_config.default_tangents_primvar_name)) {
    GeomPrimvar pvar;

    if (!GetGeomPrimvar(env.stage, &mesh,
                        env.mesh_config.default_tangents_primvar_name, &pvar,
                        &local_err, &_warn)) {
      return false;
    }

    std::string warn_msg;
    if (!ToVertexAttribute(pvar, env.mesh_config.default_tangents_primvar_name,
                           num_vertices, num_faces, num_face_vertex_indices,
                           dst.tangents, &local_err, env.timecode, env.tinterp,
                           &warn_msg)) {
      return false;
    }
    if (!warn_msg.empty()) {
      local_warn += warn_msg;
    }
  }

  if (mesh.has_primvar(env.mesh_config.default_binormals_primvar_name)) {
    GeomPrimvar pvar;

    if (!GetGeomPrimvar(env.stage, &mesh,
                        env.mesh_config.default_binormals_primvar_name, &pvar,
                        &local_err, &_warn)) {
      return false;
    }

    std::string warn_msg;
    if (!ToVertexAttribute(pvar, env.mesh_config.default_binormals_primvar_name,
                           num_vertices, num_faces, num_face_vertex_indices,
                           dst.binormals, &local_err, env.timecode, env.tinterp,
                           &warn_msg)) {
      return false;
    }
    if (!warn_msg.empty()) {
      local_warn += warn_msg;
    }
  }

  // Refined skin weights / blendshape offsets produced by the subdivision
  // block below and consumed by the skinning / BlendShape conversion steps.
  std::vector<int> subdiv_skin_joint_indices;
  std::vector<float> subdiv_skin_joint_weights;
  bool subdiv_skin_refined = false;
  // Per BlendShape: compacted refined point indices, primary xyz offsets and
  // optional in-between xyz offsets keyed by their authored weight.
  struct RefinedBlendShapeOffsets {
    std::vector<uint32_t> indices;
    std::vector<float> point_offsets;
    std::map<float, std::vector<float>> inbetween_offsets;
  };
  std::map<const BlendShape *, RefinedBlendShapeOffsets> subdiv_bs_offsets;

  //
  // Apply subdivision: geometry plus the gathered UV, display, tangent,
  // skin-weight and blendshape-offset primvars, which refine in lockstep
  // through tinysubdiv (faceVarying channels through the seam-split fvar
  // path, vertex/varying as per-point primvars, uniform via face_source
  // replication, constant untouched).
  //
  if (want_subdivision) {
    size_t cp_byte_size;
    if (!safe::mul(dst.points.size(), sizeof(value::float3), &cp_byte_size)) {
      PUSH_ERROR_AND_RETURN("Control point buffer size overflow.");
    }
    std::vector<float> control_points(dst.points.size() * 3);
    memcpy(control_points.data(), dst.points.data(), cp_byte_size);

    // Float-typed attributes that refine with the geometry. Direction-like
    // attributes (tangents/binormals) are renormalized after refinement.
    std::vector<VertexAttribute *> subdiv_attrs;
    std::vector<uint8_t> subdiv_attr_normalize;
    for (auto &uv : uvAttrs) {
      subdiv_attrs.push_back(&uv.second);
      subdiv_attr_normalize.push_back(0);
    }
    if (!vcolor.get_data().empty()) {
      subdiv_attrs.push_back(&vcolor);
      subdiv_attr_normalize.push_back(0);
    }
    if (!vopacity.get_data().empty()) {
      subdiv_attrs.push_back(&vopacity);
      subdiv_attr_normalize.push_back(0);
    }
    if (!dst.tangents.get_data().empty()) {
      subdiv_attrs.push_back(&dst.tangents);
      subdiv_attr_normalize.push_back(1);
    }
    if (!dst.binormals.get_data().empty()) {
      subdiv_attrs.push_back(&dst.binormals);
      subdiv_attr_normalize.push_back(1);
    }

    // Floats per item, for the float32 formats tinysubdiv refines.
    auto attr_components = [](const VertexAttribute &a) -> uint32_t {
      switch (a.format) {
        case VertexAttributeFormat::Float:
          return 1;
        case VertexAttributeFormat::Vec2:
          return 2;
        case VertexAttributeFormat::Vec3:
          return 3;
        case VertexAttributeFormat::Vec4:
          return 4;
        default:
          return 0;
      }
    };

    std::vector<tsd::FVarChannelView> fvar_channels;
    std::vector<VertexAttribute *> fvar_attrs;
    std::vector<uint8_t> fvar_normalize;
    std::vector<tsd::VertexPrimvarView> vertex_primvars;
    std::vector<VertexAttribute *> vertex_attrs;
    std::vector<uint8_t> vertex_normalize;
    std::vector<VertexAttribute *> uniform_attrs;

    for (size_t ai = 0; ai < subdiv_attrs.size(); ai++) {
      VertexAttribute *vattr = subdiv_attrs[ai];
      const uint32_t components = attr_components(*vattr);
      if (components == 0 || vattr->element_size() != 1 ||
          !vattr->indices.empty()) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Subdivision requires flattened float1/2/3/4 primvars with "
            "elementSize 1 (primvar `{}`).",
            vattr->name));
      }
      if (vattr->variability == VertexVariability::FaceVarying) {
        tsd::FVarChannelView ch;
        ch.values = reinterpret_cast<const float *>(vattr->get_data().data());
        if (!SizeToU32(vattr->vertex_count(), &ch.num_values)) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Subdivision primvar `{}` exceeds 32-bit value count.", vattr->name));
        }
        ch.indices = nullptr;  // flattened; seams recovered by value equality
        ch.stride = components;
        fvar_channels.push_back(ch);
        fvar_attrs.push_back(vattr);
        fvar_normalize.push_back(subdiv_attr_normalize[ai]);
      } else if (vattr->variability == VertexVariability::Vertex ||
                 vattr->variability == VertexVariability::Varying) {
        tsd::VertexPrimvarView pv;
        pv.values = reinterpret_cast<const float *>(vattr->get_data().data());
        pv.stride = components;
        pv.varying = (vattr->variability == VertexVariability::Varying);
        vertex_primvars.push_back(pv);
        vertex_attrs.push_back(vattr);
        vertex_normalize.push_back(subdiv_attr_normalize[ai]);
      } else if (vattr->variability == VertexVariability::Uniform) {
        uniform_attrs.push_back(vattr);
      }
      // Constant: unaffected by refinement.
    }

    //
    // Skin weights: expand the sparse per-vertex (jointIndex, weight) pairs
    // into dense per-joint columns over the joints actually used, refine
    // them like any other per-point primvar, then re-sparsify back to the
    // authored influence count below.
    //
    const bool has_skin = mesh.has_primvar("skel:jointIndices") &&
                          mesh.has_primvar("skel:jointWeights");
    std::vector<int> skin_used_joints;          // dense column -> joint id
    std::vector<std::vector<float>> skin_chan;  // per-channel column data
    uint32_t skin_K = 0;
    const size_t skin_channel_begin = vertex_primvars.size();
    if (has_skin) {
      GeomPrimvar ji;
      GeomPrimvar jw;
      if (!GetGeomPrimvar(env.stage, &mesh, "skel:jointIndices", &ji, &local_err) ||
          !GetGeomPrimvar(env.stage, &mesh, "skel:jointWeights", &jw, &local_err)) {
        return false;
      }
      if (!ji.has_interpolation() || !jw.has_interpolation()) {
        PUSH_ERROR_AND_RETURN(
            "`skel:jointIndices`/`skel:jointWeights` primvars must author "
            "`interpolation` metadata.");
      }
      if ((ji.get_interpolation() != Interpolation::Vertex) &&
          (ji.get_interpolation() != Interpolation::Varying)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "`skel:jointIndices` primvar must use `vertex` or `varying` "
            "interpolation before subdivision, but got `{}`.",
            to_string(ji.get_interpolation())));
      }
      if ((jw.get_interpolation() != Interpolation::Vertex) &&
          (jw.get_interpolation() != Interpolation::Varying)) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "`skel:jointWeights` primvar must use `vertex` or `varying` "
            "interpolation before subdivision, but got `{}`.",
            to_string(jw.get_interpolation())));
      }
      skin_K = ji.get_elementSize();
      if (skin_K == 0 || skin_K != jw.get_elementSize()) {
        PUSH_ERROR_AND_RETURN(
            "`elementSize` of skel:jointIndices/skel:jointWeights must be "
            "equal and non-zero.");
      }
      if (skin_K > env.mesh_config.max_skin_elementSize) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "`elementSize` {} of skel:jointIndices/skel:jointWeights too "
            "large for subdivision. Max allowed is {}.",
            skin_K, env.mesh_config.max_skin_elementSize));
      }
      std::vector<int> jidx;
      std::vector<float> jwts;
      if (!ji.flatten_with_indices(env.timecode, &jidx, env.tinterp) ||
          !jw.flatten_with_indices(env.timecode, &jwts, env.tinterp)) {
        PUSH_ERROR_AND_RETURN(
            "Failed to flatten skel:jointIndices/skel:jointWeights.");
      }
      size_t expected_skin_count = 0;
      if (!safe::mul(size_t(num_vertices), size_t(skin_K),
                     &expected_skin_count)) {
        PUSH_ERROR_AND_RETURN("skel:jointIndices/skel:jointWeights size overflow.");
      }
      if (jidx.size() != jwts.size() || jidx.size() != expected_skin_count) {
        PUSH_ERROR_AND_RETURN(
            "skel:jointIndices/skel:jointWeights must hold elementSize "
            "entries per vertex.");
      }

      constexpr size_t kMaxSubdivSkinJoints = 256;
      lightusd::HashMap<int, uint32_t> joint_column;
      joint_column.reserve(std::min<size_t>(jidx.size(), kMaxSubdivSkinJoints));
      for (size_t i = 0; i < jidx.size(); i++) {
        if (jwts[i] == 0.0f) {
          continue;
        }
        if (jidx[i] < 0) {
          PUSH_ERROR_AND_RETURN("Negative index in skel:jointIndices.");
        }
        const auto inserted =
            joint_column.emplace(jidx[i], uint32_t(skin_used_joints.size()));
        if (inserted.second) {
          if (skin_used_joints.size() >= kMaxSubdivSkinJoints) {
            PUSH_ERROR_AND_RETURN(fmt::format(
                "Subdivision of skinned meshes supports up to {} distinct "
                "influencing joints.",
                kMaxSubdivSkinJoints));
          }
          skin_used_joints.push_back(jidx[i]);
        }
      }
      // Bound the dense expansion (memory: refined_points * used_joints).
      if (skin_used_joints.size() > kMaxSubdivSkinJoints) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "Subdivision of skinned meshes supports up to {} distinct "
            "influencing joints, but the mesh uses {}.",
            kMaxSubdivSkinJoints, skin_used_joints.size()));
      }

      const size_t J = skin_used_joints.size();
      const bool skin_varying =
          (jw.get_interpolation() == Interpolation::Varying);
      // Pack columns into stride-4 channels (the dense matrix is row-major
      // per point, so each channel gets its own contiguous buffer).
      const size_t num_chan = (J + 3) / 4;
      skin_chan.resize(num_chan);
      for (size_t c = 0; c < num_chan; c++) {
        const uint32_t stride = uint32_t(std::min<size_t>(4, J - c * 4));
        size_t channel_count = 0;
        if (!safe::mul(size_t(num_vertices), size_t(stride), &channel_count)) {
          PUSH_ERROR_AND_RETURN("Subdivision skin channel size overflow.");
        }
        skin_chan[c].assign(channel_count, 0.0f);
      }
      for (uint32_t v = 0; v < num_vertices; v++) {
        for (uint32_t k = 0; k < skin_K; k++) {
          const float w = jwts[size_t(v) * skin_K + k];
          if (w == 0.0f) {
            continue;
          }
          const auto col_it = joint_column.find(jidx[size_t(v) * skin_K + k]);
          if (col_it == joint_column.end()) {
            PUSH_ERROR_AND_RETURN("Internal error: missing subdivision skin joint column.");
          }
          const uint32_t col = col_it->second;
          const size_t c = col / 4;
          const uint32_t stride = uint32_t(std::min<size_t>(4, J - c * 4));
          skin_chan[c][size_t(v) * stride + (col % 4)] += w;
        }
      }
      for (size_t c = 0; c < num_chan; c++) {
        tsd::VertexPrimvarView pv;
        pv.values = skin_chan[c].data();
        pv.stride = uint32_t(std::min<size_t>(4, J - c * 4));
        pv.varying = skin_varying;
        vertex_primvars.push_back(pv);
      }
    }

    //
    // BlendShape point offsets: subdivision is linear in the point data, so
    // refining the (densified) offset field with the same smooth weights
    // yields exactly the offsets of the refined blended mesh.
    //
    std::vector<const BlendShape *> bs_refined;
    std::vector<std::vector<float>> bs_dense;
    const size_t bs_channel_begin = vertex_primvars.size();
    struct RefinedBlendShapeInbetween {
      const BlendShape *blendshape{nullptr};
      float weight{0.0f};
    };
    std::vector<RefinedBlendShapeInbetween> bs_ib_refined;
    std::vector<std::vector<float>> bs_ib_dense;
    for (const auto &bs_it : blendshapes) {
      const BlendShape *bs = bs_it.second;
      if (!bs) {
        continue;
      }
      std::vector<int> bs_indices;
      std::vector<value::vector3f> bs_point_offsets;
      std::vector<value::vector3f> bs_normal_offsets;
      bs->pointIndices.get_value(&bs_indices);
      bs->offsets.get_value(&bs_point_offsets);
      bs->normalOffsets.get_value(&bs_normal_offsets);
      if (bs_indices.empty() || bs_point_offsets.size() != bs_indices.size()) {
        continue;  // the BlendShape conversion step warns and skips these
      }
      if (!bs_normal_offsets.empty()) {
        PUSH_WARN(fmt::format(
            "BlendShape `{}` normalOffsets are ignored under subdivision "
            "(normals are recomputed from the refined topology).",
            bs->name));
      }
      std::vector<float> dense(size_t(num_vertices) * 3, 0.0f);
      for (size_t i = 0; i < bs_indices.size(); i++) {
        if (bs_indices[i] < 0 || uint32_t(bs_indices[i]) >= num_vertices) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "pointIndices[{}] {} in BlendShape `{}` is out of range.", i,
              bs_indices[i], bs->name));
        }
        dense[size_t(bs_indices[i]) * 3 + 0] = bs_point_offsets[i][0];
        dense[size_t(bs_indices[i]) * 3 + 1] = bs_point_offsets[i][1];
        dense[size_t(bs_indices[i]) * 3 + 2] = bs_point_offsets[i][2];
      }
      bs_dense.push_back(std::move(dense));
      bs_refined.push_back(bs);

      for (const auto &pkv : bs->props) {
        if (pkv.first.rfind("inbetweens:", 0) != 0) {
          continue;
        }
        const Property &p = pkv.second;
        if (!p.is_attribute()) {
          continue;
        }
        const Attribute &a = p.get_attribute();
        if (!a.metas().has_weight()) {
          PUSH_WARN(fmt::format(
              "In-between BlendShape `{}` has no `weight` metadata. Skipping.",
              pkv.first));
          continue;
        }
        std::vector<value::vector3f> ib_offsets;
        if (!a.get_value(&ib_offsets)) {
          continue;
        }
        if (ib_offsets.size() != bs_indices.size()) {
          PUSH_WARN(fmt::format(
              "In-between BlendShape `{}` offset count {} differs from "
              "pointIndices count {}. Skipping.",
              pkv.first, ib_offsets.size(), bs_indices.size()));
          continue;
        }

        std::vector<float> ib_dense(size_t(num_vertices) * 3, 0.0f);
        for (size_t i = 0; i < bs_indices.size(); i++) {
          ib_dense[size_t(bs_indices[i]) * 3 + 0] = ib_offsets[i][0];
          ib_dense[size_t(bs_indices[i]) * 3 + 1] = ib_offsets[i][1];
          ib_dense[size_t(bs_indices[i]) * 3 + 2] = ib_offsets[i][2];
        }
        bs_ib_refined.push_back(
            RefinedBlendShapeInbetween{bs, float(a.metas().get_weight())});
        bs_ib_dense.push_back(std::move(ib_dense));
      }
    }
    for (const std::vector<float> &dense : bs_dense) {
      tsd::VertexPrimvarView pv;
      pv.values = dense.data();
      pv.stride = 3;
      pv.varying = false;  // offsets follow the surface (smooth weights)
      vertex_primvars.push_back(pv);
    }
    const size_t bs_ib_channel_begin = vertex_primvars.size();
    for (const std::vector<float> &dense : bs_ib_dense) {
      tsd::VertexPrimvarView pv;
      pv.values = dense.data();
      pv.stride = 3;
      pv.varying = false;
      vertex_primvars.push_back(pv);
    }

    tsd::RefinedMesh refined;
    std::string subdiv_err;
    uint32_t subdiv_fvar_count = 0;
    uint32_t subdiv_vertex_pv_count = 0;
    if (!SizeToU32(fvar_channels.size(), &subdiv_fvar_count) ||
        !SizeToU32(vertex_primvars.size(), &subdiv_vertex_pv_count)) {
      PUSH_ERROR_AND_RETURN("Subdivision primvar channel count exceeds 32-bit index space.");
    }
    if (!tsd::RefineGeomMesh(
            mesh, subdivision_level, control_points,
            dst.usdFaceVertexCounts, dst.usdFaceVertexIndices,
            fvar_channels.empty() ? nullptr : fvar_channels.data(),
            subdiv_fvar_count,
            vertex_primvars.empty() ? nullptr : vertex_primvars.data(),
            subdiv_vertex_pv_count, &refined, &subdiv_err)) {
      PUSH_ERROR_AND_RETURN("Subdivision failed: " + subdiv_err);
    }

    dst.points.resize(refined.points.size() / 3);
    {
      size_t rp_byte_size;
      if (!safe::mul(refined.points.size(), sizeof(float), &rp_byte_size)) {
        PUSH_ERROR_AND_RETURN("Refined points buffer size overflow.");
      }
      memcpy(dst.points.data(), refined.points.data(), rp_byte_size);
    }
    dst.usdFaceVertexCounts = std::move(refined.face_vertex_counts);
    dst.usdFaceVertexIndices = std::move(refined.face_vertex_indices);
    subdiv_face_source = std::move(refined.face_source);

    // Renormalizes direction-like refined tuples (first 3 components).
    auto renormalize3 = [](std::vector<float> &vals, uint32_t stride) {
      if (stride < 3) {
        return;
      }
      for (size_t i = 0; i + stride <= vals.size(); i += stride) {
        const float len =
            std::sqrt(vals[i] * vals[i] + vals[i + 1] * vals[i + 1] +
                      vals[i + 2] * vals[i + 2]);
        if (len > 0.0f) {
          vals[i] /= len;
          vals[i + 1] /= len;
          vals[i + 2] /= len;
        }
      }
    };

    // Write refined values back (faceVarying: per refined corner;
    // vertex/varying: per refined point).
    for (size_t i = 0; i < fvar_attrs.size(); i++) {
      std::vector<float> vals = std::move(refined.fvar[i]);
      if (fvar_normalize[i]) {
        renormalize3(vals, attr_components(*fvar_attrs[i]));
      }
      size_t bytes = 0;
      if (!safe::mul(vals.size(), sizeof(float), &bytes)) {
        PUSH_ERROR_AND_RETURN("Subdivision faceVarying primvar byte size overflow.");
      }
      fvar_attrs[i]->get_data().resize(bytes);
      memcpy(fvar_attrs[i]->get_data().data(), vals.data(),
             bytes);
    }
    for (size_t i = 0; i < vertex_attrs.size(); i++) {
      std::vector<float> vals = std::move(refined.vertex_primvars[i]);
      if (vertex_normalize[i]) {
        renormalize3(vals, attr_components(*vertex_attrs[i]));
      }
      size_t bytes = 0;
      if (!safe::mul(vals.size(), sizeof(float), &bytes)) {
        PUSH_ERROR_AND_RETURN("Subdivision vertex primvar byte size overflow.");
      }
      vertex_attrs[i]->get_data().resize(bytes);
      memcpy(vertex_attrs[i]->get_data().data(), vals.data(),
             bytes);
    }

    const size_t refined_points = dst.points.size();

    // Re-sparsify refined skin weights: per refined point, clamp negative
    // (smooth) contributions, keep the strongest K influences, renormalize.
    if (has_skin) {
      const size_t J = skin_used_joints.size();
      size_t skin_out_count = 0;
      if (!safe::mul(refined_points, size_t(skin_K), &skin_out_count)) {
        PUSH_ERROR_AND_RETURN("Refined skin output size overflow.");
      }
      subdiv_skin_joint_indices.assign(skin_out_count, 0);
      subdiv_skin_joint_weights.assign(skin_out_count, 0.0f);
      std::vector<float> top_w(skin_K, 0.0f);
      std::vector<uint32_t> top_j(skin_K, 0);
      for (size_t v = 0; v < refined_points && J > 0; v++) {
        // Top-K selection over the dense per-joint weights.
        const uint32_t K = skin_K;
        uint32_t used = 0;
        for (uint32_t col = 0; col < J; col++) {
          const size_t c = col / 4;
          const uint32_t stride = uint32_t(std::min<size_t>(4, J - c * 4));
          float w = refined.vertex_primvars[skin_channel_begin + c]
                           [v * stride + (col % 4)];
          if (w <= 0.0f) {
            continue;  // clamp slight negative smooth overshoot
          }
          // Insertion into the descending top-K list.
          uint32_t pos = used < K ? used : K;
          while (pos > 0 && top_w[pos - 1] < w) {
            if (pos < K) {
              top_w[pos] = top_w[pos - 1];
              top_j[pos] = top_j[pos - 1];
            }
            pos--;
          }
          if (pos < K) {
            top_w[pos] = w;
            top_j[pos] = col;
            if (used < K) {
              used++;
            }
          }
        }
        float sum = 0.0f;
        for (uint32_t k = 0; k < used; k++) {
          sum += top_w[k];
        }
        if (sum > 0.0f) {
          const size_t dst_base = v * size_t(skin_K);
          for (uint32_t k = 0; k < used; k++) {
            subdiv_skin_joint_indices[dst_base + k] =
                skin_used_joints[top_j[k]];
            subdiv_skin_joint_weights[dst_base + k] = top_w[k] / sum;
          }
        }
      }
      subdiv_skin_refined = true;
    }

    // Compact refined blendshape offsets. Include points touched by either the
    // primary shape or any in-between so all compacted offset arrays remain
    // parallel.
    for (size_t i = 0; i < bs_refined.size(); i++) {
      const BlendShape *bs = bs_refined[i];
      const std::vector<float> &vals =
          refined.vertex_primvars[bs_channel_begin + i];
      std::vector<uint32_t> indices;
      std::vector<float> offsets;
      std::vector<size_t> ib_channels;
      for (size_t ib_i = 0; ib_i < bs_ib_refined.size(); ib_i++) {
        if (bs_ib_refined[ib_i].blendshape == bs) {
          ib_channels.push_back(ib_i);
        }
      }
      std::map<float, std::vector<float>> inbetween_offsets;
      for (size_t v = 0; v < refined_points; v++) {
        const float x = vals[3 * v];
        const float y = vals[3 * v + 1];
        const float z = vals[3 * v + 2];
        bool keep = (x != 0.0f || y != 0.0f || z != 0.0f);
        for (size_t ib_i : ib_channels) {
          const std::vector<float> &ib_vals =
              refined.vertex_primvars[bs_ib_channel_begin + ib_i];
          if (ib_vals[3 * v] != 0.0f || ib_vals[3 * v + 1] != 0.0f ||
              ib_vals[3 * v + 2] != 0.0f) {
            keep = true;
            break;
          }
        }
        if (keep) {
          indices.push_back(uint32_t(v));
          offsets.push_back(x);
          offsets.push_back(y);
          offsets.push_back(z);
          for (size_t ib_i : ib_channels) {
            const std::vector<float> &ib_vals =
                refined.vertex_primvars[bs_ib_channel_begin + ib_i];
            std::vector<float> &ib_out =
                inbetween_offsets[bs_ib_refined[ib_i].weight];
            ib_out.push_back(ib_vals[3 * v]);
            ib_out.push_back(ib_vals[3 * v + 1]);
            ib_out.push_back(ib_vals[3 * v + 2]);
          }
        }
      }
      RefinedBlendShapeOffsets refined_bs;
      refined_bs.indices = std::move(indices);
      refined_bs.point_offsets = std::move(offsets);
      refined_bs.inbetween_offsets = std::move(inbetween_offsets);
      subdiv_bs_offsets[bs] = std::move(refined_bs);
    }
    // Uniform (per-face) attributes replicate through face_source.
    for (VertexAttribute *vattr : uniform_attrs) {
      const uint32_t components = attr_components(*vattr);
      const float *src_vals =
          reinterpret_cast<const float *>(vattr->get_data().data());
      size_t value_count = 0;
      if (!safe::mul(subdiv_face_source.size(), size_t(components),
                     &value_count)) {
        PUSH_ERROR_AND_RETURN(
            "Subdivision uniform primvar value count overflow.");
      }
      std::vector<float> vals(value_count);
      for (size_t rf = 0; rf < subdiv_face_source.size(); rf++) {
        const size_t dst_base = rf * size_t(components);
        const size_t src_base = size_t(components) * subdiv_face_source[rf];
        for (uint32_t c = 0; c < components; c++) {
          vals[dst_base + c] = src_vals[src_base + c];
        }
      }
      size_t bytes = 0;
      if (!safe::mul(vals.size(), sizeof(float), &bytes)) {
        PUSH_ERROR_AND_RETURN(
            "Subdivision uniform primvar byte size overflow.");
      }
      vattr->get_data().resize(bytes);
      memcpy(vattr->get_data().data(), vals.data(), bytes);
    }

    subdivision_applied = true;

    // Remap material subset face indices onto the refined mesh: a base face
    // maps to every refined face whose face_source points back at it. The
    // remapped ids land in `triangulatedIndices` (the effective index set);
    // the triangulation pass below remaps them again when enabled.
    if (!dst.material_subsetMap.empty()) {
      // Invert face_source: base face -> contiguous run of refined faces.
      std::vector<uint32_t> counts(src_num_faces, 0);
      for (uint32_t srcf : subdiv_face_source) {
        if (srcf >= src_num_faces) {
          PUSH_ERROR_AND_RETURN(
              "Subdivision produced invalid face provenance.");
        }
        counts[srcf]++;
      }
      std::vector<uint32_t> offsets(size_t(src_num_faces) + 1, 0);
      for (uint32_t f = 0; f < src_num_faces; f++) {
        offsets[f + 1] = offsets[f] + counts[f];
      }
      std::vector<uint32_t> refined_ids(subdiv_face_source.size());
      {
        std::vector<uint32_t> cursor(offsets.begin(), offsets.end() - 1);
        for (uint32_t rf = 0; rf < uint32_t(subdiv_face_source.size());
             rf++) {
          refined_ids[cursor[subdiv_face_source[rf]]++] = rf;
        }
      }

      for (auto &sit : dst.material_subsetMap) {
        std::vector<int> remapped;
        for (int src_face : sit.second.usdIndices) {
          if (src_face < 0 || uint32_t(src_face) >= src_num_faces) {
            PUSH_ERROR_AND_RETURN(fmt::format(
                "MaterialSubset `{}` face index {} exceeds source face count "
                "{}.",
                sit.first, src_face, src_num_faces));
          }
          for (uint32_t k = offsets[uint32_t(src_face)];
               k < offsets[uint32_t(src_face) + 1]; k++) {
            remapped.push_back(int(refined_ids[k]));
          }
        }
        sit.second.triangulatedIndices = std::move(remapped);
      }
    }

    // Refresh topology-derived counts for the refined mesh.
    if (!SizeToU32(dst.points.size(), &num_vertices) ||
        !SizeToU32(dst.usdFaceVertexCounts.size(), &num_faces) ||
        !SizeToU32(dst.usdFaceVertexIndices.size(), &num_face_vertex_indices)) {
      PUSH_ERROR_AND_RETURN("Refined mesh counts exceed 32-bit index space.");
    }
  }

  //LIGHTUSD_LOG_I("done uvAttr");

  // displayColor/displayOpacity were gathered above (and refined when
  // subdivision ran); single constant values become the mesh-level color.
  if (!vcolor.get_data().empty()) {
    if ((vcolor.elementSize == 1) && (vcolor.vertex_count() == 1) &&
        (vcolor.stride_bytes() == 3 * 4)) {
      memcpy(&dst.displayColor, vcolor.data.data(), vcolor.stride_bytes());
      dst.has_authored_displayColor = true;  // authored constant, not the default
    } else {
      dst.vertex_colors = vcolor;
    }
  }

  if (!vopacity.get_data().empty()) {
    if ((vopacity.elementSize == 1) && (vopacity.vertex_count() == 1) &&
        (vopacity.stride_bytes() == 4)) {
      memcpy(&dst.displayOpacity, vopacity.data.data(),
             vopacity.stride_bytes());
    } else {
      dst.vertex_opacities = vopacity;
    }
  }



  //
  // Check if the Mesh can be drawn with single index buffer during converting
  // normals/texcoords/displayColors/displayOpacities, since OpenGL and Vulkan
  // does not support drawing a primitive with multiple index buffers.
  //
  // If the Mesh contains any face-varying attribute,
  // First try to convert it 'vertex' variabily, if it fails, all attribute are
  // converted to face-varying so that the Mesh can be drawn without index
  // buffer. This will hurt the performance of rendering in OpenGL/Vulkan,
  // especially when the Mesh is animated with skinning.
  //
  // We leave user-defined primvar as-is, so no check for it.
  //
  bool is_single_indexable{true};

  //
  // Convert normals
  //
  if (!subdivision_applied) {
    Interpolation interp = mesh.get_normalsInterpolation();
    std::vector<value::normal3f> normals;

    if (mesh.has_primvar("normals")) {  // primvars:normals
      GeomPrimvar pvar;
      if (!GetGeomPrimvar(env.stage, &mesh, "normals", &pvar, &local_err, &_warn)) {
        return false;
      }

      // Check if normals primvar has timesamples
      const lightusd::Attribute &normals_attr = pvar.get_attribute();
      if (normals_attr.has_timesamples()) {
        std::string msg = fmt::format(
            "Geometry primvar 'normals' has timesamples (animated values). "
            "RenderMesh conversion uses value at specified time (timecode={}). "
            "To capture animation, you need to convert at multiple timesamples. "
            "Consider using ConvertMesh() at each timeframe or implementing "
            "per-frame conversion. Animated normals are particularly important "
            "for correct shading and normal mapping.",
            env.timecode);
        local_warn += msg + "\n";
        DCOUT("WARN: " << msg);
      }

      // V2: try mmap path for primvar normals (handles indexed + non-indexed)
      bool got_normals_pv = false;
      if (pvar.has_indices()) {
        got_normals_pv = TryReadMMapArrayWithIndices<value::normal3f>(
            env.stage, prim_path_str, "primvars:normals", pvar,
            env.timecode, &normals);
      } else {
        got_normals_pv = TryReadMMapArray<value::normal3f>(
            env.stage, prim_path_str, "primvars:normals", &normals);
      }
      if (!got_normals_pv) {
        if (!pvar.flatten_with_indices(env.timecode, &normals, env.tinterp,
                                       &local_err)) {
          PUSH_ERROR_AND_RETURN("Failed to expand `normals` primvar.");
        }
      }

    } else if (mesh.normals.authored()) {  // look 'normals'
      // Try mmap zero-copy for direct normals attribute
      bool got_normals = TryReadMMapArray<value::normal3f>(
          env.stage, prim_path_str, "normals", &normals);
      if (!got_normals) {
        if (!EvaluateTypedAnimatableAttribute(env.stage, mesh.normals, "normals",
                                              &normals, &local_err, env.timecode,
                                              env.tinterp)) {
        }
      }
      // V2 safety: deferred array but both mmap and Stage reads produced empty
      if (normals.empty() && env.stage.has_mmap_zero_copy()) {
        const MMapArrayRef *ref = env.stage.mmap_table()->find(
            prim_path_str, "normals");
        if (ref && ref->element_count > 0) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "mmap deferred 'normals' for {} ({} elements) could not be read. "
              "mmap buffer may be invalid.", abs_prim_path, ref->element_count));
        }
      }
    }

    size_t resize_size;
    if (!safe::n_to_size<value::normal3f>(normals.size(), &resize_size)) {
      return false;
    }
    dst.normals.get_data().resize(resize_size);
    size_t memcpy_size;
    if (!safe::n_to_size<value::normal3f>(normals.size(), &memcpy_size)) {
      return false;
    }
    if (memcpy_size > 0) {
      memcpy(dst.normals.get_data().data(), normals.data(), memcpy_size);
    }
    dst.normals.elementSize = 1;
    dst.normals.stride = sizeof(value::normal3f);
    dst.normals.format = VertexAttributeFormat::Vec3;

    if (interp == Interpolation::Varying) {
      dst.normals.variability = VertexVariability::Varying;
    } else if (interp == Interpolation::Constant) {
      dst.normals.variability = VertexVariability::Constant;
    } else if (interp == Interpolation::Uniform) {
      dst.normals.variability = VertexVariability::Uniform;
    } else if (interp == Interpolation::Vertex) {
      dst.normals.variability = VertexVariability::Vertex;
    } else if (interp == Interpolation::FaceVarying) {
      dst.normals.variability = VertexVariability::FaceVarying;
    } else {
      PUSH_ERROR_AND_RETURN(
          "[Internal Error] Invalid interpolation value for normals.");
    }
    dst.normals.indices.clear();
    dst.normals.name = "normals";

    if (is_single_indexable &&
        (dst.normals.variability == VertexVariability::FaceVarying)) {
      VertexAttribute va_normals;
      if (TryConvertFacevaryingToVertex(
              dst.normals, &va_normals, dst.usdFaceVertexIndices, &local_warn,
              env.mesh_config.facevarying_to_vertex_eps)) {
        DCOUT("normals is converted to 'vertex' varying.");
        dst.normals = std::move(va_normals);
      } else {
        // Exact-eps dedup failed.  Try quantized dedup: normals that agree
        // at 10-bit SNORM resolution (~0.1°) are treated as identical.
        // This catches subdivision surface limit normals where the same
        // logical normal differs by floating-point noise across face-vertices.
        if (TryQuantizedNormalDedup(dst.normals, dst.usdFaceVertexIndices)) {
          DCOUT("normals converted to 'vertex' via quantized dedup.");
        } else {
          DCOUT(
              "normals cannot be converted to 'vertex' varying. Staying "
              "'facevarying'");
          DCOUT("warn = " << _warn);
          is_single_indexable = false;
        }
      }
    }
  }

  //
  // Convert UVs
  //

  for (auto &it : uvAttrs) {
    uint64_t slotId = it.first;
    VertexAttribute &vattr = it.second;

    if (vattr.format != VertexAttributeFormat::Vec2) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Texcoord VertexAttribute must be Vec2 type.\n"));
    }

    if (vattr.element_size() != 1) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("elementSize must be 1 for Texcoord attribute."));
    }

    DCOUT("Add texcoord attr `" << vattr.name << "` to slot Id " << slotId);

    if (is_single_indexable &&
        (vattr.variability == VertexVariability::FaceVarying)) {
      VertexAttribute va_uvs;
      if (TryConvertFacevaryingToVertex(
              vattr, &va_uvs, dst.usdFaceVertexIndices, &local_warn,
              env.mesh_config.facevarying_to_vertex_eps)) {
        DCOUT("texcoord[" << slotId << "] is converted to 'vertex' varying.");
        dst.texcoords[uint32_t(slotId)] = std::move(va_uvs);
      } else {
        DCOUT("texcoord[" << slotId
                          << "] cannot be converted to 'vertex' varying. "
                             "Staying 'facevarying'");
        is_single_indexable = false;
        dst.texcoords[uint32_t(slotId)] = std::move(vattr);
      }
    } else {
      dst.texcoords[uint32_t(slotId)] = std::move(vattr);
    }
  }

  // Rebake mesh UV sets used by combined-UDIM textures so each tile lands in
  // its atlas cell: uv' = uv * scale + offset.
  if (!mesh_udim_remaps.empty()) {
    for (auto &kv : dst.texcoords) {
      VertexAttribute &uvattr = kv.second;
      const auto rit = mesh_udim_remaps.find(uvattr.name);
      if (rit == mesh_udim_remaps.end()) {
        continue;
      }
      const std::array<float, 4> &r = rit->second;
      // Skip identity remap (e.g. single-tile UDIM at 1001).
      if (r[0] == 1.0f && r[1] == 1.0f && r[2] == 0.0f && r[3] == 0.0f) {
        continue;
      }
      if (uvattr.format != VertexAttributeFormat::Vec2) {
        continue;
      }
      std::vector<uint8_t> &data = uvattr.get_data();
      const size_t n = uvattr.vertex_count();
      if (data.size() < n * 2 * sizeof(float)) {
        continue;
      }
      float *fp = reinterpret_cast<float *>(data.data());
      for (size_t i = 0; i < n; i++) {
        fp[i * 2 + 0] = fp[i * 2 + 0] * r[0] + r[2];
        fp[i * 2 + 1] = fp[i * 2 + 1] * r[1] + r[3];
      }
      DCOUT("Rebaked UDIM texcoord `" << uvattr.name << "` ("
                                      << n << " elems) scale=(" << r[0] << ","
                                      << r[1] << ") offset=(" << r[2] << ","
                                      << r[3] << ")");
    }
  }

  if (dst.vertex_colors.vertex_count() > 1) {
    // Use const reference instead of copy for validation
    const VertexAttribute &vattr = dst.vertex_colors;

    if (vattr.format != VertexAttributeFormat::Vec3) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Color VertexAttribute must be Vec3 type.\n"));
    }

    if (vattr.element_size() != 1) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("elementSize = 1 expected for VertexColor, but got {}",
                      vattr.element_size()));
    }

    if (is_single_indexable &&
        (vattr.variability == VertexVariability::FaceVarying)) {
      VertexAttribute va;
      if (TryConvertFacevaryingToVertex(
              dst.vertex_colors, &va, dst.usdFaceVertexIndices, &local_warn,
              env.mesh_config.facevarying_to_vertex_eps)) {
        dst.vertex_colors = std::move(va);
      } else {
        DCOUT(
            "vertex_colors cannot be converted to 'vertex' varying. Staying "
            "'facevarying'");
        is_single_indexable = false;
      }
    }
  }

  if (dst.vertex_opacities.vertex_count() > 1) {
    // Use const reference instead of copy for validation
    const VertexAttribute &vattr = dst.vertex_opacities;

    if (vattr.format != VertexAttributeFormat::Float) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("Opacity VertexAttribute must be Float type.\n"));
    }

    if (vattr.element_size() != 1) {
      PUSH_ERROR_AND_RETURN(
          fmt::format("elementSize = 1 expected for VertexOpacity, but got {}",
                      vattr.element_size()));
    }

    if (is_single_indexable &&
        (vattr.variability == VertexVariability::FaceVarying)) {
      VertexAttribute va;
      if (TryConvertFacevaryingToVertex(
              dst.vertex_opacities, &va, dst.usdFaceVertexIndices, &local_warn,
              env.mesh_config.facevarying_to_vertex_eps)) {
        dst.vertex_opacities = std::move(va);
      } else {
        DCOUT(
            "vertex_opacities cannot be converted to 'vertex' varying. Staying "
            "'facevarying'");
        is_single_indexable = false;
      }
    }
  }

  DCOUT(mesh.name << " : is_single_indexable = " << is_single_indexable);

  //
  // Convert built-in vertex attributes to either 'vertex' or 'facevarying'
  //
  {
    if (!ConvertVertexVariabilityImpl(dst.normals, is_single_indexable,
                                      dst.usdFaceVertexCounts,
                                      dst.usdFaceVertexIndices)) {
      return false;
    }
    for (auto &it : dst.texcoords) {
      if (!ConvertVertexVariabilityImpl(it.second, is_single_indexable,
                                        dst.usdFaceVertexCounts,
                                        dst.usdFaceVertexIndices)) {
        return false;
      }
    }

    if (!ConvertVertexVariabilityImpl(dst.vertex_colors, is_single_indexable,
                                      dst.usdFaceVertexCounts,
                                      dst.usdFaceVertexIndices)) {
      return false;
    }
    if (!ConvertVertexVariabilityImpl(dst.vertex_opacities, is_single_indexable,
                                      dst.usdFaceVertexCounts,
                                      dst.usdFaceVertexIndices)) {
      return false;
    }
  }


  ///
  /// 4. Triangulate
  ///  - triangulate faceVertexCounts, faceVertexIndices
  ///  - Remap faceIndex in MaterialSubset(GeomSubset).
  ///  - Triangulate vertex attributes(normals, uvcoords, vertex
  ///  colors/opacities).
  ///
  bool triangulate = env.mesh_config.triangulate;

  //
  // Evaluate `holeIndices`(faces to be removed from the rendered surface).
  // Skip when subdivision ran: the subdiv pipeline consumes holeIndices
  // natively as hole TAGS (dropping the refined children of the tagged base
  // face), and the face numbering here is already refined-space.
  //
  std::set<uint32_t> holeFaceIndices;
  if (mesh.holeIndices.authored() && !subdivision_applied) {
    std::vector<int32_t> hole_indices;
    bool ret = EvaluateTypedAnimatableAttribute(
        env.stage, mesh.holeIndices, "holeIndices", &hole_indices, &local_err,
        env.timecode, value::TimeSampleInterpolationType::Held);
    if (!ret) {
      return false;
    }

    for (size_t i = 0; i < hole_indices.size(); i++) {
      if ((hole_indices[i] < 0) ||
          (size_t(hole_indices[i]) >= dst.usdFaceVertexCounts.size())) {
        PUSH_WARN(fmt::format(
            "holeIndices[{}] = {} is out of range [0, {}). Ignored.", i,
            hole_indices[i], dst.usdFaceVertexCounts.size()));
        continue;
      }
      holeFaceIndices.insert(uint32_t(hole_indices[i]));
    }

    if (holeFaceIndices.size() && !triangulate) {
      PUSH_WARN(
          "`holeIndices` is authored, but hole face removal is only applied "
          "when triangulation is enabled. Hole faces are kept in the "
          "polygonal output.");
    }
  }

  if (triangulate) {
    DCOUT("Triangulate mesh");
    std::vector<uint32_t> triangulatedFaceVertexCounts;  // should be all 3's
    std::vector<uint32_t> triangulatedFaceVertexIndices;
    std::vector<uint32_t>
        triangulatedToOrigFaceVertexIndexMap;  // used for rearrange facevertex
                                               // attrib
    std::vector<uint32_t>
        triangulatedFaceCounts;  // used for rearrange face indices(e.g
                                 // GeomSubset indices)

    std::string err;

    if (!TriangulatePolygon<value::float3, float>(
            dst.points, dst.usdFaceVertexCounts, dst.usdFaceVertexIndices,
            triangulatedFaceVertexCounts, triangulatedFaceVertexIndices,
            triangulatedToOrigFaceVertexIndexMap, triangulatedFaceCounts,
            env.mesh_config.triangulation_method,
            _warn, err, holeFaceIndices)) {
      PUSH_ERROR_AND_RETURN("Triangulation failed: " + err);
    }

    if (dst.material_subsetMap.size()) {
      // Remap faceId in GeomSubsets

      //
      // size: len(triangulatedFaceCounts)
      // value: array index in triangulatedFaceVertexCounts
      // Up to 4GB faces.
      //
      std::vector<uint32_t> faceIndexOffsets;
      faceIndexOffsets.resize(triangulatedFaceCounts.size());

      size_t faceIndexOffset = 0;
      for (size_t i = 0; i < triangulatedFaceCounts.size(); i++) {
        size_t ncount = triangulatedFaceCounts[i];

        faceIndexOffsets[i] = uint32_t(faceIndexOffset);
        // DCOUT("faceIndexOffset[" << i << "] = " << faceIndexOffsets[i]);

        faceIndexOffset += ncount;

        if (faceIndexOffset >= (std::numeric_limits<uint32_t>::max)()) {
          PUSH_ERROR_AND_RETURN("Triangulated Mesh contains 4G or more faces.");
        }
      }

      // Remap indices in MaterialSubset
      //
      // example:
      //
      // faceVertexCounts = [4, 4]
      // faceVertexIndices = [0, 1, 2, 3, 4, 5, 6, 7]
      //
      // triangulatedFaceVertexCounts = [3, 3, 3, 3]
      // triangulatedFaceVertexIndices = [0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7]
      // triangulatedFaceCounts = [2, 2]
      //
      // geomsubset.indices = [0, 1] # index to faceVertexCounts
      // faceIndexOffsets = [0, 2]
      //
      // => triangulated geomsubset.indices = [0, 1, 2, 3] # index to
      // triangulatedFaceVertexCounts
      //
      //
      for (auto &it : dst.material_subsetMap) {
        std::vector<int> triangulated_indices;

        // When subdivision ran, the effective face ids were remapped into
        // triangulatedIndices above (refined face ids); otherwise the
        // authored usdIndices apply.
        const std::vector<int> effective_indices =
            subdivision_applied ? it.second.triangulatedIndices
                                : it.second.usdIndices;
        for (size_t i = 0; i < effective_indices.size(); i++) {
          int32_t srcIndex = effective_indices[i];
          if (srcIndex < 0 || size_t(srcIndex) >= faceIndexOffsets.size()) {
            PUSH_ERROR_AND_RETURN(fmt::format(
                "GeomSubset '{}': index {} out of range [0, {}).",
                it.first, srcIndex, faceIndexOffsets.size()));
          }

          uint32_t baseFaceIndex = faceIndexOffsets[size_t(srcIndex)];
          // DCOUT(i << ", baseFaceIndex = " << baseFaceIndex);

          for (size_t k = 0; k < triangulatedFaceCounts[uint32_t(srcIndex)];
               k++) {
            if ((baseFaceIndex + k) > size_t((std::numeric_limits<int32_t>::max)())) {
              PUSH_ERROR_AND_RETURN(fmt::format("Index value exceeds 2GB."));
            }
            // assume triangulated faceIndex in each polygon is monotonically
            // increasing.
            triangulated_indices.push_back(int(baseFaceIndex + k));
          }
        }

        it.second.triangulatedIndices = std::move(triangulated_indices);
      }
    }

    //
    // Triangulate built-in vertex attributes.
    //
    {
      if (!TriangulateVertexAttribute(dst.normals, dst.usdFaceVertexCounts,
                                      triangulatedToOrigFaceVertexIndexMap,
                                      triangulatedFaceCounts,
                                      triangulatedFaceVertexIndices, &local_err)) {
        PUSH_ERROR_AND_RETURN("Failed to triangulate normals attribute.");
      }

      if (!TriangulateVertexAttribute(dst.tangents, dst.usdFaceVertexCounts,
                                      triangulatedToOrigFaceVertexIndexMap,
                                      triangulatedFaceCounts,
                                      triangulatedFaceVertexIndices, &local_err)) {
        PUSH_ERROR_AND_RETURN("Failed to triangulate tangents attribute.");
      }

      if (!TriangulateVertexAttribute(dst.binormals, dst.usdFaceVertexCounts,
                                      triangulatedToOrigFaceVertexIndexMap,
                                      triangulatedFaceCounts,
                                      triangulatedFaceVertexIndices, &local_err)) {
        PUSH_ERROR_AND_RETURN("Failed to triangulate binormals attribute.");
      }

      for (auto &it : dst.texcoords) {
        if (!TriangulateVertexAttribute(it.second, dst.usdFaceVertexCounts,
                                        triangulatedToOrigFaceVertexIndexMap,
                                        triangulatedFaceCounts,
                                        triangulatedFaceVertexIndices, &local_err)) {
          PUSH_ERROR_AND_RETURN(fmt::format(
              "Failed to triangulate texcoords[{}] attribute.", it.first));
        }
      }

      if (!TriangulateVertexAttribute(
              dst.vertex_colors, dst.usdFaceVertexCounts,
              triangulatedToOrigFaceVertexIndexMap, triangulatedFaceCounts,
              triangulatedFaceVertexIndices, &local_err)) {
        PUSH_ERROR_AND_RETURN("Failed to triangulate vertex_colors attribute.");
      }

      if (!TriangulateVertexAttribute(
              dst.vertex_opacities, dst.usdFaceVertexCounts,
              triangulatedToOrigFaceVertexIndexMap, triangulatedFaceCounts,
              triangulatedFaceVertexIndices, &local_err)) {
        PUSH_ERROR_AND_RETURN(
            "Failed to triangulate vertopacitiesex_colors attribute.");
      }
    }

    dst.triangulatedFaceVertexCounts = std::move(triangulatedFaceVertexCounts);
    dst.triangulatedFaceVertexIndices =
        std::move(triangulatedFaceVertexIndices);

    dst.triangulatedToOrigFaceVertexIndexMap =
        std::move(triangulatedToOrigFaceVertexIndexMap);
    dst.triangulatedFaceCounts = std::move(triangulatedFaceCounts);
  }

  // Free triangulation intermediates — only needed during attribute
  // triangulation above. Tools that map triangles back to source faces can ask
  // to keep triangulatedFaceCounts via keep_triangulation_intermediates.
  { std::vector<uint32_t> tmp; dst.triangulatedToOrigFaceVertexIndexMap.swap(tmp); }
  if (!env.mesh_config.keep_triangulation_intermediates) {
    std::vector<uint32_t> tmp;
    dst.triangulatedFaceCounts.swap(tmp);
  }

  // Free pre-triangulation topology under lowmem guard.
  // faceVertexIndices()/faceVertexCounts() accessors return the triangulated
  // versions when is_triangulated() is true.
  if (env.mesh_config.lowmem && dst.is_triangulated()) {
    { std::vector<uint32_t> tmp; dst.usdFaceVertexCounts.swap(tmp); }
    { std::vector<uint32_t> tmp; dst.usdFaceVertexIndices.swap(tmp); }
  }

  //
  // 5. Vertex skin weights(jointIndex and jointWeights)
  //
  if (mesh.has_primvar("skel:jointIndices") &&
      mesh.has_primvar("skel:jointWeights")) {
    DCOUT("Convert skin weights");
    auto clearInvalidSkinning = [&](const std::string &reason) {
      PUSH_WARN(fmt::format(
          "Ignoring invalid skinning data on mesh {} and rendering it "
          "as a static mesh. {}",
          abs_prim_path.full_path_name(), reason));
      dst.skel_id = -1;
      dst.joint_and_weights.jointIndices.clear();
      dst.joint_and_weights.jointWeights.clear();
      dst.joint_and_weights.elementSize = 0;
      dst.joint_and_weights.hasGeomBindTransform = false;
      dst.joint_and_weights.geomBindTransform = value::matrix4d::identity();
      local_err.clear();
    };

    auto convertSkinning = [&]() -> bool {
      GeomPrimvar jointIndices;
      GeomPrimvar jointWeights;

    if (!GetGeomPrimvar(env.stage, &mesh, "skel:jointIndices", &jointIndices,
                        &local_err)) {
      clearInvalidSkinning("Could not read primvars:skel:jointIndices: " + local_err);
      return false;
    }

    if (!GetGeomPrimvar(env.stage, &mesh, "skel:jointWeights", &jointWeights,
                        &local_err)) {
      clearInvalidSkinning("Could not read primvars:skel:jointWeights: " + local_err);
      return false;
    }

    // interpolation must be 'vertex'
    if (!jointIndices.has_interpolation()) {
      clearInvalidSkinning(
          "`skel:jointIndices` primvar must author `interpolation` metadata.");
      return false;
    }

    // TODO: Disallow Varying?
    if ((jointIndices.get_interpolation() != Interpolation::Vertex) &&
        (jointIndices.get_interpolation() != Interpolation::Varying)) {
      clearInvalidSkinning(fmt::format(
          "`skel:jointIndices` primvar must use `vertex` or `varying` "
          "interpolation, but got `{}`.",
          to_string(jointIndices.get_interpolation())));
      return false;
    }

    if (!jointWeights.has_interpolation()) {
      clearInvalidSkinning(
          "`skel:jointWeights` primvar must author `interpolation` metadata.");
      return false;
    }

    // TODO: Disallow Varying?
    if ((jointWeights.get_interpolation() != Interpolation::Vertex) &&
        (jointWeights.get_interpolation() != Interpolation::Varying)) {
      clearInvalidSkinning(fmt::format(
          "`skel:jointWeights` primvar must use `vertex` or `varying` "
          "interpolation, but got `{}`.",
          to_string(jointWeights.get_interpolation())));
      return false;
    }

    uint32_t jointIndicesElementSize = jointIndices.get_elementSize();
    uint32_t jointWeightsElementSize = jointWeights.get_elementSize();

    if (jointIndicesElementSize == 0) {
      clearInvalidSkinning(
          "`elementSize` metadata of `skel:jointIndices` is zero.");
      return false;
    }

    if (jointWeightsElementSize == 0) {
      clearInvalidSkinning(
          "`elementSize` metadata of `skel:jointWeights` is zero.");
      return false;
    }

    if (jointIndicesElementSize > env.mesh_config.max_skin_elementSize) {
      clearInvalidSkinning(fmt::format(
          "`elementSize` {} of `skel:jointIndices` too large. Max allowed is "
          "set to {}",
          jointIndicesElementSize, env.mesh_config.max_skin_elementSize));
      return false;
    }

    if (jointWeightsElementSize > env.mesh_config.max_skin_elementSize) {
      clearInvalidSkinning(fmt::format(
          "`elementSize` {} of `skel:jointWeights` too large. Max allowed is "
          "set to {}",
          jointWeightsElementSize, env.mesh_config.max_skin_elementSize));
      return false;
    }

    if (jointIndicesElementSize != jointWeightsElementSize) {
      clearInvalidSkinning(fmt::format(
          "`elementSize` {} of `skel:jointIndices` must equal "
          "`elementSize` {} of `skel:jointWeights`.",
          jointIndicesElementSize, jointWeightsElementSize));
      return false;
    }

    std::vector<int> jointIndicesArray;
    if (!jointIndices.flatten_with_indices(env.timecode, &jointIndicesArray,
                                           env.tinterp)) {
      clearInvalidSkinning(
          "Failed to flatten Indexed Primvar `skel:jointIndices`. Ensure it "
          "is type `int[]`.");
      return false;
    }

    std::vector<float> jointWeightsArray;
    // V2: try mmap path for jointWeights (float array, may be deferred)
    bool got_jw = false;
    if (jointWeights.has_indices()) {
      got_jw = TryReadMMapArrayWithIndices<float>(
          env.stage, prim_path_str, "primvars:skel:jointWeights", jointWeights,
          env.timecode, &jointWeightsArray);
    } else {
      got_jw = TryReadMMapArray<float>(
          env.stage, prim_path_str, "primvars:skel:jointWeights",
          &jointWeightsArray);
    }
    if (!got_jw) {
      if (!jointWeights.flatten_with_indices(env.timecode, &jointWeightsArray,
                                             env.tinterp)) {
        clearInvalidSkinning(
            "Failed to flatten Indexed Primvar `skel:jointWeights`. Ensure it "
            "is type `float[]`.");
        return false;
      }
    }

    if (jointIndicesArray.size() != jointWeightsArray.size()) {
      clearInvalidSkinning(fmt::format(
          "`skel:jointIndices` nitems {} must equal `skel:jointWeights` "
          "nitems {}.",
          jointIndicesArray.size(), jointWeightsArray.size()));
      return false;
    }

    if (jointIndicesArray.empty()) {
      clearInvalidSkinning("`skel:jointIndices` is empty array.");
      return false;
    }

    // Validate joint indices: reject negative values always, and
    // out-of-range values when the mesh-local `skel:joints` order gives us a
    // definitive bound. Indices referencing skeleton joints directly are
    // range-checked later during the name-remap pass.
    {
      const std::vector<value::token> mesh_joints = mesh.get_joints();
      const int32_t joint_count =
          mesh_joints.empty() ? -1 : int32_t(mesh_joints.size());
      for (int32_t idx : jointIndicesArray) {
        if (idx < 0) {
          clearInvalidSkinning(fmt::format(
              "`skel:jointIndices` contains a negative index {}.", idx));
          return false;
        }
        if (joint_count >= 0 && idx >= joint_count) {
          clearInvalidSkinning(fmt::format(
              "`skel:jointIndices` index {} is out-of-range (mesh-local "
              "`skel:joints` has {} entries).",
              idx, joint_count));
          return false;
        }
      }
    }

    if (subdivision_applied && subdiv_skin_refined) {
      // Weights were refined through subdivision (dense per-joint columns,
      // re-sparsified to elementSize influences per refined point).
      dst.joint_and_weights.jointIndices = std::move(subdiv_skin_joint_indices);
      dst.joint_and_weights.jointWeights = std::move(subdiv_skin_joint_weights);
    } else {
      dst.joint_and_weights.jointIndices = jointIndicesArray;
      dst.joint_and_weights.jointWeights = jointWeightsArray;
    }
    dst.joint_and_weights.elementSize = int(jointIndicesElementSize);

    // Helper lambda to round up bone count to standard GPU skinning values
    auto roundBoneCountUp = [](uint32_t count) -> uint32_t {
      // Standard GPU skinning bone counts: 4, 8, 16, 32, 48, 64, 80, 96, 128
      const uint32_t standardCounts[] = {4, 8, 16, 32, 48, 64, 80, 96, 128};
      for (uint32_t stdCount : standardCounts) {
        if (count <= stdCount) {
          return stdCount;
        }
      }
      return 128; // Max supported
    };

    // Skeleton binding: first try explicit relationship, then fallback to ancestor discovery
    {
      Path skelPath;
      bool hasSkelPath = false;

      // First try explicit skel:skeleton relationship
      if (mesh.skeleton.has_value()) {
        DCOUT("Convert Skeleton (explicit relationship)");

        if (mesh.skeleton.value().is_path()) {
          skelPath = mesh.skeleton.value().targetPath;
          hasSkelPath = true;
        } else if (mesh.skeleton.value().is_pathvector()) {
          const size_t target_count = mesh.skeleton.value().targetPathVector.size();
          if (target_count == 1) {
            skelPath = mesh.skeleton.value().targetPathVector[0];
            hasSkelPath = true;
          } else {
            PushError(fmt::format(
                "`skel:skeleton` must have exactly one target for {}, but got {}.",
                abs_prim_path.full_path_name(), target_count));
            return false;
          }
        } else {
          PushError(fmt::format(
              "`skel:skeleton` has invalid definition for {}.",
              abs_prim_path.full_path_name()));
          return false;
        }
      }

      // Fallback: find skeleton by ancestor if mesh has skinning data but no explicit binding
      const Skeleton *discoveredSkelPtr{nullptr};
      if (!hasSkelPath && !dst.joint_and_weights.jointIndices.empty()) {
        DCOUT("Mesh has skinning data but no skel:skeleton - trying ancestor discovery");
        if (_allSkeletons && _allSkelRoots) {
          Path meshPath(abs_prim_path.full_path_name(), "");
          if (SkelRootSkeletonResolver::FindByAncestor(
                  meshPath, *_allSkeletons, *_allSkelRoots,
                  &_skelRootToSkeleton, &skelPath, &discoveredSkelPtr)) {
            hasSkelPath = true;
            DCOUT("Found skeleton by ancestor: " << skelPath.prim_part());
          } else {
            PUSH_WARN(
                "Mesh has skinning data but no skeleton bound: " +
                abs_prim_path.full_path_name() +
                ". Skinning data is preserved for later skeleton attachment.");
          }
        }
      }

      if (hasSkelPath && skelPath.is_valid()) {
        // Check if skeleton already exists via O(1) hash lookup
        std::string skelPathStr = skelPath.prim_part();
        auto skel_cache_it = _skelPathToIndex.find(skelPathStr);

        if (skel_cache_it != _skelPathToIndex.end()) {
          // Skeleton already converted, reuse it
          dst.skel_id = skel_cache_it->second;
        } else {
          int32_t skel_id = int32_t(skeletons.size());

          SkelHierarchy skel;

          // Use ConvertSkeletonFromPtr if we have the skeleton pointer from discovery,
          // otherwise use ConvertSkeletonImplWithPath for explicit relationship case
          std::string skel_err;
          if (discoveredSkelPtr) {
            // Extract prim name from path (last component)
            std::string primName = skelPath.prim_part();
            size_t lastSlash = primName.rfind('/');
            if (lastSlash != std::string::npos) {
              primName = primName.substr(lastSlash + 1);
            }
            if (!ConvertSkeletonFromPtr(env, skelPath, *discoveredSkelPtr, primName, &skel)) {
              skel_err = local_err;
            }
          } else {
            if (!ConvertSkeletonImplWithPath(env, skelPath, &skel)) {
              skel_err = local_err;
            }
          }

          if (!skel_err.empty()) {
            clearInvalidSkinning(fmt::format(
                "Skeleton {} could not be converted: {}",
                skelPathStr, skel_err));
            return false;
          } else {
            DCOUT("Converted skeleton attached to : " << abs_prim_path);

            _skelPathToIndex[skelPathStr] = skel_id;
            skeletons.emplace_back(std::move(skel));
            DCOUT("add skeleton\n");

            dst.skel_id = skel_id;
          }
        }

      }
    }

    // Apply bone reduction if enabled (after skeleton binding so hierarchy info is available)
    if (env.mesh_config.enable_bone_reduction &&
        (env.mesh_config.target_bone_count < jointIndicesElementSize)) {
      uint32_t numVertices = uint32_t(jointIndicesArray.size() / jointIndicesElementSize);

      DCOUT("Reducing bone influences from " << jointIndicesElementSize
            << " to " << env.mesh_config.target_bone_count
            << " per vertex (" << numVertices << " vertices)");

      // Configure bone reduction with advanced settings
      BoneReductionConfig bone_config;
      bone_config.target_bone_count = env.mesh_config.target_bone_count;
      bone_config.strategy = BoneReductionStrategy::ErrorMetric;
      bone_config.min_weight_threshold = 0.001f;
      bone_config.error_tolerance = 0.5f;
      bone_config.normalize_weights = true;

      // Use pre-computed flat topology from SkelHierarchy if available
      BoneHierarchyInfo hierarchy_storage;
      BoneHierarchyInfo *hierarchy_info = nullptr;

      if (dst.skel_id >= 0 && dst.skel_id < int(skeletons.size())) {
        const auto &skelH = skeletons[size_t(dst.skel_id)];
        if (!skelH.parent_joint_indices.empty()) {
          hierarchy_storage.parent_indices = skelH.parent_joint_indices;
          hierarchy_info = &hierarchy_storage;
        }
      }

      BoneReductionStats reduction_stats;

      if (!ReduceBoneInfluences(
              dst.joint_and_weights.jointIndices,
              dst.joint_and_weights.jointWeights,
              jointIndicesElementSize,
              numVertices,
              bone_config,
              hierarchy_info,
              &reduction_stats)) {
        PushInfo("Bone reduction failed, using original bone influences.");
      } else {
        // Update elementSize to reflect reduced bone count
        dst.joint_and_weights.elementSize = int(env.mesh_config.target_bone_count);
        DCOUT("Bone reduction complete. New elementSize: " << dst.joint_and_weights.elementSize);
        DCOUT("  Modified vertices: " << reduction_stats.num_vertices_modified << " / " << numVertices);
        DCOUT("  Avg weight error: " << reduction_stats.avg_weight_error);
        DCOUT("  Max weight error: " << reduction_stats.max_weight_error);
      }
    }
    // Round bone count without reduction (pad with zeros)
    else if (env.mesh_config.round_bone_count && !env.mesh_config.enable_bone_reduction) {
      uint32_t currentElementSize = jointIndicesElementSize;
      uint32_t roundedElementSize = roundBoneCountUp(currentElementSize);

      if (roundedElementSize > currentElementSize) {
        uint32_t numVertices = uint32_t(jointIndicesArray.size() / jointIndicesElementSize);

        DCOUT("Rounding bone count from " << currentElementSize
              << " to " << roundedElementSize
              << " per vertex (" << numVertices << " vertices)");

        // Create new arrays with padded size
        std::vector<int32_t> paddedIndices(numVertices * roundedElementSize, 0);
        std::vector<float> paddedWeights(numVertices * roundedElementSize, 0.0f);

        // Copy existing data and pad with zeros
        for (uint32_t v = 0; v < numVertices; v++) {
          for (uint32_t j = 0; j < currentElementSize; j++) {
            uint32_t srcIdx = v * currentElementSize + j;
            uint32_t dstIdx = v * roundedElementSize + j;
            paddedIndices[dstIdx] = dst.joint_and_weights.jointIndices[srcIdx];
            paddedWeights[dstIdx] = dst.joint_and_weights.jointWeights[srcIdx];
          }
          // Remaining slots are already zero-initialized
        }

        dst.joint_and_weights.jointIndices = std::move(paddedIndices);
        dst.joint_and_weights.jointWeights = std::move(paddedWeights);
        dst.joint_and_weights.elementSize = int(roundedElementSize);

        DCOUT("Bone count rounded. New elementSize: " << dst.joint_and_weights.elementSize);
      }
    }

    // Explicit joint orders
    // If the mesh has `skel:joints`, remap jointIndex.
    {
      std::vector<value::token> joints = mesh.get_joints();
      if ((dst.skel_id >= 0) && (dst.skel_id < int(skeletons.size())) && joints.size()) {
        //  DCOUT("has explicit joint orders.\n");

        const auto &skel = skeletons[size_t(dst.skel_id)];

        // Cache BuildSkelNameToIndexMap per skeleton ID (avoids rebuilding
        // per mesh). Only reached from serial (merge-phase) skinned-mesh
        // conversion, so no lock is required here.
        auto cache_it = _skelNameToIndexCache.find(dst.skel_id);
        if (cache_it == _skelNameToIndexCache.end()) {
          cache_it = _skelNameToIndexCache.emplace(dst.skel_id, BuildSkelNameToIndexMap(skel)).first;
        }
        const auto &name_to_index_map = cache_it->second;

        // Flat vector remap: index_remap[i] = skeleton joint index for mesh joint i
        std::vector<int> index_remap(joints.size(), -1);

        for (size_t i = 0; i < joints.size(); i++) {
          std::string joint_name = joints[i].str();

          auto nit = name_to_index_map.find(joint_name);
          if (nit == name_to_index_map.end()) {
            clearInvalidSkinning(fmt::format(
                "Joint `{}` from skel:joints was not found in the bound "
                "Skeleton.", joint_name));
            return false;
          }

          index_remap[i] = nit->second;
        }

        for (size_t i = 0; i < dst.joint_and_weights.jointIndices.size(); i++) {
          int src_idx = dst.joint_and_weights.jointIndices[i];
          if (src_idx >= 0 && size_t(src_idx) < index_remap.size() && index_remap[size_t(src_idx)] >= 0) {
            dst.joint_and_weights.jointIndices[i] = index_remap[size_t(src_idx)];
          }
        }
      }

    }

    // geomBindTransform(optional).
    // If not authored, defaults to identity matrix (already set in struct initialization)
    if (mesh.has_primvar("skel:geomBindTransform")) {
      GeomPrimvar bindTransformPvar;

      if (!GetGeomPrimvar(env.stage, &mesh, "skel:geomBindTransform",
                          &bindTransformPvar, &local_err)) {
        clearInvalidSkinning("Could not read primvars:skel:geomBindTransform: " +
                             local_err);
        return false;
      }

      value::matrix4d bindTransform;
      if (!bindTransformPvar.get_value(&bindTransform)) {
        clearInvalidSkinning(
            "Failed to get `skel:geomBindTransform` attribute. Ensure it is "
            "type `matrix4d`.");
        return false;
      }

      dst.joint_and_weights.geomBindTransform = bindTransform;
      dst.joint_and_weights.hasGeomBindTransform = true;
    } else {
      // geomBindTransform not authored - use identity (already set by default)
      // Flag remains false to indicate fallback was used
      DCOUT("skel:geomBindTransform not authored, using identity matrix");
    }
    return true;
    };
    if (!convertSkinning() && !local_err.empty()) {
      return false;
    }
  }

  //
  // 6. BlendShapes
  //
  //    NOTE: (Built-in) BlendShape attributes are per-point, so it is not
  //    affected by triangulation and single-indexable indices build.
  //
  for (const auto &it : blendshapes) {
    const std::string &bs_path = it.first;
    const BlendShape *bs = it.second;

    if (!bs) {
      continue;
    }

    std::vector<int> vertex_indices;
    std::vector<value::vector3f> normal_offsets;
    std::vector<value::vector3f> vertex_offsets;

    bs->pointIndices.get_value(&vertex_indices);
    bs->normalOffsets.get_value(&normal_offsets);
    bs->offsets.get_value(&vertex_offsets);

    ShapeTarget shapeTarget;
    shapeTarget.abs_path = bs_path;
    shapeTarget.prim_name = bs->name;
    shapeTarget.display_name = bs->metas().has_displayName() ? bs->metas().get_displayName() : "";

    if (subdivision_applied) {
      // Offsets were refined through subdivision (exact: refinement is
      // linear in the point data). normalOffsets were dropped with a
      // warning; normals are recomputed from the refined topology.
      auto sit = subdiv_bs_offsets.find(bs);
      if (sit == subdiv_bs_offsets.end()) {
        PUSH_WARN(fmt::format(
            "`pointIndices` in BlendShape `{}` is not authored or empty. "
            "Skipping.",
            bs->name));
        continue;
      }
      shapeTarget.pointIndices = std::move(sit->second.indices);
      {
        const std::vector<float> &flat = sit->second.point_offsets;
        size_t flat_byte_size;
        if (!safe::mul(flat.size(), sizeof(float), &flat_byte_size)) {
          PUSH_ERROR_AND_RETURN("Blendshape offset buffer size overflow.");
        }
        shapeTarget.pointOffsets.resize(flat.size() / 3);
        memcpy(shapeTarget.pointOffsets.data(), flat.data(), flat_byte_size);
      }
      for (auto &ib_kv : sit->second.inbetween_offsets) {
        InbetweenShapeTarget ibt;
        ibt.weight = ib_kv.first;
        const std::vector<float> &ib_flat = ib_kv.second;
        size_t ib_flat_byte_size;
        if (!safe::mul(ib_flat.size(), sizeof(float), &ib_flat_byte_size)) {
          PUSH_ERROR_AND_RETURN("Inbetween offset buffer size overflow.");
        }
        ibt.pointOffsets.resize(ib_flat.size() / 3);
        memcpy(ibt.pointOffsets.data(), ib_flat.data(), ib_flat_byte_size);
        shapeTarget.inbetweens[ibt.weight] = std::move(ibt);
      }
      dst.targets[bs->name] = shapeTarget;
      DCOUT("Converted refined blendshape target: " << bs->name);
      continue;
    }

    if (vertex_indices.empty()) {
      PUSH_WARN(
          fmt::format("`pointIndices` in BlendShape `{}` is not authored or "
                      "empty. Skipping.",
                      bs->name));
    }

    // Validate and convert indices in-place.
    shapeTarget.pointIndices.reserve(vertex_indices.size());
    for (size_t i = 0; i < vertex_indices.size(); i++) {
      if (vertex_indices[i] < 0) {
        PUSH_ERROR_AND_RETURN(fmt::format(
            "negative index in `pointIndices`. Prim path: `{}`", bs_path));
      }

      if (uint32_t(vertex_indices[i]) >= dst.points.size()) {
        PUSH_ERROR_AND_RETURN(
            fmt::format("pointIndices[{}] {} exceeds the number of points in "
                        "GeomMesh {}. Prim path: `{}`",
                        i, vertex_indices[i], dst.points.size(), bs_path));
      }

      shapeTarget.pointIndices.push_back(uint32_t(vertex_indices[i]));
    }

    if (vertex_offsets.size() &&
        (vertex_offsets.size() == vertex_indices.size())) {
      shapeTarget.pointOffsets.resize(vertex_offsets.size());
      memcpy(shapeTarget.pointOffsets.data(), vertex_offsets.data(),
             sizeof(value::normal3f) * vertex_offsets.size());
    }

    if (normal_offsets.size() &&
        (normal_offsets.size() == vertex_indices.size())) {
      shapeTarget.normalOffsets.resize(normal_offsets.size());
      memcpy(shapeTarget.normalOffsets.data(), normal_offsets.data(),
             sizeof(value::normal3f) * normal_offsets.size());
    }

    // In-between BlendShape attributes (`inbetweens:<name>`). Their offsets are
    // parallel to `pointIndices`, same as the primary `offsets`. Stored
    // un-reordered here; ReorderVertexVaryingAttributes splats them alongside
    // the primary offsets so they stay aligned after single-indexable build.
    for (const auto &pkv : bs->props) {
      if (pkv.first.rfind("inbetweens:", 0) != 0) {  // namespace prefix
        continue;
      }
      const Property &p = pkv.second;
      if (!p.is_attribute()) {
        continue;
      }
      const Attribute &a = p.get_attribute();
      if (!a.metas().has_weight()) {
        PUSH_WARN(fmt::format(
            "In-between BlendShape `{}` has no `weight` metadata. Skipping.",
            pkv.first));
        continue;
      }
      std::vector<value::vector3f> ib_offsets;
      if (!a.get_value(&ib_offsets)) {
        continue;
      }
      if (ib_offsets.size() != vertex_indices.size()) {
        PUSH_WARN(fmt::format(
            "In-between BlendShape `{}` offset count {} differs from "
            "pointIndices count {}. Skipping.",
            pkv.first, ib_offsets.size(), vertex_indices.size()));
        continue;
      }
      InbetweenShapeTarget ibt;
      ibt.weight = float(a.metas().get_weight());
      ibt.pointOffsets.resize(ib_offsets.size());
      memcpy(ibt.pointOffsets.data(), ib_offsets.data(),
             sizeof(value::vector3f) * ib_offsets.size());
      shapeTarget.inbetweens[ibt.weight] = std::move(ibt);
    }

    // Duplicate BlendShape prim names would silently overwrite an earlier
    // target; surface it. (USD requires unique child names, so this
    // indicates malformed upstream data.)
    auto target_it = dst.targets.find(bs->name);
    if (target_it != dst.targets.end()) {
      PUSH_WARN(fmt::format(
          "Duplicate BlendShape name `{}` on mesh `{}`; keeping the last "
          "converted target.",
          bs->name, abs_prim_path.full_path_name()));
    }
    dst.targets[bs->name] = shapeTarget;
    DCOUT("Converted blendshape target: " << bs->name);
  }


  //
  // 7. Compute normals
  //
  //    Compute normals when normals is not present or compute tangents
  //    requiested but normals is not present. Normals are computed with
  //    'vertex' variability to compute smooth normals for shared vertex.
  //
  //    When triangulated, normals are computed for triangulated mesh.
  //
  bool compute_normals =
      (env.mesh_config.compute_normals && dst.normals.empty());
  bool compute_tangents =
      (env.mesh_config.compute_tangents_and_binormals && dst.tangents.empty());

  // When compute_tangents_only_with_normal_map is set, skip tangent
  // computation for meshes without a normal map texture connection.
  if (compute_tangents && env.mesh_config.compute_tangents_only_with_normal_map) {
    bool has_normal_map = false;
    // Check mesh-level material
    if (dst.material_id >= 0 &&
        dst.material_id < int(materials.size())) {
      const auto &rmat = materials[size_t(dst.material_id)];
      if (rmat.surfaceShader && rmat.surfaceShader->normal.is_texture()) {
        has_normal_map = true;
      }
      if (!has_normal_map && rmat.openPBRShader &&
          rmat.openPBRShader->normal.is_texture()) {
        has_normal_map = true;
      }
    }
    // Check per-subset materials
    if (!has_normal_map) {
      for (const auto &kv : dst.material_subsetMap) {
        int mid = kv.second.material_id;
        if (mid >= 0 && mid < int(materials.size())) {
          const auto &rmat = materials[size_t(mid)];
          if ((rmat.surfaceShader && rmat.surfaceShader->normal.is_texture()) ||
              (rmat.openPBRShader && rmat.openPBRShader->normal.is_texture())) {
            has_normal_map = true;
            break;
          }
        }
      }
    }
    if (!has_normal_map) {
      DCOUT("No normal map texture assigned — skipping tangent computation.");
      compute_tangents = false;
    }
  }

  if (compute_normals || (compute_tangents && dst.normals.empty())) {
    //LIGHTUSD_LOG_I("Build normals");
    DCOUT("Compute normals");
    std::vector<vec3> normals;
    // For leftHanded orientation meshes, flip the computed geometric normals
    // (faces wind clockwise).
    if (!ComputeNormals(dst.points, dst.faceVertexCounts(),
                       dst.faceVertexIndices(), normals, &local_err,
                       /* flip_normals */ !dst.is_rightHanded)) {
      DCOUT("compute normals failed.");
      return false;
    }

    {
      size_t nrm_byte_size;
      if (!safe::mul(normals.size(), sizeof(vec3), &nrm_byte_size)) {
        return false;
      }
      dst.normals.set_buffer(
          reinterpret_cast<const uint8_t *>(normals.data()), nrm_byte_size);
    }
    dst.normals.elementSize = 1;
    dst.normals.variability = VertexVariability::Vertex;
    dst.normals.format = VertexAttributeFormat::Vec3;
    dst.normals.stride = 0;
    dst.normals.indices.clear();
    dst.normals.name = "normals";

    if (!is_single_indexable) {
      auto result = VertexToFaceVarying(
          dst.normals.get_data(), dst.normals.stride_bytes(),
          dst.faceVertexCounts(), dst.faceVertexIndices());
      if (!result) {
        PUSH_WARN(fmt::format(
            "Convert vertex/varying `normals` attribute failed for Mesh '{}': {}. Normals removed from RenderMesh.",
            abs_prim_path.full_path_name(), result.error()));
        // Clear normals from RenderMesh since conversion failed
        dst.normals.data.clear();
        dst.normals.indices.clear();
      } else {
        dst.normals.data = result.value();
        dst.normals.variability = VertexVariability::FaceVarying;
      }
    }
  }


  //
  // 8. Deferred tangent check — must happen BEFORE index build decision,
  //    because the index build is skipped when compute_tangents is true
  //    (expecting the tangent block to handle it). If we defer tangents,
  //    compute_tangents becomes false and the index build must run now.
  //
  if (compute_tangents && env.mesh_config.defer_tangent_computation) {
    // Mark for lazy computation — actual tangent work deferred until
    // ComputeDeferredTangents() is called.
    dst.tangent_computation_deferred = true;
    compute_tangents = false;
    DCOUT("Tangent computation deferred.");
  }

  //
  // 8a. Build indices
  //
  // When force_fast_index_build is set, skip tangent computation so that
  // BuildVertexIndicesFastImpl is always used (reproduces WASM code path).
  if (env.mesh_config.force_fast_index_build) {
    compute_tangents = false;
  }

  // Skip fast index build when tangent computation will follow, because
  // BuildVertexIndicesImpl (called after tangent computation) requires
  // facevarying attributes, and BuildVertexIndicesFastImpl converts them
  // to vertex variability.
  if (env.mesh_config.build_vertex_indices && (!is_single_indexable) &&
      !compute_tangents) {
    if (!env.mesh_config.prefer_non_indexed) {
      DCOUT("Build vertex indices");
      //LIGHTUSD_LOG_I("Build vertex indices");

      if (!BuildVertexIndicesFastImpl(dst)) {
        return false;
      }

      is_single_indexable = true;
    }
  }

  //
  // 8b. Compute tangents (immediate, not deferred).
  //
  if (compute_tangents) {
    DCOUT("Compute tangents.");

    // TODO: Support arbitrary slotID
    if (!dst.texcoords.count(0)) {
      PUSH_WARN("texcoord is not assigned to the mesh. Skipping tangent/binormal computation.\n");
    } else {

    // Access existing normals/texcoords from RenderMesh via pointer cast
    // instead of copying into separate vectors (saves ~200MB for large meshes).
    const vec3 *normals_ptr = reinterpret_cast<const vec3 *>(dst.normals.buffer());
    size_t normals_count = dst.normals.vertex_count();
    const vec2 *texcoords_ptr = reinterpret_cast<const vec2 *>(dst.texcoords[0].buffer());
    size_t texcoords_count = dst.texcoords[0].vertex_count();

    std::vector<vec3> tangents;
    std::vector<vec3> binormals;
    std::vector<uint32_t> vertex_indices;

    // When facevarying, expand per-vertex points to per-face-vertex so all
    // arrays (vertices, texcoords, normals) have the same size.
    std::vector<vec3> facevarying_points;
    const std::vector<vec3> *points_ptr = &dst.points;
    if (!is_single_indexable) {
      const auto &fvi = dst.faceVertexIndices();
      facevarying_points.resize(fvi.size());
      for (size_t i = 0; i < fvi.size(); i++) {
        if (fvi[i] < dst.points.size()) {
          facevarying_points[i] = dst.points[fvi[i]];
        }
      }
      points_ptr = &facevarying_points;
    }

    // Dispatch tangent computation based on configured method.
    // Default is Lengyel (fast, lightweight). MikkTSpace/FastMikkTSpace/Hybrid
    // are optional higher-quality methods.
    bool used_mikktspace = false;

    if (env.mesh_config.tangent_method == MeshConverterConfig::TangentComputationMethod::MikkTSpace ||
        env.mesh_config.tangent_method == MeshConverterConfig::TangentComputationMethod::FastMikkTSpace ||
        env.mesh_config.tangent_method == MeshConverterConfig::TangentComputationMethod::Hybrid) {
      std::string mikk_err;
      bool mikktspace_ok = false;
      bool use_fast = (env.mesh_config.tangent_method == MeshConverterConfig::TangentComputationMethod::FastMikkTSpace);
      bool use_hybrid = (env.mesh_config.tangent_method == MeshConverterConfig::TangentComputationMethod::Hybrid);

      // Vectors required only by the plain MikkTSpace entry point (which
      // takes const std::vector&); Fast/Hybrid use the zero-copy pointer API.
      std::vector<value::float3> fv_normals_vec;
      std::vector<value::float2> fv_texcoords_vec;
      if (!use_hybrid && !use_fast && !is_single_indexable) {
        fv_normals_vec.assign(normals_ptr, normals_ptr + normals_count);
        fv_texcoords_vec.assign(texcoords_ptr, texcoords_ptr + texcoords_count);
      }
      if (!is_single_indexable) {
        // Facevarying inputs are already contiguous per-face-vertex arrays;
        // call the zero-copy pointer overloads instead of copying into
        // temporary vectors.
        const value::float3 *pos_ptr =
            reinterpret_cast<const value::float3 *>(points_ptr->data());
        size_t tangent_face_count = dst.faceVertexCounts().size();
        if (use_hybrid) {
          mikktspace_ok = fast_mikkt::ComputeTangentsHybrid(
              pos_ptr, normals_ptr, texcoords_ptr,
              dst.faceVertexCounts().data(), tangent_face_count,
              points_ptr->size(),
              &tangents, &binormals, nullptr, &mikk_err);
        } else if (use_fast) {
          mikktspace_ok = fast_mikkt::ComputeTangentsFastMikkTSpace(
              pos_ptr, normals_ptr, texcoords_ptr,
              dst.faceVertexCounts().data(), tangent_face_count,
              points_ptr->size(),
              &tangents, &binormals, &mikk_err);
        } else {
          mikktspace_ok = ComputeTangentsMikkTSpace(
              *points_ptr, fv_normals_vec, fv_texcoords_vec,
              dst.faceVertexCounts(),
              &tangents, &binormals, &mikk_err);
        }
      } else {
        const auto &fvi = dst.faceVertexIndices();
        std::vector<value::float3> fv_positions(fvi.size());
        fv_normals_vec.resize(fvi.size());
        fv_texcoords_vec.resize(fvi.size());
        for (size_t i = 0; i < fvi.size(); i++) {
          if (fvi[i] < dst.points.size()) fv_positions[i] = dst.points[fvi[i]];
          if (fvi[i] < normals_count) fv_normals_vec[i] = normals_ptr[fvi[i]];
          if (fvi[i] < texcoords_count) fv_texcoords_vec[i] = texcoords_ptr[fvi[i]];
        }
        size_t tangent_face_count = dst.faceVertexCounts().size();
        if (use_hybrid) {
          mikktspace_ok = fast_mikkt::ComputeTangentsHybrid(
              fv_positions.data(), fv_normals_vec.data(),
              fv_texcoords_vec.data(), dst.faceVertexCounts().data(),
              tangent_face_count, fvi.size(),
              &tangents, &binormals, nullptr, &mikk_err);
        } else if (use_fast) {
          mikktspace_ok = fast_mikkt::ComputeTangentsFastMikkTSpace(
              fv_positions.data(), fv_normals_vec.data(),
              fv_texcoords_vec.data(), dst.faceVertexCounts().data(),
              tangent_face_count, fvi.size(),
              &tangents, &binormals, &mikk_err);
        } else {
          mikktspace_ok = ComputeTangentsMikkTSpace(
              fv_positions, fv_normals_vec, fv_texcoords_vec,
              dst.faceVertexCounts(),
              &tangents, &binormals, &mikk_err);
        }
      }

      if (mikktspace_ok) {
        used_mikktspace = true;
      } else {
        DCOUT("MikkTSpace/Hybrid tangent computation failed: " << mikk_err
              << ". Falling back to Lengyel method.");
      }
    }

    if (!used_mikktspace) {
      // Lengyel method (default, or fallback if MikkTSpace failed)
      std::vector<vec2> tc_vec(texcoords_ptr, texcoords_ptr + texcoords_count);
      std::vector<vec3> nm_vec(normals_ptr, normals_ptr + normals_count);
      if (!ComputeTangentsAndBinormals(*points_ptr, dst.faceVertexCounts(),
                                       dst.faceVertexIndices(), tc_vec,
                                       nm_vec, !is_single_indexable, &tangents,
                                       &binormals, &vertex_indices, &local_err,
                                       env.mesh_config.max_vertex_valence,
                                       env.mesh_config.facevarying_to_vertex_eps)) {
        PUSH_ERROR_AND_RETURN("Failed to compute tangents/binormals.");
      }
    }

    // Store tangents/binormals into dst with facevarying variability.
    if (used_mikktspace) {
      // MikkTSpace output is already facevarying
      size_t tan_size;
      if (!safe::n_to_size<vec3>(tangents.size(), &tan_size)) {
        return false;
      }
      dst.tangents.data.resize(tan_size);
      size_t tan_memcpy_size;
      if (!safe::n_to_size<vec3>(tangents.size(), &tan_memcpy_size)) {
        return false;
      }
      memcpy(dst.tangents.data.data(), tangents.data(), tan_memcpy_size);

      size_t bin_size;
      if (!safe::n_to_size<vec3>(binormals.size(), &bin_size)) {
        return false;
      }
      dst.binormals.data.resize(bin_size);
      size_t bin_memcpy_size;
      if (!safe::n_to_size<vec3>(binormals.size(), &bin_memcpy_size)) {
        return false;
      }
      memcpy(dst.binormals.data.data(), binormals.data(), bin_memcpy_size);
    } else {
      // Lengyel output needs index expansion
      size_t vi_tan_size;
      if (!safe::n_to_size<vec3>(vertex_indices.size(), &vi_tan_size)) {
        return false;
      }
      dst.tangents.data.resize(vi_tan_size);
      size_t vi_bin_size;
      if (!safe::n_to_size<vec3>(vertex_indices.size(), &vi_bin_size)) {
        return false;
      }
      dst.binormals.data.resize(vi_bin_size);
      vec3 *dst_tangents = reinterpret_cast<vec3 *>(dst.tangents.data.data());
      vec3 *dst_binormals = reinterpret_cast<vec3 *>(dst.binormals.data.data());
      for (size_t i = 0; i < vertex_indices.size(); i++) {
        dst_tangents[i] = tangents[vertex_indices[i]];
        dst_binormals[i] = binormals[vertex_indices[i]];
      }
    }

    dst.tangents.format = VertexAttributeFormat::Vec3;
    dst.tangents.stride = 0;
    dst.tangents.elementSize = 1;
    dst.tangents.variability = VertexVariability::FaceVarying;

    dst.binormals.format = VertexAttributeFormat::Vec3;
    dst.binormals.stride = 0;
    dst.binormals.elementSize = 1;
    dst.binormals.variability = VertexVariability::FaceVarying;

    // 2. Convert tangents/binormals to vertex variability if needed.
    if (env.mesh_config.build_vertex_indices) {
      if (is_single_indexable) {
        // Normals/texcoords are already vertex variability.
        // Convert facevarying tangents to vertex using the face-vertex indices.
        const std::vector<uint32_t> &fvIdx =
            dst.triangulatedFaceVertexIndices.size()
                ? dst.triangulatedFaceVertexIndices
                : dst.usdFaceVertexIndices;

        size_t numFvs = fvIdx.size();
        uint32_t numPts = static_cast<uint32_t>(dst.points.size());

        // tangents — accumulate then normalize
        if (dst.tangents.vertex_count() == numFvs) {
          const value::float3 *fvT = reinterpret_cast<const value::float3 *>(
              dst.tangents.data.data());
          std::vector<value::float3> vtxT(numPts, {0.0f, 0.0f, 0.0f});
          for (size_t i = 0; i < numFvs; i++) {
            if (fvIdx[i] < numPts) {
              vtxT[fvIdx[i]][0] += fvT[i][0];
              vtxT[fvIdx[i]][1] += fvT[i][1];
              vtxT[fvIdx[i]][2] += fvT[i][2];
            }
          }
          for (uint32_t vi = 0; vi < numPts; vi++) {
            float len = std::sqrt(vtxT[vi][0] * vtxT[vi][0] +
                                  vtxT[vi][1] * vtxT[vi][1] +
                                  vtxT[vi][2] * vtxT[vi][2]);
            if (len > 1e-8f) {
              vtxT[vi][0] /= len;
              vtxT[vi][1] /= len;
              vtxT[vi][2] /= len;
            }
          }
          {
            size_t tang_byte_size;
            if (!safe::mul(vtxT.size(), sizeof(value::float3),
                           &tang_byte_size)) {
              return false;
            }
            dst.tangents.set_buffer(
                reinterpret_cast<const uint8_t *>(vtxT.data()),
                tang_byte_size);
          }
          dst.tangents.variability = VertexVariability::Vertex;
        }

        // binormals — accumulate then normalize
        if (dst.binormals.vertex_count() == numFvs) {
          const value::float3 *fvB = reinterpret_cast<const value::float3 *>(
              dst.binormals.data.data());
          std::vector<value::float3> vtxB(numPts, {0.0f, 0.0f, 0.0f});
          for (size_t i = 0; i < numFvs; i++) {
            if (fvIdx[i] < numPts) {
              vtxB[fvIdx[i]][0] += fvB[i][0];
              vtxB[fvIdx[i]][1] += fvB[i][1];
              vtxB[fvIdx[i]][2] += fvB[i][2];
            }
          }
          for (uint32_t vi = 0; vi < numPts; vi++) {
            float len = std::sqrt(vtxB[vi][0] * vtxB[vi][0] +
                                  vtxB[vi][1] * vtxB[vi][1] +
                                  vtxB[vi][2] * vtxB[vi][2]);
            if (len > 1e-8f) {
              vtxB[vi][0] /= len;
              vtxB[vi][1] /= len;
              vtxB[vi][2] /= len;
            }
          }
          {
            size_t bin_byte_size;
            if (!safe::mul(vtxB.size(), sizeof(value::float3),
                           &bin_byte_size)) {
              return false;
            }
            dst.binormals.set_buffer(
                reinterpret_cast<const uint8_t *>(vtxB.data()),
                bin_byte_size);
          }
          dst.binormals.variability = VertexVariability::Vertex;
        }
      } else {
        // All attributes are still facevarying - use full rebuild.
        if (!BuildVertexIndicesImpl(dst, env.mesh_config.max_vertex_valence,
                                     env.mesh_config.facevarying_to_vertex_eps)) {
          return false;
        }
        is_single_indexable = true;
      }
    }
  } // else (texcoords available)
  } // if (compute_tangents)

  // Quantize tangents if a packed format is configured.
  if (!dst.tangents.empty() && !dst.binormals.empty()) {
    QuantizeMeshTangents(dst, env.mesh_config.tangent_storage);
  }

  // Quantize normals if a packed format is configured.
  if (!dst.normals.empty()) {
    QuantizeMeshNormals(dst, env.mesh_config.normal_storage);
  }

  dst.is_single_indexable = is_single_indexable;

  dst.prim_name = mesh.name;
  dst.abs_path = abs_prim_path.full_path_name();
  dst.display_name = mesh.metas().has_displayName() ? mesh.metas().get_displayName() : "";

  //
  // Check for MeshLightAPI - if present, mark this mesh as an area light
  //
  const auto &prim_metas = mesh.metas();
  if (prim_metas.has_apiSchemas()) {
    const auto api_schemas = prim_metas.get_apiSchemas();
    bool has_meshlight_api = false;

    for (const auto &schema_pair : api_schemas.names) {
      if (schema_pair.first == APISchemas::APIName::MeshLightAPI) {
        has_meshlight_api = true;
        break;
      }
    }

    if (has_meshlight_api) {
      DCOUT("Mesh has MeshLightAPI: " << abs_prim_path.full_path_name());

      dst.is_area_light = true;

      // Extract MeshLightAPI properties from mesh
      // MeshLightAPI inherits from LightAPI, which uses "inputs:" prefix

      // color
      if (auto it = mesh.props.find("inputs:color"); it != mesh.props.end()) {
        const Property &prop = it->second;
        const Attribute &attr = prop.get_attribute();
        const primvar::PrimVar &pvar = attr.get_var();
        if (auto val = pvar.get_value<value::color3f>()) {
          dst.light_color[0] = val.value()[0];
          dst.light_color[1] = val.value()[1];
          dst.light_color[2] = val.value()[2];
        }
      }

      // intensity
      if (auto it = mesh.props.find("inputs:intensity");
          it != mesh.props.end()) {
        const Property &prop = it->second;
        const Attribute &attr = prop.get_attribute();
        const primvar::PrimVar &pvar = attr.get_var();
        if (auto val = pvar.get_value<float>()) {
          dst.light_intensity = val.value();
        }
      }

      // exposure (optional)
      if (auto it = mesh.props.find("inputs:exposure"); it != mesh.props.end()) {
        const Property &prop = it->second;
        const Attribute &attr = prop.get_attribute();
        const primvar::PrimVar &pvar = attr.get_var();
        if (auto val = pvar.get_value<float>()) {
          dst.light_exposure = val.value();
        }
      }

      // normalize
      if (auto it = mesh.props.find("inputs:normalize");
          it != mesh.props.end()) {
        const Property &prop = it->second;
        const Attribute &attr = prop.get_attribute();
        const primvar::PrimVar &pvar = attr.get_var();
        if (auto val = pvar.get_value<bool>()) {
          dst.light_normalize = val.value();
        }
      }

      // materialSyncMode
      if (auto it = mesh.props.find("inputs:materialSyncMode");
          it != mesh.props.end()) {
        const Property &prop = it->second;
        const Attribute &attr = prop.get_attribute();
        const primvar::PrimVar &pvar = attr.get_var();
        if (auto val = pvar.get_value<value::token>()) {
          dst.light_material_sync_mode = val.value().str();
        }
      }

      // Set default if not specified
      if (dst.light_material_sync_mode.empty()) {
        dst.light_material_sync_mode = "materialGlowTintsLight";  // USD default
      }

      DCOUT("  Area light properties:"
            << " color=(" << dst.light_color[0] << "," << dst.light_color[1] << "," << dst.light_color[2] << ")"
            << " intensity=" << dst.light_intensity
            << " exposure=" << dst.light_exposure
            << " normalize=" << dst.light_normalize
            << " materialSyncMode=" << dst.light_material_sync_mode);
    }
  }


  // Free source GeomMesh large arrays to reduce peak memory.
  // The const_cast is safe here: ConvertMesh has finished reading all mesh
  // data, and the GeomMesh attribute data is no longer needed.
  if (env.mesh_config.lowmem) {
    auto *pmesh = const_cast<GeomMesh *>(&mesh);

    // Core geometry
    pmesh->points.set_value({});
    pmesh->normals.set_value({});
    pmesh->faceVertexIndices.set_value({});
    pmesh->faceVertexCounts.set_value({});
    pmesh->velocities.set_value({});

    // SubD attributes (not used in ConvertMesh, but may be large)
    pmesh->cornerIndices.set_value({});
    pmesh->cornerSharpnesses.set_value({});
    pmesh->creaseIndices.set_value({});
    pmesh->creaseLengths.set_value({});
    pmesh->creaseSharpnesses.set_value({});
    pmesh->holeIndices.set_value({});

    // All primvar data (primvars:normals, primvars:st, primvars:displayColor,
    // skel:jointIndices, skel:jointWeights, etc.) — already copied to RenderMesh.
    { std::map<std::string, Property> tmp; pmesh->props.swap(tmp); }
  }

  (*dstMesh) = std::move(dst);

  return true;
}

bool RenderSceneConverter::ConvertMeshWorkItem(
    const RenderSceneConverterEnv &env, const MeshWorkItem &item,
    RenderMesh *dst) {
  if (!dst) {
    PUSH_ERROR_AND_RETURN("`dst` mesh pointer is nullptr");
  }

  // Empty subset/blendshape inputs shared by the parametric kinds.
  const std::vector<const GeomSubset *> no_subsets;
  const std::vector<std::pair<std::string, const BlendShape *>> no_blendshapes;

  const std::vector<const GeomSubset *> &subsets =
      item.material_subsets.empty() ? no_subsets : item.material_subsets;
  const std::vector<std::pair<std::string, const BlendShape *>> &blendshapes =
      item.blendshapes.empty() ? no_blendshapes : item.blendshapes;

  switch (item.kind) {
    case MeshWorkItem::Kind::Mesh:
      return ConvertMesh(env, item.abs_path,
                         *static_cast<const GeomMesh *>(item.prim),
                         item.material_path, item.subset_material_path_map,
                         materialMap, subsets, blendshapes, dst);
    case MeshWorkItem::Kind::Cube:
      return ConvertCube(env, item.abs_path,
                         *static_cast<const GeomCube *>(item.prim),
                         item.material_path, item.subset_material_path_map,
                         materialMap, subsets, blendshapes, dst);
    case MeshWorkItem::Kind::Sphere:
      return ConvertSphere(env, item.abs_path,
                           *static_cast<const GeomSphere *>(item.prim),
                           item.material_path, item.subset_material_path_map,
                           materialMap, subsets, blendshapes, dst);
    case MeshWorkItem::Kind::Cylinder:
      return ConvertCylinder(env, item.abs_path,
                             *static_cast<const GeomCylinder *>(item.prim),
                             item.material_path, item.subset_material_path_map,
                             materialMap, subsets, blendshapes, dst);
    case MeshWorkItem::Kind::Cone:
      return ConvertCone(env, item.abs_path,
                         *static_cast<const GeomCone *>(item.prim),
                         item.material_path, item.subset_material_path_map,
                         materialMap, subsets, blendshapes, dst);
    case MeshWorkItem::Kind::Capsule:
      return ConvertCapsule(env, item.abs_path,
                            *static_cast<const GeomCapsule *>(item.prim),
                            item.material_path, item.subset_material_path_map,
                            materialMap, subsets, blendshapes, dst);
    case MeshWorkItem::Kind::Plane:
      return ConvertPlane(env, item.abs_path,
                          *static_cast<const GeomPlane *>(item.prim),
                          item.material_path, item.subset_material_path_map,
                          materialMap, subsets, blendshapes, dst);
  }

  PUSH_ERROR_AND_RETURN("Unknown MeshWorkItem kind.");
}

// static
bool RenderSceneConverter::ComputeDeferredTangents(
    RenderMesh *mesh,
    MeshConverterConfig::TangentComputationMethod method,
    MeshConverterConfig::TangentStorageFormat storage,
    std::string *err) {
  if (!mesh) {
    if (err) *err = "mesh is nullptr.";
    return false;
  }

  if (!mesh->tangent_computation_deferred) {
    // Already computed or not deferred — nothing to do.
    return true;
  }

  if (mesh->normals.empty()) {
    if (err) *err = "Cannot compute tangents: normals not available.";
    return false;
  }

  if (!mesh->texcoords.count(0)) {
    if (err) *err = "Cannot compute tangents: texcoord slot 0 not available.";
    return false;
  }

  const auto &fvi = mesh->faceVertexIndices();
  const auto &fvc = mesh->faceVertexCounts();
  const auto &uv = mesh->texcoords.at(0);
  if (uv.format != VertexAttributeFormat::Vec2 || uv.elementSize != 1) {
    if (err) *err = "Cannot compute tangents: unsupported UV format.";
    return false;
  }
  const size_t uv_stride = uv.stride ? uv.stride : sizeof(vec2);
  if (uv_stride < sizeof(vec2) || uv.data.size() < sizeof(vec2)) {
    if (err) *err = "Cannot compute tangents: invalid UV buffer.";
    return false;
  }
  // Expand each input independently: equal counts do not imply equal indexing.
  std::vector<vec3> fv_points(fvi.size()), fv_normals(fvi.size());
  std::vector<vec2> fv_texcoords(fvi.size());
  size_t corner = 0;
  for (size_t face = 0; face < fvc.size(); ++face) {
    if (fvc[face] < 3 || fvc[face] > fvi.size() - corner) {
      if (err) *err = "Cannot compute tangents: invalid face topology.";
      return false;
    }
    for (uint32_t v = 0; v < fvc[face]; ++v, ++corner) {
      size_t normal_index = 0, uv_index = 0;
      if (fvi[corner] >= mesh->points.size() ||
          !MeshAttributeCornerIndex(*mesh, mesh->normals, corner, face, &normal_index) ||
          !ReadNormalAttribute(mesh->normals, normal_index, &fv_normals[corner]) ||
          !MeshAttributeCornerIndex(*mesh, uv, corner, face, &uv_index) ||
          uv_index > (uv.data.size() - sizeof(vec2)) / uv_stride) {
        if (err) *err = "Cannot compute tangents: invalid attribute index or buffer.";
        return false;
      }
      fv_points[corner] = mesh->points[fvi[corner]];
      memcpy(&fv_texcoords[corner], uv.data.data() + uv_index * uv_stride, sizeof(vec2));
    }
  }
  if (corner != fvi.size() || corner == 0) {
    if (err) *err = "Cannot compute tangents: invalid face topology.";
    return false;
  }
  std::vector<vec3> tangents, binormals;
  std::vector<uint32_t> vertex_indices;
  bool used_mikktspace = false;
  if (method == MeshConverterConfig::TangentComputationMethod::MikkTSpace ||
      method == MeshConverterConfig::TangentComputationMethod::FastMikkTSpace ||
      method == MeshConverterConfig::TangentComputationMethod::Hybrid) {
    std::string mikk_err;
    if (method == MeshConverterConfig::TangentComputationMethod::Hybrid) {
      used_mikktspace = fast_mikkt::ComputeTangentsHybrid(
          fv_points, fv_normals, fv_texcoords, fvc, &tangents, &binormals, nullptr, &mikk_err);
    } else if (method == MeshConverterConfig::TangentComputationMethod::FastMikkTSpace) {
      used_mikktspace = fast_mikkt::ComputeTangentsFastMikkTSpace(
          fv_points, fv_normals, fv_texcoords, fvc, &tangents, &binormals, &mikk_err);
    } else {
      used_mikktspace = ComputeTangentsMikkTSpace(
          fv_points, fv_normals, fv_texcoords, fvc, &tangents, &binormals, &mikk_err);
    }
    if (!used_mikktspace && err) *err = "MikkTSpace/Hybrid tangent computation failed: " + mikk_err + ". Falling back to Lengyel.";
  }
  if (!used_mikktspace) {
    std::string lengyel_err;
    if (!ComputeTangentsAndBinormals(fv_points, fvc, fvi, fv_texcoords,
                                     fv_normals, true, &tangents, &binormals,
                                     &vertex_indices, &lengyel_err)) {
      if (err) *err = "Lengyel tangent computation failed: " + lengyel_err;
      return false;
    }
  }

  // Store results
  if (used_mikktspace) {
    size_t tan_size;
    if (!safe::n_to_size<vec3>(tangents.size(), &tan_size)) {
      return false;
    }
    mesh->tangents.data.resize(tan_size);
    size_t tan_memcpy_size;
    if (!safe::n_to_size<vec3>(tangents.size(), &tan_memcpy_size)) {
      return false;
    }
    memcpy(mesh->tangents.data.data(), tangents.data(), tan_memcpy_size);

    size_t bin_size;
    if (!safe::n_to_size<vec3>(binormals.size(), &bin_size)) {
      return false;
    }
    mesh->binormals.data.resize(bin_size);
    size_t bin_memcpy_size;
    if (!safe::n_to_size<vec3>(binormals.size(), &bin_memcpy_size)) {
      return false;
    }
    memcpy(mesh->binormals.data.data(), binormals.data(), bin_memcpy_size);
  } else {
    // Lengyel output needs index expansion
    size_t vi_tan_size;
    if (!safe::n_to_size<vec3>(vertex_indices.size(), &vi_tan_size)) {
      return false;
    }
    mesh->tangents.data.resize(vi_tan_size);
    size_t vi_bin_size;
    if (!safe::n_to_size<vec3>(vertex_indices.size(), &vi_bin_size)) {
      return false;
    }
    mesh->binormals.data.resize(vi_bin_size);
    vec3 *dst_tangents = reinterpret_cast<vec3 *>(mesh->tangents.data.data());
    vec3 *dst_binormals = reinterpret_cast<vec3 *>(mesh->binormals.data.data());
    for (size_t i = 0; i < vertex_indices.size(); i++) {
      dst_tangents[i] = tangents[vertex_indices[i]];
      dst_binormals[i] = binormals[vertex_indices[i]];
    }
  }

  mesh->tangents.format = VertexAttributeFormat::Vec3;
  mesh->tangents.stride = 0;
  mesh->tangents.elementSize = 1;
  mesh->tangents.variability = VertexVariability::FaceVarying;

  mesh->binormals.format = VertexAttributeFormat::Vec3;
  mesh->binormals.stride = 0;
  mesh->binormals.elementSize = 1;
  mesh->binormals.variability = VertexVariability::FaceVarying;

  // Quantize tangents if a packed format is requested.
  if (!mesh->tangents.empty() && !mesh->binormals.empty()) {
    if (!QuantizeMeshTangents(*mesh, storage)) {
      if (err) *err = "Cannot quantize tangents: invalid normal data.";
      return false;
    }
  }

  mesh->tangent_computation_deferred = false;
  return true;
}

}  // namespace tydra
}  // namespace lightusd
