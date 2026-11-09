// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
#include <sstream>
#include <numeric>
#include <unordered_set>

#include "usd-export.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "common-macros.inc"
#include "tiny-format.hh"
#include "math-util.inc"

namespace lightusd {
namespace tydra {

#define PushError(msg) { \
  if (err) { \
    (*err) += msg + "\n"; \
  } \
}

namespace detail {

static void CountNodes(const SkelNode &node, size_t &count, int32_t depth = 0) {
  if (size_t(depth) >= kMaxDefaultTraversalLimit) return;

  count++;

  for (const auto &child : node.children) {
    CountNodes(child, count, depth + 1);
  }
}

static bool FlattenSkelNode(const SkelNode &node,
  std::vector<value::token> &joints,
  std::vector<value::token> &jointNames,
  std::vector<value::matrix4d> &bindTransforms,
  std::vector<value::matrix4d> &restTransforms, std::string *err,
  int32_t depth = 0) {

  if (size_t(depth) >= kMaxDefaultTraversalLimit) {
    if (err) {
      (*err) += "FlattenSkelNode: recursion too deep.\n";
    }
    return false;
  }

  size_t idx = size_t(node.joint_id);
  if (idx >= joints.size()) {
    if (err) {
      (*err) += "joint_id out-of-bounds.";
    }
    return false;
  }

  joints[idx] = value::token(node.joint_path);
  jointNames[idx] = value::token(node.joint_name);
  bindTransforms[idx] = node.bind_transform;
  restTransforms[idx] = node.rest_transform;

  for (const auto &child : node.children) {
    if (!FlattenSkelNode(child, joints, jointNames, bindTransforms, restTransforms, err, depth + 1)) {
      return false;
    }
  }

  return true;
}


static bool ExportSkeleton(const SkelHierarchy &skel,
                           const std::vector<std::string> &animSourcePaths,
                           Skeleton *dst, std::string *err) {

  size_t num_joints{0};
  CountNodes(skel.root_node, num_joints);

  std::vector<value::token> joints(num_joints);
  std::vector<value::token> jointNames(num_joints);
  std::vector<value::matrix4d> bindTransforms(num_joints);
  std::vector<value::matrix4d> restTransforms(num_joints);

  // Flatten hierachy.
  if (!FlattenSkelNode(skel.root_node, joints, jointNames, bindTransforms, restTransforms, err)) {
    return false;
  }

  dst->joints.set_value(joints);

  // Do not author jointNames when for all i: joints[i] == jointNames[i];
  bool name_is_same = true;
  for (size_t i = 0; i < num_joints; i++) {
    if (joints[i].str() != jointNames[i].str()) {
      name_is_same = false;
      break;
    }
  }

  if (!name_is_same) {
    dst->jointNames.set_value(jointNames);
  }

  dst->bindTransforms.set_value(bindTransforms);
  dst->restTransforms.set_value(restTransforms);

  if (animSourcePaths.size() == 1) {
    Path animSourceTarget(animSourcePaths[0], "");
    Relationship animSourceRel;
    animSourceRel.set(animSourceTarget);
    // TODO: add `prepend` qualifier?
    dst->animationSource = animSourceRel;
  } else if (animSourcePaths.size() > 1) {
    std::vector<Path> animSourceTargets;
    animSourceTargets.reserve(animSourcePaths.size());
    for (const auto &path : animSourcePaths) {
      animSourceTargets.emplace_back(path, "");
    }
    Relationship animSourceRel;
    animSourceRel.set(animSourceTargets);
    dst->animationSource = animSourceRel;
  }

  dst->name = skel.prim_name;

  return true;
}

static bool ExportBlendShape(const ShapeTarget &target, BlendShape *dst, std::string *err) {
  (void)err;

  dst->name = target.prim_name;
  if (target.display_name.size()) {
    dst->metas().set_displayName(target.display_name);
  }

  if (target.pointIndices.size()) {
    std::vector<int> indices(target.pointIndices.size());
    for (size_t i = 0; i < target.pointOffsets.size(); i++) {
      indices[i] = int(target.pointIndices[i]);
    }
    dst->pointIndices = indices;
  }

  if (target.pointOffsets.size()) {
    std::vector<value::vector3f> offsets(target.pointOffsets.size());
    for (size_t i = 0; i < target.pointOffsets.size(); i++) {
      offsets[i][0] = target.pointOffsets[i][0];
      offsets[i][1] = target.pointOffsets[i][1];
      offsets[i][2] = target.pointOffsets[i][2];
    }
    dst->offsets = offsets;
  }

  if (target.normalOffsets.size()) {
    std::vector<value::vector3f> normalOffsets(target.normalOffsets.size());
    for (size_t i = 0; i < target.normalOffsets.size(); i++) {
      normalOffsets[i][0] = target.normalOffsets[i][0];
      normalOffsets[i][1] = target.normalOffsets[i][1];
      normalOffsets[i][2] = target.normalOffsets[i][2];
    }
    dst->normalOffsets = normalOffsets;
  }

  return true;
}

static bool ExportSkelAnimation(const AnimationClip &anim, SkelAnimation *dst, std::string *err) {
  // TODO: This function needs to be updated to work with the new AnimationClip structure
  // (glTF/Three.js compatible) which uses samplers[] and channels[] instead of the old
  // channels_map and blendshape_weights_map. For now, just return false.
  (void)anim;
  (void)dst;
  if (err) {
    *err = "ExportSkelAnimation: Not yet implemented for new AnimationClip structure";
  }
  return false;
}


static bool ToGeomMesh(const RenderScene &scene, const RenderMesh &rmesh, GeomMesh *dst, std::vector<GeomSubset> *dst_subsets, std::string *err) {

  std::vector<int> fvCounts(rmesh.faceVertexCounts().size());
  for (size_t i = 0; i < rmesh.faceVertexCounts().size(); i++) {
    fvCounts[i] = int(rmesh.faceVertexCounts()[i]);
  }
  dst->faceVertexCounts.set_value(fvCounts);

  std::vector<int> fvIndices(rmesh.faceVertexIndices().size());
  for (size_t i = 0; i < rmesh.faceVertexIndices().size(); i++) {
    fvIndices[i] = int(rmesh.faceVertexIndices()[i]);
  }
  dst->faceVertexIndices.set_value(fvIndices);

  std::vector<value::point3f> points(rmesh.points.size());
  for (size_t i = 0; i < rmesh.points.size(); i++) {
    points[i][0] = rmesh.points[i][0];
    points[i][1] = rmesh.points[i][1];
    points[i][2] = rmesh.points[i][2];
  }

  dst->points = points;

  dst->name = rmesh.prim_name;
  if (rmesh.display_name.size()) {
    dst->meta.set_displayName(rmesh.display_name);
  }

  if (!rmesh.vertex_colors.empty()) {

    const auto &vattr = rmesh.vertex_colors;
    std::vector<value::color3f> displayColor(vattr.vertex_count());
    const float *psrc = reinterpret_cast<const float *>(vattr.buffer());
    for (size_t i = 0; i < vattr.vertex_count(); i++) {
      displayColor[i][0] = psrc[3 * i + 0];
      displayColor[i][1] = psrc[3 * i + 1];
      displayColor[i][2] = psrc[3 * i + 2];
    }

    GeomPrimvar displayColorPvar;
    displayColorPvar.set_name("displayColor");
    displayColorPvar.set_value(displayColor);
    if (vattr.is_facevarying()) {
      displayColorPvar.set_interpolation(Interpolation::FaceVarying);
    } else if (vattr.is_vertex()) {
      displayColorPvar.set_interpolation(Interpolation::Vertex);
    } else if (vattr.is_uniform()) {
      displayColorPvar.set_interpolation(Interpolation::Uniform);
    } else if (vattr.is_constant()) {
      displayColorPvar.set_interpolation(Interpolation::Constant);
    } else {
      PUSH_ERROR_AND_RETURN("Invalid variability in RenderMesh.vertex_colors");
    }

    // primvar name is extracted from Primvar::name
    dst->set_primvar(displayColorPvar);
  }

  if (!rmesh.vertex_opacities.empty()) {

    const auto &vattr = rmesh.vertex_opacities;
    std::vector<float> displayOpacity(vattr.vertex_count());
    const float *psrc = reinterpret_cast<const float *>(vattr.buffer());
    for (size_t i = 0; i < vattr.vertex_count(); i++) {
      displayOpacity[i] = psrc[i];
    }

    GeomPrimvar displayOpacityPvar;
    displayOpacityPvar.set_name("displayOpacity");
    displayOpacityPvar.set_value(displayOpacity);
    if (vattr.is_facevarying()) {
      displayOpacityPvar.set_interpolation(Interpolation::FaceVarying);
    } else if (vattr.is_vertex()) {
      displayOpacityPvar.set_interpolation(Interpolation::Vertex);
    } else if (vattr.is_uniform()) {
      displayOpacityPvar.set_interpolation(Interpolation::Uniform);
    } else if (vattr.is_constant()) {
      displayOpacityPvar.set_interpolation(Interpolation::Constant);
    } else {
      PUSH_ERROR_AND_RETURN("Invalid variability in RenderMesh.vertex_opacities");
    }

    // primvar name is extracted from Primvar::name
    dst->set_primvar(displayOpacityPvar);
  }

  if (!rmesh.normals.empty()) {
    // export as primvars:normals

    const auto &vattr = rmesh.normals;
    std::vector<value::normal3f> normals(vattr.vertex_count());
    const float *psrc = reinterpret_cast<const float *>(vattr.buffer());
    for (size_t i = 0; i < vattr.vertex_count(); i++) {
      normals[i][0] = psrc[3 * i + 0];
      normals[i][1] = psrc[3 * i + 1];
      normals[i][2] = psrc[3 * i + 2];
    }

    GeomPrimvar normalPvar;
    normalPvar.set_name("normals");
    normalPvar.set_value(normals);
    if (vattr.is_facevarying()) {
      normalPvar.set_interpolation(Interpolation::FaceVarying);
    } else if (vattr.is_vertex()) {
      normalPvar.set_interpolation(Interpolation::Vertex);
    } else if (vattr.is_uniform()) {
      normalPvar.set_interpolation(Interpolation::Uniform);
    } else if (vattr.is_constant()) {
      normalPvar.set_interpolation(Interpolation::Constant);
    } else {
      PUSH_ERROR_AND_RETURN("Invalid variability in RenderMesh.normals");
    }

    // primvar name is extracted from Primvar::name
    dst->set_primvar(normalPvar);
  }

  // Primary texcoord only.
  // TODO: Multi-texcoords support
  if (rmesh.texcoords.count(0)) {
    const auto &vattr = rmesh.texcoords.at(0);
    DCOUT("export UV: " << vattr.name);
    std::vector<value::texcoord2f> texcoords(vattr.vertex_count());
    const float *psrc = reinterpret_cast<const float *>(vattr.buffer());
    for (size_t i = 0; i < vattr.vertex_count(); i++) {
      texcoords[i][0] = psrc[2 * i + 0];
      texcoords[i][1] = psrc[2 * i + 1];
    }

    GeomPrimvar uvPvar;
    uvPvar.set_name(vattr.name);
    uvPvar.set_value(texcoords);
    if (vattr.is_facevarying()) {
      uvPvar.set_interpolation(Interpolation::FaceVarying);
    } else if (vattr.is_vertex()) {
      uvPvar.set_interpolation(Interpolation::Vertex);
    } else if (vattr.is_uniform()) {
      uvPvar.set_interpolation(Interpolation::Uniform);
    } else if (vattr.is_constant()) {
      uvPvar.set_interpolation(Interpolation::Constant);
    } else {
      PUSH_ERROR_AND_RETURN("Invalid variability in RenderMesh.texcoord0");
    }

    dst->set_primvar(uvPvar);
  }

  if (rmesh.joint_and_weights.jointIndices.size() && rmesh.joint_and_weights.jointWeights.size() && (rmesh.joint_and_weights.elementSize > 0)) {
    GeomPrimvar weightpvar;
    weightpvar.set_name("skel:jointWeights");
    weightpvar.set_value(rmesh.joint_and_weights.jointWeights);
    weightpvar.set_elementSize(uint32_t(rmesh.joint_and_weights.elementSize));
    weightpvar.set_interpolation(Interpolation::Vertex);
    dst->set_primvar(weightpvar);

    GeomPrimvar idxpvar;
    idxpvar.set_name("skel:jointIndices");
    idxpvar.set_value(rmesh.joint_and_weights.jointIndices);
    idxpvar.set_elementSize(uint32_t(rmesh.joint_and_weights.elementSize));
    idxpvar.set_interpolation(Interpolation::Vertex);
    dst->set_primvar(idxpvar);

    // matrix4d primvars:skel:geomBindTransform
    GeomPrimvar geomBindTransform;
    geomBindTransform.set_name("skel:geomBindTransform");
    geomBindTransform.set_value(rmesh.joint_and_weights.geomBindTransform);
    dst->set_primvar(geomBindTransform);
  }

  dst->doubleSided = rmesh.doubleSided;
  if (!rmesh.is_rightHanded) {
    dst->orientation.set_value(Orientation::LeftHanded);
  }

  bool has_material{false};

  if (rmesh.material_id > -1) {
    const RenderMaterial &rmat = scene.materials[size_t(rmesh.material_id)];
    Relationship mat_rel;

    Path mat_path("/materials/" + rmat.name, "");
    mat_rel.set(mat_path);
    dst->set_materialBinding(mat_rel);

    has_material = true;

    // TODO: backface material
  }

  if (rmesh.material_subsetMap.size()) {
    // Unrestricted for now.
    dst->set_subsetFamilyType(value::token("materialBind"), GeomSubset::FamilyType::Unrestricted);
    for (const auto &matsubset : rmesh.material_subsetMap) {
      GeomSubset subset;
      subset.name = matsubset.first;

      std::vector<int> indices;
      for (size_t i = 0; i < matsubset.second.indices().size(); i++) {
        indices.push_back(int(matsubset.second.indices()[i]));
      }
      subset.indices = indices;
      subset.familyName = value::token("materialBind");

      if (matsubset.second.material_id == -1) {
        PUSH_ERROR_AND_RETURN(fmt::format("material_id is not assigned to material_subset {}", matsubset.first));
      }
      const RenderMaterial &rmat = scene.materials[size_t(matsubset.second.material_id)];

      Path mat_path("/materials/" + rmat.name, "");
      Relationship mat_rel;
      mat_rel.set(mat_path);
      subset.set_materialBinding(mat_rel);

      // TODO: backface material

      dst_subsets->emplace_back(std::move(subset));
    }

    has_material = true;
  }

  if (has_material) {

    APISchemas mbAPISchema;
    mbAPISchema.listOpQual = ListEditQual::Prepend;
    mbAPISchema.names.push_back({APISchemas::APIName::MaterialBindingAPI, ""});

    dst->metas().set_apiSchemas(mbAPISchema);
  }

  return true;
}

///
/// Convert Material and Shader graph as Material Prim(it encloses Shaders).
///
static bool ToMaterialPrim(const RenderScene &scene, const std::string &abs_path, size_t material_id, Prim *dst, std::string *err) {

  const RenderMaterial &rmat = scene.materials[material_id];

  // Check if the material has a surface shader
  if (!rmat.surfaceShader.has_value()) {
    // Create a material with default/empty values if no surface shader
    Material mat;
    mat.name = rmat.name;
    (*dst) = Prim(std::move(mat));
    return true;
  }

  // TODO: create two UsdUVTextures for RGBA imagge(rgb and alpha)
  auto ConstructUVTexture = [&](const UVTexture &tex, const std::string &param_name, const std::string &abs_mat_path, /* inout */std::vector<Shader> &shader_nodes) -> bool {


    std::string preaderPrimPath = abs_mat_path + "/uvmap_" + param_name + ".outputs:result";
    Path preaderPath(preaderPrimPath, "");

    if ((tex.texture_image_id < 0) || (size_t(tex.texture_image_id) >= scene.images.size())) {
      PUSH_ERROR_AND_RETURN(fmt::format("Invalid texture_image_id for `{}`", param_name));
    }

    const TextureImage &src_teximg = scene.images[size_t(tex.texture_image_id)];

    UsdUVTexture image_tex;
    image_tex.name = "Image_Texture_" + param_name;

    // For a sparse (keep-as-is) UDIM texture, write back the original
    // `<UDIM>` pattern rather than the representative tile's path, so the
    // UDIM asset round-trips through USDA/USDC.
    std::string file_asset_path = src_teximg.asset_identifier;
    if (tex.is_udim && tex.udim_texture_id >= 0 &&
        size_t(tex.udim_texture_id) < scene.udim_textures.size()) {
      const std::string &udim_path =
          scene.udim_textures[size_t(tex.udim_texture_id)].asset_identifier;
      if (!udim_path.empty()) {
        file_asset_path = udim_path;
      }
    }

    DCOUT("asset_identifier: " << file_asset_path);
    if (file_asset_path.empty()) {
      PUSH_ERROR_AND_RETURN(fmt::format("file asset name is empty for texture image `{}`", param_name));
    }
    value::AssetPath fileAssetPath(file_asset_path);
    image_tex.file = fileAssetPath;

    // TODO: Set colorSpace in attribute meta.
    Animatable<UsdUVTexture::SourceColorSpace> sourceColorSpace;
    if (src_teximg.colorSpace == ColorSpace::sRGB) {
      sourceColorSpace.set_default(UsdUVTexture::SourceColorSpace::SRGB);
    } else if (src_teximg.colorSpace == ColorSpace::Raw) {
      sourceColorSpace.set_default(UsdUVTexture::SourceColorSpace::Raw);
    } else {
      sourceColorSpace.set_default(UsdUVTexture::SourceColorSpace::Auto);
    }
    image_tex.sourceColorSpace.set_value(sourceColorSpace);

    image_tex.st.set_connection(preaderPath);
    image_tex.st.set_value_empty(); // connection only

    Animatable<UsdUVTexture::Wrap> wrapS;
    if (tex.wrapS == UVTexture::WrapMode::CLAMP_TO_EDGE) {
      wrapS.set_default(UsdUVTexture::Wrap::Clamp);
    } else if (tex.wrapS == UVTexture::WrapMode::REPEAT) {
      wrapS.set_default(UsdUVTexture::Wrap::Repeat);
    } else if (tex.wrapS == UVTexture::WrapMode::MIRROR) {
      wrapS.set_default(UsdUVTexture::Wrap::Mirror);
    } else if (tex.wrapS == UVTexture::WrapMode::CLAMP_TO_BORDER) {
      wrapS.set_default(UsdUVTexture::Wrap::Black);
    } else {
      wrapS.set_default(UsdUVTexture::Wrap::Repeat);
    }
    image_tex.wrapS.set_value(wrapS);

    Animatable<UsdUVTexture::Wrap> wrapT;
    if (tex.wrapT == UVTexture::WrapMode::CLAMP_TO_EDGE) {
      wrapT.set_default(UsdUVTexture::Wrap::Clamp);
    } else if (tex.wrapT == UVTexture::WrapMode::REPEAT) {
      wrapT.set_default(UsdUVTexture::Wrap::Repeat);
    } else if (tex.wrapT == UVTexture::WrapMode::MIRROR) {
      wrapT.set_default(UsdUVTexture::Wrap::Mirror);
    } else if (tex.wrapT == UVTexture::WrapMode::CLAMP_TO_BORDER) {
      wrapT.set_default(UsdUVTexture::Wrap::Black);
    } else {
      wrapT.set_default(UsdUVTexture::Wrap::Repeat);
    }
    image_tex.wrapT.set_value(wrapS);

    for (const auto &c : tex.authoredOutputChannels) {
      if (c == UVTexture::Channel::R) {
        image_tex.outputsR.set_authored(true);
      } else if (c == UVTexture::Channel::G) {
        image_tex.outputsG.set_authored(true);
      } else if (c == UVTexture::Channel::B) {
        image_tex.outputsB.set_authored(true);
      } else if (c == UVTexture::Channel::RGB) {
        image_tex.outputsRGB.set_authored(true);
      } else if (c == UVTexture::Channel::RGBA) {
        PUSH_ERROR_AND_RETURN("rgba texture is not supported yet.");
      }
    }

    UsdTransform2d uv_xform;
    uv_xform.name = "place2d_" + param_name;
    if (tex.has_transform2d) {
      // TODO: Deompose tex.transform and use it?
      uv_xform.translation = tex.tx_translation;
      uv_xform.rotation = tex.tx_rotation;
      uv_xform.scale = tex.tx_scale;

      Shader uv_xformShader;
      uv_xformShader.name = "place2d_" + param_name;
      uv_xformShader.info_id = kUsdTransform2d;
      uv_xformShader.value = uv_xform;
      shader_nodes.emplace_back(std::move(uv_xformShader));
    }

    UsdPrimvarReader_float2 preader;
    preader.name = "primvar_reader_" + param_name;

    value::float2 fallback_value;
    fallback_value[0] = tex.fallback_uv[0];
    fallback_value[1] = tex.fallback_uv[1];
    if (!math::is_close(fallback_value, {0.0f, 0.0f})) {
      preader.fallback = fallback_value;
    }

    Animatable<std::string> varname;
    // TODO: Ensure primvar with 'varname_uv' exists in bound RenderMesh.
    DCOUT("varname = " << tex.varname_uv);
    varname.set_default(tex.varname_uv);
    preader.varname.set_value(varname);
    preader.result.set_authored(true);


    Shader preaderShader;
    preaderShader.name = "uvmap_" + param_name;
    preaderShader.info_id = kUsdPrimvarReader_float2;
    preaderShader.value = preader;

    shader_nodes.emplace_back(std::move(preaderShader));

    Shader imageTexShader;
    imageTexShader.name = "Image_Texture_" + param_name;
    imageTexShader.info_id = kUsdUVTexture;
    imageTexShader.value = image_tex;

    shader_nodes.emplace_back(std::move(imageTexShader));

    return true;
  };


  // Layout
  //
  // - Material
  //   - Shader(UsdPreviewSurface)
  //     - UsdUVTexture(Shader)
  //     - TexTransform2d(Shader)
  //     - PrimvarReader
  //

  Material mat;

  mat.name = rmat.name;

  Shader shader;  // root Shader
  shader.name = "defaultPBR";

  std::string abs_mat_path = abs_path + "/" + mat.name;
  std::string abs_shader_path = abs_mat_path + "/" + shader.name;

  std::vector<Shader> shader_nodes;

  {
    UsdPreviewSurface surfaceShader;  // Concrete Shader node object

    //
    // Asssign actual shader object to Shader::value.
    // Also do not forget set its shader node type name through Shader::info_id
    //
    shader.info_id = kUsdPreviewSurface;  // "UsdPreviewSurface" token

    //
    // Currently no shader network/connection API.
    // Manually construct it.
    //
    surfaceShader.outputsSurface.set_authored(
        true);  // Author `token outputs:surface`

    surfaceShader.useSpecularWorkflow = rmat.surfaceShader->useSpecularWorkflow ? 1 : 0;

    if (rmat.surfaceShader->diffuseColor.is_texture()) {

      if (size_t(rmat.surfaceShader->diffuseColor.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'diffuseColor' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->diffuseColor.texture_id)], "diffuseColor", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'diffuseColor' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "diffuseColor", "outputs:rgb");
      surfaceShader.diffuseColor.set_connection(connPath);
      surfaceShader.diffuseColor.set_value_empty();
    } else {
      value::color3f diffuseCol;
      diffuseCol.r = rmat.surfaceShader->diffuseColor.value[0];
      diffuseCol.g = rmat.surfaceShader->diffuseColor.value[1];
      diffuseCol.b = rmat.surfaceShader->diffuseColor.value[2];

      surfaceShader.diffuseColor.set_value(diffuseCol);
    }

    if (rmat.surfaceShader->specularColor.is_texture()) {

      if (size_t(rmat.surfaceShader->specularColor.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'specularColor' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->specularColor.texture_id)], "specularColor", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'specularColor' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "specularColor", "outputs:rgb");
      surfaceShader.specularColor.set_connection(connPath);
      surfaceShader.specularColor.set_value_empty();
    } else {
      value::color3f col;
      col.r = rmat.surfaceShader->specularColor.value[0];
      col.g = rmat.surfaceShader->specularColor.value[1];
      col.b = rmat.surfaceShader->specularColor.value[2];
      surfaceShader.specularColor = col;
    }

    if (rmat.surfaceShader->emissiveColor.is_texture()) {

      if (size_t(rmat.surfaceShader->emissiveColor.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'emissiveColor' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->emissiveColor.texture_id)], "emissiveColor", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'emissiveColor' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "emissiveColor", "outputs:rgb");
      surfaceShader.emissiveColor.set_connection(connPath);
      surfaceShader.emissiveColor.set_value_empty();
    } else {
      value::color3f col;
      col.r = rmat.surfaceShader->emissiveColor.value[0];
      col.g = rmat.surfaceShader->emissiveColor.value[1];
      col.b = rmat.surfaceShader->emissiveColor.value[2];
      surfaceShader.emissiveColor = col;
    }

    if (rmat.surfaceShader->metallic.is_texture()) {

      if (size_t(rmat.surfaceShader->metallic.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'metallic' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->metallic.texture_id)], "metallic", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'metallic' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "metallic", "outputs:r");
      surfaceShader.metallic.set_connection(connPath);
      surfaceShader.metallic.set_value_empty();
    } else {
      surfaceShader.metallic = rmat.surfaceShader->metallic.value;
    }

    if (rmat.surfaceShader->roughness.is_texture()) {

      if (size_t(rmat.surfaceShader->roughness.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'roughness' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->roughness.texture_id)], "roughness", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'roughness' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "roughness", "outputs:r");
      surfaceShader.roughness.set_connection(connPath);
      surfaceShader.roughness.set_value_empty();
    } else {
      surfaceShader.roughness = rmat.surfaceShader->roughness.value;
    }

    if (rmat.surfaceShader->clearcoat.is_texture()) {

      if (size_t(rmat.surfaceShader->clearcoat.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'clearcoat' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->clearcoat.texture_id)], "clearcoat", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'clearcoat' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "clearcoat", "outputs:r");
      surfaceShader.clearcoat.set_connection(connPath);
      surfaceShader.clearcoat.set_value_empty();
    } else {
      surfaceShader.clearcoat = rmat.surfaceShader->clearcoat.value;
    }

    if (rmat.surfaceShader->clearcoatRoughness.is_texture()) {

      if (size_t(rmat.surfaceShader->clearcoatRoughness.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'clearcoatRoughness' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->clearcoatRoughness.texture_id)], "clearcoatRoughness", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'clearcoatRoughness' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "clearcoatRoughness", "outputs:r");
      surfaceShader.clearcoatRoughness.set_connection(connPath);
      surfaceShader.clearcoatRoughness.set_value_empty();
    } else {
      surfaceShader.clearcoatRoughness = rmat.surfaceShader->clearcoatRoughness.value;
    }

    if (rmat.surfaceShader->opacity.is_texture()) {

      if (size_t(rmat.surfaceShader->opacity.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'opacity' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->opacity.texture_id)], "opacity", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'opacity' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "opacity", "outputs:r");
      surfaceShader.opacity.set_connection(connPath);
      surfaceShader.opacity.set_value_empty();
    } else {
      surfaceShader.opacity = rmat.surfaceShader->opacity.value;
    }

    if (rmat.surfaceShader->opacityThreshold.is_texture()) {

      if (size_t(rmat.surfaceShader->opacityThreshold.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'opacityThreshold' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->opacityThreshold.texture_id)], "opacityThreshold", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'opacityThreshold' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "opacityThreshold", "outputs:r");
      surfaceShader.opacityThreshold.set_connection(connPath);
      surfaceShader.opacityThreshold.set_value_empty();
    } else {
      surfaceShader.opacityThreshold = rmat.surfaceShader->opacityThreshold.value;
    }

    if (rmat.surfaceShader->ior.is_texture()) {

      if (size_t(rmat.surfaceShader->ior.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'ior' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->ior.texture_id)], "ior", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'ior' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "ior", "outputs:r");
      surfaceShader.ior.set_connection(connPath);
      surfaceShader.ior.set_value_empty();
    } else {
      surfaceShader.ior = rmat.surfaceShader->ior.value;
    }

    if (rmat.surfaceShader->occlusion.is_texture()) {

      if (size_t(rmat.surfaceShader->occlusion.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'occlusion' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->occlusion.texture_id)], "occlusion", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'occlusion' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "occlusion", "outputs:r");
      surfaceShader.occlusion.set_connection(connPath);
      surfaceShader.occlusion.set_value_empty();
    } else {
      surfaceShader.occlusion = rmat.surfaceShader->occlusion.value;
    }

    if (rmat.surfaceShader->normal.is_texture()) {

      if (size_t(rmat.surfaceShader->normal.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'normal' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->normal.texture_id)], "normal", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'normal' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "normal", "outputs:rgb");
      surfaceShader.normal.set_connection(connPath);
      surfaceShader.normal.set_value_empty();
    } else {
      value::normal3f n;
      n[0] = rmat.surfaceShader->normal.value[0];
      n[1] = rmat.surfaceShader->normal.value[1];
      n[2] = rmat.surfaceShader->normal.value[2];
      surfaceShader.normal = n;
    }

    if (rmat.surfaceShader->displacement.is_texture()) {

      if (size_t(rmat.surfaceShader->displacement.texture_id) > scene.textures.size()) {
        PUSH_ERROR_AND_RETURN("Invalid texture_id for 'displacement' texture.");
      }

      if (!ConstructUVTexture(scene.textures[size_t(rmat.surfaceShader->displacement.texture_id)], "displacement", abs_mat_path, shader_nodes)) {
        PUSH_ERROR_AND_RETURN("Failed to convert 'displacement' texture.");
      }

      Path connPath(abs_mat_path + "/Image_Texture_" + "displacement", "outputs:rgb");
      surfaceShader.displacement.set_connection(connPath);
      surfaceShader.displacement.set_value_empty();

    } else {
      surfaceShader.displacement = rmat.surfaceShader->displacement.value;
    }

    // Connect to UsdPreviewSurface's outputs:surface by setting targetPath.
    //
    // token outputs:surface = </path/to/mat/defaultPBR.outputs:surface>
    mat.surface.set(Path(/* prim path */ abs_shader_path,
                                   /* prop path */ "outputs:surface"));

    //
    // Shaer::value is `value::Value` type, so can use '=' to assign Shader
    // object.
    //
    shader.value = std::move(surfaceShader);
  }

  Prim shaderPrim(shader);

  Prim matPrim(mat);

  matPrim.add_child(std::move(shaderPrim));

  for (const auto &node : shader_nodes) {
    Prim shaderNodePrim(node);

    if (!matPrim.add_child(std::move(shaderNodePrim), /* rename_primname */false, err)) {
      return false;
    }
  }

  (*dst) = std::move(matPrim);
  return true;
}

} // namespace detail

bool export_to_usda(const RenderScene &scene,
  std::string &usda_str, std::string *warn, std::string *err) {

  (void)warn;

  Stage stage;

  stage.metas().comment = "Exported from LightUSD Tydra.";
  if (scene.meta.upAxis == "X") {
    stage.metas().upAxis = Axis::X;
  } else if (scene.meta.upAxis == "Y") {
    stage.metas().upAxis = Axis::Y;
  } else if (scene.meta.upAxis == "Z") {
    stage.metas().upAxis = Axis::Z;
  }

  SkelRoot skelRoot;
  skelRoot.set_name("skelRoot");
  Prim skelRootPrim(skelRoot);

  bool has_skelroot{false};

  // TODO: Construct Node hierarchy

  std::unordered_map<uint32_t, Skeleton> skelMap;

  for (size_t i = 0; i < scene.meshes.size(); i++) {
    GeomMesh mesh;
    std::vector<GeomSubset> subsets;
    if (!detail::ToGeomMesh(scene, scene.meshes[i], &mesh, &subsets, err)) {
      return false;
    }

    bool has_skel{false};

    if ((scene.meshes[i].skel_id > -1) && (size_t(scene.meshes[i].skel_id) < scene.skeletons.size())) {
      DCOUT("Export Skeleton");

      uint32_t skel_id = uint32_t(scene.meshes[i].skel_id);
      std::string skel_name;
      if (!skelMap.count(skel_id)) {
        const SkelHierarchy &src_skel = scene.skeletons[size_t(scene.meshes[i].skel_id)];

        std::vector<std::string> src_animsources;
        if (!src_skel.anim_ids.empty()) {
          src_animsources.reserve(src_skel.anim_ids.size());
          for (int anim_id : src_skel.anim_ids) {
            if ((anim_id > -1) && (size_t(anim_id) < scene.animations.size())) {
              src_animsources.push_back("/animations/" + scene.animations[size_t(anim_id)].prim_name);
            }
          }
        } else if ((src_skel.anim_id > -1) && (size_t(src_skel.anim_id) < scene.animations.size())) {
          src_animsources.push_back("/animations/" + scene.animations[size_t(src_skel.anim_id)].prim_name);
        }

        Skeleton skel;
        if (!detail::ExportSkeleton(src_skel, src_animsources, &skel, err)) {
          return false;
        }
        skel_name = skel.name;

        APISchemas skelAPISchema;
        skelAPISchema.listOpQual = ListEditQual::Prepend;
        skelAPISchema.names.push_back({APISchemas::APIName::SkelBindingAPI, ""});
        skel.metas().set_apiSchemas(skelAPISchema);

        skelMap[skel_id] = std::move(skel);
      } else {
        skel_name = skelMap[skel_id].name;
      }

      has_skel = true;
      has_skelroot = true;

      // Add some skel settings to GeomMesh.

      // rel skel:skeleton
      Relationship skelRel;
      // FIXME: Set abs_path
      Path skelTargetPath("/skelRoot/" + skel_name, "");
      skelRel.set(skelTargetPath);
      mesh.skeleton = skelRel;


      // Add apiSchemas both for GeomMesh and Skeleton
      //
      // prepend apiSchemas = ["SkelBindingAPI"]
      //
      APISchemas meshSkelAPISchema;
      meshSkelAPISchema.listOpQual = ListEditQual::Prepend;
      meshSkelAPISchema.names.push_back({APISchemas::APIName::SkelBindingAPI, ""});
      if (mesh.metas().has_apiSchemas()) {
        // Assume existing apiSchemas uses prepend listEditOp.
        APISchemas existingSchemas = mesh.metas().get_apiSchemas();
        existingSchemas.names.push_back({APISchemas::APIName::SkelBindingAPI, ""});
        mesh.metas().set_apiSchemas(existingSchemas);
      } else {
        mesh.metas().set_apiSchemas(meshSkelAPISchema);
      }

    }

    std::vector<BlendShape> bss;
    if (scene.meshes[i].targets.size()) {

      std::vector<value::token> bsNames;
      std::vector<Path> bsTargetPaths;

      for (const auto &target : scene.meshes[i].targets) {
        BlendShape bs;
        if (!detail::ExportBlendShape(target.second, &bs, err)) {
          return false;
        }

        bss.emplace_back(bs);
        bsNames.push_back(value::token(target.first));
        // TODO: Set abs_path
        std::string bs_path;
        if (has_skel) {
          bs_path = "/skelRoot/" + mesh.name + "/" + target.first;
        } else {
          bs_path = "/" + mesh.name + "/" + target.first;
        }
        Path targetPath = Path(bs_path, "");
        bsTargetPaths.push_back(targetPath);
      }

      Relationship bsTargetRel;
      bsTargetRel.set(bsTargetPaths);

      mesh.blendShapeTargets = bsTargetRel;
      mesh.blendShapes.set_value(bsNames);

    }

    Prim meshPrim(mesh);

    // Add BlendShape prim under GeomMesh prim.
    if (bss.size()) {
      for (size_t t = 0; t < bss.size(); t++) {
        Prim bsPrim(bss[t]);
        if (!meshPrim.add_child(std::move(bsPrim), /* rename_primname_if_requred */false, err)) {
          return false;
        }
      }
    }

    // Add GeomSubset prim under GeomMesh prim.
    if (subsets.size()) {
      for (size_t s = 0; s < subsets.size(); s++) {
        Prim subsetPrim(subsets[s]);
        if (!meshPrim.add_child(std::move(subsetPrim), /* rename_primname_if_required */false, err)) {
          return false;
        }
      }
    }

    if (has_skel) {

      skelRootPrim.add_child(std::move(meshPrim), /* rename_primname_if_require */false);

    } else {
      // Put Prims under Scope Prim
      Scope scope;
      scope.name = "scope" + std::to_string(i);

      Prim scopePrim(scope);
      scopePrim.add_child(std::move(meshPrim));

      stage.add_root_prim(std::move(scopePrim));
    }

  }

  if (has_skelroot) {

    for (const auto &item : skelMap) {
      Prim skelPrim(item.second);
      skelRootPrim.add_child(std::move(skelPrim), /* rename_primname_if_require */false);
    }

    stage.add_root_prim(std::move(skelRootPrim));
  }

  // TODO: Move to SkelRoot
  {
    Scope animGroup;
    animGroup.name = "animations";

    Prim animGroupPrim(animGroup);

    for (size_t i = 0; i < scene.animations.size(); i++) {
      SkelAnimation skelAnim;
      if (!detail::ExportSkelAnimation(scene.animations[i], &skelAnim, err)) {
        return false;
      }

      // TODO: Put SkelAnimation under SkelRoot
      Prim prim(skelAnim);
      animGroupPrim.add_child(std::move(prim));
    }
    stage.add_root_prim(std::move(animGroupPrim));
  }

  {
    Scope matGroup;
    matGroup.name = "materials";

    Prim matGroupPrim(matGroup);

    DCOUT("export " << scene.materials.size() << " materials");

    for (size_t i = 0; i < scene.materials.size(); i++) {
      // init with dummy object(Model Prim)
      Model dummy;
      Prim matPrim(std::move(dummy));
      if (!detail::ToMaterialPrim(scene, "/materials", i, &matPrim, err)) {
        return false;
      }
      if (!matGroupPrim.add_child(std::move(matPrim), /* rename_primname_if_required */false, err)) {
        PUSH_ERROR_AND_RETURN(fmt::format("Failed to add child Prim: {}", err));
      }

    }
    stage.add_root_prim(std::move(matGroupPrim));
  }

  usda_str =stage.ExportToString();

  return true;
}


} // namespace tydra
} // namespace lightusd
