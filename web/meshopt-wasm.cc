// SPDX-License-Identifier: Apache-2.0
#include <emscripten/emscripten.h>
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
#include <emscripten/bind.h>
#include <emscripten/val.h>
#endif
#include <cmath>
#include <cstdint>
#include <vector>
#include "external/meshoptimizer/meshoptimizer.h"
#include "lightrt-wasm-api.h"
namespace {
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
template <typename T>
std::vector<T> CopyArray(const emscripten::val &src, const char *ctor) {
  const size_t n = src["length"].as<size_t>();
  std::vector<T> out(n);
  if (!n) return out;
  (void)ctor;
  emscripten::val view(emscripten::typed_memory_view(n, out.data()));
  view.call<void>("set", src);
  return out;
}
#endif
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
template <typename T>
emscripten::val TypedArrayCopy(const char *ctor, const T *data, size_t n) {
  // JS typed-array lengths are Numbers; size_t is a BigInt in memory64.
  emscripten::val out=emscripten::val::global(ctor).new_(double(n));
  if(n) out.call<void>("set",emscripten::val(emscripten::typed_memory_view(n,data)));
  return out;
}
#endif
// Error-driven mesh reduction for Lucia's worker-side retopology/LOD path.
// The binding deliberately returns an index buffer that references the input
// vertices; JS can then compact all aligned USD/render attributes together.
int32_t SimplifyMesh(const float* pos, uint32_t pos_count,
                     const uint32_t* idx, uint32_t idx_count,
                     const float* nrm, uint32_t nrm_count,
                     const float* tex, uint32_t tex_count,
                     const uint8_t* lock, uint32_t lock_count,
                     uint32_t target_index_count, float target_error,
                     uint32_t options, uint32_t* out, uint32_t out_cap,
                     float* result_error) {
  if (!pos || !idx || !out || !result_error ||
      (nrm_count && !nrm) || (tex_count && !tex) ||
      (lock_count && !lock) || !pos_count || pos_count % 3 ||
      !idx_count || idx_count % 3 || idx_count > (1u << 27) ||
      out_cap < idx_count || target_index_count < 3 ||
      target_index_count > idx_count || target_index_count % 3 ||
      target_error < 0 || !std::isfinite(target_error)) return -1;
  const size_t vertex_count = pos_count / 3;
  for (uint32_t i = 0; i < idx_count; ++i) if (idx[i] >= vertex_count) return -1;
  if (lock_count && lock_count != vertex_count) return -1;
  const bool with_attributes = nrm_count == vertex_count * 3 &&
                               tex_count == vertex_count * 2;
  const bool with_locks = lock_count != 0;
  size_t count = 0;
  *result_error = 0.0f;
  if (with_attributes || with_locks) {
    const size_t attribute_count = with_attributes ? 5 : 1;
    std::vector<float> attributes(vertex_count * attribute_count, 0.0f);
    if (with_attributes) {
      for (size_t i = 0; i < vertex_count; ++i) {
        attributes[i * 5 + 0] = nrm[i * 3 + 0];
        attributes[i * 5 + 1] = nrm[i * 3 + 1];
        attributes[i * 5 + 2] = nrm[i * 3 + 2];
        attributes[i * 5 + 3] = tex[i * 2 + 0];
        attributes[i * 5 + 4] = tex[i * 2 + 1];
      }
    }
    const float weights[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    count = meshopt_simplifyWithAttributes(out, idx, idx_count, pos,
        vertex_count, sizeof(float) * 3, attributes.data(),
        attribute_count * sizeof(float), weights, attribute_count,
        with_locks ? lock : nullptr, target_index_count, target_error,
        options, result_error);
  } else {
    count = meshopt_simplify(out, idx, idx_count, pos, vertex_count,
        sizeof(float) * 3, target_index_count, target_error, options,
        result_error);
  }
  // Preserve the deterministic cache-friendly triangle order.
  if (count >= 3) meshopt_optimizeVertexCache(out, out, count, vertex_count);
  return static_cast<int32_t>(count);
}

#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
class MeshoptSimplifier {
 public:
  emscripten::val simplify(const emscripten::val &positions,
                           const emscripten::val &indices,
                           const emscripten::val &normals,
                           const emscripten::val &uvs,
                           const emscripten::val &locks,
                           size_t target_index_count, float target_error,
                           unsigned int options) const {
    const auto pos = CopyArray<float>(positions, "Float32Array");
    const auto idx = CopyArray<uint32_t>(indices, "Uint32Array");
    const auto nrm = CopyArray<float>(normals, "Float32Array");
    const auto tex = CopyArray<float>(uvs, "Float32Array");
    const auto lock = CopyArray<uint8_t>(locks, "Uint8Array");
    std::vector<uint32_t> out(idx.size());
    float error = 0.0f;
    if (target_index_count > UINT32_MAX) return emscripten::val::undefined();
    const int32_t count = SimplifyMesh(pos.data(), static_cast<uint32_t>(pos.size()),
        idx.data(), static_cast<uint32_t>(idx.size()), nrm.data(),
        static_cast<uint32_t>(nrm.size()), tex.data(),
        static_cast<uint32_t>(tex.size()), lock.data(),
        static_cast<uint32_t>(lock.size()),
        static_cast<uint32_t>(target_index_count), target_error, options,
        out.data(), static_cast<uint32_t>(out.size()), &error);
    if (count < 0) return emscripten::val::undefined();
    emscripten::val result = emscripten::val::object();
    result.set("indices", TypedArrayCopy("Uint32Array", out.data(), count));
    result.set("error", error);
    result.set("sourceVertexCount", pos.size() / 3);
    result.set("vertexCacheOptimized", true);
    return result;
  }
};
#endif
}
#if defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
extern "C" EMSCRIPTEN_KEEPALIVE int32_t lightusd_meshopt_simplify(
    const float* pos, uint32_t pos_count, const uint32_t* idx,
    uint32_t idx_count, const float* nrm, uint32_t nrm_count,
    const float* tex, uint32_t tex_count, const uint8_t* lock,
    uint32_t lock_count, uint32_t target_index_count, float target_error,
    uint32_t options, uint32_t* out, uint32_t out_cap, float* result_error) {
  return SimplifyMesh(pos, pos_count, idx, idx_count, nrm, nrm_count,
                      tex, tex_count, lock, lock_count, target_index_count,
                      target_error, options, out, out_cap, result_error);
}
#endif
#if !defined(LIGHTUSD_NEXT_TYPED_BINDINGS)
EMSCRIPTEN_BINDINGS(lightusd_meshoptimizer) {
  emscripten::class_<MeshoptSimplifier>("MeshoptSimplifier")
      .constructor<>()
      .function("simplify", &MeshoptSimplifier::simplify);
}
#endif
