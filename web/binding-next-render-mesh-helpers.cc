// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "binding-next-render.hh"
namespace lightusd {
namespace web_next {
tr::MeshConfig::TangentComputationMethod RenderStream::tangentMethod_() const {
    if (tangent_method_ == "mikk" || tangent_method_ == "mikktspace") {
      return tr::MeshConfig::TangentComputationMethod::MikkTSpace;
    }
    if (tangent_method_ == "fast" || tangent_method_ == "fastmikk" ||
        tangent_method_ == "fastmikktspace") {
      return tr::MeshConfig::TangentComputationMethod::FastMikkTSpace;
    }
    if (tangent_method_ == "lengyel") {
      return tr::MeshConfig::TangentComputationMethod::Lengyel;
    }
    return tr::MeshConfig::TangentComputationMethod::Hybrid;
  }

bool RenderStream::computeScratchTangents_() {
    s_tangents_.clear();
    const size_t vertex_count = s_points_.size() / 3;
    if (vertex_count == 0 || s_normals_.size() != vertex_count * 3 ||
        s_uv_.size() != vertex_count * 2) {
      return false;
    }

    // A triangle soup has no shared vertices, so accumulating a tangent and
    // bitangent for every vertex is unnecessary. Write the final frame for
    // each triangle directly, avoiding two additional 3-float arrays. This is
    // a substantial peak-memory reduction for face-varying meshes.
    if (s_indices_.empty()) {
      if (vertex_count >
              (std::numeric_limits<size_t>::max)() / (4 * sizeof(float)) ||
          !tr::ProbeAlloc(vertex_count * 4 * sizeof(float))) {
        return false;
      }
      s_tangents_.assign(vertex_count * 4, 0.0f);
      for (size_t i = 0; i + 2 < vertex_count; i += 3) {
        const float *p0 = &s_points_[i * 3];
        const float *p1 = &s_points_[(i + 1) * 3];
        const float *p2 = &s_points_[(i + 2) * 3];
        const float *u0 = &s_uv_[i * 2];
        const float *u1 = &s_uv_[(i + 1) * 2];
        const float *u2 = &s_uv_[(i + 2) * 2];
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1],
                             p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1],
                             p2[2] - p0[2]};
        const float du1 = u1[0] - u0[0];
        const float dv1 = u1[1] - u0[1];
        const float du2 = u2[0] - u0[0];
        const float dv2 = u2[1] - u0[1];
        const float det = du1 * dv2 - du2 * dv1;
        const float inv_det = std::fabs(det) > 1.0e-12f ? 1.0f / det : 0.0f;
        const float tangent[3] = {
            (dv2 * e1[0] - dv1 * e2[0]) * inv_det,
            (dv2 * e1[1] - dv1 * e2[1]) * inv_det,
            (dv2 * e1[2] - dv1 * e2[2]) * inv_det};
        const float bitangent[3] = {
            (du1 * e2[0] - du2 * e1[0]) * inv_det,
            (du1 * e2[1] - du2 * e1[1]) * inv_det,
            (du1 * e2[2] - du2 * e1[2]) * inv_det};
        for (size_t corner = 0; corner < 3; ++corner) {
          const size_t vertex = i + corner;
          const float *normal = &s_normals_[vertex * 3];
          const float ndt = normal[0] * tangent[0] +
                            normal[1] * tangent[1] +
                            normal[2] * tangent[2];
          float tx = tangent[0] - normal[0] * ndt;
          float ty = tangent[1] - normal[1] * ndt;
          float tz = tangent[2] - normal[2] * ndt;
          const float length = std::sqrt(tx * tx + ty * ty + tz * tz);
          if (length > 1.0e-12f) {
            tx /= length;
            ty /= length;
            tz /= length;
          } else {
            tx = 1.0f;
            ty = 0.0f;
            tz = 0.0f;
          }
          const float cx = normal[1] * tz - normal[2] * ty;
          const float cy = normal[2] * tx - normal[0] * tz;
          const float cz = normal[0] * ty - normal[1] * tx;
          const float handedness =
              (cx * bitangent[0] + cy * bitangent[1] +
               cz * bitangent[2]) < 0.0f
                  ? -1.0f
                  : 1.0f;
          s_tangents_[vertex * 4 + 0] = tx;
          s_tangents_[vertex * 4 + 1] = ty;
          s_tangents_[vertex * 4 + 2] = tz;
          s_tangents_[vertex * 4 + 3] = handedness;
        }
      }
      return true;
    }

    // Accumulate directly into the final xyzw array. Handedness is computed
    // in a second triangle pass after tangent normalization, avoiding the old
    // 24B/vertex tangent + bitangent temporaries.
    if (vertex_count >
            (std::numeric_limits<size_t>::max)() / (4 * sizeof(float)) ||
        !tr::ProbeAlloc(vertex_count * 4 * sizeof(float))) {
      return false;
    }
    s_tangents_.assign(vertex_count * 4, 0.0f);

    auto visitTri = [&](uint32_t i0, uint32_t i1, uint32_t i2,
                        bool handedness_pass) {
      if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) {
        return;
      }
      const float *p0 = &s_points_[size_t(i0) * 3];
      const float *p1 = &s_points_[size_t(i1) * 3];
      const float *p2 = &s_points_[size_t(i2) * 3];
      const float *u0 = &s_uv_[size_t(i0) * 2];
      const float *u1 = &s_uv_[size_t(i1) * 2];
      const float *u2 = &s_uv_[size_t(i2) * 2];
      const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
      const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
      const float du1 = u1[0] - u0[0];
      const float dv1 = u1[1] - u0[1];
      const float du2 = u2[0] - u0[0];
      const float dv2 = u2[1] - u0[1];
      const float det = du1 * dv2 - du2 * dv1;
      if (std::fabs(det) <= 1.0e-12f) return;
      const float r = 1.0f / det;
      const float sdir[3] = {(dv2 * e1[0] - dv1 * e2[0]) * r,
                             (dv2 * e1[1] - dv1 * e2[1]) * r,
                             (dv2 * e1[2] - dv1 * e2[2]) * r};
      const float tdir[3] = {(du1 * e2[0] - du2 * e1[0]) * r,
                             (du1 * e2[1] - du2 * e1[1]) * r,
                             (du1 * e2[2] - du2 * e1[2]) * r};
      const uint32_t ids[3] = {i0, i1, i2};
      for (uint32_t id : ids) {
        float *out = &s_tangents_[size_t(id) * 4];
        if (!handedness_pass) {
          out[0] += sdir[0];
          out[1] += sdir[1];
          out[2] += sdir[2];
        } else {
          const float *normal = &s_normals_[size_t(id) * 3];
          const float cx = normal[1] * out[2] - normal[2] * out[1];
          const float cy = normal[2] * out[0] - normal[0] * out[2];
          const float cz = normal[0] * out[1] - normal[1] * out[0];
          out[3] += cx * tdir[0] + cy * tdir[1] + cz * tdir[2];
        }
      }
    };

    auto visitAllTriangles = [&](bool handedness_pass) {
      for (size_t i = 0; i + 2 < s_indices_.size(); i += 3) {
        visitTri(s_indices_[i], s_indices_[i + 1], s_indices_[i + 2],
                 handedness_pass);
      }
    };
    visitAllTriangles(false);

    for (size_t v = 0; v < vertex_count; ++v) {
      const float *n = &s_normals_[v * 3];
      float *tv = &s_tangents_[v * 4];
      const float ndt = n[0] * tv[0] + n[1] * tv[1] + n[2] * tv[2];
      float tx = tv[0] - n[0] * ndt;
      float ty = tv[1] - n[1] * ndt;
      float tz = tv[2] - n[2] * ndt;
      const float len = std::sqrt(tx * tx + ty * ty + tz * tz);
      if (len > 1.0e-12f) {
        tx /= len;
        ty /= len;
        tz /= len;
      } else {
        tx = 1.0f;
        ty = 0.0f;
        tz = 0.0f;
      }
      s_tangents_[v * 4 + 0] = tx;
      s_tangents_[v * 4 + 1] = ty;
      s_tangents_[v * 4 + 2] = tz;
    }
    visitAllTriangles(true);
    for (size_t v = 0; v < vertex_count; ++v) {
      s_tangents_[v * 4 + 3] =
          s_tangents_[v * 4 + 3] < 0.0f ? -1.0f : 1.0f;
    }
    return true;
  }

bool RenderStream::readFloatArray_(const lightusd::next::UsdPrim &prim, const char *name,
                       tr::ValueArrayRead<float> *out) {
    if (!tr::ReadFloatArray(prim, name, 0.0, out)) return false;
    if (out->view.borrowed) {
      stats_.geometry_borrowed_bytes += out->view.size_bytes();
    } else {
      stats_.geometry_materialized_bytes += out->view.size_bytes();
    }
    return true;
  }

bool RenderStream::readIntArray_(const lightusd::next::UsdPrim &prim, const char *name,
                     tr::ValueArrayRead<int32_t> *out) {
    if (!tr::ReadIntArray(prim, name, 0.0, out)) return false;
    if (out->view.borrowed) {
      stats_.geometry_borrowed_bytes += out->view.size_bytes();
    } else {
      stats_.geometry_materialized_bytes += out->view.size_bytes();
    }
    return true;
  }

bool RenderStream::matBool_(const lightusd::next::UsdPrim &prim, const char *name,
                       bool fallback) {
    const lightusd::next::Value *v = prim.GetPropertyValue(name);
    if (!v) return fallback;
    if (const bool *b = v->as_bool()) return *b;
    return fallback;
  }

bool RenderStream::pointsArePlanar_(const std::vector<float> &points) {
    const size_t count = points.size() / 3;
    if (count < 3) return false;

    float lo[3] = {points[0], points[1], points[2]};
    float hi[3] = {points[0], points[1], points[2]};
    for (size_t i = 1; i < count; ++i) {
      for (size_t axis = 0; axis < 3; ++axis) {
        const float value = points[i * 3 + axis];
        lo[axis] = (std::min)(lo[axis], value);
        hi[axis] = (std::max)(hi[axis], value);
      }
    }
    const float scale = (std::max)({hi[0] - lo[0], hi[1] - lo[1],
                                    hi[2] - lo[2]});
    if (!(scale > 0.0f)) return false;
    const float epsilon = (std::max)(scale * 1.0e-5f, 1.0e-7f);
    const float epsilon_sq = epsilon * epsilon;

    const float p0[3] = {points[0], points[1], points[2]};
    size_t p1_index = count;
    for (size_t i = 1; i < count; ++i) {
      const float x = points[i * 3] - p0[0];
      const float y = points[i * 3 + 1] - p0[1];
      const float z = points[i * 3 + 2] - p0[2];
      if (x * x + y * y + z * z > epsilon_sq) {
        p1_index = i;
        break;
      }
    }
    if (p1_index == count) return false;
    const float edge[3] = {points[p1_index * 3] - p0[0],
                           points[p1_index * 3 + 1] - p0[1],
                           points[p1_index * 3 + 2] - p0[2]};
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float normal_length = 0.0f;
    for (size_t i = 1; i < count; ++i) {
      const float other[3] = {points[i * 3] - p0[0],
                              points[i * 3 + 1] - p0[1],
                              points[i * 3 + 2] - p0[2]};
      normal[0] = edge[1] * other[2] - edge[2] * other[1];
      normal[1] = edge[2] * other[0] - edge[0] * other[2];
      normal[2] = edge[0] * other[1] - edge[1] * other[0];
      normal_length = std::sqrt(normal[0] * normal[0] +
                                normal[1] * normal[1] +
                                normal[2] * normal[2]);
      if (normal_length > epsilon_sq) break;
    }
    if (!(normal_length > epsilon_sq)) return false;
    normal[0] /= normal_length;
    normal[1] /= normal_length;
    normal[2] /= normal_length;
    for (size_t i = 1; i < count; ++i) {
      const float distance =
          (points[i * 3] - p0[0]) * normal[0] +
          (points[i * 3 + 1] - p0[1]) * normal[1] +
          (points[i * 3 + 2] - p0[2]) * normal[2];
      if (std::abs(distance) > epsilon) return false;
    }
    return true;
  }

bool RenderStream::effectiveDoubleSided_(const lightusd::next::UsdPrim &prim,
                             int32_t material_id,
                             const std::vector<float> &points) const {
    // An authored USD opinion always wins, including an explicit false.
    if (prim.HasAuthoredProperty("doubleSided")) {
      return matBool_(prim, "doubleSided", false);
    }
    if (material_id < 0 ||
        static_cast<size_t>(material_id) >= materials_.size()) {
      return false;
    }
    // Some DCC exporters omit doubleSided on effect cards even though an
    // opacity-mapped, zero-thickness mesh is semantically a billboard. Infer
    // two-sided rendering only for that narrow case; opaque and volumetric
    // geometry retains the USD default of back-face culling.
    const MaterialRecord &material =
        materials_[static_cast<size_t>(material_id)];
    return !material.opacity_texture.empty() && pointsArePlanar_(points);
  }

std::vector<uint32_t> RenderStream::faceTriangleStarts_(
      const std::vector<int32_t> &fvc) {
    std::vector<uint32_t> starts;
    starts.reserve(fvc.size() + 1);
    uint32_t cursor = 0;
    for (int32_t n : fvc) {
      starts.push_back(cursor);
      if (n >= 3) cursor += static_cast<uint32_t>(n - 2);
    }
    starts.push_back(cursor);
    return starts;
  }

std::vector<int32_t> RenderStream::matIntStatic_(
      const lightusd::next::UsdPrim &prim, const char *name) {
    const lightusd::next::Value *v = prim.GetPropertyValue(name);
    if (!v) return {};
    lightusd::next::Value tmp = v->materialized_copy();
    std::vector<int32_t> *a = tmp.as_int_array();
    return a ? std::move(*a) : std::vector<int32_t>{};
  }

void RenderStream::computeNormals_(const std::vector<float> &pos,
                              const std::vector<uint32_t> &idx,
                              std::vector<float> &out) {
    out.assign(pos.size(), 0.0f);
    const size_t nv = pos.size() / 3;
    auto addTri = [&](uint32_t a, uint32_t b, uint32_t c) {
      if (a >= nv || b >= nv || c >= nv) return;
      const float ex1 = pos[b * 3] - pos[a * 3], ey1 = pos[b * 3 + 1] - pos[a * 3 + 1], ez1 = pos[b * 3 + 2] - pos[a * 3 + 2];
      const float ex2 = pos[c * 3] - pos[a * 3], ey2 = pos[c * 3 + 1] - pos[a * 3 + 1], ez2 = pos[c * 3 + 2] - pos[a * 3 + 2];
      const float nx = ey1 * ez2 - ez1 * ey2, ny = ez1 * ex2 - ex1 * ez2, nz = ex1 * ey2 - ey1 * ex2;
      for (uint32_t vi : {a, b, c}) { out[vi * 3] += nx; out[vi * 3 + 1] += ny; out[vi * 3 + 2] += nz; }
    };
    if (!idx.empty()) {
      for (size_t t = 0; t + 2 < idx.size(); t += 3) addTri(idx[t], idx[t + 1], idx[t + 2]);
    } else {
      for (uint32_t v = 0; v + 2 < nv; v += 3) addTri(v, v + 1, v + 2);
    }
    for (size_t i = 0; i < nv; ++i) {
      float x = out[i * 3], y = out[i * 3 + 1], z = out[i * 3 + 2];
      float l = std::sqrt(x * x + y * y + z * z);
      if (l > 0) { out[i * 3] = x / l; out[i * 3 + 1] = y / l; out[i * 3 + 2] = z / l; }
      else { out[i * 3 + 2] = 1.0f; }
    }
  }

emscripten::val RenderStream::heapF_(const std::vector<float> &v, int comps) const {
    emscripten::val d = emscripten::val::object();
    d.set("ptr", static_cast<double>(reinterpret_cast<uintptr_t>(v.data())));
    d.set("length", static_cast<double>(v.size()));
    d.set("comps", comps);
    d.set("dtype", std::string("f32"));
    d.set("byteLength", static_cast<double>(v.size() * sizeof(float)));
    return d;
  }

emscripten::val RenderStream::heapU32_(const std::vector<uint32_t> &v) const {
    emscripten::val d = emscripten::val::object();
    d.set("ptr", static_cast<double>(reinterpret_cast<uintptr_t>(v.data())));
    d.set("length", static_cast<double>(v.size()));
    d.set("comps", 1);
    d.set("dtype", std::string("u32"));
    d.set("byteLength", static_cast<double>(v.size() * sizeof(uint32_t)));
    return d;
  }

emscripten::val RenderStream::heapU16_(const std::vector<uint16_t> &v) const {
    emscripten::val d = emscripten::val::object();
    d.set("ptr", static_cast<double>(reinterpret_cast<uintptr_t>(v.data())));
    d.set("length", static_cast<double>(v.size()));
    d.set("comps", 1);
    d.set("dtype", std::string("u16"));
    d.set("byteLength", static_cast<double>(v.size() * sizeof(uint16_t)));
    return d;
  }

emscripten::val RenderStream::arr3_(const float *c) {
    emscripten::val a = emscripten::val::array();
    a.call<void>("push", c[0]);
    a.call<void>("push", c[1]);
    a.call<void>("push", c[2]);
    return a;
  }

std::array<double, 16> RenderStream::identityMatrix_() {
    return {1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0};
  }

std::array<double, 16> RenderStream::multiplyMatrix_(
      const std::array<double, 16> &a, const std::array<double, 16> &b) {
    std::array<double, 16> r{};
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        double v = 0.0;
        for (int k = 0; k < 4; ++k) {
          v += a[static_cast<size_t>(row * 4 + k)] *
               b[static_cast<size_t>(k * 4 + col)];
        }
        r[static_cast<size_t>(row * 4 + col)] = v;
      }
    }
    return r;
  }
}  // namespace web_next
}  // namespace lightusd
