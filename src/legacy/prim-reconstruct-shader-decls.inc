// SPDX-License-Identifier: Apache 2.0
// Forward declarations of ReconstructShader<T> explicit specializations, defined
// in prim-reconstruct-shader2.cc (Mtlx/OpenPBR) and prim-reconstruct-shader3.cc
// (Usd* nodes + primvar readers). #included INSIDE namespace lightusd::prim by the
// dispatcher prim-reconstruct-shader.cc so ReconstructPrim<Shader>'s calls bind to
// those out-of-TU specializations instead of implicitly instantiating the
// (undefined) primary template.
#define DECL_RECONSTRUCT_SHADER(__T)   \
  template <>                          \
  bool ReconstructShader<__T>(         \
      const Specifier &spec,           \
      PropertyMap &properties,         \
      const ReferenceList &references, \
      __T *out,                        \
      std::string *warn,               \
      std::string *err,                \
      const PrimReconstructOptions &options);

DECL_RECONSTRUCT_SHADER(ShaderNode)
DECL_RECONSTRUCT_SHADER(UsdPreviewSurface)
DECL_RECONSTRUCT_SHADER(UsdUVTexture)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_int)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_float)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_float2)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_float3)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_float4)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_string)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_vector)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_normal)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_point)
DECL_RECONSTRUCT_SHADER(UsdPrimvarReader_matrix)
DECL_RECONSTRUCT_SHADER(MtlxAutodeskStandardSurface)
DECL_RECONSTRUCT_SHADER(MtlxOpenPBRSurface)
DECL_RECONSTRUCT_SHADER(OpenPBRSurface)
DECL_RECONSTRUCT_SHADER(UsdTransform2d)
#undef DECL_RECONSTRUCT_SHADER
