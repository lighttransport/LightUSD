// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
#include <numeric>

#include "common-utils.hh"
#include "common-types.hh"
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
#include "value-pprint.hh"

//
#include "common-macros.inc"
#include "math-util.inc"

//
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"

namespace lightusd {

namespace tydra {

namespace {

template <typename T>
std::string DumpVertexAttributeDataImpl(const T *data, const size_t nbytes,
                                        const size_t stride_bytes,
                                        uint32_t indent) {
  size_t itemsize;

  if (stride_bytes != 0) {
    if ((nbytes % stride_bytes) != 0) {
      return fmt::format(
          "[Invalid VertexAttributeData. input bytes {} must be dividable by "
          "stride_bytes {}(Type {})]",
          nbytes, stride_bytes, value::TypeTraits<T>::type_name());
    }
    itemsize = stride_bytes;
  } else {
    if ((nbytes % sizeof(T)) != 0) {
      return fmt::format(
          "[Invalid VertexAttributeData. input bytes {} must be dividable by "
          "size {}(Type {})]",
          nbytes, sizeof(T), value::TypeTraits<T>::type_name());
    }
    itemsize = sizeof(T);
  }

  size_t nitems = nbytes / itemsize;
  std::string s;
  s += pprint::Indent(indent);
  s += value::print_strided_array_snipped<T>(
      reinterpret_cast<const uint8_t *>(data), stride_bytes, nitems);
  return s;
}

std::string DumpVertexAttributeData(const VertexAttribute &vattr,
                                    uint32_t indent) {
  // Ignore elementSize
#define APPLY_FUNC(__fmt, __basety)                            \
  if (__fmt == vattr.format) {                                 \
    return DumpVertexAttributeDataImpl(                        \
        reinterpret_cast<const __basety *>(vattr.data.data()), \
        vattr.data.size(), vattr.stride, indent);              \
  }

  APPLY_FUNC(VertexAttributeFormat::Bool, uint8_t)
  APPLY_FUNC(VertexAttributeFormat::Char, char)
  APPLY_FUNC(VertexAttributeFormat::Char2, value::char2)
  APPLY_FUNC(VertexAttributeFormat::Char3, value::char3)
  APPLY_FUNC(VertexAttributeFormat::Char4, value::char4)
  APPLY_FUNC(VertexAttributeFormat::Byte, uint8_t)
  APPLY_FUNC(VertexAttributeFormat::Byte2, value::uchar2)
  APPLY_FUNC(VertexAttributeFormat::Byte3, value::uchar3)
  APPLY_FUNC(VertexAttributeFormat::Byte4, value::uchar4)
  APPLY_FUNC(VertexAttributeFormat::Short, int16_t)
  APPLY_FUNC(VertexAttributeFormat::Short2, value::short2)
  APPLY_FUNC(VertexAttributeFormat::Short3, value::short3)
  APPLY_FUNC(VertexAttributeFormat::Short4, value::short4)
  APPLY_FUNC(VertexAttributeFormat::Ushort, uint16_t)
  APPLY_FUNC(VertexAttributeFormat::Ushort2, value::ushort2)
  APPLY_FUNC(VertexAttributeFormat::Ushort3, value::ushort3)
  APPLY_FUNC(VertexAttributeFormat::Ushort4, value::ushort4)
  APPLY_FUNC(VertexAttributeFormat::Half, value::half)
  APPLY_FUNC(VertexAttributeFormat::Half2, value::half2)
  APPLY_FUNC(VertexAttributeFormat::Half3, value::half3)
  APPLY_FUNC(VertexAttributeFormat::Half4, value::half4)
  APPLY_FUNC(VertexAttributeFormat::Float, float)
  APPLY_FUNC(VertexAttributeFormat::Vec2, value::float2)
  APPLY_FUNC(VertexAttributeFormat::Vec3, value::float3)
  APPLY_FUNC(VertexAttributeFormat::Vec4, value::float4)
  APPLY_FUNC(VertexAttributeFormat::Int, int)
  APPLY_FUNC(VertexAttributeFormat::Ivec2, value::int2)
  APPLY_FUNC(VertexAttributeFormat::Ivec3, value::int3)
  APPLY_FUNC(VertexAttributeFormat::Ivec4, value::int4)
  APPLY_FUNC(VertexAttributeFormat::Uint, uint32_t)
  APPLY_FUNC(VertexAttributeFormat::Uvec2, value::half)
  APPLY_FUNC(VertexAttributeFormat::Uvec3, value::half)
  APPLY_FUNC(VertexAttributeFormat::Uvec4, value::half)
  APPLY_FUNC(VertexAttributeFormat::Double, double)
  APPLY_FUNC(VertexAttributeFormat::Dvec2, value::double2)
  APPLY_FUNC(VertexAttributeFormat::Dvec3, value::double2)
  APPLY_FUNC(VertexAttributeFormat::Dvec4, value::double2)
  APPLY_FUNC(VertexAttributeFormat::Mat2, value::matrix2f)
  APPLY_FUNC(VertexAttributeFormat::Mat3, value::matrix3f)
  APPLY_FUNC(VertexAttributeFormat::Mat4, value::matrix4f)
  APPLY_FUNC(VertexAttributeFormat::Dmat2, value::matrix2d)
  APPLY_FUNC(VertexAttributeFormat::Dmat3, value::matrix3d)
  APPLY_FUNC(VertexAttributeFormat::Dmat4, value::matrix4d)
  else {
    return fmt::format("[InternalError. Invalid VertexAttributeFormat: Id{}]",
                       int(vattr.format));
  }

#undef APPLY_FUNC
}

std::string DumpVertexAttribute(const VertexAttribute &vattr, uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << "count " << vattr.get_data().size() << "\n";
  ss << pprint::Indent(indent) << "format " << quote(to_string(vattr.format))
     << "\n";
  ss << pprint::Indent(indent) << "variability "
     << quote(to_string(vattr.variability)) << "\n";
  ss << pprint::Indent(indent) << "elementSize " << vattr.elementSize << "\n";
  ss << pprint::Indent(indent) << "value "
     << quote(DumpVertexAttributeData(vattr, /* indent */ 0)) << "\n";
  if (vattr.indices.size()) {
    ss << pprint::Indent(indent) << "indices "
       << quote(value::print_array_snipped(vattr.indices)) << "\n";
  }

  return ss.str();
}


// Internal helper using iterative DFS to avoid deep recursion.
static void DumpNodeImpl(std::stringstream &ss, const Node &node, uint32_t indent) {
  enum Phase { ENTER, EXIT };
  struct WorkItem {
    const Node *node;
    uint32_t indent;
    Phase phase;
  };

  constexpr size_t kMaxNodeTraversalLimit = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&node, indent, ENTER});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxNodeTraversalLimit) {
    WorkItem item = stack.back();
    stack.pop_back();
    const Node &n = *item.node;
    uint32_t ind = item.indent;

    if (item.phase == EXIT) {
      // Close the "children" block and the node block
      ss << pprint::Indent(ind + 1) << "}\n";
      ss << pprint::Indent(ind) << "}\n";
      continue;
    }

    // ENTER phase: write node header
    ss << pprint::Indent(ind) << "node {\n";
    ss << pprint::Indent(ind + 1) << "category " << quote(to_string(n.category)) << "\n";
    ss << pprint::Indent(ind + 1) << "type " << quote(to_string(n.nodeType)) << "\n";
    ss << pprint::Indent(ind + 1) << "id " << n.id << "\n";
    ss << pprint::Indent(ind + 1) << "prim_name " << quote(n.prim_name) << "\n";
    ss << pprint::Indent(ind + 1) << "abs_path " << quote(n.abs_path) << "\n";
    ss << pprint::Indent(ind + 1) << "display_name " << quote(n.display_name) << "\n";
    ss << pprint::Indent(ind + 1) << "local_matrix " << quote(lightusd::to_string(n.local_matrix)) << "\n";
    ss << pprint::Indent(ind + 1) << "global_matrix " << quote(lightusd::to_string(n.global_matrix)) << "\n";

    if (n.children.size()) {
      ss << pprint::Indent(ind + 1) << "children {\n";
      // Push EXIT to close braces after all children are processed
      stack.push_back({item.node, ind, EXIT});
      // Push children in reverse order so first child is processed first
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
        stack.push_back({&(*it), ind + 1, ENTER});
      }
    } else {
      ss << pprint::Indent(ind) << "}\n";
    }
  }
}

std::string DumpNode(const Node &node, uint32_t indent) {
  std::stringstream ss;
  DumpNodeImpl(ss, node, indent);
  return ss.str();
}

void DumpMaterialSubset(std::stringstream &ss, const MaterialSubset &msubset,
                        uint32_t indent) {
  ss << pprint::Indent(indent) << "material_subset {\n";
  ss << pprint::Indent(indent + 1) << "material_id " << msubset.material_id
     << "\n";
  ss << pprint::Indent(indent + 1) << "indices "
     << quote(value::print_array_snipped(msubset.indices())) << "\n";
  ss << pprint::Indent(indent) << "}\n";
}

std::string DumpMesh(const RenderMesh &mesh, uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << "mesh {\n";

  ss << pprint::Indent(indent + 1) << "prim_name " << quote(mesh.prim_name)
     << "\n";
  ss << pprint::Indent(indent + 1) << "abs_path " << quote(mesh.abs_path)
     << "\n";
  ss << pprint::Indent(indent + 1) << "display_name "
     << quote(mesh.display_name) << "\n";
  ss << pprint::Indent(indent + 1) << "num_points "
     << std::to_string(mesh.points.size()) << "\n";
  ss << pprint::Indent(indent + 1) << "points \""
     << value::print_array_snipped(mesh.points) << "\"\n";
  ss << pprint::Indent(indent + 1) << "num_faceVertexCounts "
     << std::to_string(mesh.faceVertexCounts().size()) << "\n";
  ss << pprint::Indent(indent + 1) << "faceVertexCounts \""
     << value::print_array_snipped(mesh.faceVertexCounts()) << "\"\n";
  ss << pprint::Indent(indent + 1) << "num_faceVertexIndices "
     << std::to_string(mesh.faceVertexIndices().size()) << "\n";
  ss << pprint::Indent(indent + 1) << "faceVertexIndices \""
     << value::print_array_snipped(mesh.faceVertexIndices()) << "\"\n";
  ss << pprint::Indent(indent + 1) << "materialId "
     << std::to_string(mesh.material_id) << "\n";
  ss << pprint::Indent(indent + 1) << "normals {\n"
     << DumpVertexAttribute(mesh.normals, indent + 2) << "\n";
  ss << pprint::Indent(indent + 1) << "}\n";
  ss << pprint::Indent(indent + 1) << "num_texcoordSlots "
     << std::to_string(mesh.texcoords.size()) << "\n";
  for (const auto &uvs : mesh.texcoords) {
    ss << pprint::Indent(indent + 1) << "texcoords_"
       << std::to_string(uvs.first) << " {\n"
       << DumpVertexAttribute(uvs.second, indent + 2) << "\n";
    ss << pprint::Indent(indent + 1) << "}\n";
  }
  if (mesh.binormals.data.size()) {
    ss << pprint::Indent(indent + 1) << "binormals {\n"
       << DumpVertexAttribute(mesh.binormals, indent + 2) << "\n";
    ss << pprint::Indent(indent + 1) << "}\n";
  }
  if (mesh.tangents.data.size()) {
    ss << pprint::Indent(indent + 1) << "tangents {\n"
       << DumpVertexAttribute(mesh.tangents, indent + 2) << "\n";
    ss << pprint::Indent(indent + 1) << "}\n";
  }

  ss << pprint::Indent(indent + 1) << "skel_id " << mesh.skel_id << "\n";

  if (mesh.joint_and_weights.jointIndices.size()) {
    ss << pprint::Indent(indent + 1) << "skin {\n";
    ss << pprint::Indent(indent + 2) << "geomBindTransform "
       << quote(lightusd::to_string(mesh.joint_and_weights.geomBindTransform))
       << "\n";
    ss << pprint::Indent(indent + 2) << "elementSize "
       << mesh.joint_and_weights.elementSize << "\n";
    ss << pprint::Indent(indent + 2) << "jointIndices "
       << quote(value::print_array_snipped(mesh.joint_and_weights.jointIndices))
       << "\n";
    ss << pprint::Indent(indent + 2) << "jointWeights "
       << quote(value::print_array_snipped(mesh.joint_and_weights.jointWeights))
       << "\n";
    ss << pprint::Indent(indent + 1) << "}\n";
  }
  if (mesh.targets.size()) {
    ss << pprint::Indent(indent + 1) << "shapeTargets {\n";

    for (const auto &target : mesh.targets) {
      ss << pprint::Indent(indent + 2) << target.first << " {\n";
      ss << pprint::Indent(indent + 3) << "prim_name " << quote(target.second.prim_name) << "\n";
      ss << pprint::Indent(indent + 3) << "abs_path " << quote(target.second.abs_path) << "\n";
      ss << pprint::Indent(indent + 3) << "display_name " << quote(target.second.display_name) << "\n";
      ss << pprint::Indent(indent + 3) << "pointIndices " << quote(value::print_array_snipped(target.second.pointIndices)) << "\n";
      ss << pprint::Indent(indent + 3) << "pointOffsets " << quote(value::print_array_snipped(target.second.pointOffsets)) << "\n";
      ss << pprint::Indent(indent + 3) << "normalOffsets " << quote(value::print_array_snipped(target.second.normalOffsets)) << "\n";
      ss << pprint::Indent(indent + 2) << "}\n";
    }

    ss << pprint::Indent(indent + 1) << "}\n";

  }
  if (mesh.material_subsetMap.size()) {
    ss << pprint::Indent(indent + 1) << "material_subsets {\n";
    for (const auto &msubset : mesh.material_subsetMap) {
      DumpMaterialSubset(ss, msubset.second, indent + 2);
    }
    ss << pprint::Indent(indent + 1) << "}\n";
  }

  // TODO: primvars

  ss << "\n";

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

namespace detail {

void DumpSkelNode(std::stringstream &ss, const SkelNode &node, uint32_t indent) {
  enum Phase { ENTER, EXIT };
  struct WorkItem {
    const SkelNode *node;
    uint32_t indent;
    Phase phase;
  };

  constexpr size_t kMaxNodeTraversalLimit = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&node, indent, ENTER});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxNodeTraversalLimit) {
    WorkItem item = stack.back();
    stack.pop_back();
    const SkelNode &n = *item.node;
    uint32_t ind = item.indent;

    if (item.phase == EXIT) {
      // Close the "children" block and the node block
      ss << pprint::Indent(ind + 1) << "}\n";
      ss << pprint::Indent(ind) << "}\n";
      continue;
    }

    // ENTER phase: write node header
    ss << pprint::Indent(ind) << n.joint_name << " {\n";
    ss << pprint::Indent(ind + 1) << "joint_path " << quote(n.joint_path) << "\n";
    ss << pprint::Indent(ind + 1) << "joint_id " << n.joint_id << "\n";
    ss << pprint::Indent(ind + 1) << "bind_transform " << quote(lightusd::to_string(n.bind_transform)) << "\n";
    ss << pprint::Indent(ind + 1) << "rest_transform " << quote(lightusd::to_string(n.rest_transform)) << "\n";

    if (n.children.size()) {
      ss << pprint::Indent(ind + 1) << "children {\n";
      // Push EXIT to close braces after all children are processed
      stack.push_back({item.node, ind, EXIT});
      // Push children in reverse order so first child is processed first
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
        stack.push_back({&(*it), ind + 2, ENTER});
      }
    } else {
      ss << pprint::Indent(ind) << "}\n";
    }
  }
}


} // namespace detail

std::string DumpSkeleton(const SkelHierarchy &skel, uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << "skeleton {\n";

  ss << pprint::Indent(indent + 1) << "name " << quote(skel.prim_name) << "\n";
  ss << pprint::Indent(indent + 1) << "abs_path " << quote(skel.abs_path)
     << "\n";
  ss << pprint::Indent(indent + 1) << "anim_id " << skel.anim_id
     << "\n";
  if (!skel.anim_ids.empty()) {
    ss << pprint::Indent(indent + 1) << "anim_ids "
       << quote(value::print_array_snipped(skel.anim_ids)) << "\n";
  }
  ss << pprint::Indent(indent + 1) << "display_name "
     << quote(skel.display_name) << "\n";

  detail::DumpSkelNode(ss, skel.root_node, indent + 1);

  ss << "\n";

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

namespace detail {


// void DumpAnimChannel(std::stringstream &ss, const std::string &name, const std::map<AnimationChannel::ChannelType, AnimationChannel> &channels, uint32_t indent) {
//
//   ss << pprint::Indent(indent) << name << " {\n";
//
//   for (const auto &channel : channels) {
//     if (channel.first == AnimationChannel::ChannelType::Translation) {
//       ss << pprint::Indent(indent + 1) << "translations " << quote(detail::PrintAnimationSamples(channel.second.translations.samples)) << "\n";
//     } else if (channel.first == AnimationChannel::ChannelType::Rotation) {
//       ss << pprint::Indent(indent + 1) << "rotations " << quote(detail::PrintAnimationSamples(channel.second.rotations.samples)) << "\n";
//     } else if (channel.first == AnimationChannel::ChannelType::Scale) {
//       ss << pprint::Indent(indent + 1) << "scales " << quote(detail::PrintAnimationSamples(channel.second.scales.samples)) << "\n";
//     }
//   }
//
//   ss << pprint::Indent(indent) << "}\n";
// }


} // namespace detail

std::string DumpAnimation(const AnimationClip &anim, uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << "animation {\n";

  ss << pprint::Indent(indent + 1) << "name " << quote(anim.name) << "\n";
  ss << pprint::Indent(indent + 1) << "prim_name " << quote(anim.prim_name) << "\n";
  ss << pprint::Indent(indent + 1) << "abs_path " << quote(anim.abs_path) << "\n";
  ss << pprint::Indent(indent + 1) << "display_name " << quote(anim.display_name) << "\n";
  ss << pprint::Indent(indent + 1) << "duration " << anim.duration << "\n";
  ss << pprint::Indent(indent + 1) << "num_samplers " << anim.samplers.size() << "\n";
  ss << pprint::Indent(indent + 1) << "num_channels " << anim.channels.size() << "\n";

  // Dump channels
  for (size_t i = 0; i < anim.channels.size(); i++) {
    const auto &ch = anim.channels[i];
    ss << pprint::Indent(indent + 1) << "channel[" << i << "] {\n";
    ss << pprint::Indent(indent + 2) << "target_node: " << ch.target_node << "\n";
    ss << pprint::Indent(indent + 2) << "sampler: " << ch.sampler << "\n";
    ss << pprint::Indent(indent + 2) << "path: ";
    switch (ch.path) {
      case AnimationPath::Translation: ss << "Translation"; break;
      case AnimationPath::Rotation: ss << "Rotation"; break;
      case AnimationPath::Scale: ss << "Scale"; break;
      case AnimationPath::Weights: ss << "Weights"; break;
      case AnimationPath::CustomProperty: ss << "CustomProperty"; break;
    }
    ss << "\n";
    if (!ch.target_prim_path.empty()) {
      ss << pprint::Indent(indent + 2) << "target_prim_path: "
         << quote(ch.target_prim_path) << "\n";
    }
    if (ch.is_custom_property && !ch.property_name.empty()) {
      ss << pprint::Indent(indent + 2) << "property_name: "
         << ch.property_name << "\n";
    }
    if (!ch.blendshape_target_names.empty()) {
      ss << pprint::Indent(indent + 2) << "blendshape_target_names: [";
      for (size_t j = 0; j < ch.blendshape_target_names.size(); j++) {
        if (j > 0) {
          ss << ", ";
        }
        ss << quote(ch.blendshape_target_names[j]);
      }
      ss << "]\n";
    }
    ss << pprint::Indent(indent + 1) << "}\n";
  }

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}


std::string DumpCamera(const RenderCamera &camera, uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << "camera {\n";

  ss << pprint::Indent(indent + 1) << "name " << quote(camera.name) << "\n";
  ss << pprint::Indent(indent + 1) << "abs_path " << quote(camera.abs_path)
     << "\n";
  ss << pprint::Indent(indent + 1) << "display_name "
     << quote(camera.display_name) << "\n";
  ss << pprint::Indent(indent + 1) << "shutterOpen "
     << std::to_string(camera.shutterOpen) << "\n";
  ss << pprint::Indent(indent + 1) << "shutterClose "
     << std::to_string(camera.shutterClose) << "\n";

  ss << "\n";

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

std::string DumpOpenPBRSurface(const OpenPBRSurfaceShader &shader,
                               uint32_t indent) {
  std::stringstream ss;

  ss << "OpenPBRSurfaceShader {\n";

  // Base layer
  ss << pprint::Indent(indent + 1) << "base_weight = ";
  if (shader.base_weight.is_texture()) {
    ss << "texture_id[" << shader.base_weight.texture_id << "]";
  } else {
    ss << shader.base_weight.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "base_color = ";
  if (shader.base_color.is_texture()) {
    ss << "texture_id[" << shader.base_color.texture_id << "]";
  } else {
    ss << shader.base_color.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "base_roughness = ";
  if (shader.base_roughness.is_texture()) {
    ss << "texture_id[" << shader.base_roughness.texture_id << "]";
  } else {
    ss << shader.base_roughness.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "base_metalness = ";
  if (shader.base_metalness.is_texture()) {
    ss << "texture_id[" << shader.base_metalness.texture_id << "]";
  } else {
    ss << shader.base_metalness.value;
  }
  ss << "\n";

  // Specular layer
  ss << pprint::Indent(indent + 1) << "specular_weight = ";
  if (shader.specular_weight.is_texture()) {
    ss << "texture_id[" << shader.specular_weight.texture_id << "]";
  } else {
    ss << shader.specular_weight.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "specular_color = ";
  if (shader.specular_color.is_texture()) {
    ss << "texture_id[" << shader.specular_color.texture_id << "]";
  } else {
    ss << shader.specular_color.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "specular_roughness = ";
  if (shader.specular_roughness.is_texture()) {
    ss << "texture_id[" << shader.specular_roughness.texture_id << "]";
  } else {
    ss << shader.specular_roughness.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "specular_ior = ";
  if (shader.specular_ior.is_texture()) {
    ss << "texture_id[" << shader.specular_ior.texture_id << "]";
  } else {
    ss << shader.specular_ior.value;
  }
  ss << "\n";

  // Coat layer
  ss << pprint::Indent(indent + 1) << "coat_weight = ";
  if (shader.coat_weight.is_texture()) {
    ss << "texture_id[" << shader.coat_weight.texture_id << "]";
  } else {
    ss << shader.coat_weight.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "coat_color = ";
  if (shader.coat_color.is_texture()) {
    ss << "texture_id[" << shader.coat_color.texture_id << "]";
  } else {
    ss << shader.coat_color.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "coat_roughness = ";
  if (shader.coat_roughness.is_texture()) {
    ss << "texture_id[" << shader.coat_roughness.texture_id << "]";
  } else {
    ss << shader.coat_roughness.value;
  }
  ss << "\n";

  // Emission
  ss << pprint::Indent(indent + 1) << "emission_luminance = ";
  if (shader.emission_luminance.is_texture()) {
    ss << "texture_id[" << shader.emission_luminance.texture_id << "]";
  } else {
    ss << shader.emission_luminance.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "emission_color = ";
  if (shader.emission_color.is_texture()) {
    ss << "texture_id[" << shader.emission_color.texture_id << "]";
  } else {
    ss << shader.emission_color.value;
  }
  ss << "\n";

  // Transmission
  ss << pprint::Indent(indent + 1) << "transmission_weight = ";
  if (shader.transmission_weight.is_texture()) {
    ss << "texture_id[" << shader.transmission_weight.texture_id << "]";
  } else {
    ss << shader.transmission_weight.value;
  }
  ss << "\n";

  // Subsurface
  ss << pprint::Indent(indent + 1) << "subsurface_weight = ";
  if (shader.subsurface_weight.is_texture()) {
    ss << "texture_id[" << shader.subsurface_weight.texture_id << "]";
  } else {
    ss << shader.subsurface_weight.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "subsurface_color = ";
  if (shader.subsurface_color.is_texture()) {
    ss << "texture_id[" << shader.subsurface_color.texture_id << "]";
  } else {
    ss << shader.subsurface_color.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent) << "}";

  return ss.str();
}

std::string DumpPreviewSurface(const PreviewSurfaceShader &shader,
                               uint32_t indent) {
  std::stringstream ss;

  ss << "PreviewSurfaceShader {\n";

  ss << pprint::Indent(indent + 1)
     << "useSpecularWorkflow = " << std::to_string(shader.useSpecularWorkflow)
     << "\n";

  ss << pprint::Indent(indent + 1) << "diffuseColor = ";
  if (shader.diffuseColor.is_texture()) {
    ss << "texture_id[" << shader.diffuseColor.texture_id << "]";
  } else {
    ss << shader.diffuseColor.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "metallic = ";
  if (shader.metallic.is_texture()) {
    ss << "texture_id[" << shader.metallic.texture_id << "]";
  } else {
    ss << shader.metallic.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "roughness = ";
  if (shader.roughness.is_texture()) {
    ss << "texture_id[" << shader.roughness.texture_id << "]";
  } else {
    ss << shader.roughness.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "ior = ";
  if (shader.ior.is_texture()) {
    ss << "texture_id[" << shader.ior.texture_id << "]";
  } else {
    ss << shader.ior.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "clearcoat = ";
  if (shader.clearcoat.is_texture()) {
    ss << "texture_id[" << shader.clearcoat.texture_id << "]";
  } else {
    ss << shader.clearcoat.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "clearcoatRoughness = ";
  if (shader.clearcoatRoughness.is_texture()) {
    ss << "texture_id[" << shader.clearcoatRoughness.texture_id << "]";
  } else {
    ss << shader.clearcoatRoughness.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "opacity = ";
  if (shader.opacity.is_texture()) {
    ss << "texture_id[" << shader.opacity.texture_id << "]";
  } else {
    ss << shader.opacity.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "opacityThreshold = ";
  if (shader.opacityThreshold.is_texture()) {
    ss << "texture_id[" << shader.opacityThreshold.texture_id << "]";
  } else {
    ss << shader.opacityThreshold.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "normal = ";
  if (shader.normal.is_texture()) {
    ss << "texture_id[" << shader.normal.texture_id << "]";
  } else {
    ss << shader.normal.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "displacement = ";
  if (shader.displacement.is_texture()) {
    ss << "texture_id[" << shader.displacement.texture_id << "]";
  } else {
    ss << shader.displacement.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "occlusion = ";
  if (shader.occlusion.is_texture()) {
    ss << "texture_id[" << shader.occlusion.texture_id << "]";
  } else {
    ss << shader.occlusion.value;
  }
  ss << "\n";

  ss << pprint::Indent(indent) << "}";

  return ss.str();
}

std::string DumpMaterial(const RenderMaterial &material, uint32_t indent) {
  std::stringstream ss;

  ss << pprint::Indent(indent) << "material {\n";

  ss << pprint::Indent(indent + 1) << "name " << quote(material.name) << "\n";
  ss << pprint::Indent(indent + 1) << "abs_path " << quote(material.abs_path)
     << "\n";
  ss << pprint::Indent(indent + 1) << "display_name "
     << quote(material.display_name) << "\n";

  ss << pprint::Indent(indent + 1) << "surfaceShader = ";
  if (material.surfaceShader.has_value()) {
    ss << DumpPreviewSurface(*material.surfaceShader, indent + 1);
  } else {
    ss << "null";
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "openPBRShader = ";
  if (material.openPBRShader.has_value()) {
    ss << DumpOpenPBRSurface(*material.openPBRShader, indent + 1);
  } else {
    ss << "null";
  }
  ss << "\n";

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

std::string DumpUVTexture(const UVTexture &texture, uint32_t indent) {
  std::stringstream ss;

  // TODO
  ss << "UVTexture {\n";
  ss << pprint::Indent(indent + 1) << "primvar_name " << texture.varname_uv
     << "\n";
  ss << pprint::Indent(indent + 1) << "connectedOutputChannel ";
     ss << to_string(texture.connectedOutputChannel) << "\n";

  ss << pprint::Indent(indent + 1) << "authoredOutputChannels ";

  for (const auto &c : texture.authoredOutputChannels) {
     ss << to_string(c) << " ";
  }
  ss << "\n";

  ss << pprint::Indent(indent + 1) << "bias " << texture.bias << "\n";
  ss << pprint::Indent(indent + 1) << "scale " << texture.scale << "\n";
  ss << pprint::Indent(indent + 1) << "wrapS " << to_string(texture.wrapS)
     << "\n";
  ss << pprint::Indent(indent + 1) << "wrapT " << to_string(texture.wrapT)
     << "\n";
  ss << pprint::Indent(indent + 1) << "fallback_uv " << texture.fallback_uv
     << "\n";
  ss << pprint::Indent(indent + 1) << "textureImageID "
     << std::to_string(texture.texture_image_id) << "\n";
  ss << pprint::Indent(indent + 1) << "has UsdTransform2d "
     << std::to_string(texture.has_transform2d) << "\n";
  if (texture.has_transform2d) {
    ss << pprint::Indent(indent + 2) << "rotation " << texture.tx_rotation
       << "\n";
    ss << pprint::Indent(indent + 2) << "scale " << texture.tx_scale << "\n";
    ss << pprint::Indent(indent + 2) << "translation " << texture.tx_translation
       << "\n";
    ss << pprint::Indent(indent + 2) << "computed_transform "
       << texture.transform << "\n";
  }

  ss << "\n";

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

std::string DumpImage(const TextureImage &image, uint32_t indent) {
  std::stringstream ss;

  ss << "TextureImage {\n";
  ss << pprint::Indent(indent + 1) << "asset_identifier \""
     << image.asset_identifier << "\"\n";
  ss << pprint::Indent(indent + 1) << "decoded \""
     << image.decoded << "\"\n";
  ss << pprint::Indent(indent + 1) << "channels "
     << std::to_string(image.channels) << "\n";
  ss << pprint::Indent(indent + 1) << "width " << std::to_string(image.width)
     << "\n";
  ss << pprint::Indent(indent + 1) << "height " << std::to_string(image.height)
     << "\n";
  ss << pprint::Indent(indent + 1) << "miplevel "
     << std::to_string(image.miplevel) << "\n";
  ss << pprint::Indent(indent + 1) << "colorSpace "
     << to_string(image.colorSpace) << "\n";
  ss << pprint::Indent(indent + 1) << "usdColorSpace "
     << to_string(image.usdColorSpace) << "\n";
  ss << pprint::Indent(indent + 1) << "bufferID "
     << std::to_string(image.buffer_id) << "\n";

  ss << "\n";

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

std::string DumpBuffer(const BufferData &buffer, uint32_t indent) {
  std::stringstream ss;

  ss << "Buffer {\n";
  ss << pprint::Indent(indent + 1) << "bytes " << buffer.data.size() << "\n";
  ss << pprint::Indent(indent + 1) << "componentType "
     << to_string(buffer.componentType) << "\n";

  ss << "\n";

  ss << pprint::Indent(indent) << "}\n";

  return ss.str();
}

}  // namespace

// Escape string for JSON output
static std::string json_escape(const std::string &s) {
  std::string result;
  result.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

// Escape string for YAML output (double-quoted style)
static std::string yaml_escape(const std::string &s) {
  // For simple strings without special characters, no escaping needed
  bool needs_quotes = false;
  for (char c : s) {
    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' ||
        c == ':' || c == '#' || static_cast<unsigned char>(c) < 0x20) {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes && !s.empty()) {
    return s;
  }
  // Use double-quoted style with escaping
  return "\"" + json_escape(s) + "\"";
}

// Helper to get YAML indent
static std::string yaml_indent(uint32_t level) {
  return std::string(level * 2, ' ');
}

// Helper to get JSON indent
static std::string json_indent(uint32_t level) {
  return std::string(level * 2, ' ');
}

//
// YAML format output functions
//

static void DumpNodeYAML(std::stringstream &ss, const Node &node, uint32_t indent) {
  struct WorkItem {
    const Node *node;
    uint32_t indent;
  };

  constexpr size_t kMaxNodeTraversalLimit = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&node, indent});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxNodeTraversalLimit) {
    WorkItem item = stack.back();
    stack.pop_back();
    const Node &n = *item.node;
    uint32_t ind = item.indent;

    ss << yaml_indent(ind) << "- type: " << yaml_escape(to_string(n.nodeType)) << "\n";
    ss << yaml_indent(ind) << "  id: " << n.id << "\n";
    ss << yaml_indent(ind) << "  prim_name: " << yaml_escape(n.prim_name) << "\n";
    ss << yaml_indent(ind) << "  abs_path: " << yaml_escape(n.abs_path) << "\n";
    ss << yaml_indent(ind) << "  display_name: " << yaml_escape(n.display_name) << "\n";
    ss << yaml_indent(ind) << "  local_matrix: " << yaml_escape(lightusd::to_string(n.local_matrix)) << "\n";
    ss << yaml_indent(ind) << "  global_matrix: " << yaml_escape(lightusd::to_string(n.global_matrix)) << "\n";
    if (!n.children.empty()) {
      ss << yaml_indent(ind) << "  children:\n";
      // Push children in reverse order so first child is processed first
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
        stack.push_back({&(*it), ind + 2});
      }
    }
  }
}

static void DumpVertexAttributeYAML(std::stringstream &ss, const VertexAttribute &vattr, uint32_t indent) {
  ss << yaml_indent(indent) << "count: " << vattr.get_data().size() << "\n";
  ss << yaml_indent(indent) << "format: " << yaml_escape(to_string(vattr.format)) << "\n";
  ss << yaml_indent(indent) << "variability: " << yaml_escape(to_string(vattr.variability)) << "\n";
  ss << yaml_indent(indent) << "elementSize: " << vattr.elementSize << "\n";
  ss << yaml_indent(indent) << "value: " << yaml_escape(DumpVertexAttributeData(vattr, 0)) << "\n";
  if (!vattr.indices.empty()) {
    ss << yaml_indent(indent) << "indices: " << yaml_escape(value::print_array_snipped(vattr.indices)) << "\n";
  }
}

static void DumpMeshYAML(std::stringstream &ss, const RenderMesh &mesh, uint32_t indent) {
  ss << yaml_indent(indent) << "- prim_name: " << yaml_escape(mesh.prim_name) << "\n";
  ss << yaml_indent(indent) << "  abs_path: " << yaml_escape(mesh.abs_path) << "\n";
  ss << yaml_indent(indent) << "  display_name: " << yaml_escape(mesh.display_name) << "\n";
  ss << yaml_indent(indent) << "  num_points: " << mesh.points.size() << "\n";
  ss << yaml_indent(indent) << "  points: " << yaml_escape(value::print_array_snipped(mesh.points)) << "\n";
  ss << yaml_indent(indent) << "  num_faceVertexCounts: " << mesh.faceVertexCounts().size() << "\n";
  ss << yaml_indent(indent) << "  faceVertexCounts: " << yaml_escape(value::print_array_snipped(mesh.faceVertexCounts())) << "\n";
  ss << yaml_indent(indent) << "  num_faceVertexIndices: " << mesh.faceVertexIndices().size() << "\n";
  ss << yaml_indent(indent) << "  faceVertexIndices: " << yaml_escape(value::print_array_snipped(mesh.faceVertexIndices())) << "\n";
  ss << yaml_indent(indent) << "  materialId: " << mesh.material_id << "\n";
  ss << yaml_indent(indent) << "  normals:\n";
  DumpVertexAttributeYAML(ss, mesh.normals, indent + 2);
  ss << yaml_indent(indent) << "  num_texcoordSlots: " << mesh.texcoords.size() << "\n";
  for (const auto &uvs : mesh.texcoords) {
    ss << yaml_indent(indent) << "  texcoords_" << uvs.first << ":\n";
    DumpVertexAttributeYAML(ss, uvs.second, indent + 2);
  }
  if (mesh.binormals.data.size()) {
    ss << yaml_indent(indent) << "  binormals:\n";
    DumpVertexAttributeYAML(ss, mesh.binormals, indent + 2);
  }
  if (mesh.tangents.data.size()) {
    ss << yaml_indent(indent) << "  tangents:\n";
    DumpVertexAttributeYAML(ss, mesh.tangents, indent + 2);
  }
  ss << yaml_indent(indent) << "  skel_id: " << mesh.skel_id << "\n";
  if (!mesh.joint_and_weights.jointIndices.empty()) {
    ss << yaml_indent(indent) << "  skin:\n";
    ss << yaml_indent(indent + 2) << "geomBindTransform: " << yaml_escape(lightusd::to_string(mesh.joint_and_weights.geomBindTransform)) << "\n";
    ss << yaml_indent(indent + 2) << "elementSize: " << mesh.joint_and_weights.elementSize << "\n";
    ss << yaml_indent(indent + 2) << "jointIndices: " << yaml_escape(value::print_array_snipped(mesh.joint_and_weights.jointIndices)) << "\n";
    ss << yaml_indent(indent + 2) << "jointWeights: " << yaml_escape(value::print_array_snipped(mesh.joint_and_weights.jointWeights)) << "\n";
  }
  if (!mesh.targets.empty()) {
    ss << yaml_indent(indent) << "  shapeTargets:\n";
    for (const auto &target : mesh.targets) {
      ss << yaml_indent(indent + 2) << target.first << ":\n";
      ss << yaml_indent(indent + 3) << "prim_name: " << yaml_escape(target.second.prim_name) << "\n";
      ss << yaml_indent(indent + 3) << "abs_path: " << yaml_escape(target.second.abs_path) << "\n";
      ss << yaml_indent(indent + 3) << "display_name: " << yaml_escape(target.second.display_name) << "\n";
      ss << yaml_indent(indent + 3) << "pointIndices: " << yaml_escape(value::print_array_snipped(target.second.pointIndices)) << "\n";
      ss << yaml_indent(indent + 3) << "pointOffsets: " << yaml_escape(value::print_array_snipped(target.second.pointOffsets)) << "\n";
      ss << yaml_indent(indent + 3) << "normalOffsets: " << yaml_escape(value::print_array_snipped(target.second.normalOffsets)) << "\n";
    }
  }
  if (!mesh.material_subsetMap.empty()) {
    ss << yaml_indent(indent) << "  material_subsets:\n";
    for (const auto &msubset : mesh.material_subsetMap) {
      ss << yaml_indent(indent + 2) << "- material_id: " << msubset.second.material_id << "\n";
      ss << yaml_indent(indent + 2) << "  indices: " << yaml_escape(value::print_array_snipped(msubset.second.indices())) << "\n";
    }
  }
}

static void DumpSkelNodeYAML(std::stringstream &ss, const SkelNode &node, uint32_t indent) {
  struct WorkItem {
    const SkelNode *node;
    uint32_t indent;
  };

  constexpr size_t kMaxNodeTraversalLimit = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&node, indent});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxNodeTraversalLimit) {
    WorkItem item = stack.back();
    stack.pop_back();
    const SkelNode &n = *item.node;
    uint32_t ind = item.indent;

    ss << yaml_indent(ind) << "- joint_name: " << yaml_escape(n.joint_name) << "\n";
    ss << yaml_indent(ind) << "  joint_path: " << yaml_escape(n.joint_path) << "\n";
    ss << yaml_indent(ind) << "  joint_id: " << n.joint_id << "\n";
    ss << yaml_indent(ind) << "  bind_transform: " << yaml_escape(lightusd::to_string(n.bind_transform)) << "\n";
    ss << yaml_indent(ind) << "  rest_transform: " << yaml_escape(lightusd::to_string(n.rest_transform)) << "\n";
    if (!n.children.empty()) {
      ss << yaml_indent(ind) << "  children:\n";
      // Push children in reverse order so first child is processed first
      for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
        stack.push_back({&(*it), ind + 2});
      }
    }
  }
}

static void DumpSkeletonYAML(std::stringstream &ss, const SkelHierarchy &skel, uint32_t indent) {
  ss << yaml_indent(indent) << "- name: " << yaml_escape(skel.prim_name) << "\n";
  ss << yaml_indent(indent) << "  abs_path: " << yaml_escape(skel.abs_path) << "\n";
  ss << yaml_indent(indent) << "  anim_id: " << skel.anim_id << "\n";
  if (!skel.anim_ids.empty()) {
    ss << yaml_indent(indent) << "  anim_ids: " << yaml_escape(value::print_array_snipped(skel.anim_ids)) << "\n";
  }
  ss << yaml_indent(indent) << "  display_name: " << yaml_escape(skel.display_name) << "\n";
  ss << yaml_indent(indent) << "  root_node:\n";
  DumpSkelNodeYAML(ss, skel.root_node, indent + 2);
}

static void DumpAnimationYAML(std::stringstream &ss, const AnimationClip &anim, uint32_t indent) {
  ss << yaml_indent(indent) << "- name: " << yaml_escape(anim.name) << "\n";
  ss << yaml_indent(indent) << "  prim_name: " << yaml_escape(anim.prim_name) << "\n";
  ss << yaml_indent(indent) << "  abs_path: " << yaml_escape(anim.abs_path) << "\n";
  ss << yaml_indent(indent) << "  display_name: " << yaml_escape(anim.display_name) << "\n";
  ss << yaml_indent(indent) << "  duration: " << anim.duration << "\n";
  ss << yaml_indent(indent) << "  num_samplers: " << anim.samplers.size() << "\n";
  ss << yaml_indent(indent) << "  num_channels: " << anim.channels.size() << "\n";
  if (!anim.channels.empty()) {
    ss << yaml_indent(indent) << "  channels:\n";
    for (size_t i = 0; i < anim.channels.size(); i++) {
      const auto &ch = anim.channels[i];
      ss << yaml_indent(indent + 2) << "- target_node: " << ch.target_node << "\n";
      ss << yaml_indent(indent + 2) << "  sampler: " << ch.sampler << "\n";
      ss << yaml_indent(indent + 2) << "  path: ";
      switch (ch.path) {
        case AnimationPath::Translation: ss << "Translation"; break;
        case AnimationPath::Rotation: ss << "Rotation"; break;
        case AnimationPath::Scale: ss << "Scale"; break;
        case AnimationPath::Weights: ss << "Weights"; break;
        case AnimationPath::CustomProperty: ss << "CustomProperty"; break;
      }
      ss << "\n";
      if (!ch.target_prim_path.empty()) {
        ss << yaml_indent(indent + 2) << "target_prim_path: "
           << yaml_escape(ch.target_prim_path) << "\n";
      }
      if (ch.is_custom_property && !ch.property_name.empty()) {
        ss << yaml_indent(indent + 2) << "property_name: "
           << yaml_escape(ch.property_name) << "\n";
      }
      if (!ch.blendshape_target_names.empty()) {
        ss << yaml_indent(indent + 2) << "blendshape_target_names:\n";
        for (const std::string &name : ch.blendshape_target_names) {
          ss << yaml_indent(indent + 3) << "- " << yaml_escape(name)
             << "\n";
        }
      }
    }
  }
}

static void DumpCameraYAML(std::stringstream &ss, const RenderCamera &camera, uint32_t indent) {
  ss << yaml_indent(indent) << "- name: " << yaml_escape(camera.name) << "\n";
  ss << yaml_indent(indent) << "  abs_path: " << yaml_escape(camera.abs_path) << "\n";
  ss << yaml_indent(indent) << "  display_name: " << yaml_escape(camera.display_name) << "\n";
  ss << yaml_indent(indent) << "  shutterOpen: " << camera.shutterOpen << "\n";
  ss << yaml_indent(indent) << "  shutterClose: " << camera.shutterClose << "\n";
}

static void DumpPreviewSurfaceYAML(std::stringstream &ss, const PreviewSurfaceShader &shader, uint32_t indent) {
  ss << yaml_indent(indent) << "useSpecularWorkflow: " << (shader.useSpecularWorkflow ? "true" : "false") << "\n";

  auto dump_param = [&](const char* name, const auto& param) {
    ss << yaml_indent(indent) << name << ": ";
    if (param.is_texture()) {
      ss << "texture_id[" << param.texture_id << "]";
    } else {
      ss << param.value;
    }
    ss << "\n";
  };

  dump_param("diffuseColor", shader.diffuseColor);
  dump_param("metallic", shader.metallic);
  dump_param("roughness", shader.roughness);
  dump_param("ior", shader.ior);
  dump_param("clearcoat", shader.clearcoat);
  dump_param("clearcoatRoughness", shader.clearcoatRoughness);
  dump_param("opacity", shader.opacity);
  dump_param("opacityThreshold", shader.opacityThreshold);
  dump_param("normal", shader.normal);
  dump_param("displacement", shader.displacement);
  dump_param("occlusion", shader.occlusion);
}

static void DumpOpenPBRSurfaceYAML(std::stringstream &ss, const OpenPBRSurfaceShader &shader, uint32_t indent) {
  auto dump_param = [&](const char* name, const auto& param) {
    ss << yaml_indent(indent) << name << ": ";
    if (param.is_texture()) {
      ss << "texture_id[" << param.texture_id << "]";
    } else {
      ss << param.value;
    }
    ss << "\n";
  };

  dump_param("base_weight", shader.base_weight);
  dump_param("base_color", shader.base_color);
  dump_param("base_roughness", shader.base_roughness);
  dump_param("base_metalness", shader.base_metalness);
  dump_param("specular_weight", shader.specular_weight);
  dump_param("specular_color", shader.specular_color);
  dump_param("specular_roughness", shader.specular_roughness);
  dump_param("specular_ior", shader.specular_ior);
  dump_param("coat_weight", shader.coat_weight);
  dump_param("coat_color", shader.coat_color);
  dump_param("coat_roughness", shader.coat_roughness);
  dump_param("emission_luminance", shader.emission_luminance);
  dump_param("emission_color", shader.emission_color);
  dump_param("transmission_weight", shader.transmission_weight);
  dump_param("subsurface_weight", shader.subsurface_weight);
  dump_param("subsurface_color", shader.subsurface_color);
  // Geometry properties
  dump_param("normal", shader.normal);
  dump_param("tangent", shader.tangent);
  dump_param("displacement", shader.displacement);
  // Normal/tangent map related scalars
  ss << yaml_indent(indent) << "normal_map_scale: " << shader.normal_map_scale << "\n";
  ss << yaml_indent(indent) << "tangent_rotation: " << shader.tangent_rotation << "\n";
}

static void DumpMaterialYAML(std::stringstream &ss, const RenderMaterial &material, uint32_t indent) {
  ss << yaml_indent(indent) << "- name: " << yaml_escape(material.name) << "\n";
  ss << yaml_indent(indent) << "  abs_path: " << yaml_escape(material.abs_path) << "\n";
  ss << yaml_indent(indent) << "  display_name: " << yaml_escape(material.display_name) << "\n";
  if (material.surfaceShader.has_value()) {
    ss << yaml_indent(indent) << "  surfaceShader:\n";
    DumpPreviewSurfaceYAML(ss, *material.surfaceShader, indent + 2);
  } else {
    ss << yaml_indent(indent) << "  surfaceShader: null\n";
  }
  if (material.openPBRShader.has_value()) {
    ss << yaml_indent(indent) << "  openPBRShader:\n";
    DumpOpenPBRSurfaceYAML(ss, *material.openPBRShader, indent + 2);
  } else {
    ss << yaml_indent(indent) << "  openPBRShader: null\n";
  }
}

static void DumpUVTextureYAML(std::stringstream &ss, const UVTexture &texture, uint32_t indent) {
  ss << yaml_indent(indent) << "- primvar_name: " << yaml_escape(texture.varname_uv) << "\n";
  ss << yaml_indent(indent) << "  connectedOutputChannel: " << to_string(texture.connectedOutputChannel) << "\n";
  ss << yaml_indent(indent) << "  authoredOutputChannels: [";
  bool first = true;
  for (const auto &c : texture.authoredOutputChannels) {
    if (!first) ss << ", ";
    first = false;
    ss << to_string(c);
  }
  ss << "]\n";
  ss << yaml_indent(indent) << "  bias: " << texture.bias << "\n";
  ss << yaml_indent(indent) << "  scale: " << texture.scale << "\n";
  ss << yaml_indent(indent) << "  wrapS: " << to_string(texture.wrapS) << "\n";
  ss << yaml_indent(indent) << "  wrapT: " << to_string(texture.wrapT) << "\n";
  ss << yaml_indent(indent) << "  fallback_uv: " << texture.fallback_uv << "\n";
  ss << yaml_indent(indent) << "  textureImageID: " << texture.texture_image_id << "\n";
  ss << yaml_indent(indent) << "  has_transform2d: " << (texture.has_transform2d ? "true" : "false") << "\n";
  if (texture.has_transform2d) {
    ss << yaml_indent(indent) << "  transform2d:\n";
    ss << yaml_indent(indent + 2) << "rotation: " << texture.tx_rotation << "\n";
    ss << yaml_indent(indent + 2) << "scale: " << texture.tx_scale << "\n";
    ss << yaml_indent(indent + 2) << "translation: " << texture.tx_translation << "\n";
    ss << yaml_indent(indent + 2) << "computed_transform: " << texture.transform << "\n";
  }
}

static void DumpImageYAML(std::stringstream &ss, const TextureImage &image, uint32_t indent) {
  ss << yaml_indent(indent) << "- asset_identifier: " << yaml_escape(image.asset_identifier) << "\n";
  ss << yaml_indent(indent) << "  decoded: " << (image.decoded ? "true" : "false") << "\n";
  ss << yaml_indent(indent) << "  channels: " << image.channels << "\n";
  ss << yaml_indent(indent) << "  width: " << image.width << "\n";
  ss << yaml_indent(indent) << "  height: " << image.height << "\n";
  ss << yaml_indent(indent) << "  miplevel: " << image.miplevel << "\n";
  ss << yaml_indent(indent) << "  colorSpace: " << to_string(image.colorSpace) << "\n";
  ss << yaml_indent(indent) << "  usdColorSpace: " << to_string(image.usdColorSpace) << "\n";
  ss << yaml_indent(indent) << "  bufferID: " << image.buffer_id << "\n";
}

static void DumpBufferYAML(std::stringstream &ss, const BufferData &buffer, uint32_t indent) {
  ss << yaml_indent(indent) << "- bytes: " << buffer.data.size() << "\n";
  ss << yaml_indent(indent) << "  componentType: " << to_string(buffer.componentType) << "\n";
}

static std::string DumpRenderSceneYAML(const RenderScene &scene) {
  std::stringstream ss;

  // YAML header comment
  ss << "# LightUSD RenderScene YAML Output\n";
  ss << "# Format: YAML (human-readable)\n";
  ss << "---\n\n";

  // Metadata section
  ss << "metadata:\n";
  ss << "  format_version: \"1.0\"\n";
  ss << "  generator: LightUSD/Tydra\n";
  ss << "  source_file: " << yaml_escape(scene.usd_filename) << "\n";
  ss << "  scene:\n";
  ss << "    upAxis: " << yaml_escape(scene.meta.upAxis) << "\n";
  ss << "    metersPerUnit: " << scene.meta.metersPerUnit << "\n";
  ss << "    framesPerSecond: " << scene.meta.framesPerSecond << "\n";
  ss << "    timeCodesPerSecond: " << scene.meta.timeCodesPerSecond << "\n";
  if (scene.meta.startTimeCode.has_value()) {
    ss << "    startTimeCode: " << *scene.meta.startTimeCode << "\n";
  }
  if (scene.meta.endTimeCode.has_value()) {
    ss << "    endTimeCode: " << *scene.meta.endTimeCode << "\n";
  }
  ss << "    autoPlay: " << (scene.meta.autoPlay ? "true" : "false") << "\n";
  if (!scene.meta.copyright.empty()) {
    ss << "    copyright: " << yaml_escape(scene.meta.copyright) << "\n";
  }
  if (!scene.meta.comment.empty()) {
    ss << "    comment: " << yaml_escape(scene.meta.comment) << "\n";
  }

  // Summary section
  ss << "\nsummary:\n";
  ss << "  default_root_node: " << scene.default_root_node << "\n";
  ss << "  num_nodes: " << scene.nodes.size() << "\n";
  ss << "  num_meshes: " << scene.meshes.size() << "\n";
  ss << "  num_skeletons: " << scene.skeletons.size() << "\n";
  ss << "  num_animations: " << scene.animations.size() << "\n";
  ss << "  num_cameras: " << scene.cameras.size() << "\n";
  ss << "  num_lights: " << scene.lights.size() << "\n";
  ss << "  num_materials: " << scene.materials.size() << "\n";
  ss << "  num_textures: " << scene.textures.size() << "\n";
  ss << "  num_images: " << scene.images.size() << "\n";
  ss << "  num_buffers: " << scene.buffers.size() << "\n";

  // Nodes
  ss << "\nnodes:\n";
  for (size_t i = 0; i < scene.nodes.size(); i++) {
    DumpNodeYAML(ss, scene.nodes[i], 1);
  }

  // Meshes
  ss << "\nmeshes:\n";
  for (size_t i = 0; i < scene.meshes.size(); i++) {
    DumpMeshYAML(ss, scene.meshes[i], 1);
  }

  // Skeletons
  ss << "\nskeletons:\n";
  for (size_t i = 0; i < scene.skeletons.size(); i++) {
    DumpSkeletonYAML(ss, scene.skeletons[i], 1);
  }

  // Animations
  ss << "\nanimations:\n";
  for (size_t i = 0; i < scene.animations.size(); i++) {
    DumpAnimationYAML(ss, scene.animations[i], 1);
  }

  // Cameras
  ss << "\ncameras:\n";
  for (size_t i = 0; i < scene.cameras.size(); i++) {
    DumpCameraYAML(ss, scene.cameras[i], 1);
  }

  // Materials
  ss << "\nmaterials:\n";
  for (size_t i = 0; i < scene.materials.size(); i++) {
    DumpMaterialYAML(ss, scene.materials[i], 1);
  }

  // Textures
  ss << "\ntextures:\n";
  for (size_t i = 0; i < scene.textures.size(); i++) {
    DumpUVTextureYAML(ss, scene.textures[i], 1);
  }

  // Images
  ss << "\nimages:\n";
  for (size_t i = 0; i < scene.images.size(); i++) {
    DumpImageYAML(ss, scene.images[i], 1);
  }

  // Buffers
  ss << "\nbuffers:\n";
  for (size_t i = 0; i < scene.buffers.size(); i++) {
    DumpBufferYAML(ss, scene.buffers[i], 1);
  }

  return ss.str();
}

//
// JSON format output functions
//

static void DumpNodeJSON(std::stringstream &ss, const Node &node, uint32_t indent, bool last) {
  enum Phase { ENTER, EXIT };
  struct WorkItem {
    const Node *node;
    uint32_t indent;
    Phase phase;
    bool last;  // whether this is the last sibling (for comma placement)
  };

  constexpr size_t kMaxNodeTraversalLimit = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&node, indent, ENTER, last});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxNodeTraversalLimit) {
    WorkItem item = stack.back();
    stack.pop_back();
    const Node &n = *item.node;
    uint32_t ind = item.indent;

    if (item.phase == EXIT) {
      // Close the children array and the node object
      ss << json_indent(ind + 1) << "]\n";
      ss << json_indent(ind) << "}" << (item.last ? "" : ",") << "\n";
      continue;
    }

    // ENTER phase: write node header
    ss << json_indent(ind) << "{\n";
    ss << json_indent(ind + 1) << "\"type\": \"" << json_escape(to_string(n.nodeType)) << "\",\n";
    ss << json_indent(ind + 1) << "\"id\": " << n.id << ",\n";
    ss << json_indent(ind + 1) << "\"prim_name\": \"" << json_escape(n.prim_name) << "\",\n";
    ss << json_indent(ind + 1) << "\"abs_path\": \"" << json_escape(n.abs_path) << "\",\n";
    ss << json_indent(ind + 1) << "\"display_name\": \"" << json_escape(n.display_name) << "\",\n";
    ss << json_indent(ind + 1) << "\"local_matrix\": \"" << json_escape(lightusd::to_string(n.local_matrix)) << "\",\n";
    ss << json_indent(ind + 1) << "\"global_matrix\": \"" << json_escape(lightusd::to_string(n.global_matrix)) << "\"";
    if (!n.children.empty()) {
      ss << ",\n" << json_indent(ind + 1) << "\"children\": [\n";
      // Push EXIT to close array/object after all children are processed
      stack.push_back({item.node, ind, EXIT, item.last});
      // Push children in reverse order so first child is processed first
      for (size_t i = n.children.size(); i > 0; --i) {
        bool child_last = (i == n.children.size());
        stack.push_back({&n.children[i - 1], ind + 2, ENTER, child_last});
      }
    } else {
      ss << "\n";
      ss << json_indent(ind) << "}" << (item.last ? "" : ",") << "\n";
    }
  }
}

static void DumpVertexAttributeJSON(std::stringstream &ss, const VertexAttribute &vattr, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"count\": " << vattr.get_data().size() << ",\n";
  ss << json_indent(indent + 1) << "\"format\": \"" << json_escape(to_string(vattr.format)) << "\",\n";
  ss << json_indent(indent + 1) << "\"variability\": \"" << json_escape(to_string(vattr.variability)) << "\",\n";
  ss << json_indent(indent + 1) << "\"elementSize\": " << vattr.elementSize << ",\n";
  ss << json_indent(indent + 1) << "\"value\": \"" << json_escape(DumpVertexAttributeData(vattr, 0)) << "\"";
  if (!vattr.indices.empty()) {
    ss << ",\n" << json_indent(indent + 1) << "\"indices\": \"" << json_escape(value::print_array_snipped(vattr.indices)) << "\"\n";
  } else {
    ss << "\n";
  }
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpMeshJSON(std::stringstream &ss, const RenderMesh &mesh, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"prim_name\": \"" << json_escape(mesh.prim_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"abs_path\": \"" << json_escape(mesh.abs_path) << "\",\n";
  ss << json_indent(indent + 1) << "\"display_name\": \"" << json_escape(mesh.display_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"num_points\": " << mesh.points.size() << ",\n";
  ss << json_indent(indent + 1) << "\"points\": \"" << json_escape(value::print_array_snipped(mesh.points)) << "\",\n";
  ss << json_indent(indent + 1) << "\"num_faceVertexCounts\": " << mesh.faceVertexCounts().size() << ",\n";
  ss << json_indent(indent + 1) << "\"faceVertexCounts\": \"" << json_escape(value::print_array_snipped(mesh.faceVertexCounts())) << "\",\n";
  ss << json_indent(indent + 1) << "\"num_faceVertexIndices\": " << mesh.faceVertexIndices().size() << ",\n";
  ss << json_indent(indent + 1) << "\"faceVertexIndices\": \"" << json_escape(value::print_array_snipped(mesh.faceVertexIndices())) << "\",\n";
  ss << json_indent(indent + 1) << "\"materialId\": " << mesh.material_id << ",\n";
  ss << json_indent(indent + 1) << "\"normals\": ";
  DumpVertexAttributeJSON(ss, mesh.normals, indent + 1, true);
  ss << json_indent(indent + 1) << "\"num_texcoordSlots\": " << mesh.texcoords.size() << ",\n";
  ss << json_indent(indent + 1) << "\"skel_id\": " << mesh.skel_id << "\n";
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpSkelNodeJSON(std::stringstream &ss, const SkelNode &node, uint32_t indent, bool last) {
  enum Phase { ENTER, EXIT };
  struct WorkItem {
    const SkelNode *node;
    uint32_t indent;
    Phase phase;
    bool last;  // whether this is the last sibling (for comma placement)
  };

  constexpr size_t kMaxNodeTraversalLimit = 1024 * 1024;
  std::vector<WorkItem> stack;
  stack.push_back({&node, indent, ENTER, last});
  size_t iter = 0;

  while (!stack.empty() && iter++ < kMaxNodeTraversalLimit) {
    WorkItem item = stack.back();
    stack.pop_back();
    const SkelNode &n = *item.node;
    uint32_t ind = item.indent;

    if (item.phase == EXIT) {
      // Close the children array and the node object
      ss << json_indent(ind + 1) << "]\n";
      ss << json_indent(ind) << "}" << (item.last ? "" : ",") << "\n";
      continue;
    }

    // ENTER phase: write node header
    ss << json_indent(ind) << "{\n";
    ss << json_indent(ind + 1) << "\"joint_name\": \"" << json_escape(n.joint_name) << "\",\n";
    ss << json_indent(ind + 1) << "\"joint_path\": \"" << json_escape(n.joint_path) << "\",\n";
    ss << json_indent(ind + 1) << "\"joint_id\": " << n.joint_id << ",\n";
    ss << json_indent(ind + 1) << "\"bind_transform\": \"" << json_escape(lightusd::to_string(n.bind_transform)) << "\",\n";
    ss << json_indent(ind + 1) << "\"rest_transform\": \"" << json_escape(lightusd::to_string(n.rest_transform)) << "\"";
    if (!n.children.empty()) {
      ss << ",\n" << json_indent(ind + 1) << "\"children\": [\n";
      // Push EXIT to close array/object after all children are processed
      stack.push_back({item.node, ind, EXIT, item.last});
      // Push children in reverse order so first child is processed first
      for (size_t i = n.children.size(); i > 0; --i) {
        bool child_last = (i == n.children.size());
        stack.push_back({&n.children[i - 1], ind + 2, ENTER, child_last});
      }
    } else {
      ss << "\n";
      ss << json_indent(ind) << "}" << (item.last ? "" : ",") << "\n";
    }
  }
}

static void DumpSkeletonJSON(std::stringstream &ss, const SkelHierarchy &skel, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"name\": \"" << json_escape(skel.prim_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"abs_path\": \"" << json_escape(skel.abs_path) << "\",\n";
  ss << json_indent(indent + 1) << "\"anim_id\": " << skel.anim_id << ",\n";
  ss << json_indent(indent + 1) << "\"anim_ids\": [";
  for (size_t i = 0; i < skel.anim_ids.size(); i++) {
    if (i > 0) {
      ss << ", ";
    }
    ss << skel.anim_ids[i];
  }
  ss << "],\n";
  ss << json_indent(indent + 1) << "\"display_name\": \"" << json_escape(skel.display_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"root_node\": ";
  DumpSkelNodeJSON(ss, skel.root_node, indent + 1, true);
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpAnimationJSON(std::stringstream &ss, const AnimationClip &anim, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"name\": \"" << json_escape(anim.name) << "\",\n";
  ss << json_indent(indent + 1) << "\"prim_name\": \"" << json_escape(anim.prim_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"abs_path\": \"" << json_escape(anim.abs_path) << "\",\n";
  ss << json_indent(indent + 1) << "\"display_name\": \"" << json_escape(anim.display_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"duration\": " << anim.duration << ",\n";
  ss << json_indent(indent + 1) << "\"num_samplers\": " << anim.samplers.size() << ",\n";
  ss << json_indent(indent + 1) << "\"num_channels\": " << anim.channels.size() << ",\n";
  ss << json_indent(indent + 1) << "\"channels\": [\n";
  for (size_t i = 0; i < anim.channels.size(); i++) {
    const auto &ch = anim.channels[i];
    ss << json_indent(indent + 2) << "{\n";
    ss << json_indent(indent + 3) << "\"target_node\": " << ch.target_node << ",\n";
    ss << json_indent(indent + 3) << "\"sampler\": " << ch.sampler << ",\n";
    ss << json_indent(indent + 3) << "\"path\": \"";
    switch (ch.path) {
      case AnimationPath::Translation: ss << "Translation"; break;
      case AnimationPath::Rotation: ss << "Rotation"; break;
      case AnimationPath::Scale: ss << "Scale"; break;
      case AnimationPath::Weights: ss << "Weights"; break;
      case AnimationPath::CustomProperty: ss << "CustomProperty"; break;
    }
    ss << "\"";
    if (!ch.target_prim_path.empty() ||
        (ch.is_custom_property && !ch.property_name.empty()) ||
        !ch.blendshape_target_names.empty()) {
      ss << ",";
    }
    ss << "\n";
    if (!ch.target_prim_path.empty()) {
      ss << json_indent(indent + 3) << "\"target_prim_path\": \""
         << json_escape(ch.target_prim_path) << "\"";
      if ((ch.is_custom_property && !ch.property_name.empty()) ||
          !ch.blendshape_target_names.empty()) {
        ss << ",";
      }
      ss << "\n";
    }
    if (ch.is_custom_property && !ch.property_name.empty()) {
      ss << json_indent(indent + 3) << "\"property_name\": \""
         << json_escape(ch.property_name) << "\"";
      if (!ch.blendshape_target_names.empty()) {
        ss << ",";
      }
      ss << "\n";
    }
    if (!ch.blendshape_target_names.empty()) {
      ss << json_indent(indent + 3) << "\"blendshape_target_names\": [";
      for (size_t j = 0; j < ch.blendshape_target_names.size(); j++) {
        if (j > 0) {
          ss << ", ";
        }
        ss << "\"" << json_escape(ch.blendshape_target_names[j]) << "\"";
      }
      ss << "]\n";
    }
    ss << json_indent(indent + 2) << "}" << (i == anim.channels.size() - 1 ? "" : ",") << "\n";
  }
  ss << json_indent(indent + 1) << "]\n";
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpCameraJSON(std::stringstream &ss, const RenderCamera &camera, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"name\": \"" << json_escape(camera.name) << "\",\n";
  ss << json_indent(indent + 1) << "\"abs_path\": \"" << json_escape(camera.abs_path) << "\",\n";
  ss << json_indent(indent + 1) << "\"display_name\": \"" << json_escape(camera.display_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"shutterOpen\": " << camera.shutterOpen << ",\n";
  ss << json_indent(indent + 1) << "\"shutterClose\": " << camera.shutterClose << "\n";
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpMaterialJSON(std::stringstream &ss, const RenderMaterial &material, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"name\": \"" << json_escape(material.name) << "\",\n";
  ss << json_indent(indent + 1) << "\"abs_path\": \"" << json_escape(material.abs_path) << "\",\n";
  ss << json_indent(indent + 1) << "\"display_name\": \"" << json_escape(material.display_name) << "\",\n";
  ss << json_indent(indent + 1) << "\"hasSurfaceShader\": " << (material.surfaceShader.has_value() ? "true" : "false") << ",\n";
  ss << json_indent(indent + 1) << "\"hasOpenPBRShader\": " << (material.openPBRShader.has_value() ? "true" : "false") << "\n";
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpUVTextureJSON(std::stringstream &ss, const UVTexture &texture, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"primvar_name\": \"" << json_escape(texture.varname_uv) << "\",\n";
  ss << json_indent(indent + 1) << "\"connectedOutputChannel\": \"" << to_string(texture.connectedOutputChannel) << "\",\n";
  ss << json_indent(indent + 1) << "\"bias\": " << texture.bias << ",\n";
  ss << json_indent(indent + 1) << "\"scale\": " << texture.scale << ",\n";
  ss << json_indent(indent + 1) << "\"wrapS\": \"" << to_string(texture.wrapS) << "\",\n";
  ss << json_indent(indent + 1) << "\"wrapT\": \"" << to_string(texture.wrapT) << "\",\n";
  ss << json_indent(indent + 1) << "\"textureImageID\": " << texture.texture_image_id << ",\n";
  ss << json_indent(indent + 1) << "\"has_transform2d\": " << (texture.has_transform2d ? "true" : "false") << "\n";
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpImageJSON(std::stringstream &ss, const TextureImage &image, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"asset_identifier\": \"" << json_escape(image.asset_identifier) << "\",\n";
  ss << json_indent(indent + 1) << "\"decoded\": " << (image.decoded ? "true" : "false") << ",\n";
  ss << json_indent(indent + 1) << "\"channels\": " << image.channels << ",\n";
  ss << json_indent(indent + 1) << "\"width\": " << image.width << ",\n";
  ss << json_indent(indent + 1) << "\"height\": " << image.height << ",\n";
  ss << json_indent(indent + 1) << "\"miplevel\": " << image.miplevel << ",\n";
  ss << json_indent(indent + 1) << "\"colorSpace\": \"" << to_string(image.colorSpace) << "\",\n";
  ss << json_indent(indent + 1) << "\"usdColorSpace\": \"" << to_string(image.usdColorSpace) << "\",\n";
  ss << json_indent(indent + 1) << "\"bufferID\": " << image.buffer_id << "\n";
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static void DumpBufferJSON(std::stringstream &ss, const BufferData &buffer, uint32_t indent, bool last) {
  ss << json_indent(indent) << "{\n";
  ss << json_indent(indent + 1) << "\"bytes\": " << buffer.data.size() << ",\n";
  ss << json_indent(indent + 1) << "\"componentType\": \"" << to_string(buffer.componentType) << "\"\n";
  ss << json_indent(indent) << "}" << (last ? "" : ",") << "\n";
}

static std::string DumpRenderSceneJSON(const RenderScene &scene) {
  std::stringstream ss;

  ss << "{\n";

  // Metadata section
  ss << json_indent(1) << "\"metadata\": {\n";
  ss << json_indent(2) << "\"format_version\": \"1.0\",\n";
  ss << json_indent(2) << "\"generator\": \"LightUSD/Tydra\",\n";
  ss << json_indent(2) << "\"source_file\": \"" << json_escape(scene.usd_filename) << "\",\n";
  ss << json_indent(2) << "\"scene\": {\n";
  ss << json_indent(3) << "\"upAxis\": \"" << json_escape(scene.meta.upAxis) << "\",\n";
  ss << json_indent(3) << "\"metersPerUnit\": " << scene.meta.metersPerUnit << ",\n";
  ss << json_indent(3) << "\"framesPerSecond\": " << scene.meta.framesPerSecond << ",\n";
  ss << json_indent(3) << "\"timeCodesPerSecond\": " << scene.meta.timeCodesPerSecond << ",\n";
  if (scene.meta.startTimeCode.has_value()) {
    ss << json_indent(3) << "\"startTimeCode\": " << *scene.meta.startTimeCode << ",\n";
  }
  if (scene.meta.endTimeCode.has_value()) {
    ss << json_indent(3) << "\"endTimeCode\": " << *scene.meta.endTimeCode << ",\n";
  }
  ss << json_indent(3) << "\"autoPlay\": " << (scene.meta.autoPlay ? "true" : "false") << ",\n";
  ss << json_indent(3) << "\"copyright\": \"" << json_escape(scene.meta.copyright) << "\",\n";
  ss << json_indent(3) << "\"comment\": \"" << json_escape(scene.meta.comment) << "\"\n";
  ss << json_indent(2) << "}\n";
  ss << json_indent(1) << "},\n";

  // Summary section
  ss << json_indent(1) << "\"summary\": {\n";
  ss << json_indent(2) << "\"default_root_node\": " << scene.default_root_node << ",\n";
  ss << json_indent(2) << "\"num_nodes\": " << scene.nodes.size() << ",\n";
  ss << json_indent(2) << "\"num_meshes\": " << scene.meshes.size() << ",\n";
  ss << json_indent(2) << "\"num_skeletons\": " << scene.skeletons.size() << ",\n";
  ss << json_indent(2) << "\"num_animations\": " << scene.animations.size() << ",\n";
  ss << json_indent(2) << "\"num_cameras\": " << scene.cameras.size() << ",\n";
  ss << json_indent(2) << "\"num_lights\": " << scene.lights.size() << ",\n";
  ss << json_indent(2) << "\"num_materials\": " << scene.materials.size() << ",\n";
  ss << json_indent(2) << "\"num_textures\": " << scene.textures.size() << ",\n";
  ss << json_indent(2) << "\"num_images\": " << scene.images.size() << ",\n";
  ss << json_indent(2) << "\"num_buffers\": " << scene.buffers.size() << "\n";
  ss << json_indent(1) << "},\n";

  // Nodes
  ss << json_indent(1) << "\"nodes\": [\n";
  for (size_t i = 0; i < scene.nodes.size(); i++) {
    DumpNodeJSON(ss, scene.nodes[i], 2, i == scene.nodes.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Meshes
  ss << json_indent(1) << "\"meshes\": [\n";
  for (size_t i = 0; i < scene.meshes.size(); i++) {
    DumpMeshJSON(ss, scene.meshes[i], 2, i == scene.meshes.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Skeletons
  ss << json_indent(1) << "\"skeletons\": [\n";
  for (size_t i = 0; i < scene.skeletons.size(); i++) {
    DumpSkeletonJSON(ss, scene.skeletons[i], 2, i == scene.skeletons.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Animations
  ss << json_indent(1) << "\"animations\": [\n";
  for (size_t i = 0; i < scene.animations.size(); i++) {
    DumpAnimationJSON(ss, scene.animations[i], 2, i == scene.animations.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Cameras
  ss << json_indent(1) << "\"cameras\": [\n";
  for (size_t i = 0; i < scene.cameras.size(); i++) {
    DumpCameraJSON(ss, scene.cameras[i], 2, i == scene.cameras.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Materials
  ss << json_indent(1) << "\"materials\": [\n";
  for (size_t i = 0; i < scene.materials.size(); i++) {
    DumpMaterialJSON(ss, scene.materials[i], 2, i == scene.materials.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Textures
  ss << json_indent(1) << "\"textures\": [\n";
  for (size_t i = 0; i < scene.textures.size(); i++) {
    DumpUVTextureJSON(ss, scene.textures[i], 2, i == scene.textures.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Images
  ss << json_indent(1) << "\"images\": [\n";
  for (size_t i = 0; i < scene.images.size(); i++) {
    DumpImageJSON(ss, scene.images[i], 2, i == scene.images.size() - 1);
  }
  ss << json_indent(1) << "],\n";

  // Buffers
  ss << json_indent(1) << "\"buffers\": [\n";
  for (size_t i = 0; i < scene.buffers.size(); i++) {
    DumpBufferJSON(ss, scene.buffers[i], 2, i == scene.buffers.size() - 1);
  }
  ss << json_indent(1) << "]\n";

  ss << "}\n";

  return ss.str();
}

std::string DumpRenderScene(const RenderScene &scene,
                            const std::string &format) {
  if (format == "json") {
    return DumpRenderSceneJSON(scene);
  } else if (format == "yaml") {
    return DumpRenderSceneYAML(scene);
  }

  // Default: KDL format (original format)
  std::stringstream ss;

  ss << "title " << quote(scene.usd_filename) << "\n";
  ss << "default_root_node " << scene.default_root_node << "\n";
  ss << "// # of Root Nodes : " << scene.nodes.size() << "\n";
  ss << "// # of Meshes : " << scene.meshes.size() << "\n";
  ss << "// # of Skeletons : " << scene.skeletons.size() << "\n";
  ss << "// # of Animations : " << scene.animations.size() << "\n";
  ss << "// # of Cameras : " << scene.cameras.size() << "\n";
  ss << "// # of Materials : " << scene.materials.size() << "\n";
  ss << "// # of UVTextures : " << scene.textures.size() << "\n";
  ss << "// # of TextureImages : " << scene.images.size() << "\n";
  ss << "// # of Buffers : " << scene.buffers.size() << "\n";

  ss << "\n";

  ss << "nodes {\n";
  for (size_t i = 0; i < scene.nodes.size(); i++) {
    ss << DumpNode(scene.nodes[i], 1);
  }
  ss << "}\n";

  ss << "meshes {\n";
  for (size_t i = 0; i < scene.meshes.size(); i++) {
    ss << "[" << i << "] " << DumpMesh(scene.meshes[i], 1);
  }
  ss << "}\n";

  ss << "skeletons {\n";
  for (size_t i = 0; i < scene.skeletons.size(); i++) {
    ss << "[" << i << "] " << DumpSkeleton(scene.skeletons[i], 1);
  }
  ss << "}\n";

  ss << "animations {\n";
  for (size_t i = 0; i < scene.animations.size(); i++) {
    ss << "[" << i << "] " << DumpAnimation(scene.animations[i], 1);
  }
  ss << "}\n";

  ss << "cameras {\n";
  for (size_t i = 0; i < scene.cameras.size(); i++) {
    ss << "[" << i << "] " << DumpCamera(scene.cameras[i], 1);
  }
  ss << "}\n";

  ss << "\n";
  ss << "materials {\n";
  for (size_t i = 0; i < scene.materials.size(); i++) {
    ss << "[" << i << "] " << DumpMaterial(scene.materials[i], 1);
  }
  ss << "}\n";

  ss << "\n";
  ss << "textures {\n";
  for (size_t i = 0; i < scene.textures.size(); i++) {
    ss << "[" << i << "] " << DumpUVTexture(scene.textures[i], 1);
  }
  ss << "}\n";

  ss << "\n";
  ss << "images {\n";
  for (size_t i = 0; i < scene.images.size(); i++) {
    ss << "[" << i << "] " << DumpImage(scene.images[i], 1);
  }
  ss << "}\n";

  ss << "\n";
  ss << "buffers {\n";
  for (size_t i = 0; i < scene.buffers.size(); i++) {
    ss << "[" << i << "] " << DumpBuffer(scene.buffers[i], 1);
  }
  ss << "}\n";

  return ss.str();
}

}  // namespace tydra
}  // namespace lightusd
