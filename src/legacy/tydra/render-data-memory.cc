// SPDX-License-Identifier: Apache-2.0
// Copyright 2022 - 2023, Syoyo Fujita.
// Copyright 2023 - Present, Light Transport Entertainment Inc.
//
// Render-scene storage accounting.

#include <cstdint>
#include <string>
#include <vector>

#include "tydra/render-data.hh"

namespace lightusd {
namespace tydra {

// Memory usage estimation implementations

size_t RenderMesh::estimate_memory_usage() const {
  size_t total = sizeof(RenderMesh);

  // String storage
  total += prim_name.capacity();
  total += abs_path.capacity();
  total += display_name.capacity();

  // Vertex data
  total += points.capacity() * sizeof(vec3);

  // Index data
  total += usdFaceVertexIndices.capacity() * sizeof(uint32_t);
  total += usdFaceVertexCounts.capacity() * sizeof(uint32_t);
  total += triangulatedFaceVertexIndices.capacity() * sizeof(uint32_t);
  total += triangulatedFaceVertexCounts.capacity() * sizeof(uint32_t);
  total += triangulatedToOrigFaceVertexIndexMap.capacity() * sizeof(uint32_t);
  total += triangulatedFaceCounts.capacity() * sizeof(uint32_t);

  // Vertex attributes helper
  auto estimate_vertex_attr = [](const VertexAttribute& attr) -> size_t {
    size_t size = sizeof(VertexAttribute);
    size += attr.name.capacity();
    size += attr.data.capacity();
    size += attr.indices.capacity() * sizeof(uint32_t);
    return size;
  };

  total += estimate_vertex_attr(normals);
  total += estimate_vertex_attr(tangents);
  total += estimate_vertex_attr(binormals);
  total += estimate_vertex_attr(vertex_colors);
  total += estimate_vertex_attr(vertex_opacities);

  // Texcoords map
  for (const auto& texcoord_pair : texcoords) {
    total += sizeof(uint32_t) + estimate_vertex_attr(texcoord_pair.second);
  }

  // StringAndIdMap for texcoords
  total += texcoordSlotIdMap.size() * (sizeof(uint64_t) + sizeof(std::string));
  for (auto it = texcoordSlotIdMap.s_begin(); it != texcoordSlotIdMap.s_end(); ++it) {
    total += it->first.capacity();
  }

  // Joint and weights
  total += sizeof(JointAndWeight);
  total += joint_and_weights.jointIndices.capacity() * sizeof(int);
  total += joint_and_weights.jointWeights.capacity() * sizeof(float);

  // Blend shapes
  for (const auto& blend_shape_pair : targets) {
    total += blend_shape_pair.first.capacity() + sizeof(ShapeTarget);
    const auto& st = blend_shape_pair.second;
    total += st.prim_name.capacity();
    total += st.abs_path.capacity();
    total += st.display_name.capacity();
    total += st.pointIndices.capacity() * sizeof(uint32_t);
    total += st.pointOffsets.capacity() * sizeof(vec3);
    total += st.normalOffsets.capacity() * sizeof(vec3);
    for (const auto& ib_pair : st.inbetweens) {
      total += sizeof(float) + sizeof(InbetweenShapeTarget);
      total += ib_pair.second.pointOffsets.capacity() * sizeof(vec3);
      total += ib_pair.second.normalOffsets.capacity() * sizeof(vec3);
    }
  }

  // Material subset map
  for (const auto& subset_pair : material_subsetMap) {
    total += subset_pair.first.capacity() + sizeof(MaterialSubset);
    const auto& ms = subset_pair.second;
    total += ms.prim_name.capacity();
    total += ms.abs_path.capacity();
    total += ms.display_name.capacity();
    total += ms.usdIndices.capacity() * sizeof(int);
    total += ms.triangulatedIndices.capacity() * sizeof(int);
  }

  return total;
}

// Helper to estimate Node tree memory iteratively.
static size_t EstimateNodeMemory(const Node& node) {
  size_t total = 0;
  std::vector<const Node*> stack;
  stack.push_back(&node);
  size_t iter = 0;
  constexpr size_t kMaxIter = 1024 * 1024;
  while (!stack.empty() && iter++ < kMaxIter) {
    const Node* n = stack.back();
    stack.pop_back();
    total += sizeof(Node);
    total += n->prim_name.capacity();
    total += n->abs_path.capacity();
    total += n->display_name.capacity();
    total += n->children.capacity() * sizeof(Node);
    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
      stack.push_back(&(*it));
      total -= sizeof(Node); // avoid double-counting (same as recursive version)
    }
  }
  return total;
}

// Helper to estimate SkelNode tree memory iteratively.
static size_t EstimateSkelNodeMemory(const SkelNode& node) {
  size_t total = 0;
  std::vector<const SkelNode*> stack;
  stack.push_back(&node);
  size_t iter = 0;
  constexpr size_t kMaxIter = 1024 * 1024;
  while (!stack.empty() && iter++ < kMaxIter) {
    const SkelNode* n = stack.back();
    stack.pop_back();
    total += sizeof(SkelNode);
    total += n->joint_path.capacity();
    total += n->joint_name.capacity();
    total += n->children.capacity() * sizeof(SkelNode);
    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
      stack.push_back(&(*it));
      total -= sizeof(SkelNode); // avoid double-counting (same as recursive version)
    }
  }
  return total;
}

size_t RenderScene::estimate_memory_usage() const {
  size_t total = sizeof(RenderScene);

  // Scene metadata and filename
  total += usd_filename.capacity();
  total += sizeof(SceneMetadata);

  // Nodes (recursive tree)
  total += nodes.capacity() * sizeof(Node);
  for (const auto& node : nodes) {
    total += EstimateNodeMemory(node) - sizeof(Node);
  }

  // Texture images
  total += images.capacity() * sizeof(TextureImage);
  for (const auto& img : images) {
    total += img.asset_identifier.capacity();
  }

  // Materials
  total += materials.capacity() * sizeof(RenderMaterial);
  for (const auto& mat : materials) {
    total += mat.name.capacity();
    total += mat.abs_path.capacity();
    total += mat.display_name.capacity();
    total += mat.displacement_shader_path.capacity();
    total += mat.volume_shader_path.capacity();
    // Spectral data vectors (if present)
    if (mat.surfaceShader.has_value()) {
      const auto& s = *mat.surfaceShader;
      if (s.spd_reflectance.has_value()) {
        total += s.spd_reflectance->samples.capacity() * sizeof(vec2);
      }
      if (s.spd_ior.has_value()) {
        total += s.spd_ior->samples.capacity() * sizeof(vec2);
      }
    }
  }

  total += cameras.capacity() * sizeof(RenderCamera);
  total += lights.capacity() * sizeof(RenderLight);

  total += textures.capacity() * sizeof(UVTexture);
  for (const auto& texture : textures) {
    total += texture.prim_name.capacity();
    total += texture.abs_path.capacity();
    total += texture.display_name.capacity();
  }

  total += udim_textures.capacity() * sizeof(UDIMTexture);
  for (const auto& udim : udim_textures) {
    total += udim.prim_name.capacity();
    total += udim.abs_path.capacity();
    total += udim.display_name.capacity();
    total += udim.asset_identifier.capacity();
    total += udim.imageTileIds.size() *
             (sizeof(uint32_t) + sizeof(int32_t));
  }

  // Meshes - use the detailed estimation
  total += meshes.capacity() * sizeof(RenderMesh);
  for (const auto& mesh : meshes) {
    total += mesh.estimate_memory_usage() - sizeof(RenderMesh);
  }

  // Animations
  total += animations.capacity() * sizeof(AnimationClip);
  for (const auto& clip : animations) {
    total += clip.name.capacity();
    total += clip.prim_name.capacity();
    total += clip.abs_path.capacity();
    total += clip.display_name.capacity();
    total += clip.samplers.capacity() * sizeof(KeyframeSampler);
    for (const auto& sampler : clip.samplers) {
      total += sampler.times.capacity() * sizeof(float);
      total += sampler.values.capacity() * sizeof(float);
    }
    total += clip.channels.capacity() * sizeof(AnimationChannel);
  }

  // Skeletons
  total += skeletons.capacity() * sizeof(SkelHierarchy);
  for (const auto& skel : skeletons) {
    total += skel.prim_name.capacity();
    total += skel.abs_path.capacity();
    total += skel.display_name.capacity();
    total += EstimateSkelNodeMemory(skel.root_node) - sizeof(SkelNode);
    total += skel.anim_ids.capacity() * sizeof(int);
    total += skel.parent_joint_indices.capacity() * sizeof(int);
    total += skel.bind_transforms.capacity() * sizeof(value::matrix4d);
    total += skel.rest_transforms.capacity() * sizeof(value::matrix4d);
  }

  total += buffers.capacity() * sizeof(BufferData);
  for (const auto& buffer : buffers) {
    total += buffer.data.capacity();
  }

  return total;
}


}  // namespace tydra
}  // namespace lightusd
