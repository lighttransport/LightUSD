// SPDX-License-Identifier: Apache-2.0
#include "binding-geometry.h"
#include "tsd/tinysubdiv.hh"
#include <emscripten/emscripten.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
namespace {
EM_JS(int, NextSubdivEmit,
      (uint32_t callback_id, const float* positions, uint32_t position_count,
       const float* normals, uint32_t normal_count,
       const uint32_t* indices, uint32_t index_count,
       const uint32_t* face_source, uint32_t face_count,
       const float* uvs, uint32_t uv_count,
       uint32_t num_vertices, uint32_t num_faces, uint32_t batch_index), {
  return Module['__lightusdNextSubdivEmit'](
      callback_id, Number(positions), position_count, Number(normals),
      normal_count, Number(indices), index_count, Number(face_source),
      face_count, Number(uvs), uv_count, num_vertices, num_faces, batch_index);
});
class SubdivStreamer {
 public:
  int refineStream(const float* points, uint32_t point_values,
                   const uint32_t* fvc, uint32_t face_count,
                   const uint32_t* fvi, uint32_t index_count,
                   const float* uv_values, uint32_t uv_value_count,
                   const uint32_t* uv_indices, uint32_t uv_index_count,
                   const lu_geometry_subdiv_options& options,
                   uint32_t callback_id) {
    namespace tsd = lightusd::tsd;
    error_.clear();
    if ((point_values % 3) != 0) {
      error_ = "points length must be a multiple of 3";
      return 1;
    }
    if (!face_count || !index_count) {
      error_ = "empty mesh";
      return 1;
    }
    const bool has_uv = uv_value_count >= 2 && (uv_value_count % 2) == 0;

    tsd::MeshView mesh;
    mesh.points = points;
    mesh.num_points = point_values / 3;
    mesh.face_vertex_counts = fvc;
    mesh.num_faces = face_count;
    mesh.face_vertex_indices = fvi;
    mesh.num_face_vertex_indices = index_count;

    tsd::FVarChannelView uvchan;
    if (has_uv) {
      uvchan.values = uv_values;
      uvchan.num_values = uv_value_count / 2;
      uvchan.indices = uv_index_count ? uv_indices : nullptr;
      uvchan.stride = 2;
      uvchan.interpolation = (options.uv_interpolation == 1)
                                 ? tsd::FVarLinearInterpolation::CornersPlus1
                                 : tsd::FVarLinearInterpolation::All;
    }

    tsd::Options opts;
    opts.scheme = (options.scheme == 1)   ? tsd::Scheme::Loop
                  : (options.scheme == 2) ? tsd::Scheme::Bilinear
                                  : tsd::Scheme::CatmullClark;
    opts.boundary = (options.boundary == 1)   ? tsd::BoundaryInterpolation::EdgeOnly
                    : (options.boundary == 2) ? tsd::BoundaryInterpolation::None
                                      : tsd::BoundaryInterpolation::EdgeAndCorner;
    opts.level = options.level;
    opts.remove_holes = true;

    tsd::StreamOptions so;
    so.batch_faces = (options.batch_faces > 0) ? uint32_t(options.batch_faces) : 4096u;
    so.emit_triangles = true;
    so.want_normals = options.want_normals != 0;
    so.dedup_within_batch = true;
    so.block_faces = (options.block_faces > 0) ? uint32_t(options.block_faces) : 0u;
    so.halo_rings = (options.halo_rings > 0) ? uint32_t(options.halo_rings) : 0u;

    struct SinkCtx { uint32_t callback_id; bool want_normals; };
    SinkCtx ctx{callback_id, so.want_normals};

    auto sink = [](void *user, const tsd::StreamBatch *b) -> bool {
      SinkCtx *c = static_cast<SinkCtx *>(user);
      const bool have_normals = c->want_normals && b->normals;
      const bool have_uv = b->num_fvar == 1 && b->fvar[0].values;
      return NextSubdivEmit(
          c->callback_id, b->positions, b->num_vertices * 3,
          have_normals ? b->normals : nullptr,
          have_normals ? b->num_vertices * 3 : 0,
          b->indices, b->num_indices, b->face_source, b->num_faces,
          have_uv ? b->fvar[0].values : nullptr,
          have_uv ? b->num_indices * 2 : 0,
          b->num_vertices, b->num_faces, b->batch_index) != 0;
    };

    const tsd::Result r = tsd::RefineStream(
        mesh, has_uv ? &uvchan : nullptr, has_uv ? 1u : 0u, nullptr, 0, opts, so,
        sink, &ctx, &error_);
    if (r != tsd::Result::Success) {
      error_ = std::string("RefineStream failed (") + tsd::to_string(r) +
               "): " + error_;
      return 1;
    }
    return 0;
  }
  const std::string& error() const { return error_; }
 private:
  std::string error_;
};


struct Slot { void* object; uint32_t generation, kind; };
Slot* slots = nullptr;
uint32_t count = 0;
constexpr uint32_t bits = 20, mask = (1u << bits) - 1;
Slot* Lookup(uint32_t handle) {
  uint32_t index = handle & mask;
  if (!index || index > count) return nullptr;
  Slot* slot = &slots[index - 1];
  return slot->object && slot->generation == handle >> bits ? slot : nullptr;
}
}
extern "C" {
EMSCRIPTEN_KEEPALIVE void* lu_feature_alloc(uint32_t size) { return std::malloc(size); }
EMSCRIPTEN_KEEPALIVE void lu_feature_free(void* data) { std::free(data); }
EMSCRIPTEN_KEEPALIVE uint32_t lu_geometry_create(uint32_t kind) {
  if (kind != 2) return 0;
  uint32_t index = 0;
  while (index < count && (slots[index].object || !slots[index].generation)) ++index;
  if (index == count) {
    if (count == mask) return 0;
    void* allocation = std::realloc(slots, (size_t(count) + 1) * sizeof(Slot));
    if (!allocation) return 0;
    slots = static_cast<Slot*>(allocation); slots[count++] = {nullptr, 1, 2};
  }
  slots[index].object = new (std::nothrow) SubdivStreamer;
  return slots[index].object ? (slots[index].generation << bits) | (index + 1) : 0;
}
EMSCRIPTEN_KEEPALIVE void lu_geometry_destroy(uint32_t handle) {
  Slot* slot = Lookup(handle);
  if (!slot) return;
  delete static_cast<SubdivStreamer*>(slot->object); slot->object = nullptr;
  slot->generation = slot->generation == (UINT32_MAX >> bits) ? 0 : slot->generation + 1;
}
EMSCRIPTEN_KEEPALIVE int32_t lu_geometry_subdiv_refine(
    uint32_t handle, const float* points, uint32_t point_values,
    const uint32_t* face_counts, uint32_t face_count,
    const uint32_t* face_indices, uint32_t index_count,
    const float* uv_values, uint32_t uv_value_count,
    const uint32_t* uv_indices, uint32_t uv_index_count,
    const lu_geometry_subdiv_options* options, uint32_t callback_id) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 2 || !options ||
      options->struct_size < sizeof(lu_geometry_subdiv_options) ||
      (!points && point_values) || (!face_counts && face_count) ||
      (!face_indices && index_count) || (!uv_values && uv_value_count) ||
      (!uv_indices && uv_index_count) || !callback_id) return -1;
  return static_cast<SubdivStreamer*>(slot->object)->refineStream(
      points, point_values, face_counts, face_count, face_indices, index_count,
      uv_values, uv_value_count, uv_indices, uv_index_count, *options, callback_id);
}

EMSCRIPTEN_KEEPALIVE int32_t lu_geometry_subdiv_error(
    uint32_t handle, uint8_t* out, uint32_t cap) {
  const Slot* slot = Lookup(handle);
  if (!slot || slot->kind != 2) return -1;
  const auto& error = static_cast<SubdivStreamer*>(slot->object)->error();
  if (error.size() > INT32_MAX) return -1;
  if (out && cap >= error.size()) std::memcpy(out, error.data(), error.size());
  return static_cast<int32_t>(error.size());
}


}
