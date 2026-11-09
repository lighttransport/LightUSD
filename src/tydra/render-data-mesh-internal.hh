// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Internal (non-public) declarations shared between the render-data-mesh*.cc
// translation units. Splitting render-data-mesh.cc into several TUs keeps each
// one's compile time manageable; this header carries declarations of helpers
// that cross TU boundaries.
//
#pragma once

#include <string>
#include <vector>

#include "tydra/render-data.hh"           // RenderMesh, VertexAttribute, vec2, vec3
#include "tydra/render-data-internal.hh"  // MeshConverterConfig

namespace lightusd {
namespace tydra {

// Checked decoding shared by deferred computation and legacy WASM adapters.
bool ReadNormalAttribute(const VertexAttribute &attribute, size_t index, vec3 *out);
bool ReadNormalForTangent(const RenderMesh &mesh, size_t index, vec3 *out);
bool MeshAttributeCornerIndex(const RenderMesh &mesh, const VertexAttribute &attribute,
                              size_t corner, size_t face, size_t *out);

// Tangent / normal computation + quantization (render-data-mesh-tangent.cc):
bool ComputeTangentsAndBinormals(
    const std::vector<vec3> &vertices,
    const std::vector<uint32_t> &faceVertexCounts,
    const std::vector<uint32_t> &faceVertexIndices,
    const std::vector<vec2> &texcoords, const std::vector<vec3> &normals,
    bool is_facevarying_input,  // false: 'vertex' varying
    std::vector<vec3> *tangents, std::vector<vec3> *binormals,
    std::vector<uint32_t> *out_vertex_indices, std::string *err,
    uint32_t max_vertex_valence = 16, float dedup_eps = 0.0f);

// flip_normals: negate the computed geometric normal. Set for `leftHanded`
// orientation meshes (faces wind clockwise, so the CCW cross product points
// the wrong way).
bool ComputeNormals(const std::vector<vec3> &vertices,
                           const std::vector<uint32_t> &faceVertexCounts,
                           const std::vector<uint32_t> &faceVertexIndices,
                           std::vector<vec3> &normals, std::string *err,
                           bool flip_normals = false);

bool QuantizeMeshTangents(
    RenderMesh &mesh,
    MeshConverterConfig::TangentStorageFormat format);

bool TryQuantizedNormalDedup(
    VertexAttribute &normals,
    const std::vector<uint32_t> &faceVertexIndices);

bool QuantizeMeshNormals(
    RenderMesh &mesh,
    MeshConverterConfig::NormalStorageFormat format);

}  // namespace tydra
}  // namespace lightusd
