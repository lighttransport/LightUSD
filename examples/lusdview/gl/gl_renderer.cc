// SPDX-License-Identifier: Apache-2.0
#include "gl/gl_renderer.hh"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "gl/gl_util.hh"
#include "lod_math.hh"  // UnitCubeCorner, kBoxIndices (LOD box proxies)
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "light3d/material.h"
#include "light3d/math.h"

namespace lusdview {

namespace {

GLint GLWrap(int w) {
  switch (w) {
    case 1: return GL_REPEAT;
    case 2: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_BORDER;
    default: return GL_CLAMP_TO_EDGE;
  }
}

// Keep the legacy inline GL shader strings readable while sharing the same
// eight-sample finite-light pattern as the Vulkan raster shaders.
std::string UpgradeFiniteLightSamples(std::string source) {
  auto replaceAll = [&source](const char* from, const char* to) {
    const std::string needle(from);
    const std::string replacement(to);
    size_t pos = 0;
    while ((pos = source.find(needle, pos)) != std::string::npos) {
      source.replace(pos, needle.size(), replacement);
      pos += replacement.size();
    }
  };
  // PortalLight carries the same width/height/orientation payload as RectLight.
  // Extend the legacy inline shader expressions before expanding their sample
  // pattern so every GL draw path treats a portal as a rectangular opening.
  replaceAll("int(pt.w+0.5)==4)?4:1",
             "int(pt.w+0.5)==4||int(pt.w+0.5)==7||int(pt.w+0.5)==8)?4:1");
  replaceAll("lt==4)?4:1", "lt==4||lt==7||lt==8)?4:1");
  replaceAll("if(int(pt.w+0.5)==3){",
             "if(int(pt.w+0.5)==3||int(pt.w+0.5)==8){");
  replaceAll("if(lt==3){", "if(lt==3||lt==8){");
  replaceAll(")?4:1", ")?8:1");
  replaceAll("((si&1)==0?-0.25:0.25)",
             "((float(si%4)+0.5)*0.25-0.5)");
  replaceAll("((si&2)==0?-0.25:0.25)",
             "((float(si/4)+0.5)*0.5-0.5)");
  replaceAll("float k=0.3535533906;",
             "float k=0.5,a=6.28318530718*(float(si)+0.5)/8.0;");
  replaceAll("(((si&1)==0?-k:k)*", "(cos(a)*k*");
  replaceAll("(((si&2)==0?-k:k)*", "(sin(a)*k*");
  replaceAll(
      "void main(){",
      "float geometrySampleCoord(int li,int si,int axis){int f=si*3+axis;"
      "return uLightIesProfile[li*6+f/4][f%4];}\nvoid main(){");
  replaceAll(
      "vec3 samplePos=pt.xyz;vec3 ax=",
      "vec3 samplePos=int(pt.w+0.5)==7?vec3(geometrySampleCoord(li,si,0),"
      "geometrySampleCoord(li,si,1),geometrySampleCoord(li,si,2)):pt.xyz;vec3 ax=");
  replaceAll(
      "vec3 samplePos=pt.xyz,ax=",
      "vec3 samplePos=lt==7?vec3(geometrySampleCoord(li,si,0),"
      "geometrySampleCoord(li,si,1),geometrySampleCoord(li,si,2)):pt.xyz,ax=");
  replaceAll("float ies=sampleIes(",
             "float ies=lt==7?1.0:sampleIes(");
  return source;
}

void SetUvUniform(GLint loc0, GLint loc1, const DrawUvXformCPU& uv) {
  glUniform3f(loc0, uv.m00, uv.m01, uv.tx);
  glUniform3f(loc1, uv.m10, uv.m11, uv.ty);
}

void UploadRasterLightArray(GLuint program, const RasterLightSet& lights) {
  float positionType[kMaxRasterLights * 4]{};
  float directionAngle[kMaxRasterLights * 4]{};
  float colorDiffuse[kMaxRasterLights * 4]{};
  float specularShape[kMaxRasterLights * 4]{};
  float areaParams[kMaxRasterLights * 4]{};
  float iesAxisX[kMaxRasterLights * 4]{};
  float iesAxisY[kMaxRasterLights * 4]{};
  float iesProfile[kMaxRasterLights * 24]{};
  for (int i = 0; i < lights.count; ++i) {
    const RasterLightGPU& src = lights.lights[static_cast<size_t>(i)];
    std::memcpy(positionType + i * 4, src.positionType, 4 * sizeof(float));
    std::memcpy(directionAngle + i * 4, src.directionAngle, 4 * sizeof(float));
    std::memcpy(colorDiffuse + i * 4, src.colorDiffuse, 4 * sizeof(float));
    std::memcpy(specularShape + i * 4, src.specularShape, 4 * sizeof(float));
    std::memcpy(areaParams + i * 4, src.areaParams, 4 * sizeof(float));
    std::memcpy(iesAxisX + i * 4, src.iesAxisX, 4 * sizeof(float));
    std::memcpy(iesAxisY + i * 4, src.iesAxisY, 4 * sizeof(float));
    std::memcpy(iesProfile + i * 24, src.iesProfile, 24 * sizeof(float));
  }
  glUniform1i(glGetUniformLocation(program, "uLightCount"), lights.count);
  glUniform4fv(glGetUniformLocation(program, "uLightPositionType"), lights.count,
               positionType);
  glUniform4fv(glGetUniformLocation(program, "uLightDirectionAngle"), lights.count,
               directionAngle);
  glUniform4fv(glGetUniformLocation(program, "uLightColorDiffuse"), lights.count,
               colorDiffuse);
  glUniform4fv(glGetUniformLocation(program, "uLightSpecularShape"), lights.count,
               specularShape);
  glUniform4fv(glGetUniformLocation(program, "uLightAreaParams"), lights.count,
               areaParams);
  glUniform4fv(glGetUniformLocation(program, "uLightIesAxisX"), lights.count,
               iesAxisX);
  glUniform4fv(glGetUniformLocation(program, "uLightIesAxisY"), lights.count,
               iesAxisY);
  glUniform4fv(glGetUniformLocation(program, "uLightIesProfile"), lights.count * 6,
               iesProfile);
}

void UploadRasterLightMask(GLuint program, const RasterLightSet& lights,
                           int meshIndex) {
  const uint32_t mask = RasterLightMaskForMesh(lights, meshIndex);
  glUniform1ui(glGetUniformLocation(program, "uLightMask"), mask);
}

void UploadRasterLightMaskValue(GLuint program, uint32_t mask) {
  glUniform1ui(glGetUniformLocation(program, "uLightMask"), mask);
}

void UploadClippingPlanes(GLuint program, const RenderFrameParams& params) {
  const int count = std::clamp(params.clippingPlaneCount, 0,
                               RenderFrameParams::kMaxClippingPlanes);
  glUniform1i(glGetUniformLocation(program, "uClippingPlaneCount"), count);
  if (count > 0) {
    glUniform4fv(glGetUniformLocation(program, "uClippingPlanes"), count,
                 params.clippingPlanes);
  }
}

// Compressed-format enum values that may be absent from older glad headers.
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#endif
#ifndef GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT
#define GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT 0x8E8F
#endif
#ifndef GL_COMPRESSED_RGB8_ETC2
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#endif
#ifndef GL_COMPRESSED_SRGB8_ETC2
#define GL_COMPRESSED_SRGB8_ETC2 0x9275
#endif
#ifndef GL_COMPRESSED_RGBA8_ETC2_EAC
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#endif
#ifndef GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC
#define GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC 0x9279
#endif
#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#endif
#ifndef GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR
#define GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR 0x93D0
#endif

GLenum GLCompressedFormat(DrawCompressedFormat format, bool srgb) {
  switch (format) {
    case DrawCompressedFormat::BC1:
      return srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
                  : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    case DrawCompressedFormat::BC3:
      return srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
                  : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    case DrawCompressedFormat::BC5:
      // GL_ARB_texture_compression_rgtc (core since GL 3.0). No sRGB variant.
      return GL_COMPRESSED_RG_RGTC2;
    case DrawCompressedFormat::BC6H:
      // GL_ARB_texture_compression_bptc float (HDR). No sRGB variant.
      return GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT;
    case DrawCompressedFormat::BC7:
      // GL_ARB_texture_compression_bptc (core since GL 4.2).
      return srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
                  : GL_COMPRESSED_RGBA_BPTC_UNORM;
    case DrawCompressedFormat::ETC2_RGB:
      return srgb ? GL_COMPRESSED_SRGB8_ETC2 : GL_COMPRESSED_RGB8_ETC2;
    case DrawCompressedFormat::ETC2_RGBA:
      return srgb ? GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC
                  : GL_COMPRESSED_RGBA8_ETC2_EAC;
    case DrawCompressedFormat::ASTC_4x4:
      return srgb ? GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR
                  : GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
    case DrawCompressedFormat::None:
    default:
      return 0;
  }
}

light3d::Mat4 ToMat4(const float* m) {
  light3d::Mat4 r;
  std::memcpy(r.m, m, sizeof(r.m));
  return r;
}

// Normal matrix (column-major 3x3) = transpose(inverse(upper-left 3x3 of world)).
void NormalMatrix3(const float m[16], float out[9]) {
  const float a00 = m[0], a01 = m[4], a02 = m[8];
  const float a10 = m[1], a11 = m[5], a12 = m[9];
  const float a20 = m[2], a21 = m[6], a22 = m[10];
  const float det = a00 * (a11 * a22 - a12 * a21) -
                    a01 * (a10 * a22 - a12 * a20) +
                    a02 * (a10 * a21 - a11 * a20);
  if (std::fabs(det) < 1e-12f) {
    out[0] = 1; out[1] = 0; out[2] = 0;
    out[3] = 0; out[4] = 1; out[5] = 0;
    out[6] = 0; out[7] = 0; out[8] = 1;
    return;
  }
  const float inv = 1.0f / det;
  // inv[row][col]
  out[0] = (a11 * a22 - a12 * a21) * inv;   // i00
  out[1] = -(a01 * a22 - a02 * a21) * inv;  // i01
  out[2] = (a01 * a12 - a02 * a11) * inv;   // i02
  out[3] = -(a10 * a22 - a12 * a20) * inv;  // i10
  out[4] = (a00 * a22 - a02 * a20) * inv;   // i11
  out[5] = -(a00 * a12 - a02 * a10) * inv;  // i12
  out[6] = (a10 * a21 - a11 * a20) * inv;   // i20
  out[7] = -(a00 * a21 - a01 * a20) * inv;  // i21
  out[8] = (a00 * a11 - a01 * a10) * inv;   // i22
}

}  // namespace

GLRenderer::~GLRenderer() { shutdown(); }

bool GLRenderer::init(GLFWwindow* window, std::string* err) {
  window_ = window;
  caps_.backend_name = "OpenGL";
  caps_.usesZeroToOneDepth = false;
  caps_.flipViewportV = true;
  caps_.supportsGpuSkinning = true;
  caps_.supportsExtendedGpuSkinning = true;
  const GLubyte* renderer = glGetString(GL_RENDERER);
  const GLubyte* vendor = glGetString(GL_VENDOR);
  const GLubyte* version = glGetString(GL_VERSION);
  caps_.gpu_name = renderer ? reinterpret_cast<const char*>(renderer) : "unknown";
  if (vendor && caps_.gpu_name.find(reinterpret_cast<const char*>(vendor)) ==
                    std::string::npos) {
    caps_.gpu_name = std::string(reinterpret_cast<const char*>(vendor)) + " " +
                     caps_.gpu_name;
  }
  caps_.api_info = version ? reinterpret_cast<const char*>(version) : "OpenGL";
  {
    // GL exposes no device-type enum, so classify from the renderer string.
    // Only the software rasterizers need identifying; everything else is a GPU.
    std::string lower = caps_.gpu_name;
    for (char& c : lower) c = static_cast<char>(std::tolower(c));
    const bool software = lower.find("llvmpipe") != std::string::npos ||
                          lower.find("softpipe") != std::string::npos ||
                          lower.find("swrast") != std::string::npos ||
                          lower.find("lavapipe") != std::string::npos ||
                          lower.find("software rasterizer") != std::string::npos;
    caps_.device_type = software ? "cpu" : "gpu";
  }
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize_);
  if (GLAD_GL_ARB_texture_filter_anisotropic) {
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxTextureAnisotropy_);
    if (!std::isfinite(maxTextureAnisotropy_) || maxTextureAnisotropy_ < 1.0f)
      maxTextureAnisotropy_ = 1.0f;
    maxTextureAnisotropy_ = std::min(maxTextureAnisotropy_, 16.0f);
  }

  // Compressed-texture format support (extension strings + core versions).
  {
    std::string exts;
    GLint next = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    exts.reserve(static_cast<size_t>(next) * 24);
    for (GLint i = 0; i < next; ++i) {
      const GLubyte* e = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
      if (e) { exts += reinterpret_cast<const char*>(e); exts += ' '; }
    }
    auto has = [&](const char* s) { return exts.find(s) != std::string::npos; };
    const bool bptc = has("texture_compression_bptc") || GLAD_GL_VERSION_4_2;
    caps_.supportsBC = has("GL_EXT_texture_compression_s3tc") || bptc;  // BC1/3/7
    caps_.supportsBC5 = has("texture_compression_rgtc") || GLAD_GL_VERSION_3_0;
    caps_.supportsBC6H = bptc;
    caps_.supportsASTC = has("GL_KHR_texture_compression_astc_ldr");
    caps_.supportsETC2 = has("GL_ARB_ES3_compatibility") || GLAD_GL_VERSION_4_3;
  }

  // The complete graph interpreter is intentionally kept for capable hardware,
  // but software GL drivers are disproportionately sensitive to the register
  // pressure from its 64-node arrays plus the full OpenPBR lobe stack. The
  // compact path retains the baked MaterialX semantic lanes and shared direct
  // light packet, so llvmpipe/softpipe get deterministic raster output without
  // sacrificing the high-feature path on hardware GL.
  const bool useCompactMaterial = caps_.device_type == "cpu";
  program_ = glutil::CompileProgram(
      light3d::getMaterialVertexShaderGL330(),
      useCompactMaterial ? light3d::getMaterialFragmentShaderGL330Fallback()
                          : light3d::getMaterialFragmentShaderGL330(),
      err);
  if (!program_) {
    // Low-sampler GL implementations (notably software Mesa) can reject the
    // full MaterialX/OpenPBR shader before scene initialization. Keep the
    // raster backend usable with a one-texture bounded preview; capable
    // drivers continue using the complete shader above.
    std::string fallbackErr;
    program_ = glutil::CompileProgram(
        light3d::getMaterialVertexShaderGL330(),
        light3d::getMaterialFragmentShaderGL330Fallback(), &fallbackErr);
    if (!program_) {
      if (err && err->empty()) *err = "Failed to build GL material program: " + fallbackErr;
      return false;
    }
    std::fprintf(stderr, "[lusdview] full GL material shader unavailable; using low-sampler fallback: %s\n",
                 err ? err->c_str() : "fragment sampler limit");
  }
  glUseProgram(program_);
  uMVP_ = glGetUniformLocation(program_, "uModelViewProj");
  uModel_ = glGetUniformLocation(program_, "uModel");
  uNormalMat_ = glGetUniformLocation(program_, "uNormalMatrix");
  uCameraPos_ = glGetUniformLocation(program_, "uCameraPos");
  uLightDir_ = glGetUniformLocation(program_, "uLightDir");
  uLightColor_ = glGetUniformLocation(program_, "uLightColor");
  uHasIbl_ = glGetUniformLocation(program_, "uHasIbl");
  uIblColor_ = glGetUniformLocation(program_, "uIblColor");
  uExposure_ = glGetUniformLocation(program_, "uExposure");
  uEnvRotation_ = glGetUniformLocation(program_, "uEnvRotation");
  uPrefilteredLods_ = glGetUniformLocation(program_, "uPrefilteredLods");
  uHasShadowMap_ = glGetUniformLocation(program_, "uHasShadowMap");
  uShadowLightSlot_ = glGetUniformLocation(program_, "uShadowLightSlot");
  uShadowViewProj_ = glGetUniformLocation(program_, "uShadowViewProj");
  uHasPointShadowMap_ = glGetUniformLocation(program_, "uHasPointShadowMap");
  uPointShadowLightPos_ = glGetUniformLocation(program_, "uPointShadowLightPos");
  uPointShadowViewProj_ = glGetUniformLocation(program_, "uPointShadowViewProj");
  uGeometricNormal_ = glGetUniformLocation(program_, "uGeometricNormal");
  uRenderMode_ = glGetUniformLocation(program_, "uRenderMode");
  uMatId_ = glGetUniformLocation(program_, "uMatId");
  uDepthScale_ = glGetUniformLocation(program_, "uDepthScale");
  uSceneMin_ = glGetUniformLocation(program_, "uSceneMin");
  uSceneExtent_ = glGetUniformLocation(program_, "uSceneExtent");
  uMeshId_ = glGetUniformLocation(program_, "uMeshId");
  uDoubleSided_ = glGetUniformLocation(program_, "uDoubleSided");
  uPurpose_ = glGetUniformLocation(program_, "uPurpose");
  uKind_ = glGetUniformLocation(program_, "uKind");
  uFaceIdTex_ = glGetUniformLocation(program_, "uFaceIdTex");
  uFaceBase_ = glGetUniformLocation(program_, "uFaceBase");
  uHasFaceId_ = glGetUniformLocation(program_, "uHasFaceId");
  uBasePtex_ = glGetUniformLocation(program_, "uBasePtex");
  uBasePtexGrid_ = glGetUniformLocation(program_, "uBasePtexGrid");
  uMetallicPtexGrid_ = glGetUniformLocation(program_, "uMetallicPtexGrid");
  uRoughnessPtexGrid_ = glGetUniformLocation(program_, "uRoughnessPtexGrid");
  uNormalPtexGrid_ = glGetUniformLocation(program_, "uNormalPtexGrid");
  uEmissivePtexGrid_ = glGetUniformLocation(program_, "uEmissivePtexGrid");
  uOpacityPtexGrid_ = glGetUniformLocation(program_, "uOpacityPtexGrid");
  uOcclusionPtexGrid_ = glGetUniformLocation(program_, "uOcclusionPtexGrid");
  uSpecularColorPtexGrid_ = glGetUniformLocation(program_, "uSpecularColorPtexGrid");
  uCoatWeightPtexGrid_ = glGetUniformLocation(program_, "uCoatWeightPtexGrid");
  uCoatColorPtexGrid_ = glGetUniformLocation(program_, "uCoatColorPtexGrid");
  uCoatRoughnessPtexGrid_ = glGetUniformLocation(program_, "uCoatRoughnessPtexGrid");
  uBaseColor_ = glGetUniformLocation(program_, "uBaseColor");
  uMetallic_ = glGetUniformLocation(program_, "uMetallic");
  uRoughness_ = glGetUniformLocation(program_, "uRoughness");
  uUseSpecularWorkflow_ = glGetUniformLocation(program_, "uUseSpecularWorkflow");
  uOpenPbrSpecularModel_ = glGetUniformLocation(program_, "uOpenPbrSpecularModel");
  uSpecularColor_ = glGetUniformLocation(program_, "uSpecularColor");
  uIor_ = glGetUniformLocation(program_, "uIor");
  uOcclusion_ = glGetUniformLocation(program_, "uOcclusion");
  uHasSpecularColorTex_ = glGetUniformLocation(program_, "uHasSpecularColorTex");
  uSpecularColorUv0_ = glGetUniformLocation(program_, "uSpecularColorUv0");
  uSpecularColorUv1_ = glGetUniformLocation(program_, "uSpecularColorUv1");
  uSpecularColorUvSet_ = glGetUniformLocation(program_, "uSpecularColorUvSet");
  uSpecularColorScale_ = glGetUniformLocation(program_, "uSpecularColorScale");
  uSpecularColorBias_ = glGetUniformLocation(program_, "uSpecularColorBias");
  uHasCoatNormalTex_ = glGetUniformLocation(program_, "uHasCoatNormalTex");
  uCoatNormalUv0_ = glGetUniformLocation(program_, "uCoatNormalUv0");
  uCoatNormalUv1_ = glGetUniformLocation(program_, "uCoatNormalUv1");
  uCoatNormalUvSet_ = glGetUniformLocation(program_, "uCoatNormalUvSet");
  uCoatNormalScale_ = glGetUniformLocation(program_, "uCoatNormalScale");
  uCoatNormalBias_ = glGetUniformLocation(program_, "uCoatNormalBias");
  uHasCoatWeightTex_ = glGetUniformLocation(program_, "uHasCoatWeightTex");
  uHasCoatColorTex_ = glGetUniformLocation(program_, "uHasCoatColorTex");
  uHasCoatRoughnessTex_ = glGetUniformLocation(program_, "uHasCoatRoughnessTex");
  uCoatWeightUv0_ = glGetUniformLocation(program_, "uCoatWeightUv0");
  uCoatWeightUv1_ = glGetUniformLocation(program_, "uCoatWeightUv1");
  uCoatColorUv0_ = glGetUniformLocation(program_, "uCoatColorUv0");
  uCoatColorUv1_ = glGetUniformLocation(program_, "uCoatColorUv1");
  uCoatRoughnessUv0_ = glGetUniformLocation(program_, "uCoatRoughnessUv0");
  uCoatRoughnessUv1_ = glGetUniformLocation(program_, "uCoatRoughnessUv1");
  uCoatWeightUvSet_ = glGetUniformLocation(program_, "uCoatWeightUvSet");
  uCoatColorUvSet_ = glGetUniformLocation(program_, "uCoatColorUvSet");
  uCoatRoughnessUvSet_ = glGetUniformLocation(program_, "uCoatRoughnessUvSet");
  uCoatWeightChannel_ = glGetUniformLocation(program_, "uCoatWeightChannel");
  uCoatRoughnessChannel_ =
      glGetUniformLocation(program_, "uCoatRoughnessChannel");
  uCoatWeightScale_ = glGetUniformLocation(program_, "uCoatWeightScale");
  uCoatWeightBias_ = glGetUniformLocation(program_, "uCoatWeightBias");
  uCoatColorScale_ = glGetUniformLocation(program_, "uCoatColorScale");
  uCoatColorBias_ = glGetUniformLocation(program_, "uCoatColorBias");
  uCoatRoughnessScale_ = glGetUniformLocation(program_, "uCoatRoughnessScale");
  uCoatRoughnessBias_ = glGetUniformLocation(program_, "uCoatRoughnessBias");
  uAdvancedTexIsUdim_ = glGetUniformLocation(program_, "uAdvancedTexIsUdim");
  uAdvancedUdimRoutes_ = glGetUniformLocation(program_, "uAdvancedUdimRoutes");
  uAdvancedUdimSlots_ = glGetUniformLocation(program_, "uAdvancedUdimSlots");
  uCoatNormalTexIsUdim_ =
      glGetUniformLocation(program_, "uCoatNormalTexIsUdim");
  uCoatNormalUdimRoute_ =
      glGetUniformLocation(program_, "uCoatNormalUdimRoute");
  uCoatNormalUdimSlot_ =
      glGetUniformLocation(program_, "uCoatNormalUdimSlot");
  uGraphNodeCount_ = glGetUniformLocation(program_, "uGraphNodeCount");
  uGraphOutputs_ = glGetUniformLocation(program_, "uGraphOutputs[0]");
  uGraphUsable_ = glGetUniformLocation(program_, "uGraphUsable");
  uGraphNode0_ = glGetUniformLocation(program_, "uGraphNode0");
  uGraphNode1_ = glGetUniformLocation(program_, "uGraphNode1");
  uGraphNode2_ = glGetUniformLocation(program_, "uGraphNode2");
  uGraphNode3_ = glGetUniformLocation(program_, "uGraphNode3");
  uGraphNode4_ = glGetUniformLocation(program_, "uGraphNode4");
  uGraphNode5_ = glGetUniformLocation(program_, "uGraphNode5");
  for (int i = 0; i < 8; ++i) {
    uGraphTex_[i] = glGetUniformLocation(
        program_, (std::string("uGraphTex") + std::to_string(i)).c_str());
    // 31 is the point-shadow cube and 32 is the Ptex rectangle buffer. Sharing
    // either with a sampler2D makes the subsequent draw GL_INVALID_OPERATION,
    // even when the graph route is disabled.
    if (uGraphTex_[i] >= 0) glUniform1i(uGraphTex_[i], 33 + i);
  }
  uGraphUdimRoutes_ = glGetUniformLocation(program_, "uGraphUdimRoutes[0]");
  uCoatWeight_ = glGetUniformLocation(program_, "uCoatWeight");
  uCoatColor_ = glGetUniformLocation(program_, "uCoatColor");
  uCoatRoughness_ = glGetUniformLocation(program_, "uCoatRoughness");
  uCoatIor_ = glGetUniformLocation(program_, "uCoatIor");
  uTransmission_ = glGetUniformLocation(program_, "uTransmission");
  uTransmissionColor_ = glGetUniformLocation(program_, "uTransmissionColor");
  uTransmissionDepth_ = glGetUniformLocation(program_, "uTransmissionDepth");
  uTransmissionScatter_ = glGetUniformLocation(program_, "uTransmissionScatter");
  uTransmissionScatterAnisotropy_ =
      glGetUniformLocation(program_, "uTransmissionScatterAnisotropy");
  uVolumeDensity_ = glGetUniformLocation(program_, "uVolumeDensity");
  uVolumeAlbedo_ = glGetUniformLocation(program_, "uVolumeAlbedo");
  uVolumeEmission_ = glGetUniformLocation(program_, "uVolumeEmission");
  uVolumeEmissionScale_ =
      glGetUniformLocation(program_, "uVolumeEmissionScale");
  uDiffuseRoughness_ = glGetUniformLocation(program_, "uDiffuseRoughness");
  uTransmissionDispersionAbbeNumber_ =
      glGetUniformLocation(program_, "uTransmissionDispersionAbbeNumber");
  uSubsurfaceAnisotropy_ =
      glGetUniformLocation(program_, "uSubsurfaceAnisotropy");
  uSubsurfaceScatterAnisotropy_ =
      glGetUniformLocation(program_, "uSubsurfaceScatterAnisotropy");
  uSpecularAnisotropy_ = glGetUniformLocation(program_, "uSpecularAnisotropy");
  uSpecularRotation_ = glGetUniformLocation(program_, "uSpecularRotation");
  uSpecularRoughnessAnisotropy_ =
      glGetUniformLocation(program_, "uSpecularRoughnessAnisotropy");
  uCoatAnisotropy_ = glGetUniformLocation(program_, "uCoatAnisotropy");
  uCoatRotation_ = glGetUniformLocation(program_, "uCoatRotation");
  uCoatRoughnessAnisotropy_ =
      glGetUniformLocation(program_, "uCoatRoughnessAnisotropy");
  uTransmissionDispersion_ =
      glGetUniformLocation(program_, "uTransmissionDispersion");
  uTransmissionDispersionScale_ =
      glGetUniformLocation(program_, "uTransmissionDispersionScale");
  uCoatAffectColor_ = glGetUniformLocation(program_, "uCoatAffectColor");
  uCoatAffectRoughness_ =
      glGetUniformLocation(program_, "uCoatAffectRoughness");
  uCoatDarkening_ = glGetUniformLocation(program_, "uCoatDarkening");
  uBaseWeight_ = glGetUniformLocation(program_, "uBaseWeight");
  uSpecularWeight_ = glGetUniformLocation(program_, "uSpecularWeight");
  uSubsurface_ = glGetUniformLocation(program_, "uSubsurface");
  uSubsurfaceColor_ = glGetUniformLocation(program_, "uSubsurfaceColor");
  uSubsurfaceScale_ = glGetUniformLocation(program_, "uSubsurfaceScale");
  uSubsurfaceRadius_ = glGetUniformLocation(program_, "uSubsurfaceRadius");
  uSheenWeight_ = glGetUniformLocation(program_, "uSheenWeight");
  uSheenColor_ = glGetUniformLocation(program_, "uSheenColor");
  uSheenRoughness_ = glGetUniformLocation(program_, "uSheenRoughness");
  uThinFilmWeight_ = glGetUniformLocation(program_, "uThinFilmWeight");
  uThinFilmThickness_ = glGetUniformLocation(program_, "uThinFilmThickness");
  uThinFilmIor_ = glGetUniformLocation(program_, "uThinFilmIor");
  uEmissive_ = glGetUniformLocation(program_, "uEmissive");
  uAlpha_ = glGetUniformLocation(program_, "uAlpha");
  uAlphaMode_ = glGetUniformLocation(program_, "uAlphaMode");
  uAlphaCutoff_ = glGetUniformLocation(program_, "uAlphaCutoff");
  uHasBaseColorTex_ = glGetUniformLocation(program_, "uHasBaseColorTex");
  uHasMetallicTex_ = glGetUniformLocation(program_, "uHasMetallicTex");
  uHasRoughnessTex_ = glGetUniformLocation(program_, "uHasRoughnessTex");
  uHasNormalTex_ = glGetUniformLocation(program_, "uHasNormalTex");
  uHasEmissiveTex_ = glGetUniformLocation(program_, "uHasEmissiveTex");
  uHasOpacityTex_ = glGetUniformLocation(program_, "uHasOpacityTex");
  uHasOcclusionTex_ = glGetUniformLocation(program_, "uHasOcclusionTex");
  uBaseColorTexIsUdim_ = glGetUniformLocation(program_, "uBaseColorTexIsUdim");
  uMetallicTexIsUdim_ = glGetUniformLocation(program_, "uMetallicTexIsUdim");
  uRoughnessTexIsUdim_ = glGetUniformLocation(program_, "uRoughnessTexIsUdim");
  uNormalTexIsUdim_ = glGetUniformLocation(program_, "uNormalTexIsUdim");
  uEmissiveTexIsUdim_ = glGetUniformLocation(program_, "uEmissiveTexIsUdim");
  uOpacityTexIsUdim_ = glGetUniformLocation(program_, "uOpacityTexIsUdim");
  uOcclusionTexIsUdim_ = glGetUniformLocation(program_, "uOcclusionTexIsUdim");
  uBaseColorUv0_ = glGetUniformLocation(program_, "uBaseColorUv0");
  uBaseColorUv1_ = glGetUniformLocation(program_, "uBaseColorUv1");
  uMetallicUv0_ = glGetUniformLocation(program_, "uMetallicUv0");
  uMetallicUv1_ = glGetUniformLocation(program_, "uMetallicUv1");
  uRoughnessUv0_ = glGetUniformLocation(program_, "uRoughnessUv0");
  uRoughnessUv1_ = glGetUniformLocation(program_, "uRoughnessUv1");
  uNormalUv0_ = glGetUniformLocation(program_, "uNormalUv0");
  uNormalUv1_ = glGetUniformLocation(program_, "uNormalUv1");
  uEmissiveUv0_ = glGetUniformLocation(program_, "uEmissiveUv0");
  uEmissiveUv1_ = glGetUniformLocation(program_, "uEmissiveUv1");
  uOpacityUv0_ = glGetUniformLocation(program_, "uOpacityUv0");
  uOpacityUv1_ = glGetUniformLocation(program_, "uOpacityUv1");
  uOcclusionUv0_ = glGetUniformLocation(program_, "uOcclusionUv0");
  uOcclusionUv1_ = glGetUniformLocation(program_, "uOcclusionUv1");
  uUvSet_ = glGetUniformLocation(program_, "uUvSet");
  uRoughnessUvSet_ = glGetUniformLocation(program_, "uRoughnessUvSet");
  uBaseColorTexScale_ = glGetUniformLocation(program_, "uBaseColorTexScale");
  uBaseColorTexBias_ = glGetUniformLocation(program_, "uBaseColorTexBias");
  uNormalTexScale_ = glGetUniformLocation(program_, "uNormalTexScale");
  uNormalTexBias_ = glGetUniformLocation(program_, "uNormalTexBias");
  uEmissiveTexScale_ = glGetUniformLocation(program_, "uEmissiveTexScale");
  uEmissiveTexBias_ = glGetUniformLocation(program_, "uEmissiveTexBias");
  uMetallicChannel_ = glGetUniformLocation(program_, "uMetallicChannel");
  uRoughnessChannel_ = glGetUniformLocation(program_, "uRoughnessChannel");
  uMetallicTexScale_ = glGetUniformLocation(program_, "uMetallicTexScale");
  uMetallicTexBias_ = glGetUniformLocation(program_, "uMetallicTexBias");
  uRoughnessTexScale_ = glGetUniformLocation(program_, "uRoughnessTexScale");
  uRoughnessTexBias_ = glGetUniformLocation(program_, "uRoughnessTexBias");
  uOpacityUvSet_ = glGetUniformLocation(program_, "uOpacityUvSet");
  uOpacityChannel_ = glGetUniformLocation(program_, "uOpacityChannel");
  uOpacityTexScale_ = glGetUniformLocation(program_, "uOpacityTexScale");
  uOpacityTexBias_ = glGetUniformLocation(program_, "uOpacityTexBias");
  uOcclusionUvSet_ = glGetUniformLocation(program_, "uOcclusionUvSet");
  uOcclusionChannel_ = glGetUniformLocation(program_, "uOcclusionChannel");
  uOcclusionTexScale_ = glGetUniformLocation(program_, "uOcclusionTexScale");
  uOcclusionTexBias_ = glGetUniformLocation(program_, "uOcclusionTexBias");
  uUdimSlots_ = glGetUniformLocation(program_, "uUdimSlots");
  uOpacityUdimSlot_ = glGetUniformLocation(program_, "uOpacityUdimSlot");
  uOcclusionUdimSlot_ = glGetUniformLocation(program_, "uOcclusionUdimSlot");
  uRoughnessUdimSlot_ = glGetUniformLocation(program_, "uRoughnessUdimSlot");
  uHasDisplacement_ = glGetUniformLocation(program_, "uHasDisplacement");
  uHasDisplacementTex_ = glGetUniformLocation(program_, "uHasDisplacementTex");
  uDisplacementConst_ = glGetUniformLocation(program_, "uDisplacementConst");
  uDisplacementScale_ = glGetUniformLocation(program_, "uDisplacementScale");
  uDisplacementTexScale_ = glGetUniformLocation(program_, "uDisplacementTexScale");
  uDisplacementTexBias_ = glGetUniformLocation(program_, "uDisplacementTexBias");
  uSkinningEnabled_ = glGetUniformLocation(program_, "uSkinningEnabled");
  uExtendedSkinningEnabled_ = glGetUniformLocation(program_, "uExtendedSkinningEnabled");
  uBoneTexWidth_ = glGetUniformLocation(program_, "uBoneTexWidth");
  uBoneMatrixCount_ = glGetUniformLocation(program_, "uBoneMatrixCount");
  uInfluenceTexWidth_ = glGetUniformLocation(program_, "uInfluenceTexWidth");
  // Fixed sampler -> texture-unit bindings.
  glUniform1i(glGetUniformLocation(program_, "uBaseColorTex"), 0);
  glUniform1i(glGetUniformLocation(program_, "uMetallicTex"), 1);
  glUniform1i(glGetUniformLocation(program_, "uRoughnessTex"), 18);
  glUniform1i(glGetUniformLocation(program_, "uNormalTex"), 2);
  glUniform1i(glGetUniformLocation(program_, "uEmissiveTex"), 3);
  glUniform1i(glGetUniformLocation(program_, "uOpacityTex"), 14);
  glUniform1i(glGetUniformLocation(program_, "uBaseColorUdimTex"), 11);
  glUniform1i(glGetUniformLocation(program_, "uUdimLutAtlas"), 12);
  glUniform1i(glGetUniformLocation(program_, "uMetallicUdimTex"), 13);
  glUniform1i(glGetUniformLocation(program_, "uRoughnessUdimTex"), 22);
  glUniform1i(glGetUniformLocation(program_, "uNormalUdimTex"), 15);
  glUniform1i(glGetUniformLocation(program_, "uOpacityUdimTex"), 16);
  glUniform1i(glGetUniformLocation(program_, "uEmissiveUdimTex"), 17);
  glUniform1i(glGetUniformLocation(program_, "uOcclusionTex"), 23);
  glUniform1i(glGetUniformLocation(program_, "uOcclusionUdimTex"), 24);
  // Advanced ordinary slots keep independent 2D units. Their UDIM arrays are
  // routed per draw through currently-unused core array units.
  glUniform1i(glGetUniformLocation(program_, "uSpecularColorTex"), 26);
  glUniform1i(glGetUniformLocation(program_, "uCoatWeightTex"), 27);
  glUniform1i(glGetUniformLocation(program_, "uCoatColorTex"), 28);
  glUniform1i(glGetUniformLocation(program_, "uCoatRoughnessTex"), 29);
  glUniform1i(glGetUniformLocation(program_, "uCoatNormalTex"), 30);
  glUniform1i(glGetUniformLocation(program_, "uBoneTex"), 4);
  glUniform1i(glGetUniformLocation(program_, "uInfluenceTex"), 5);
  glUniform1i(uFaceIdTex_, 6);  // source-face-id texture buffer
  glUniform1i(glGetUniformLocation(program_, "uDisplacementTex"), 7);
  glUniform1i(glGetUniformLocation(program_, "uMorphDeltaTex"), 8);  // GPU morph
  glUniform1i(glGetUniformLocation(program_, "uMorphCoeffTex"), 9);
  glUniform1i(glGetUniformLocation(program_, "uMorphChanTex"), 10);  // skip pre-check
  glUniform1i(glGetUniformLocation(program_, "uIrradianceMap"), 19);  // dome IBL
  glUniform1i(glGetUniformLocation(program_, "uPrefilteredMap"), 20);
  glUniform1i(glGetUniformLocation(program_, "uBrdfLut"), 21);
  glUniform1i(glGetUniformLocation(program_, "uShadowMap"), 25);
  glUniform1i(glGetUniformLocation(program_, "uPointShadowMap"), 31);
  glUniform1i(glGetUniformLocation(program_, "uPtexRectTex"), 32);
  // Filter across cube-face borders (core since GL 3.2); matters for the
  // low-res prefiltered/irradiance cubes.
  glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
  uHasMorph_ = glGetUniformLocation(program_, "uHasMorph");
  glUseProgram(0);

  // One deterministic depth map shared by all raster materials.
  static const char* kShadowFS = R"glsl(#version 330 core
in vec2 vUV;
in vec2 vUV1;
uniform int uAlphaMode;
uniform float uAlpha;
uniform float uAlphaCutoff;
uniform bool uHasBaseAlphaTex;
uniform bool uHasOpacityTex;
uniform sampler2D uBaseAlphaTex;
uniform sampler2D uOpacityTex;
uniform sampler2DArray uBaseAlphaUdimTex;
uniform sampler2DArray uOpacityUdimTex;
uniform isampler2D uUdimLutAtlas;
uniform bool uBaseAlphaIsUdim;
uniform bool uOpacityIsUdim;
uniform int uBaseAlphaUdimSlot;
uniform int uOpacityUdimSlot;
uniform vec3 uBaseAlphaUv0;
uniform vec3 uBaseAlphaUv1;
uniform int uBaseAlphaUvSet;
uniform vec4 uBaseAlphaScale;
uniform vec4 uBaseAlphaBias;
uniform vec3 uOpacityUv0;
uniform vec3 uOpacityUv1;
uniform int uOpacityUvSet;
uniform int uOpacityChannel;
uniform float uOpacityScale;
uniform float uOpacityBias;
vec2 xf(vec2 uv, vec3 a, vec3 b) {
  return vec2(a.x * uv.x + a.y * uv.y + a.z,
              b.x * uv.x + b.y * uv.y + b.z);
}
float channelOf(vec4 c, int ch) {
  return ch == 1 ? c.g : ch == 2 ? c.b : ch == 3 ? c.a : c.r;
}
vec4 sampleUdim(sampler2DArray tex, int slot, vec2 uv, vec4 missing) {
  ivec2 tile = ivec2(floor(uv));
  int idx = tile.x + tile.y * 10;
  if (idx < 0 || idx >= 100) return missing;
  int layer = texelFetch(uUdimLutAtlas, ivec2(idx, slot), 0).r;
  return layer < 0 ? missing : texture(tex, vec3(fract(uv), float(layer)));
}
void main() {
  if (uAlphaMode != 1) return;
  float a = uAlpha;
  if (uHasBaseAlphaTex) {
    vec2 uv = uBaseAlphaUvSet == 1 ? vUV1 : vUV;
    vec2 tuv = xf(uv, uBaseAlphaUv0, uBaseAlphaUv1);
    vec4 c = uBaseAlphaIsUdim
        ? sampleUdim(uBaseAlphaUdimTex, uBaseAlphaUdimSlot, tuv, vec4(1.0))
        : texture(uBaseAlphaTex, tuv);
    a *= (c * uBaseAlphaScale + uBaseAlphaBias).a;
  }
  if (uHasOpacityTex) {
    vec2 uv = uOpacityUvSet == 1 ? vUV1 : vUV;
    vec2 tuv = xf(uv, uOpacityUv0, uOpacityUv1);
    vec4 c = uOpacityIsUdim
        ? sampleUdim(uOpacityUdimTex, uOpacityUdimSlot, tuv, vec4(1.0))
        : texture(uOpacityTex, tuv);
    a *= channelOf(c, uOpacityChannel) * uOpacityScale + uOpacityBias;
  }
  if (a < uAlphaCutoff) discard;
}
)glsl";
  std::string shadowErr;
  shadowProgram_ = glutil::CompileProgram(light3d::getMaterialVertexShaderGL330(),
                                          kShadowFS, &shadowErr);
  if (!shadowProgram_) {
    if (err) *err = "Failed to build GL shadow program: " + shadowErr;
    return false;
  }
  sMVP_ = glGetUniformLocation(shadowProgram_, "uModelViewProj");
  sModel_ = glGetUniformLocation(shadowProgram_, "uModel");
  sNormalMat_ = glGetUniformLocation(shadowProgram_, "uNormalMatrix");
  sSkinningEnabled_ = glGetUniformLocation(shadowProgram_, "uSkinningEnabled");
  sExtendedSkinningEnabled_ = glGetUniformLocation(shadowProgram_, "uExtendedSkinningEnabled");
  sBoneTexWidth_ = glGetUniformLocation(shadowProgram_, "uBoneTexWidth");
  sBoneMatrixCount_ = glGetUniformLocation(shadowProgram_, "uBoneMatrixCount");
  sInfluenceTexWidth_ = glGetUniformLocation(shadowProgram_, "uInfluenceTexWidth");
  sHasMorph_ = glGetUniformLocation(shadowProgram_, "uHasMorph");
  sHasDisplacement_ = glGetUniformLocation(shadowProgram_, "uHasDisplacement");
  sHasDisplacementTex_ = glGetUniformLocation(shadowProgram_, "uHasDisplacementTex");
  sDisplacementConst_ = glGetUniformLocation(shadowProgram_, "uDisplacementConst");
  sDisplacementScale_ = glGetUniformLocation(shadowProgram_, "uDisplacementScale");
  sDisplacementTexScale_ = glGetUniformLocation(shadowProgram_, "uDisplacementTexScale");
  sDisplacementTexBias_ = glGetUniformLocation(shadowProgram_, "uDisplacementTexBias");
  sAlphaMode_ = glGetUniformLocation(shadowProgram_, "uAlphaMode");
  sAlpha_ = glGetUniformLocation(shadowProgram_, "uAlpha");
  sAlphaCutoff_ = glGetUniformLocation(shadowProgram_, "uAlphaCutoff");
  sHasBaseAlphaTex_ = glGetUniformLocation(shadowProgram_, "uHasBaseAlphaTex");
  sHasOpacityTex_ = glGetUniformLocation(shadowProgram_, "uHasOpacityTex");
  sBaseAlphaIsUdim_ = glGetUniformLocation(shadowProgram_, "uBaseAlphaIsUdim");
  sOpacityIsUdim_ = glGetUniformLocation(shadowProgram_, "uOpacityIsUdim");
  sBaseAlphaUdimSlot_ = glGetUniformLocation(shadowProgram_, "uBaseAlphaUdimSlot");
  sOpacityUdimSlot_ = glGetUniformLocation(shadowProgram_, "uOpacityUdimSlot");
  sBaseAlphaUv0_ = glGetUniformLocation(shadowProgram_, "uBaseAlphaUv0");
  sBaseAlphaUv1_ = glGetUniformLocation(shadowProgram_, "uBaseAlphaUv1");
  sBaseAlphaUvSet_ = glGetUniformLocation(shadowProgram_, "uBaseAlphaUvSet");
  sBaseAlphaScale_ = glGetUniformLocation(shadowProgram_, "uBaseAlphaScale");
  sBaseAlphaBias_ = glGetUniformLocation(shadowProgram_, "uBaseAlphaBias");
  sOpacityUv0_ = glGetUniformLocation(shadowProgram_, "uOpacityUv0");
  sOpacityUv1_ = glGetUniformLocation(shadowProgram_, "uOpacityUv1");
  sOpacityUvSet_ = glGetUniformLocation(shadowProgram_, "uOpacityUvSet");
  sOpacityChannel_ = glGetUniformLocation(shadowProgram_, "uOpacityChannel");
  sOpacityScale_ = glGetUniformLocation(shadowProgram_, "uOpacityScale");
  sOpacityBias_ = glGetUniformLocation(shadowProgram_, "uOpacityBias");
  glUseProgram(shadowProgram_);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uBoneTex"), 4);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uInfluenceTex"), 5);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uDisplacementTex"), 7);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uMorphDeltaTex"), 8);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uMorphCoeffTex"), 9);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uMorphChanTex"), 10);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uBaseAlphaTex"), 0);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uOpacityTex"), 14);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uBaseAlphaUdimTex"), 11);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uOpacityUdimTex"), 16);
  glUniform1i(glGetUniformLocation(shadowProgram_, "uUdimLutAtlas"), 12);
  glGenTextures(1, &shadowDepthTex_);
  glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 2048, 2048, 0,
               GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  const float border[4] = {1, 1, 1, 1};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
  glGenTextures(1, &pointShadowDepthTex_);
  glBindTexture(GL_TEXTURE_CUBE_MAP, pointShadowDepthTex_);
  for (int face = 0; face < 6; ++face) {
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT24,
                 2048, 2048, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
  }
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &shadowFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                         shadowDepthTex_, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    if (err) *err = "Failed to create GL shadow framebuffer";
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glUseProgram(0);

  // GPU tessellation displacement program (GL >= 4.0 only). Failure to build is
  // non-fatal: the renderer simply keeps using the coarse per-vertex path.
  if (GLAD_GL_VERSION_4_0) {
    buildTessProgram();
  }

  // Instanced flat-shaded program: per-instance 3x4 object-to-world (attribs 6-8,
  // divisor 1) + per-instance/prototype displayColor (attrib 9), drawn with
  // glDrawElementsInstanced. Self-contained flat shader (Lambert + ambient +
  // Blinn spec, same hardcoded headlight as the material shader) so it needs no
  // material uniforms -- the base color comes from the per-instance vColor.
  static const char* kInstancedVS =
      "#version 330 core\n"
      "layout(location=0) in vec3 aPosition;\n"
      "layout(location=1) in vec3 aNormal;\n"
      // Per-instance 3x4 object-to-world (row-major o2w; rows = output x/y/z):
      // worldP.c = dot(vec4(pos,1), aRow[c]).  48 B/instance vs a full mat4's 64.
      // The non-instanced program's skin slots (3/4) are taken by these rows, so
      // instanced skinning carries its joints/weights at 6/7 instead (attrib 8 is
      // the morph CSR). All instances of a prototype share ONE bone block: USD
      // instancing requires identical composed contents, hence one skeleton+pose.
      "layout(location=3) in vec4 aRow0;\n"
      "layout(location=4) in vec4 aRow1;\n"
      "layout(location=5) in vec4 aRow2;\n"
      "layout(location=6) in uvec4 aJoint;\n"
      "layout(location=7) in vec4 aWeight;\n"
      "layout(location=9) in vec3 aColor;\n"     // per-instance color or constant
      "layout(location=10) in vec4 aVtxColor;\n"  // displayColor.rgb + displayOpacity
      "layout(location=11) in float aOpacity;\n"  // per-instance opacity or constant
      // GPU blendshape morph (shared by instanced prototypes): per-vertex CSR
      // (offset,count) + delta/coeff/channelId texture-buffers, summed into the
      // local position before the per-instance transform. Same scheme + units
      // (8/9/10) as the non-instanced material shader.
      "layout(location=8) in uvec2 aMorphOffsetCount;\n"
      "uniform mat4 uViewProj;\n"
      "uniform bool uHasMorph;\n"
      "uniform samplerBuffer uMorphDeltaTex;\n"
      "uniform samplerBuffer uMorphCoeffTex;\n"
      "uniform usamplerBuffer uMorphChanTex;\n"
      // Skeletal skinning, shared with the non-instanced program: the same 4xN
      // RGBA32F bone texture (unit 4), absolute joint rows. Skinning happens in
      // PROTOTYPE-LOCAL space, before the per-instance transform.
      "uniform bool uSkinningEnabled;\n"
      "uniform sampler2D uBoneTex;\n"
      "uniform int uBoneTexWidth;\n"
      "uniform int uBoneMatrixCount;\n"
      "mat4 fetchBone(uint idx){\n"
      "  int base=int(idx)*4;\n"
      "  return mat4(\n"
      "    texelFetch(uBoneTex,ivec2((base+0)%uBoneTexWidth,(base+0)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+1)%uBoneTexWidth,(base+1)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+2)%uBoneTexWidth,(base+2)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+3)%uBoneTexWidth,(base+3)/uBoneTexWidth),0));\n"
      "}\n"
      "out vec3 vWorldPos;\n"
      "out vec3 vNormal;\n"
      "out vec3 vColor;\n"
      "out float vOpacity;\n"
      "flat out int vInstanceId;\n"
      "void main(){\n"
      "  vInstanceId = gl_InstanceID;\n"
      "  vec3 pos = aPosition;\n"
      "  if (uHasMorph && aMorphOffsetCount.y > 0u) {\n"
      "    int mbase = int(aMorphOffsetCount.x);\n"
      "    int mcount = min(int(aMorphOffsetCount.y), 256);\n"
      "    for (int i = 0; i < 256; ++i) {\n"
      "      if (i >= mcount) break;\n"
      "      int ch = int(texelFetch(uMorphChanTex, mbase + i).r);\n"
      "      float c = texelFetch(uMorphCoeffTex, ch).r;\n"
      "      if (abs(c) < 1e-6) continue;\n"
      "      pos += c * texelFetch(uMorphDeltaTex, mbase + i).yzw;\n"
      "    }\n"
      "  }\n"
      // Linear-blend skinning (prototype-local). Vertices of an unskinned mesh --
      // or of an unskinned vertex in a skinned one -- carry zero weights and pass
      // through untouched, exactly as in the non-instanced program.
      "  vec3 nrm = aNormal;\n"
      "  float wsum = aWeight.x + aWeight.y + aWeight.z + aWeight.w;\n"
      "  uint maxJoint = max(max(aJoint.x, aJoint.y), max(aJoint.z, aJoint.w));\n"
      "  if (uSkinningEnabled && wsum > 0.0 && int(maxJoint) < uBoneMatrixCount) {\n"
      "    mat4 skin = fetchBone(aJoint.x) * aWeight.x + fetchBone(aJoint.y) * aWeight.y\n"
      "              + fetchBone(aJoint.z) * aWeight.z + fetchBone(aJoint.w) * aWeight.w;\n"
      "    pos = (skin * vec4(pos, 1.0)).xyz;\n"
      "    nrm = normalize((skin * vec4(aNormal, 0.0)).xyz);\n"
      "  }\n"
      "  vec4 p = vec4(pos, 1.0);\n"
      "  vec3 wp = vec3(dot(p, aRow0), dot(p, aRow1), dot(p, aRow2));\n"
      "  vec3 n = vec3(dot(nrm, aRow0.xyz), dot(nrm, aRow1.xyz),\n"
      "                dot(nrm, aRow2.xyz));\n"
      "  vWorldPos = wp;\n"
      "  vNormal = normalize(n);\n"
      // Prototype per-vertex displayColor x per-instance color (both default 1).
      "  vColor = aColor * aVtxColor.rgb;\n"
      "  vOpacity = clamp(aOpacity * aVtxColor.a, 0.0, 1.0);\n"
      "  gl_Position = uViewProj * vec4(wp, 1.0);\n"
      "}\n";
  static const char* kInstancedFS =
      "#version 330 core\n"
      "in vec3 vWorldPos;\n"
      "in vec3 vNormal;\n"
      "in vec3 vColor;\n"
      "in float vOpacity;\n"
      "flat in int vInstanceId;\n"
      "uniform int uClippingPlaneCount;\n"
      "uniform vec4 uClippingPlanes[8];\n"
      "uniform vec3 uCameraPos;\n"
      "uniform vec3 uLightDir;\n"
      "uniform vec3 uLightColor;\n"
      "uniform int uLightCount; uniform uint uLightMask;\n"
      "uniform vec4 uLightPositionType[16],uLightDirectionAngle[16];\n"
      "uniform vec4 uLightColorDiffuse[16],uLightSpecularShape[16],uLightAreaParams[16];\n"
      "uniform vec4 uLightIesAxisX[16],uLightIesAxisY[16],uLightIesProfile[96];\n"
      "float sampleIes(int li,vec3 d,vec3 a,vec3 ax,vec3 ay){if(dot(uLightIesProfile[li*6],uLightIesProfile[li*6])+dot(uLightIesProfile[li*6+1],uLightIesProfile[li*6+1])<1e-8)return 1.;float fy=clamp(degrees(acos(clamp(dot(d,a),-1.,1.)))/60.,0.,3.);int y0=int(floor(fy)),y1=min(y0+1,3);float az=degrees(atan(dot(d,ay),dot(d,ax)));if(az<0.)az+=360.;float fx=az/60.;int x0=min(int(floor(fx)),5),x1=(x0+1)%6;float tx=fx-float(x0);float a0=mix(uLightIesProfile[y0*6+x0/4][x0%4],uLightIesProfile[y0*6+x1/4][x1%4],tx),a1=mix(uLightIesProfile[y1*6+x0/4][x0%4],uLightIesProfile[y1*6+x1/4][x1%4],tx);return mix(a0,a1,fy-float(y0));}\n"
      // DomeLight IBL (diffuse-only here: prototypes carry no material scalars).
      "uniform bool uHasIbl;\n"
      "uniform vec3 uIblColor;\n"
      "uniform float uExposure;\n"
      "uniform mat3 uEnvRotation;\n"
      "uniform samplerCube uIrradianceMap;\n"
      "uniform bool uHasShadowMap; uniform int uShadowLightSlot;\n"
      "uniform mat4 uShadowViewProj; uniform sampler2D uShadowMap;\n"
      "uniform bool uHasPointShadowMap; uniform vec3 uPointShadowLightPos; uniform mat4 uPointShadowViewProj[6]; uniform samplerCube uPointShadowMap;\n"
      "uniform vec3 uEmissive;\n"  // selection-highlight override (else 0)
      // Debug-AOV uniforms (mirror the non-instanced material shader). Instanced
      // prototypes carry no UVs or material scalars, so UV / roughness / metallic /
      // emissive modes fall through to neutral gray; opacity is carried explicitly.
      "uniform int uRenderMode;\n"
      "uniform float uDepthScale;\n"
      "uniform vec3 uSceneMin;\n"
      "uniform vec3 uSceneExtent;\n"
      "uniform int uMeshId;\n"
      "uniform bool uGeometricNormal;\n"
      "uniform bool uDoubleSided;\n"
      "uniform int uPurpose;\n"
      "uniform int uKind;\n"
      "out vec4 FragColor;\n"
      "vec3 idColor(int id){\n"
      "  if (id < 0) return vec3(0.45);\n"
      "  uint h = (uint(id) + 1u) * 2654435761u;\n"
      "  return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) * (1.0/255.0);\n"
      "}\n"
      "vec3 purposeColor(int p){\n"
      "  if (p==1) return vec3(0.2,0.8,0.3);\n"
      "  if (p==2) return vec3(0.2,0.45,0.95);\n"
      "  if (p==3) return vec3(0.95,0.75,0.1);\n"
      "  return vec3(0.5);\n"
      "}\n"
      "vec3 kindColor(int k){\n"
      "  if (k==1) return vec3(0.2,0.8,0.8);\n"
      "  if (k==2) return vec3(0.85,0.3,0.85);\n"
      "  if (k==3) return vec3(0.95,0.6,0.15);\n"
      "  if (k==4) return vec3(0.5,0.85,0.4);\n"
      "  return vec3(0.35);\n"
      "}\n"
      // Linear -> sRGB OETF for the final shaded output (see material.cpp):
      // scene lit in linear, FBO is RGBA8. Lit path only.
      "vec3 linearToSrgb(vec3 c){\n"
      "  c = clamp(c, 0.0, 1.0);\n"
      "  vec3 lo = c * 12.92;\n"
      "  vec3 hi = 1.055 * pow(c, vec3(1.0/2.4)) - 0.055;\n"
      "  return mix(lo, hi, vec3(greaterThan(c, vec3(0.0031308))));\n"
      "}\n"
      "float ggxD(float nh,float r){float a=max(r*r,0.002),a2=a*a,d=nh*nh*(a2-1.0)+1.0;return a2/max(3.14159265*d*d,1e-6);}\n"
      "float ggxG1(float nx,float r){float k=(r+1.0)*(r+1.0)*0.125;return nx/max(nx*(1.0-k)+k,1e-6);}\n"
      "vec3 fresnel(float vh,vec3 f0){return f0+(vec3(1.0)-f0)*pow(1.0-clamp(vh,0.0,1.0),5.0);}\n"
      "float shadowVis(vec3 wp,vec3 n,vec3 l){if(uHasPointShadowMap){vec3 d=wp-uPointShadowLightPos,a=abs(d);int f=a.x>=a.y&&a.x>=a.z?(d.x>=0?0:1):(a.y>=a.z?(d.y>=0?2:3):(d.z>=0?4:5));vec4 c=uPointShadowViewProj[f]*vec4(wp,1);vec3 p=c.xyz/c.w;p=p*.5+.5;if(p.z<=0||p.z>=1)return 1.0;float b=max(.00035,.0015*(1-max(dot(n,l),0)));return (p.z-b<=texture(uPointShadowMap,normalize(d)).r)?1.0:0.0;}if(!uHasShadowMap)return 1.0;vec4 c=uShadowViewProj*vec4(wp,1);vec3 p=c.xyz/c.w;p=p*.5+.5;if(p.z<=0||p.z>=1||any(lessThan(p.xy,vec2(0)))||any(greaterThan(p.xy,vec2(1))))return 1.0;float b=max(.00035,.0015*(1-max(dot(n,l),0))),v=0;vec2 t=1.0/vec2(textureSize(uShadowMap,0));for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)v+=p.z-b<=texture(uShadowMap,p.xy+vec2(x,y)*t).r?1:0;return v/9;}\n"
      "void main(){\n"
      "  for(int i=0;i<uClippingPlaneCount;++i) if(dot(vec4(vWorldPos,1.0),uClippingPlanes[i])<0.0) discard;\n"
      // Geometric (screen-derivative) normal: instanced prototypes usually ship
      // without authored normals, and faceted shading reads cleanly for them.
      "  vec3 Ngeo = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));\n"
      "  vec3 N = Ngeo;\n"
      "  if (!gl_FrontFacing) N = -N;\n"  // thin instanced geom is often 1-sided
      "  if (uRenderMode != 0) {\n"
      "    vec3 Nshade = uGeometricNormal ? Ngeo : normalize(vNormal);\n"
      "    if (uRenderMode == 2) { FragColor = vec4(Nshade*0.5+0.5, 1.0); return; }\n"
      "    if (uRenderMode == 4) { FragColor = vec4(Ngeo*0.5+0.5, 1.0); return; }\n"
      "    if (uRenderMode == 6) {\n"
      "      float d = clamp(length(uCameraPos - vWorldPos) / max(uDepthScale,1e-3), 0.0, 1.0);\n"
      "      FragColor = vec4(vec3(1.0-d), 1.0); return; }\n"
      "    if (uRenderMode == 7) { FragColor = vec4(vColor, 1.0); return; }\n"  // albedo
      "    if (uRenderMode == 8) { FragColor = gl_FrontFacing ? vec4(0.1,0.7,0.1,1.0) : vec4(0.7,0.1,0.1,1.0); return; }\n"
      "    if (uRenderMode == 13) { FragColor = vec4(clamp((vWorldPos-uSceneMin)/uSceneExtent,0.0,1.0), 1.0); return; }\n"
      "    if (uRenderMode == 15) { FragColor = vec4(idColor(gl_PrimitiveID), 1.0); return; }\n"  // prim id
      "    if (uRenderMode == 16) { FragColor = vec4(idColor(uMeshId), 1.0); return; }\n"          // mesh id
      "    if (uRenderMode == 19) { FragColor = uGeometricNormal ? vec4(0.95,0.1,0.85,1.0) : vec4(0.2,0.2,0.2,1.0); return; }\n"
      "    if (uRenderMode == 20) { FragColor = uDoubleSided ? vec4(0.95,0.55,0.1,1.0) : vec4(0.2,0.2,0.2,1.0); return; }\n"
      "    if (uRenderMode == 18) { FragColor = vec4(purposeColor(uPurpose), 1.0); return; }\n"
      "    if (uRenderMode == 29) { FragColor = vec4(kindColor(uKind), 1.0); return; }\n"  // kind
      "    if (uRenderMode == 26) { FragColor = vec4(idColor(vInstanceId), 1.0); return; }\n"  // instance id
      "    if (uRenderMode == 12) { FragColor = vec4(vec3(vOpacity), 1.0); return; }\n"
      "    if (uRenderMode == 25) {\n"  // curvature (screen-space geometric normal variation)
      "      vec3 n = Ngeo; float c = clamp((length(dFdx(n))+length(dFdy(n)))*8.0, 0.0, 1.0);\n"
      "      FragColor = vec4(c, 1.0-abs(c-0.5)*2.0, 1.0-c, 1.0); return; }\n"
      // Modes instanced geometry cannot supply (UV/other material scalars): neutral gray
      // so it is visually obvious the channel has no data here, vs masquerading as
      // a lit render.
      "    FragColor = vec4(0.18,0.18,0.18,1.0); return;\n"
      "  }\n"
      // Fixed dielectric GGX material for instanced prototypes, which do not
      // yet carry the full per-material scalar payload.
      "  vec3 V = normalize(uCameraPos - vWorldPos);\n"
      "  vec3 Nf = (dot(N, V) < 0.0) ? -N : N;\n"
      "  float nv=max(dot(Nf,V),1e-4),r=0.5; vec3 direct=vec3(0);\n"
      "  for(int li=0;li<16;++li){if(li>=uLightCount)break;if((uLightMask&(1u<<uint(li)))==0u)continue;\n"
      "    vec4 pt=uLightPositionType[li],da=uLightDirectionAngle[li],lc=uLightColorDiffuse[li],ss=uLightSpecularShape[li];int sc=(int(pt.w+0.5)==2||int(pt.w+0.5)==3||int(pt.w+0.5)==4)?4:1;for(int si=0;si<sc;++si){vec3 samplePos=pt.xyz;vec3 ax=normalize(uLightIesAxisX[li].xyz),ay=normalize(uLightIesAxisY[li].xyz);if(int(pt.w+0.5)==3){samplePos+=ax*((si&1)==0?-0.25:0.25)*uLightAreaParams[li].y+ay*((si&2)==0?-0.25:0.25)*uLightAreaParams[li].z;}else if(int(pt.w+0.5)==2||int(pt.w+0.5)==4){float k=0.3535533906;samplePos+=ax*(((si&1)==0?-k:k)*uLightAreaParams[li].x)+ay*(((si&2)==0?-k:k)*uLightAreaParams[li].x);}int lt=int(pt.w+0.5);vec3 L;float att=1.0;if(lt==5)L=normalize(da.xyz);else{vec3 q=samplePos-vWorldPos;float d2=max(dot(q,q),1e-6);L=q*inversesqrt(d2);att=1.0/d2;}\n"
      "    float shape=1.0;if(ss.w>0.5&&lt!=5){float cc=dot(normalize(da.xyz),-L),o=cos(radians(clamp(da.w,0.0,180.0))),inn=cos(radians(clamp(da.w*(1.0-clamp(ss.y,0.0,1.0)),0.0,180.0)));shape=smoothstep(o,max(inn,o+1e-5),cc)*pow(max(cc,0.0),max(ss.z,0.0));}float ies=sampleIes(li,normalize(-L),normalize(da.xyz),normalize(uLightIesAxisX[li].xyz),normalize(uLightIesAxisY[li].xyz));\n"
      "    float nl=max(dot(Nf,L),0.0);if(nl<=0.0||shape<=0.0)continue;vec3 H=normalize(L+V);float nh=max(dot(Nf,H),0.0),vh=max(dot(V,H),0.0);vec3 F=fresnel(vh,vec3(0.04));\n"
      "    vec3 spec=ggxD(nh,r)*ggxG1(nv,r)*ggxG1(nl,r)*F/max(4.0*nv*nl,1e-5),diff=(vec3(1.0)-F)*vColor*(1.0/3.14159265);float vis=li==uShadowLightSlot?shadowVis(vWorldPos,Nf,L):1;direct+=(diff*lc.w+spec*ss.x)*lc.rgb*(att*shape*ies*nl*vis)/float(sc);}}\n"
      "  vec3 amb = uHasIbl ? texture(uIrradianceMap, normalize(uEnvRotation * Nf)).rgb * uIblColor : vec3(0.20);\n"
      "  vec3 col = vColor*amb + direct;\n"
      "  FragColor = vec4(linearToSrgb((col + uEmissive) * exp2(uExposure)), vOpacity);\n"
      "}\n";
  const std::string instancedFs = UpgradeFiniteLightSamples(kInstancedFS);
  instProgram_ = glutil::CompileProgram(kInstancedVS, instancedFs.c_str(), err);
  if (!instProgram_) {
    if (err && err->empty()) *err = "Failed to build GL instanced program";
    return false;
  }
  glUseProgram(instProgram_);
  iUViewProj_ = glGetUniformLocation(instProgram_, "uViewProj");
  iCameraPos_ = glGetUniformLocation(instProgram_, "uCameraPos");
  iLightDir_ = glGetUniformLocation(instProgram_, "uLightDir");
  iHasIbl_ = glGetUniformLocation(instProgram_, "uHasIbl");
  iIblColor_ = glGetUniformLocation(instProgram_, "uIblColor");
  iExposure_ = glGetUniformLocation(instProgram_, "uExposure");
  iHasShadowMap_ = glGetUniformLocation(instProgram_, "uHasShadowMap");
  iShadowLightSlot_ = glGetUniformLocation(instProgram_, "uShadowLightSlot");
  iShadowViewProj_ = glGetUniformLocation(instProgram_, "uShadowViewProj");
  iHasPointShadowMap_ = glGetUniformLocation(instProgram_, "uHasPointShadowMap");
  iPointShadowLightPos_ = glGetUniformLocation(instProgram_, "uPointShadowLightPos");
  iPointShadowViewProj_ = glGetUniformLocation(instProgram_, "uPointShadowViewProj");
  iEnvRotation_ = glGetUniformLocation(instProgram_, "uEnvRotation");
  iLightColor_ = glGetUniformLocation(instProgram_, "uLightColor");
  iEmissive_ = glGetUniformLocation(instProgram_, "uEmissive");
  iRenderMode_ = glGetUniformLocation(instProgram_, "uRenderMode");
  iDepthScale_ = glGetUniformLocation(instProgram_, "uDepthScale");
  iSceneMin_ = glGetUniformLocation(instProgram_, "uSceneMin");
  iSceneExtent_ = glGetUniformLocation(instProgram_, "uSceneExtent");
  iMeshId_ = glGetUniformLocation(instProgram_, "uMeshId");
  iGeometricNormal_ = glGetUniformLocation(instProgram_, "uGeometricNormal");
  iDoubleSided_ = glGetUniformLocation(instProgram_, "uDoubleSided");
  iPurpose_ = glGetUniformLocation(instProgram_, "uPurpose");
  iKind_ = glGetUniformLocation(instProgram_, "uKind");
  iHasMorph_ = glGetUniformLocation(instProgram_, "uHasMorph");
  iSkinningEnabled_ = glGetUniformLocation(instProgram_, "uSkinningEnabled");
  iBoneTexWidth_ = glGetUniformLocation(instProgram_, "uBoneTexWidth");
  iBoneMatrixCount_ = glGetUniformLocation(instProgram_, "uBoneMatrixCount");
  glUniform1i(glGetUniformLocation(instProgram_, "uBoneTex"), 4);  // same unit as mesh
  glUniform1i(glGetUniformLocation(instProgram_, "uMorphDeltaTex"), 8);
  glUniform1i(glGetUniformLocation(instProgram_, "uMorphCoeffTex"), 9);
  glUniform1i(glGetUniformLocation(instProgram_, "uMorphChanTex"), 10);
  glUniform1i(glGetUniformLocation(instProgram_, "uIrradianceMap"), 19);
  glUniform1i(glGetUniformLocation(instProgram_, "uShadowMap"), 25);
  glUniform1i(glGetUniformLocation(instProgram_, "uPointShadowMap"), 31);
  glUseProgram(0);

  static const char* kShadowInstFS = "#version 330 core\nvoid main(){}\n";
  shadowInstProgram_ = glutil::CompileProgram(kInstancedVS, kShadowInstFS, err);
  if (!shadowInstProgram_) {
    if (err && err->empty()) *err = "Failed to build GL instanced shadow program";
    return false;
  }
  glUseProgram(shadowInstProgram_);
  siViewProj_ = glGetUniformLocation(shadowInstProgram_, "uViewProj");
  siSkinningEnabled_ = glGetUniformLocation(shadowInstProgram_, "uSkinningEnabled");
  siBoneTexWidth_ = glGetUniformLocation(shadowInstProgram_, "uBoneTexWidth");
  siBoneMatrixCount_ = glGetUniformLocation(shadowInstProgram_, "uBoneMatrixCount");
  siHasMorph_ = glGetUniformLocation(shadowInstProgram_, "uHasMorph");
  glUniform1i(glGetUniformLocation(shadowInstProgram_, "uBoneTex"), 4);
  glUniform1i(glGetUniformLocation(shadowInstProgram_, "uMorphDeltaTex"), 8);
  glUniform1i(glGetUniformLocation(shadowInstProgram_, "uMorphCoeffTex"), 9);
  glUniform1i(glGetUniformLocation(shadowInstProgram_, "uMorphChanTex"), 10);
  glUseProgram(0);

  buildWireProgram();  // flat polygon-edge wireframe (best-effort; non-fatal)

  // 1x1 white default texture (bound to unused sampler units).
  glGenTextures(1, &whiteTex_);
  glBindTexture(GL_TEXTURE_2D, whiteTex_);
  const uint8_t white[4] = {255, 255, 255, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenTextures(1, &boneTex_);
  glBindTexture(GL_TEXTURE_2D, boneTex_);
  const float ident[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, 1, 0, GL_RGBA, GL_FLOAT, ident);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  boneTexWidth_ = 4;
  boneTexHeight_ = 1;
  boneMatrixCount_ = 1;

  // Unlit, vertex-colored line program for debug helpers (grid/axes/bbox).
  {
    static const char* kLineVS =
        "#version 330 core\n"
        "layout(location=0) in vec3 aPos;\n"
        "layout(location=1) in vec3 aCol;\n"
        "uniform mat4 uVP;\n"
        "out vec3 vCol;\n"
        "void main(){ vCol=aCol; gl_Position=uVP*vec4(aPos,1.0); }\n";
    static const char* kLineFS =
        "#version 330 core\n"
        "in vec3 vCol; out vec4 fragColor;\n"
        "void main(){ fragColor=vec4(vCol,1.0); }\n";
    std::string lerr;
    lineProgram_ = glutil::CompileProgram(kLineVS, kLineFS, &lerr);
    if (lineProgram_) uLineVP_ = glGetUniformLocation(lineProgram_, "uVP");
    glGenVertexArrays(1, &lineVao_);
    glGenBuffers(1, &lineVbo_);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(HelperVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(HelperVertex),
                          (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
  }
  buildNonMeshProgram();

  // UsdVol volume raymarch program + unit-cube proxy geometry.
  {
    static const char* kVolumeVS =
        "#version 330 core\n"
        "layout(location=0) in vec3 aPos;\n"  // unit cube [0,1]^3
        "uniform mat4 uVP;\n"
        "uniform mat4 uModel;\n"  // object -> world
        "uniform vec3 uBmin;\n"
        "uniform vec3 uBmax;\n"
        "out vec3 vWorld;\n"
        "void main(){\n"
        "  vec3 objp = uBmin + aPos*(uBmax-uBmin);\n"
        "  vec4 wp = uModel*vec4(objp,1.0);\n"
        "  vWorld = wp.xyz;\n"
        "  gl_Position = uVP*wp;\n"
        "}\n";
    static const char* kVolumeFS =
        "#version 330 core\n"
        "in vec3 vWorld; out vec4 fragColor;\n"
        "uniform vec3 uCameraPos;\n"
        "uniform mat4 uInvModel;\n"  // world -> object
        "uniform vec3 uBmin; uniform vec3 uBmax;\n"
        "uniform sampler3D uDensity;\n"
        "uniform sampler3D uEmissionField; uniform sampler3D uTemperatureField;\n"
        "uniform bool uHasEmissionField; uniform bool uHasTemperatureField;\n"
        "uniform float uDensityScale;\n"
        "uniform vec3 uAlbedo; uniform vec3 uEmission;\n"
        "uniform float uBackground;\n"
        "vec3 blackbody(float value){\n"
        "  float k=value>100.0?value:1000.0+5500.0*max(value,0.0);\n"
        "  float t=clamp(k/100.0,10.0,400.0);\n"
        "  float r=t<=66.0?1.0:1.2929362*pow(t-60.0,-0.13320476);\n"
        "  float g=t<=66.0?0.39008158*log(t)-0.63184144:1.1298909*pow(t-60.0,-0.07551485);\n"
        "  float b=t>=66.0?1.0:(t<=19.0?0.0:0.5432068*log(t-10.0)-1.1962541);\n"
        "  return clamp(vec3(r,g,b),0.0,1.0);\n"
        "}\n"
        "bool rayAABB(vec3 o, vec3 d, vec3 lo, vec3 hi, out float t0, out float t1){\n"
        "  vec3 inv = 1.0/d;\n"
        "  vec3 ta=(lo-o)*inv, tb=(hi-o)*inv;\n"
        "  vec3 tmin=min(ta,tb), tmax=max(ta,tb);\n"
        "  t0=max(max(tmin.x,tmin.y),tmin.z);\n"
        "  t1=min(min(tmax.x,tmax.y),tmax.z);\n"
        "  return t1>max(t0,0.0);\n"
        "}\n"
        "void main(){\n"
        "  vec3 oo=(uInvModel*vec4(uCameraPos,1.0)).xyz;\n"
        "  vec3 od=normalize((uInvModel*vec4(vWorld,1.0)).xyz - oo);\n"
        "  float t0,t1;\n"
        "  if(!rayAABB(oo,od,uBmin,uBmax,t0,t1)) discard;\n"
        "  t0=max(t0,0.0);\n"
        "  vec3 ext=uBmax-uBmin;\n"
        "  float step=min(ext.x,min(ext.y,ext.z))/128.0;\n"
        "  if(step<=0.0) step=(t1-t0)/256.0;\n"
        "  float T=1.0; vec3 L=vec3(0.0);\n"
        "  for(int i=0;i<256;i++){\n"
        "    float t=t0+(float(i)+0.5)*step;\n"
        "    if(t>=t1) break;\n"
        "    vec3 p=oo+od*t;\n"
        "    vec3 uvw=(p-uBmin)/ext;\n"
        "    float dens=(texture(uDensity,uvw).r - uBackground)*uDensityScale;\n"
        "    if(dens>0.0){\n"
        "      float a=1.0-exp(-dens*step);\n"
        "      float ew=uHasEmissionField?max(texture(uEmissionField,uvw).r,0.0):dens;\n"
        "      vec3 ec=uEmission;\n"
        "      if(uHasTemperatureField){ float temp=max(texture(uTemperatureField,uvw).r,0.0);\n"
        "        vec3 tint=blackbody(temp); ec=dot(ec,vec3(1.0))>0.0?ec*tint:tint; ew=max(ew,temp); }\n"
        "      vec3 src=uAlbedo*a + ec*(ew*step);\n"
        "      L+=T*src; T*=(1.0-a);\n"
        "      if(T<0.003) break;\n"
        "    }\n"
        "  }\n"
        "  float alpha=1.0-T;\n"
        "  if(alpha<=0.001) discard;\n"
        "  fragColor=vec4(L,alpha);\n"  // premultiplied
        "}\n";
    std::string verr;
    volumeProgram_ = glutil::CompileProgram(kVolumeVS, kVolumeFS, &verr);
    if (volumeProgram_) {
      uVolVP_ = glGetUniformLocation(volumeProgram_, "uVP");
      uVolModel_ = glGetUniformLocation(volumeProgram_, "uModel");
      uVolInvModel_ = glGetUniformLocation(volumeProgram_, "uInvModel");
      uVolCameraPos_ = glGetUniformLocation(volumeProgram_, "uCameraPos");
      uVolBmin_ = glGetUniformLocation(volumeProgram_, "uBmin");
      uVolBmax_ = glGetUniformLocation(volumeProgram_, "uBmax");
      uVolDensity_ = glGetUniformLocation(volumeProgram_, "uDensity");
      uVolEmissionField_ = glGetUniformLocation(volumeProgram_, "uEmissionField");
      uVolTemperatureField_ = glGetUniformLocation(volumeProgram_, "uTemperatureField");
      uVolHasEmissionField_ = glGetUniformLocation(volumeProgram_, "uHasEmissionField");
      uVolHasTemperatureField_ = glGetUniformLocation(volumeProgram_, "uHasTemperatureField");
      uVolDensityScale_ = glGetUniformLocation(volumeProgram_, "uDensityScale");
      uVolAlbedo_ = glGetUniformLocation(volumeProgram_, "uAlbedo");
      uVolEmission_ = glGetUniformLocation(volumeProgram_, "uEmission");
      uVolBackground_ = glGetUniformLocation(volumeProgram_, "uBackground");
    }
    static const float kCube[24] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
                                    0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    static const unsigned int kCubeIdx[36] = {
        0, 1, 2, 0, 2, 3,  // -z
        4, 6, 5, 4, 7, 6,  // +z
        0, 4, 5, 0, 5, 1,  // -y
        3, 2, 6, 3, 6, 7,  // +y
        0, 3, 7, 0, 7, 4,  // -x
        1, 5, 6, 1, 6, 2}; // +x
    glGenVertexArrays(1, &volumeCubeVao_);
    glGenBuffers(1, &volumeCubeVbo_);
    glGenBuffers(1, &volumeCubeEbo_);
    glBindVertexArray(volumeCubeVao_);
    glBindBuffer(GL_ARRAY_BUFFER, volumeCubeVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCube), kCube, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, volumeCubeEbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kCubeIdx), kCubeIdx,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float),
                          (void*)0);
    glBindVertexArray(0);
  }
  initBoxProxy();
  return true;
}

// Shared unit-cube proxy mesh drawn with the instanced program (raster LOD). Static
// geometry (8 verts / 36 indices) + dynamic per-instance box-fit o2w (attribs 3-5)
// and tint (attrib 9), filled each frame by updateProxyInstances.
void GLRenderer::initBoxProxy() {
  float verts[8 * 3];
  for (int c = 0; c < 8; ++c) UnitCubeCorner(c, &verts[c * 3]);
  glGenVertexArrays(1, &boxProxyVao_);
  glBindVertexArray(boxProxyVao_);
  glGenBuffers(1, &boxProxyVbo_);
  glBindBuffer(GL_ARRAY_BUFFER, boxProxyVbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
  glVertexAttribDivisor(0, 0);
  glGenBuffers(1, &boxProxyEbo_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, boxProxyEbo_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kBoxIndices), kBoxIndices,
               GL_STATIC_DRAW);
  // Per-instance box-fit o2w rows (3-5, divisor 1) -- dynamic.
  glGenBuffers(1, &boxProxyInstVbo_);
  glBindBuffer(GL_ARRAY_BUFFER, boxProxyInstVbo_);
  const GLsizei mstride = 12 * sizeof(float);
  for (int r = 0; r < 3; ++r) {
    const GLuint loc = 3 + r;
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, mstride,
                          (void*)(static_cast<uintptr_t>(r * 4 * sizeof(float))));
    glVertexAttribDivisor(loc, 1);
  }
  // Per-instance tint (9, divisor 1) -- dynamic.
  glGenBuffers(1, &boxProxyColorVbo_);
  glBindBuffer(GL_ARRAY_BUFFER, boxProxyColorVbo_);
  glEnableVertexAttribArray(9);
  glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
  glVertexAttribDivisor(9, 1);
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void GLRenderer::updateProxyInstances(const float* xforms, const float* tints,
                                      uint32_t count) {
  boxProxyCount_ = count;
  if (count == 0) return;
  const size_t xb = static_cast<size_t>(count) * 12 * sizeof(float);
  const size_t cb = static_cast<size_t>(count) * 3 * sizeof(float);
  const bool grow = static_cast<size_t>(count) > boxProxyInstCap_;
  glBindBuffer(GL_ARRAY_BUFFER, boxProxyInstVbo_);
  if (grow) glBufferData(GL_ARRAY_BUFFER, xb, xforms, GL_DYNAMIC_DRAW);
  else glBufferSubData(GL_ARRAY_BUFFER, 0, xb, xforms);
  glBindBuffer(GL_ARRAY_BUFFER, boxProxyColorVbo_);
  if (grow) glBufferData(GL_ARRAY_BUFFER, cb, tints, GL_DYNAMIC_DRAW);
  else glBufferSubData(GL_ARRAY_BUFFER, 0, cb, tints);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  if (grow) boxProxyInstCap_ = count;
}

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
bool GLRenderer::initImGuiPlatform(GLFWwindow* window, std::string* err) {
  // GLFW callbacks + input: main thread (GLFW is main-thread-affine). init() runs
  // later on the render thread, so capture the window here on the main thread.
  window_ = window;
  if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
    if (err) *err = "ImGui_ImplGlfw_InitForOpenGL failed";
    return false;
  }
  return true;
}
bool GLRenderer::initImGuiBackend(std::string* err) {
  // GL objects (shaders, font texture): the context-owning thread.
  if (!ImGui_ImplOpenGL3_Init("#version 330")) {
    if (err) *err = "ImGui_ImplOpenGL3_Init failed";
    return false;
  }
  imguiInited_ = true;
  return true;
}
bool GLRenderer::initImGui(std::string* err) {
  return initImGuiPlatform(window_, err) && initImGuiBackend(err);
}
#else
bool GLRenderer::initImGui(std::string* err) {
  if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
    if (err) *err = "ImGui_ImplGlfw_InitForOpenGL failed";
    return false;
  }
  if (!ImGui_ImplOpenGL3_Init("#version 330")) {
    if (err) *err = "ImGui_ImplOpenGL3_Init failed";
    return false;
  }
  imguiInited_ = true;
  return true;
}
#endif

void GLRenderer::buildTessProgram() {
  // Object-space passthrough VS -> TCS (adaptive level) -> TES (interpolate +
  // displace along the interpolated normal) -> FS (geometric-normal shading).
  // Self-contained for the Shaded view; AOV modes keep using the coarse program.
  static const char* kVS =
      "#version 410 core\n"
      "layout(location=0) in vec3 aPosition;\n"
      "layout(location=1) in vec3 aNormal;\n"
      "layout(location=2) in vec3 aUV;\n"
      "layout(location=3) in uvec4 aJoint;\n"
      "layout(location=4) in vec4 aWeight;\n"
      "layout(location=5) in uvec2 aInfluence;\n"
      "layout(location=8) in uvec2 aMorphOffsetCount;\n"
      // GPU blendshape morph (active-channel skip) + linear-blend skinning, so the
      // tessellator subdivides the DEFORMED control mesh. Mirrors the coarse GL330
      // vertex stage (morph before skin).
      "uniform bool uHasMorph;\n"
      "uniform samplerBuffer uMorphDeltaTex;\n"
      "uniform samplerBuffer uMorphCoeffTex;\n"
      "uniform usamplerBuffer uMorphChanTex;\n"
      "uniform sampler2D uBoneTex;\n"
      "uniform sampler2D uInfluenceTex;\n"
      "uniform bool uSkinningEnabled;\n"
      "uniform bool uExtendedSkinningEnabled;\n"
      "uniform int uBoneTexWidth;\n"
      "uniform int uBoneMatrixCount;\n"
      "uniform int uInfluenceTexWidth;\n"
      "out vec3 vcPos; out vec3 vcNrm; out vec2 vcUV;\n"
      "mat4 fetchBone(uint idx){\n"
      "  int base=int(idx)*4;\n"
      "  return mat4(\n"
      "    texelFetch(uBoneTex,ivec2((base+0)%uBoneTexWidth,(base+0)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+1)%uBoneTexWidth,(base+1)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+2)%uBoneTexWidth,(base+2)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+3)%uBoneTexWidth,(base+3)/uBoneTexWidth),0));\n"
      "}\n"
      "void main(){\n"
      "  vec3 pos=aPosition; vec3 nrm=aNormal;\n"
      "  if(uHasMorph && aMorphOffsetCount.y>0u){\n"
      "    int mbase=int(aMorphOffsetCount.x);\n"
      "    int mcount=min(int(aMorphOffsetCount.y),256);\n"
      "    for(int i=0;i<256;++i){\n"
      "      if(i>=mcount) break;\n"
      "      int ch=int(texelFetch(uMorphChanTex,mbase+i).r);\n"
      "      float c=texelFetch(uMorphCoeffTex,ch).r;\n"
      "      if(abs(c)<1e-6) continue;\n"
      "      pos += c*texelFetch(uMorphDeltaTex,mbase+i).yzw;\n"
      "    }\n"
      "  }\n"
      "  float wsum=aWeight.x+aWeight.y+aWeight.z+aWeight.w;\n"
      "  uint maxJoint=max(max(aJoint.x,aJoint.y),max(aJoint.z,aJoint.w));\n"
      "  if(uSkinningEnabled && uExtendedSkinningEnabled && aInfluence.y>0u && uInfluenceTexWidth>0){\n"
      "    mat4 skin=mat4(0.0); float fws=0.0;\n"
      "    int base=int(aInfluence.x); int count=min(int(aInfluence.y),256);\n"
      "    for(int i=0;i<256;++i){\n"
      "      if(i>=count) break;\n"
      "      int linear=base+i;\n"
      "      vec4 iw=texelFetch(uInfluenceTex,ivec2(linear%uInfluenceTexWidth,linear/uInfluenceTexWidth),0);\n"
      "      uint joint=uint(iw.x+0.5); float w=iw.y;\n"
      "      if(w>0.0 && int(joint)<uBoneMatrixCount){ skin+=fetchBone(joint)*w; fws+=w; }\n"
      "    }\n"
      "    if(fws>0.0){ skin*=1.0/fws; pos=(skin*vec4(pos,1.0)).xyz; nrm=normalize((skin*vec4(aNormal,0.0)).xyz); }\n"
      "  } else if(uSkinningEnabled && wsum>0.0 && int(maxJoint)<uBoneMatrixCount){\n"
      "    mat4 skin=fetchBone(aJoint.x)*aWeight.x+fetchBone(aJoint.y)*aWeight.y+fetchBone(aJoint.z)*aWeight.z+fetchBone(aJoint.w)*aWeight.w;\n"
      "    pos=(skin*vec4(pos,1.0)).xyz; nrm=normalize((skin*vec4(aNormal,0.0)).xyz);\n"
      "  }\n"
      "  vcPos=pos; vcNrm=nrm; vcUV=aUV.xy;\n"
      "}\n";
  static const char* kTCS =
      "#version 410 core\n"
      "layout(vertices=3) out;\n"
      "in vec3 vcPos[]; in vec3 vcNrm[]; in vec2 vcUV[];\n"
      "out vec3 tcPos[]; out vec3 tcNrm[]; out vec2 tcUV[];\n"
      "uniform mat4 uModel; uniform vec3 uCameraPos; uniform float uMaxTessLevel;\n"
      // Per-edge level from the world-space edge length relative to its distance
      // to the camera: nearer / longer edges subdivide more. Clamped to the slider.
      "float edgeLevel(vec3 a, vec3 b){\n"
      "  vec3 wa=(uModel*vec4(a,1.0)).xyz, wb=(uModel*vec4(b,1.0)).xyz;\n"
      "  vec3 mid=0.5*(wa+wb); float len=length(wa-wb);\n"
      "  float dist=max(length(uCameraPos-mid),1e-3);\n"
      "  return clamp(len/dist*120.0, 1.0, uMaxTessLevel);\n"
      "}\n"
      "void main(){\n"
      "  tcPos[gl_InvocationID]=vcPos[gl_InvocationID];\n"
      "  tcNrm[gl_InvocationID]=vcNrm[gl_InvocationID];\n"
      "  tcUV[gl_InvocationID]=vcUV[gl_InvocationID];\n"
      "  if(gl_InvocationID==0){\n"
      "    float l0=edgeLevel(vcPos[1],vcPos[2]);\n"
      "    float l1=edgeLevel(vcPos[2],vcPos[0]);\n"
      "    float l2=edgeLevel(vcPos[0],vcPos[1]);\n"
      "    gl_TessLevelOuter[0]=l0; gl_TessLevelOuter[1]=l1; gl_TessLevelOuter[2]=l2;\n"
      "    gl_TessLevelInner[0]=max(max(l0,l1),l2);\n"
      "  }\n"
      "}\n";
  static const char* kTES =
      "#version 410 core\n"
      "layout(triangles, equal_spacing, ccw) in;\n"
      "in vec3 tcPos[]; in vec3 tcNrm[]; in vec2 tcUV[];\n"
      "out vec3 vWorldPos; out vec3 vNormal; out vec2 vUV;\n"
      "uniform mat4 uModelViewProj; uniform mat4 uModel; uniform mat3 uNormalMatrix;\n"
      "uniform sampler2D uDisplacementTex; uniform bool uHasDisplacementTex;\n"
      "uniform float uDisplacementConst; uniform float uDisplacementScale;\n"
      "uniform float uDisplacementTexScale; uniform float uDisplacementTexBias;\n"
      "void main(){\n"
      "  vec3 bc=gl_TessCoord;\n"
      "  vec3 pos=bc.x*tcPos[0]+bc.y*tcPos[1]+bc.z*tcPos[2];\n"
      "  vec3 nrm=normalize(bc.x*tcNrm[0]+bc.y*tcNrm[1]+bc.z*tcNrm[2]);\n"
      "  vec2 uv=bc.x*tcUV[0]+bc.y*tcUV[1]+bc.z*tcUV[2];\n"
      "  float d=uHasDisplacementTex? (textureLod(uDisplacementTex,uv,0.0).r*uDisplacementTexScale+uDisplacementTexBias) : uDisplacementConst;\n"
      "  pos += nrm*(d*uDisplacementScale);\n"
      "  vWorldPos=(uModel*vec4(pos,1.0)).xyz;\n"
      "  vNormal=normalize(uNormalMatrix*nrm);\n"
      "  vUV=uv;\n"
      "  gl_Position=uModelViewProj*vec4(pos,1.0);\n"
      "}\n";
  static const char* kFS =
      "#version 410 core\n"
      "in vec3 vWorldPos; in vec3 vNormal; in vec2 vUV;\n"
      "uniform int uClippingPlaneCount; uniform vec4 uClippingPlanes[8];\n"
      "uniform vec3 uCameraPos; uniform vec3 uBaseColor;\n"
      "uniform vec3 uLightDir; uniform vec3 uLightColor;\n"
      "uniform int uLightCount; uniform uint uLightMask;\n"
      "uniform vec4 uLightPositionType[16],uLightDirectionAngle[16];\n"
      "uniform vec4 uLightColorDiffuse[16],uLightSpecularShape[16],uLightAreaParams[16];\n"
      "uniform vec4 uLightIesAxisX[16],uLightIesAxisY[16],uLightIesProfile[96];\n"
      "uniform bool uHasIbl; uniform vec3 uIblColor; uniform mat3 uEnvRotation;\n"
      "float sampleIes(int li,vec3 d,vec3 a,vec3 ax,vec3 ay){if(dot(uLightIesProfile[li*6],uLightIesProfile[li*6])+dot(uLightIesProfile[li*6+1],uLightIesProfile[li*6+1])<1e-8)return 1.;float fy=clamp(degrees(acos(clamp(dot(d,a),-1.,1.)))/60.,0.,3.);int y0=int(floor(fy)),y1=min(y0+1,3);float az=degrees(atan(dot(d,ay),dot(d,ax)));if(az<0.)az+=360.;float fx=az/60.;int x0=min(int(floor(fx)),5),x1=(x0+1)%6;float tx=fx-float(x0);float a0=mix(uLightIesProfile[y0*6+x0/4][x0%4],uLightIesProfile[y0*6+x1/4][x1%4],tx),a1=mix(uLightIesProfile[y1*6+x0/4][x0%4],uLightIesProfile[y1*6+x1/4][x1%4],tx);return mix(a0,a1,fy-float(y0));}\n"
      "uniform float uExposure;\n"
      "uniform samplerCube uIrradianceMap;\n"
      "uniform sampler2D uBaseColorTex; uniform bool uHasBaseColorTex;\n"
      "out vec4 FragColor;\n"
      "vec3 linearToSrgb(vec3 c){c=clamp(c,0.0,1.0);vec3 lo=c*12.92;vec3 hi=1.055*pow(c,vec3(1.0/2.4))-0.055;return mix(lo,hi,vec3(greaterThan(c,vec3(0.0031308))));}\n"
      "float ggxD(float nh,float r){float a=max(r*r,0.002),a2=a*a,d=nh*nh*(a2-1.0)+1.0;return a2/max(3.14159265*d*d,1e-6);}\n"
      "float ggxG1(float nx,float r){float k=(r+1.0)*(r+1.0)*0.125;return nx/max(nx*(1.0-k)+k,1e-6);}\n"
      "vec3 fresnel(float vh,vec3 f0){return f0+(vec3(1.0)-f0)*pow(1.0-clamp(vh,0.0,1.0),5.0);}\n"
      "void main(){\n"
      "  for(int i=0;i<uClippingPlaneCount;++i) if(dot(vec4(vWorldPos,1.0),uClippingPlanes[i])<0.0) discard;\n"
      "  vec3 base=uBaseColor;\n"
      "  if(uHasBaseColorTex) base*=texture(uBaseColorTex,vUV).rgb;\n"
      // Geometric normal of the displaced surface (screen derivatives) so the new
      // height detail actually shades, matching the coarse path's displaced look.
      "  vec3 N=normalize(cross(dFdx(vWorldPos),dFdy(vWorldPos)));\n"
      "  if(!gl_FrontFacing) N=-N;\n"
      "  vec3 V=normalize(uCameraPos-vWorldPos);\n"
      "  vec3 Nf=(dot(N,V)<0.0)?-N:N;\n"
      "  float nv=max(dot(Nf,V),1e-4),r=0.5;vec3 direct=vec3(0);\n"
      "  for(int li=0;li<16;++li){if(li>=uLightCount)break;if((uLightMask&(1u<<uint(li)))==0u)continue;vec4 pt=uLightPositionType[li],da=uLightDirectionAngle[li],lc=uLightColorDiffuse[li],ss=uLightSpecularShape[li];int lt=int(pt.w+0.5),sc=(lt==2||lt==3||lt==4)?4:1;for(int si=0;si<sc;++si){vec3 samplePos=pt.xyz,ax=normalize(uLightIesAxisX[li].xyz),ay=normalize(uLightIesAxisY[li].xyz);if(lt==3){samplePos+=ax*((si&1)==0?-0.25:0.25)*uLightAreaParams[li].y+ay*((si&2)==0?-0.25:0.25)*uLightAreaParams[li].z;}else if(lt==2||lt==4){float k=0.3535533906;samplePos+=ax*(((si&1)==0?-k:k)*uLightAreaParams[li].x)+ay*(((si&2)==0?-k:k)*uLightAreaParams[li].x);}vec3 L;float att=1.0;if(lt==5)L=normalize(da.xyz);else{vec3 q=samplePos-vWorldPos;float d2=max(dot(q,q),1e-6);L=q*inversesqrt(d2);att=1.0/d2;}float shape=1.0;if(ss.w>0.5&&lt!=5){float cc=dot(normalize(da.xyz),-L),o=cos(radians(clamp(da.w,0.0,180.0))),inn=cos(radians(clamp(da.w*(1.0-clamp(ss.y,0.0,1.0)),0.0,180.0)));shape=smoothstep(o,max(inn,o+1e-5),cc)*pow(max(cc,0.0),max(ss.z,0.0));}float ies=sampleIes(li,normalize(-L),normalize(da.xyz),normalize(uLightIesAxisX[li].xyz),normalize(uLightIesAxisY[li].xyz));float nl=max(dot(Nf,L),0.0);if(nl<=0.0||shape<=0.0)continue;vec3 H=normalize(L+V);float nh=max(dot(Nf,H),0.0),vh=max(dot(V,H),0.0);vec3 F=fresnel(vh,vec3(0.04));vec3 spec=ggxD(nh,r)*ggxG1(nv,r)*ggxG1(nl,r)*F/max(4.0*nv*nl,1e-5),diff=(vec3(1)-F)*base*(1.0/3.14159265);direct+=(diff*lc.w+spec*ss.x)*lc.rgb*(att*shape*ies*nl)/float(sc);}}\n"
      "  if(uLightCount==0){vec3 L=(dot(uLightDir,uLightDir)>1e-8)?normalize(uLightDir):normalize(vec3(0.3,0.5,0.8));vec3 lc=(dot(uLightColor,uLightColor)>1e-8)?uLightColor:vec3(1);float nl=max(dot(Nf,L),0.0);vec3 H=normalize(L+V);float nh=max(dot(Nf,H),0.0),vh=max(dot(V,H),0.0);vec3 F=fresnel(vh,vec3(0.04));vec3 spec=ggxD(nh,r)*ggxG1(nv,r)*ggxG1(nl,r)*F/max(4.0*nv*nl,1e-5),diff=(vec3(1)-F)*base*(1.0/3.14159265);direct=(diff+spec)*lc*nl;}\n"
      "  vec3 amb=uHasIbl?texture(uIrradianceMap,normalize(uEnvRotation*Nf)).rgb*uIblColor:vec3(0.20);\n"
      "  vec3 col=base*amb+direct;\n"
      "  FragColor=vec4(linearToSrgb(col*exp2(uExposure)),1.0);\n"
      "}\n";
  std::string terr;
  const std::string tessFs = UpgradeFiniteLightSamples(kFS);
  tessProgram_ = glutil::CompileProgramTess(kVS, kTCS, kTES, tessFs.c_str(), &terr);
  if (!tessProgram_) {
    // Best-effort: keep coarse displacement. (Logged, not fatal.)
    fprintf(stderr, "[lusdview] GL tessellation program unavailable: %s\n",
            terr.c_str());
    return;
  }
  glUseProgram(tessProgram_);
  tMVP_ = glGetUniformLocation(tessProgram_, "uModelViewProj");
  tModel_ = glGetUniformLocation(tessProgram_, "uModel");
  tNormalMat_ = glGetUniformLocation(tessProgram_, "uNormalMatrix");
  tCameraPos_ = glGetUniformLocation(tessProgram_, "uCameraPos");
  tLightDir_ = glGetUniformLocation(tessProgram_, "uLightDir");
  tLightColor_ = glGetUniformLocation(tessProgram_, "uLightColor");
  tHasIbl_ = glGetUniformLocation(tessProgram_, "uHasIbl");
  tIblColor_ = glGetUniformLocation(tessProgram_, "uIblColor");
  tExposure_ = glGetUniformLocation(tessProgram_, "uExposure");
  tEnvRotation_ = glGetUniformLocation(tessProgram_, "uEnvRotation");
  tBaseColor_ = glGetUniformLocation(tessProgram_, "uBaseColor");
  tHasBaseColorTex_ = glGetUniformLocation(tessProgram_, "uHasBaseColorTex");
  tHasDisplacementTex_ = glGetUniformLocation(tessProgram_, "uHasDisplacementTex");
  tDisplacementConst_ = glGetUniformLocation(tessProgram_, "uDisplacementConst");
  tDisplacementScale_ = glGetUniformLocation(tessProgram_, "uDisplacementScale");
  tDisplacementTexScale_ = glGetUniformLocation(tessProgram_, "uDisplacementTexScale");
  tDisplacementTexBias_ = glGetUniformLocation(tessProgram_, "uDisplacementTexBias");
  tMaxTessLevel_ = glGetUniformLocation(tessProgram_, "uMaxTessLevel");
  tHasMorph_ = glGetUniformLocation(tessProgram_, "uHasMorph");
  tSkinningEnabled_ = glGetUniformLocation(tessProgram_, "uSkinningEnabled");
  tExtendedSkinningEnabled_ =
      glGetUniformLocation(tessProgram_, "uExtendedSkinningEnabled");
  tBoneTexWidth_ = glGetUniformLocation(tessProgram_, "uBoneTexWidth");
  tBoneMatrixCount_ = glGetUniformLocation(tessProgram_, "uBoneMatrixCount");
  tInfluenceTexWidth_ = glGetUniformLocation(tessProgram_, "uInfluenceTexWidth");
  glUniform1i(glGetUniformLocation(tessProgram_, "uBaseColorTex"), 0);
  glUniform1i(glGetUniformLocation(tessProgram_, "uDisplacementTex"), 7);
  glUniform1i(glGetUniformLocation(tessProgram_, "uMorphDeltaTex"), 8);
  glUniform1i(glGetUniformLocation(tessProgram_, "uMorphCoeffTex"), 9);
  glUniform1i(glGetUniformLocation(tessProgram_, "uMorphChanTex"), 10);
  glUniform1i(glGetUniformLocation(tessProgram_, "uBoneTex"), 4);
  glUniform1i(glGetUniformLocation(tessProgram_, "uInfluenceTex"), 5);
  glUniform1i(glGetUniformLocation(tessProgram_, "uIrradianceMap"), 19);
  glUseProgram(0);
  tessAvailable_ = true;
}

void GLRenderer::buildNonMeshProgram() {
  static const char* kVS = R"GLSL(#version 330 core
layout(location=0) in vec3 aP0;
layout(location=1) in vec3 aP1;
layout(location=2) in float aWidth;
layout(location=3) in vec4 aColor;
uniform mat4 uViewProj;
uniform vec3 uCameraPos, uCameraRight, uCameraUp;
uniform int uKind;
out vec2 vLocal;
out vec4 vColor;
out vec3 vWorld;
out vec3 vView;
flat out int vInstanceId;
void main(){
  vec2 corner = gl_VertexID==0 ? vec2(-1,-1) :
                gl_VertexID==1 ? vec2( 1,-1) :
                gl_VertexID==2 ? vec2(-1, 1) : vec2(1,1);
  vec3 p;
  if(uKind==0){
    p=aP0+(uCameraRight*corner.x+uCameraUp*corner.y)*(0.5*aWidth);
    vLocal=corner;
  }else{
    float along=corner.y*0.5+0.5;
    vec3 center=mix(aP0,aP1,along);
    vec3 tangent=normalize(aP1-aP0);
    vec3 view=normalize(uCameraPos-center);
    vec3 side=cross(tangent,view);
    if(dot(side,side)<1e-10) side=uCameraRight;
    side=normalize(side);
    p=center+side*(corner.x*0.5*aWidth);
    vLocal=vec2(corner.x,along);
  }
  vColor=aColor; vWorld=p; vView=normalize(uCameraPos-p);
  vInstanceId=gl_InstanceID;
  gl_Position=uViewProj*vec4(p,1.0);
}
)GLSL";
  static const char* kFS = R"GLSL(#version 330 core
in vec2 vLocal; in vec4 vColor; in vec3 vWorld; in vec3 vView;
flat in int vInstanceId;
uniform vec3 uCameraRight,uCameraUp,uLightDir,uLightColor;
uniform int uLightCount; uniform uint uLightMask;
uniform vec4 uLightPositionType[16],uLightDirectionAngle[16];
uniform vec4 uLightColorDiffuse[16],uLightSpecularShape[16],uLightAreaParams[16];
uniform vec4 uLightIesAxisX[16],uLightIesAxisY[16],uLightIesProfile[96];
float sampleIes(int li,vec3 d,vec3 a,vec3 ax,vec3 ay){if(dot(uLightIesProfile[li*6],uLightIesProfile[li*6])+dot(uLightIesProfile[li*6+1],uLightIesProfile[li*6+1])<1e-8)return 1.;float fy=clamp(degrees(acos(clamp(dot(d,a),-1.,1.)))/60.,0.,3.);int y0=int(floor(fy)),y1=min(y0+1,3);float az=degrees(atan(dot(d,ay),dot(d,ax)));if(az<0.)az+=360.;float fx=az/60.;int x0=min(int(floor(fx)),5),x1=(x0+1)%6;float tx=fx-float(x0);float a0=mix(uLightIesProfile[y0*6+x0/4][x0%4],uLightIesProfile[y0*6+x1/4][x1%4],tx),a1=mix(uLightIesProfile[y1*6+x0/4][x0%4],uLightIesProfile[y1*6+x1/4][x1%4],tx);return mix(a0,a1,fy-float(y0));}
uniform float uExposure;
uniform int uKind,uMaterialId,uCarrierId,uPurpose,uRenderMode;
out vec4 fragColor;
vec3 idColor(int id){uint h=(uint(max(id,0))+1u)*2654435761u;return vec3(float(h&255u),float((h>>8u)&255u),float((h>>16u)&255u))*(1.0/255.0);}
vec3 linearToSrgb(vec3 c){c=clamp(c,0.0,1.0);vec3 lo=c*12.92;vec3 hi=1.055*pow(c,vec3(1.0/2.4))-0.055;return mix(lo,hi,vec3(greaterThan(c,vec3(0.0031308))));}
float D(float nh,float r){float a=max(r*r,.002),a2=a*a,d=nh*nh*(a2-1.0)+1.0;return a2/max(3.14159265*d*d,1e-6);}
float G(float nx,float r){float k=(r+1.0)*(r+1.0)*.125;return nx/max(nx*(1.0-k)+k,1e-6);}
vec3 F(float vh,vec3 f0){return f0+(vec3(1)-f0)*pow(1.0-clamp(vh,0.0,1.0),5.0);}
void main(){
  vec3 N;
  if(uKind==0){float rr=dot(vLocal,vLocal);if(rr>1.0)discard;N=normalize(uCameraRight*vLocal.x+uCameraUp*vLocal.y+vView*sqrt(max(0.0,1.0-rr)));}
  else N=normalize(vView);
  if(uRenderMode==2){fragColor=vec4(N*.5+.5,1);return;}
  if(uRenderMode==3){fragColor=vec4(idColor(uMaterialId),1);return;}
  if(uRenderMode==15){fragColor=vec4(idColor(vInstanceId),1);return;}
  if(uRenderMode==16){fragColor=vec4(idColor(uCarrierId),1);return;}
  if(uRenderMode==18){vec3 c=uPurpose==1?vec3(.3,.7,1):uPurpose==2?vec3(1,.6,.2):uPurpose==3?vec3(.8,.3,1):vec3(.7);fragColor=vec4(c,1);return;}
  if(uRenderMode==7){fragColor=vec4(vColor.rgb,1);return;}
  if(uRenderMode==12){fragColor=vec4(vec3(vColor.a),1);return;}
  if(uRenderMode!=0){fragColor=vec4(.18,.18,.18,1);return;}
  vec3 V=normalize(vView);float nv=max(dot(N,V),1e-4),r=.5;vec3 direct=vec3(0);
  for(int li=0;li<16;++li){if(li>=uLightCount)break;if((uLightMask&(1u<<uint(li)))==0u)continue;vec4 pt=uLightPositionType[li],da=uLightDirectionAngle[li],lc=uLightColorDiffuse[li],ss=uLightSpecularShape[li];int lt=int(pt.w+.5),sc=(lt==2||lt==3||lt==4)?4:1;for(int si=0;si<sc;++si){vec3 samplePos=pt.xyz,ax=normalize(uLightIesAxisX[li].xyz),ay=normalize(uLightIesAxisY[li].xyz);if(lt==3){samplePos+=ax*((si&1)==0?-0.25:0.25)*uLightAreaParams[li].y+ay*((si&2)==0?-0.25:0.25)*uLightAreaParams[li].z;}else if(lt==2||lt==4){float k=0.3535533906;samplePos+=ax*(((si&1)==0?-k:k)*uLightAreaParams[li].x)+ay*(((si&2)==0?-k:k)*uLightAreaParams[li].x);}vec3 L;float att=1;if(lt==5)L=normalize(da.xyz);else{vec3 q=samplePos-vWorld;float d2=max(dot(q,q),1e-6);L=q*inversesqrt(d2);att=1/d2;}float shape=1;if(ss.w>.5&&lt!=5){float cc=dot(normalize(da.xyz),-L),o=cos(radians(clamp(da.w,0,180))),inn=cos(radians(clamp(da.w*(1-clamp(ss.y,0,1)),0,180)));shape=smoothstep(o,max(inn,o+1e-5),cc)*pow(max(cc,0),max(ss.z,0));}float ies=sampleIes(li,normalize(-L),normalize(da.xyz),normalize(uLightIesAxisX[li].xyz),normalize(uLightIesAxisY[li].xyz));float nl=max(dot(N,L),0);if(nl<=0||shape<=0)continue;vec3 H=normalize(V+L);float nh=max(dot(N,H),0),vh=max(dot(V,H),0);vec3 ff=F(vh,vec3(.04)),spec=D(nh,r)*G(nv,r)*G(nl,r)*ff/max(4*nv*nl,1e-5),diff=(vec3(1)-ff)*vColor.rgb*(1.0/3.14159265);direct+=(diff*lc.w+spec*ss.x)*lc.rgb*(att*shape*ies*nl)/float(sc);}}
  if(uLightCount==0){vec3 L=normalize(uLightDir),H=normalize(V+L);float nl=max(dot(N,L),0),nh=max(dot(N,H),0),vh=max(dot(V,H),0);vec3 ff=F(vh,vec3(.04)),spec=D(nh,r)*G(nv,r)*G(nl,r)*ff/max(4*nv*nl,1e-5),diff=(vec3(1)-ff)*vColor.rgb*(1.0/3.14159265);direct=(diff+spec)*uLightColor*nl;}
  vec3 col=vColor.rgb*.12+direct;
  fragColor=vec4(linearToSrgb(col*exp2(uExposure)),vColor.a);
}
)GLSL";
  std::string err;
  const std::string nonMeshFs = UpgradeFiniteLightSamples(kFS);
  nonMeshProgram_ = glutil::CompileProgram(kVS, nonMeshFs.c_str(), &err);
  if (!nonMeshProgram_) {
    std::fprintf(stderr, "[lusdview] non-mesh GL program unavailable: %s\n",
                 err.c_str());
    return;
  }
  nmViewProj_ = glGetUniformLocation(nonMeshProgram_, "uViewProj");
  nmCameraPos_ = glGetUniformLocation(nonMeshProgram_, "uCameraPos");
  nmCameraRight_ = glGetUniformLocation(nonMeshProgram_, "uCameraRight");
  nmCameraUp_ = glGetUniformLocation(nonMeshProgram_, "uCameraUp");
  nmLightDir_ = glGetUniformLocation(nonMeshProgram_, "uLightDir");
  nmLightColor_ = glGetUniformLocation(nonMeshProgram_, "uLightColor");
  nmExposure_ = glGetUniformLocation(nonMeshProgram_, "uExposure");
  nmKind_ = glGetUniformLocation(nonMeshProgram_, "uKind");
  nmMaterialId_ = glGetUniformLocation(nonMeshProgram_, "uMaterialId");
  nmCarrierId_ = glGetUniformLocation(nonMeshProgram_, "uCarrierId");
  nmPurpose_ = glGetUniformLocation(nonMeshProgram_, "uPurpose");
  nmRenderMode_ = glGetUniformLocation(nonMeshProgram_, "uRenderMode");
}

void GLRenderer::destroyMesh(GLMesh& m) {
    if (m.ebo) glDeleteBuffers(1, &m.ebo);
    if (m.influenceTex) glDeleteTextures(1, &m.influenceTex);
    if (m.influenceVbo) glDeleteBuffers(1, &m.influenceVbo);
    if (m.weightVbo) glDeleteBuffers(1, &m.weightVbo);
    if (m.jointVbo) glDeleteBuffers(1, &m.jointVbo);
    if (m.instanceVbo) glDeleteBuffers(1, &m.instanceVbo);
    if (m.instanceColorVbo) glDeleteBuffers(1, &m.instanceColorVbo);
    if (m.instanceOpacityVbo) glDeleteBuffers(1, &m.instanceOpacityVbo);
    if (m.vertexColorVbo) glDeleteBuffers(1, &m.vertexColorVbo);
    if (m.uv1Vbo) glDeleteBuffers(1, &m.uv1Vbo);
    if (m.morphInflVbo) glDeleteBuffers(1, &m.morphInflVbo);
    if (m.morphOffsetVbo) glDeleteBuffers(1, &m.morphOffsetVbo);
    if (m.morphDeltaTex) glDeleteTextures(1, &m.morphDeltaTex);
    if (m.morphDeltaBuf) glDeleteBuffers(1, &m.morphDeltaBuf);
    if (m.morphCoeffTex) glDeleteTextures(1, &m.morphCoeffTex);
    if (m.morphCoeffBuf) glDeleteBuffers(1, &m.morphCoeffBuf);
    if (m.morphChanTex) glDeleteTextures(1, &m.morphChanTex);
    if (m.morphChanBuf) glDeleteBuffers(1, &m.morphChanBuf);
    if (m.faceIdTex) glDeleteTextures(1, &m.faceIdTex);
    if (m.faceIdBuf) glDeleteBuffers(1, &m.faceIdBuf);
    if (m.wireEbo) glDeleteBuffers(1, &m.wireEbo);
    if (m.vbo) glDeleteBuffers(1, &m.vbo);
    if (m.vao) glDeleteVertexArrays(1, &m.vao);
    m = GLMesh{};
}

void GLRenderer::destroyScene() {
  for (auto& m : meshes_) destroyMesh(m);
  meshes_.clear();
  for (GLNonMeshBatch& b : nonMeshBatches_) {
    if (b.vbo) glDeleteBuffers(1, &b.vbo);
    if (b.vao) glDeleteVertexArrays(1, &b.vao);
  }
  nonMeshBatches_.clear();
  for (GLVolume& gv : volumes_) {
    if (gv.tex3d) glDeleteTextures(1, &gv.tex3d);
    if (gv.emissionTex3d) glDeleteTextures(1, &gv.emissionTex3d);
    if (gv.temperatureTex3d) glDeleteTextures(1, &gv.temperatureTex3d);
  }
  volumes_.clear();
  for (GLTexture& tex : textures_) {
    if (tex.tex2d) glDeleteTextures(1, &tex.tex2d);
    if (tex.arrayTex) glDeleteTextures(1, &tex.arrayTex);
  }
  if (udimLutAtlas_) glDeleteTextures(1, &udimLutAtlas_);
  udimLutAtlas_ = 0;
  textures_.clear();
  if (ptexRectTex_) glDeleteTextures(1, &ptexRectTex_);
  if (ptexRectBuf_) glDeleteBuffers(1, &ptexRectBuf_);
  ptexRectTex_ = 0;
  ptexRectBuf_ = 0;
  materials_.clear();
  destroyIblTextures();
}

void GLRenderer::rebuildPtexRectBuffer() {
  std::vector<uint32_t> records;
  for (GLTexture& texture : textures_) {
    texture.ptexRectOffset = records.size();
    records.insert(records.end(), texture.ptexRects.begin(),
                   texture.ptexRects.end());
  }
  if (ptexRectTex_) glDeleteTextures(1, &ptexRectTex_);
  if (ptexRectBuf_) glDeleteBuffers(1, &ptexRectBuf_);
  ptexRectTex_ = 0;
  ptexRectBuf_ = 0;
  glGenBuffers(1, &ptexRectBuf_);
  glBindBuffer(GL_TEXTURE_BUFFER, ptexRectBuf_);
  const size_t bytes = std::max<size_t>(records.size() * sizeof(uint32_t),
                                        sizeof(uint32_t));
  glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(bytes),
               records.empty() ? nullptr : records.data(), GL_STATIC_DRAW);
  glGenTextures(1, &ptexRectTex_);
  glBindTexture(GL_TEXTURE_BUFFER, ptexRectTex_);
  glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, ptexRectBuf_);
  glBindTexture(GL_TEXTURE_BUFFER, 0);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void GLRenderer::beginScene(const std::vector<DrawMaterialCPU>& materials,
                            int textureCount) {
  destroyScene();
  resizeTextureSlots(textureCount);
  materials_.reserve(materials.size());
  appendMaterials(materials, 0);
}

void GLRenderer::resizeTextureSlots(int textureCount) {
  const size_t requested = textureCount > 0 ? static_cast<size_t>(textureCount) : 0;
  if (requested <= textures_.size() && udimLutAtlas_) return;
  textures_.resize(requested);
  // Reserve texture slots (0 = not yet uploaded -> resolved to white at draw).
  // Texture pixels are published only after geometry finalization, so rebuilding
  // the empty UDIM lookup while slots grow cannot discard a live lookup table.
  if (udimLutAtlas_) glDeleteTextures(1, &udimLutAtlas_);
  udimLutAtlas_ = 0;
  const int atlasRows = std::max(textureCount, 1);
  std::vector<int16_t> emptyLut(static_cast<size_t>(atlasRows) * 100, -1);
  glGenTextures(1, &udimLutAtlas_);
  glBindTexture(GL_TEXTURE_2D, udimLutAtlas_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R16I, 100, atlasRows, 0,
               GL_RED_INTEGER, GL_SHORT, emptyLut.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
}

GLRenderer::GLMaterial GLRenderer::makeMaterial(
    const DrawMaterialCPU& m) const {
    GLMaterial gm;
    gm.baseColor[0] = m.baseColor[0];
    gm.baseColor[1] = m.baseColor[1];
    gm.baseColor[2] = m.baseColor[2];
    gm.metallic = m.metallic;
    gm.roughness = m.roughness;
    gm.emissive[0] = m.emissive[0];
    gm.emissive[1] = m.emissive[1];
    gm.emissive[2] = m.emissive[2];
    gm.alpha = m.alpha;
    gm.alphaMode = m.alphaMode;
    gm.alphaCutoff = m.alphaCutoff;
    gm.useSpecularWorkflow = m.useSpecularWorkflow;
    gm.openPbrSpecularModel = m.openPbrSpecularModel;
    gm.specularColor[0] = m.specularColor[0];
    gm.specularColor[1] = m.specularColor[1];
    gm.specularColor[2] = m.specularColor[2];
    gm.ior = m.ior;
    gm.occlusion = m.occlusion;
    gm.coatWeight = m.coatWeight;
    gm.coatColor[0] = m.coatColor[0];
    gm.coatColor[1] = m.coatColor[1];
    gm.coatColor[2] = m.coatColor[2];
    gm.coatRoughness = m.coatRoughness;
    gm.coatIor = m.coatIor;
    gm.coatWeightTex = m.coatWeightTex;
    gm.coatColorTex = m.coatColorTex;
    gm.coatRoughnessTex = m.coatRoughnessTex;
    gm.coatNormalTex = m.coatNormalTex;
    gm.specularColorTex = m.specularColorTex;
    gm.coatWeightSample = m.coatWeightSample;
    gm.coatColorSample = m.coatColorSample;
    gm.coatRoughnessSample = m.coatRoughnessSample;
    gm.coatNormalSample = m.coatNormalSample;
    gm.specularColorSample = m.specularColorSample;
    gm.baseColorTex = m.baseColorTex;  // slot indices (resolved at draw)
    gm.metallicTex = m.metallicTex;
    gm.roughnessTex = m.roughnessTex;
    gm.normalTex = m.normalTex;
    gm.emissiveTex = m.emissiveTex;
    gm.opacityTex = m.opacityTex;
    gm.occlusionTex = m.occlusionTex;
    gm.baseColorSample = m.baseColorSample;
    gm.metallicSample = m.metallicSample;
    gm.roughnessSample = m.roughnessSample;
    gm.normalSample = m.normalSample;
    gm.emissiveSample = m.emissiveSample;
    gm.opacitySample = m.opacitySample;
    gm.occlusionSample = m.occlusionSample;
    gm.opacityChannel = m.opacityChannel;
    gm.occlusionChannel = m.occlusionChannel;
    gm.occlusionTexScale = m.occlusionTexScale;
    gm.occlusionTexBias = m.occlusionTexBias;
    gm.opacityTexScale = m.opacityTexScale;
    gm.opacityTexBias = m.opacityTexBias;
    gm.metallicChannel = m.metallicChannel;
    gm.roughnessChannel = m.roughnessChannel;
    gm.metallicTexScale = m.metallicTexScale;
    gm.metallicTexBias = m.metallicTexBias;
    gm.roughnessTexScale = m.roughnessTexScale;
    gm.roughnessTexBias = m.roughnessTexBias;
    gm.displacementTex = m.displacementTex;
    gm.displacementUv = m.displacementUv;
    gm.displacementConst = m.displacementConst;
    gm.displacementTexScale = m.displacementTexScale;
    gm.displacementTexBias = m.displacementTexBias;
    gm.materialXGraph = m.materialXGraph;
    gm.lightRtOpenPBR = m.lightRtOpenPBR;
    return gm;
}

void GLRenderer::appendMaterials(const std::vector<DrawMaterialCPU>& materials,
                                 size_t first) {
  if (first >= materials.size()) return;
  materials_.reserve(materials.size());
  for (size_t i = first; i < materials.size(); ++i) {
    materials_.push_back(makeMaterial(materials[i]));
  }
}

void GLRenderer::syncSceneResources(
    const std::vector<DrawMaterialCPU>& materials, int textureCount) {
  resizeTextureSlots(textureCount);
  // Progressive loading appends stable material indices. Live constant edits
  // use updateMaterialConstants() and do not change this streaming contract.
  appendMaterials(materials, materials_.size());
}

bool GLRenderer::updateMaterialConstants(int materialId,
                                         const DrawMaterialCPU& material,
                                         std::string* err) {
  if (materialId < 0 || static_cast<size_t>(materialId) >= materials_.size()) {
    if (err) *err = "OpenGL material index is out of range";
    return false;
  }
  materials_[static_cast<size_t>(materialId)] = makeMaterial(material);
  return true;
}

bool GLRenderer::applyMaterialUpdatesTransactional(
    const std::vector<MaterialSlotUpdate>& updates, std::string* error) {
  for (const MaterialSlotUpdate& update : updates) {
    if (!update.material || update.materialIndex >= materials_.size()) {
      if (error) *error = "OpenGL material transaction slot is invalid";
      return false;
    }
  }
  for (const MaterialSlotUpdate& update : updates) {
    materials_[update.materialIndex] = makeMaterial(*update.material);
  }
  return true;
}

bool GLRenderer::replaceTextureSlotsTransactional(
    const std::vector<TextureSlotReplacement>& replacements,
    std::string* error) {
  struct Staged {
    size_t index{0};
    GLTexture texture;
  };
  std::vector<Staged> staged;
  staged.reserve(replacements.size());
  auto destroy_staged = [&]() {
    for (Staged& item : staged) {
      if (item.texture.tex2d) glDeleteTextures(1, &item.texture.tex2d);
      if (item.texture.arrayTex) glDeleteTextures(1, &item.texture.arrayTex);
    }
  };
  while (glGetError() != GL_NO_ERROR) {}
  for (size_t replacement_index = 0;
       replacement_index < replacements.size(); ++replacement_index) {
    const TextureSlotReplacement& replacement = replacements[replacement_index];
    if (!replacement.texture || replacement.textureIndex >= textures_.size() ||
        replacement.texture->isUdim || replacement.texture->isPtex ||
        textures_[replacement.textureIndex].isUdim) {
      destroy_staged();
      if (error) *error = "OpenGL texture transaction requires an ordinary valid slot";
      return false;
    }
    for (size_t prior = 0; prior < replacement_index; ++prior) {
      if (replacements[prior].textureIndex == replacement.textureIndex) {
        destroy_staged();
        if (error) *error = "OpenGL texture transaction contains a duplicate slot";
        return false;
      }
    }
    const DrawTextureCPU& source = *replacement.texture;
    GLTexture gpu;
    glGenTextures(1, &gpu.tex2d);
    glBindTexture(GL_TEXTURE_2D, gpu.tex2d);
    const GLenum compressed_format =
        GLCompressedFormat(source.compressed.format, source.srgb);
    const bool compressed = source.requestedCompressed &&
                            compressed_format != 0 &&
                            !source.compressed.data.empty();
    const GLenum format = source.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    if (compressed) {
      glCompressedTexImage2D(GL_TEXTURE_2D, 0, compressed_format,
                             source.compressed.width, source.compressed.height,
                             0, static_cast<GLsizei>(source.compressed.data.size()),
                             source.compressed.data.data());
      for (size_t level = 0; level < source.compressed.mips.size(); ++level) {
        const DrawCompressedMipCPU& mip = source.compressed.mips[level];
        glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level + 1),
                               compressed_format, mip.width, mip.height, 0,
                               static_cast<GLsizei>(mip.data.size()), mip.data.data());
      }
    } else {
      glTexImage2D(GL_TEXTURE_2D, 0, format, source.image.width,
                   source.image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   source.image.data.empty() ? nullptr : source.image.data.data());
      for (size_t level = 0; level < source.mipImages.size(); ++level) {
        const light3d::Image& mip = source.mipImages[level];
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level + 1), format,
                     mip.width, mip.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     mip.data.empty() ? nullptr : mip.data.data());
      }
    }
    const size_t mip_count = compressed ? source.compressed.mips.size()
                                        : source.mipImages.size();
    if (mip_count > 0) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                      static_cast<GLint>(mip_count));
    } else if (!source.streamingMutable) {
      glGenerateMipmap(GL_TEXTURE_2D);
    } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLWrap(source.wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLWrap(source.wrapT));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    (mip_count > 0 || !source.streamingMutable)
                        ? GL_LINEAR_MIPMAP_LINEAR
                        : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (maxTextureAnisotropy_ > 1.0f)
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                      maxTextureAnisotropy_);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
      if (gpu.tex2d) glDeleteTextures(1, &gpu.tex2d);
      destroy_staged();
      if (error) *error = "OpenGL failed to stage a texture replacement";
      return false;
    }
    gpu.width = source.image.width;
    gpu.height = source.image.height;
    gpu.srgb = source.srgb;
    gpu.regionUpdatable = !compressed && source.image.width > 0 &&
                          source.image.height > 0 &&
                          (!source.image.data.empty() || source.streamingMutable);
    if (compressed) {
      gpu.residentBytes = source.compressed.data.size();
      for (const DrawCompressedMipCPU& mip : source.compressed.mips)
        gpu.residentBytes += mip.data.size();
    } else {
      gpu.residentBytes = source.image.data.empty()
                              ? size_t(std::max(source.image.width, 0)) *
                                    size_t(std::max(source.image.height, 0)) * 4u
                              : source.image.data.size();
      for (const light3d::Image& mip : source.mipImages)
        gpu.residentBytes += mip.data.size();
      if (source.mipImages.empty() && !source.streamingMutable)
        gpu.residentBytes += gpu.residentBytes / 3u;
    }
    staged.push_back({replacement.textureIndex, std::move(gpu)});
  }
  for (Staged& item : staged) {
    GLTexture& old = textures_[item.index];
    if (old.tex2d) glDeleteTextures(1, &old.tex2d);
    if (old.arrayTex) glDeleteTextures(1, &old.arrayTex);
    old = std::move(item.texture);
  }
  return true;
}

void GLRenderer::uploadTexture(int slot, const DrawTextureCPU& t) {
  if (slot < 0 || static_cast<size_t>(slot) >= textures_.size()) return;
  GLTexture gpu;
  // sRGB color textures (base color / emissive) upload as GL_SRGB8_ALPHA8 so the
  // sampler linearizes them for the linear-space lighting (T11); normal /
  // metal-rough stay GL_RGBA8. Same rule the compressed path already applies.
  const GLenum uncompFmt = t.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
  if (t.isUdim && !t.udimTiles.empty() && t.udimTileWidth > 0 &&
      t.udimTileHeight > 0) {
    glGenTextures(1, &gpu.arrayTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, gpu.arrayTex);
    bool compressedArray = t.requestedCompressed;
    DrawCompressedFormat arrayFormat = DrawCompressedFormat::None;
    size_t layerBytes = 0;
    for (const DrawUdimTileCPU& tile : t.udimTiles) {
      if (tile.compressed.format == DrawCompressedFormat::None ||
          tile.compressed.data.empty()) {
        compressedArray = false;
        break;
      }
      if (arrayFormat == DrawCompressedFormat::None) {
        arrayFormat = tile.compressed.format;
        layerBytes = tile.compressed.data.size();
      } else if (arrayFormat != tile.compressed.format ||
                 layerBytes != tile.compressed.data.size()) {
        compressedArray = false;
        break;
      }
    }
    const GLenum compressedFmt = compressedArray
                                     ? GLCompressedFormat(arrayFormat, t.srgb)
                                     : 0;
    // Precomputed per-tile mip chains (FinalizeDrawTextures): usable for the
    // array only when every tile carries the same level count (+ equal
    // per-level payload sizes on the compressed path).
    size_t arrayMips = t.udimTiles.empty() ? 0 : t.udimTiles[0].mipImages.size();
    for (const DrawUdimTileCPU& tile : t.udimTiles) {
      if (tile.mipImages.size() != arrayMips) { arrayMips = 0; break; }
      if (compressedFmt != 0 &&
          tile.compressed.mips.size() != arrayMips) { arrayMips = 0; break; }
    }
    if (compressedFmt != 0 && arrayMips > 0) {
      for (size_t l = 1; l <= arrayMips && arrayMips > 0; ++l) {
        const DrawCompressedMipCPU& ref = t.udimTiles[0].compressed.mips[l - 1];
        for (const DrawUdimTileCPU& tile : t.udimTiles) {
          if (tile.compressed.mips[l - 1].data.size() != ref.data.size()) {
            arrayMips = 0;
            break;
          }
        }
      }
    }
    if (compressedFmt != 0) {
      std::vector<uint8_t> layers;
      layers.reserve(layerBytes * t.udimTiles.size());
      for (const DrawUdimTileCPU& tile : t.udimTiles) {
        layers.insert(layers.end(), tile.compressed.data.begin(),
                      tile.compressed.data.end());
      }
      glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, compressedFmt,
                             t.udimTileWidth, t.udimTileHeight,
                             static_cast<GLsizei>(t.udimTiles.size()), 0,
                             static_cast<GLsizei>(layers.size()), layers.data());
      for (size_t l = 1; l <= arrayMips; ++l) {
        const DrawCompressedMipCPU& ref = t.udimTiles[0].compressed.mips[l - 1];
        std::vector<uint8_t> lvl;
        lvl.reserve(ref.data.size() * t.udimTiles.size());
        for (const DrawUdimTileCPU& tile : t.udimTiles) {
          const DrawCompressedMipCPU& m = tile.compressed.mips[l - 1];
          lvl.insert(lvl.end(), m.data.begin(), m.data.end());
        }
        glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(l),
                               compressedFmt, ref.width, ref.height,
                               static_cast<GLsizei>(t.udimTiles.size()), 0,
                               static_cast<GLsizei>(lvl.size()), lvl.data());
      }
    } else {
      glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, uncompFmt, t.udimTileWidth,
                   t.udimTileHeight, static_cast<GLsizei>(t.udimTiles.size()), 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      for (size_t layer = 0; layer < t.udimTiles.size(); ++layer) {
        const light3d::Image& img = t.udimTiles[layer].image;
        if (img.width != t.udimTileWidth || img.height != t.udimTileHeight ||
            img.data.empty()) {
          continue;
        }
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0,
                        static_cast<GLint>(layer), t.udimTileWidth,
                        t.udimTileHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                        img.data.data());
      }
      for (size_t l = 1; l <= arrayMips; ++l) {
        const light3d::Image& ref = t.udimTiles[0].mipImages[l - 1];
        glTexImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(l), uncompFmt,
                     ref.width, ref.height,
                     static_cast<GLsizei>(t.udimTiles.size()), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        for (size_t layer = 0; layer < t.udimTiles.size(); ++layer) {
          const light3d::Image& img = t.udimTiles[layer].mipImages[l - 1];
          if (img.width != ref.width || img.height != ref.height ||
              img.data.empty()) {
            continue;
          }
          glTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(l), 0, 0,
                          static_cast<GLint>(layer), img.width, img.height, 1,
                          GL_RGBA, GL_UNSIGNED_BYTE, img.data.data());
        }
      }
    }
    if (arrayMips > 0) {
      glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,
                      static_cast<GLint>(arrayMips));
    } else {
      glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GLWrap(t.wrapS));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GLWrap(t.wrapT));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (maxTextureAnisotropy_ > 1.0f)
      glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY,
                      maxTextureAnisotropy_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    std::array<int16_t, 100> lut{};
    lut.fill(-1);
    for (size_t i = 0; i < t.udimLayer.size(); ++i) {
      lut[i] = static_cast<int16_t>(t.udimLayer[i]);
    }
    glBindTexture(GL_TEXTURE_2D, udimLutAtlas_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, slot, 100, 1, GL_RED_INTEGER,
                    GL_SHORT, lut.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    gpu.isUdim = true;

    glGenTextures(1, &gpu.tex2d);
    glBindTexture(GL_TEXTURE_2D, gpu.tex2d);
    const GLenum fmt = GLCompressedFormat(t.compressed.format, t.srgb);
    if (t.requestedCompressed && fmt != 0 && !t.compressed.data.empty()) {
      glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, t.compressed.width,
                             t.compressed.height, 0,
                             static_cast<GLsizei>(t.compressed.data.size()),
                             t.compressed.data.data());
    } else {
      glTexImage2D(GL_TEXTURE_2D, 0, uncompFmt, t.image.width, t.image.height, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE,
                   t.image.data.empty() ? nullptr : t.image.data.data());
    }
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLWrap(t.wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLWrap(t.wrapT));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (maxTextureAnisotropy_ > 1.0f)
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                      maxTextureAnisotropy_);
    glBindTexture(GL_TEXTURE_2D, 0);
  } else {
    glGenTextures(1, &gpu.tex2d);
    glBindTexture(GL_TEXTURE_2D, gpu.tex2d);
    // Upload as plain RGBA8 (texels used as-is; see note: the simple shader and
    // linear RGBA8 target don't re-encode gamma).
    const GLenum fmt = GLCompressedFormat(t.compressed.format, t.srgb);
    // Ptex rectangle metadata is kept in a GL texture buffer so the atlas can
    // remain BC-compressed without depending on alpha-channel preservation.
    const bool useCompressed =
        t.requestedCompressed && fmt != 0 && !t.compressed.data.empty();
    // Precomputed content-aware mips (sRGB/alpha-coverage/normal-aware,
    // FinalizeDrawTextures) replace glGenerateMipmap when present. This also
    // gives compressed textures real mips (glGenerateMipmap is typically a
    // no-op on BC-compressed textures).
    const bool precomputedMips =
        useCompressed ? !t.compressed.mips.empty() : !t.mipImages.empty();
    if (useCompressed) {
      glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, t.compressed.width,
                             t.compressed.height, 0,
                             static_cast<GLsizei>(t.compressed.data.size()),
                             t.compressed.data.data());
      for (size_t l = 0; l < t.compressed.mips.size(); ++l) {
        const DrawCompressedMipCPU& mip = t.compressed.mips[l];
        glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l + 1), fmt,
                               mip.width, mip.height, 0,
                               static_cast<GLsizei>(mip.data.size()),
                               mip.data.data());
      }
    } else {
      glTexImage2D(GL_TEXTURE_2D, 0, uncompFmt, t.image.width, t.image.height, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE,
                   t.image.data.empty() ? nullptr : t.image.data.data());
      for (size_t l = 0; l < t.mipImages.size(); ++l) {
        const light3d::Image& mip = t.mipImages[l];
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l + 1), uncompFmt,
                     mip.width, mip.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     mip.data.empty() ? nullptr : mip.data.data());
      }
    }
    if (precomputedMips) {
      const size_t levels =
          useCompressed ? t.compressed.mips.size() : t.mipImages.size();
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                      static_cast<GLint>(levels));
    } else if (!t.streamingMutable) {
      glGenerateMipmap(GL_TEXTURE_2D);
    } else {
      // Mutable page atlases update level zero incrementally. Sampling stale
      // generated levels would expose evicted pages around minification.
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLWrap(t.wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLWrap(t.wrapT));
    // Streamed textures intentionally keep only level 0 until their pages are
    // resident. Asking for a mipmapped minification filter in that state makes
    // the texture incomplete and samples black (notably breaking normal AOVs).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    (precomputedMips || !t.streamingMutable)
                        ? GL_LINEAR_MIPMAP_LINEAR
                        : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (maxTextureAnisotropy_ > 1.0f)
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                      maxTextureAnisotropy_);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  gpu.width = t.image.width;
  gpu.height = t.image.height;
  gpu.srgb = t.srgb;
  auto imageBytes = [](const light3d::Image& image) {
    return image.data.empty()
               ? size_t(std::max(image.width, 0)) *
                     size_t(std::max(image.height, 0)) * 4u
               : image.data.size();
  };
  auto compressedBytes = [](const DrawCompressedImageCPU& image) {
    size_t bytes = image.data.size();
    for (const DrawCompressedMipCPU& mip : image.mips) bytes += mip.data.size();
    return bytes;
  };
  if (t.isUdim && !t.udimTiles.empty()) {
    for (const DrawUdimTileCPU& tile : t.udimTiles) {
      const size_t compressed = compressedBytes(tile.compressed);
      gpu.residentBytes += compressed ? compressed : imageBytes(tile.image);
      if (!compressed) {
        for (const light3d::Image& mip : tile.mipImages)
          gpu.residentBytes += imageBytes(mip);
      }
    }
    // The ordinary 2D fallback is also allocated for UDIM materials.
    const size_t fallbackCompressed = compressedBytes(t.compressed);
    gpu.residentBytes +=
        fallbackCompressed ? fallbackCompressed : imageBytes(t.image);
  } else {
    const size_t compressed = compressedBytes(t.compressed);
    gpu.residentBytes = compressed ? compressed : imageBytes(t.image);
    if (!compressed) {
      for (const light3d::Image& mip : t.mipImages)
        gpu.residentBytes += imageBytes(mip);
    }
  }
  // GL generates a complete chain when the CPU did not provide one.
  if (t.mipImages.empty() && t.compressed.mips.empty() &&
      !t.streamingMutable) {
    gpu.residentBytes += gpu.residentBytes / 3u;
  }
  gpu.regionUpdatable = !t.isUdim && t.image.width > 0 && t.image.height > 0 &&
                        (!t.image.data.empty() || t.streamingMutable) &&
                        !(t.requestedCompressed &&
                          t.compressed.format != DrawCompressedFormat::None &&
                          !t.compressed.data.empty());
  if (t.isPtex) {
    gpu.ptexRects.reserve(t.ptexFaceRects.size() * 4u);
    for (const DrawPtexFaceRectCPU& rect : t.ptexFaceRects) {
      gpu.ptexRects.push_back(rect.x);
      gpu.ptexRects.push_back(rect.y);
      gpu.ptexRects.push_back(rect.width);
      gpu.ptexRects.push_back(rect.height);
    }
  }
  GLTexture& old = textures_[static_cast<size_t>(slot)];
  if (old.tex2d) glDeleteTextures(1, &old.tex2d);
  if (old.arrayTex) glDeleteTextures(1, &old.arrayTex);
  old = gpu;
  rebuildPtexRectBuffer();
  if (t.isPtex && t.compressed.data.empty() && t.ptexRectTexelOffset <
                      static_cast<uint32_t>(t.image.width * t.image.height)) {
    size_t linear = t.ptexRectTexelOffset;
    size_t remaining = t.ptexFaceRects.size() * 8u;
    while (remaining > 0) {
      const int x = static_cast<int>(linear % size_t(t.image.width));
      const int y = static_cast<int>(linear / size_t(t.image.width));
      const int count = static_cast<int>(
          std::min(remaining, size_t(t.image.width - x)));
      updateTextureRegion(slot, x, y, count, 1,
                          t.image.data.data() + linear * 4u);
      linear += static_cast<size_t>(count);
      remaining -= static_cast<size_t>(count);
    }
  }
}

void GLRenderer::evictTexture(int slot) {
  if (slot < 0 || static_cast<size_t>(slot) >= textures_.size()) return;
  GLTexture& texture = textures_[static_cast<size_t>(slot)];
  if (texture.tex2d) glDeleteTextures(1, &texture.tex2d);
  if (texture.arrayTex) glDeleteTextures(1, &texture.arrayTex);
  texture = GLTexture{};
}

size_t GLRenderer::textureResidentBytes(int slot) const {
  if (slot < 0 || static_cast<size_t>(slot) >= textures_.size()) return 0;
  return textures_[static_cast<size_t>(slot)].residentBytes;
}

bool GLRenderer::updateTextureRegion(int slot, int x, int y, int w, int h,
                                     const uint8_t* rgba, size_t rowBytes) {
  if (slot < 0 || static_cast<size_t>(slot) >= textures_.size() || !rgba ||
      x < 0 || y < 0 || w <= 0 || h <= 0) {
    return false;
  }
  GLTexture& texture = textures_[static_cast<size_t>(slot)];
  if (!texture.regionUpdatable || !texture.tex2d ||
      x + w > texture.width || y + h > texture.height) {
    return false;
  }
  const size_t stride = rowBytes ? rowBytes : size_t(w) * 4u;
  if (stride < size_t(w) * 4u || (stride & 3u) != 0) return false;
  glBindTexture(GL_TEXTURE_2D, texture.tex2d);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride / 4u));
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                  rgba);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glBindTexture(GL_TEXTURE_2D, 0);
  return true;
}

bool GLRenderer::updatePtexFaceRect(int slot, uint32_t face,
                                    const DrawPtexFaceRectCPU& rect) {
  if (slot < 0 || static_cast<size_t>(slot) >= textures_.size()) return false;
  GLTexture& texture = textures_[static_cast<size_t>(slot)];
  const size_t base = static_cast<size_t>(face) * 4u;
  if (texture.ptexRects.size() < base + 4u) return false;
  texture.ptexRects[base + 0] = rect.x;
  texture.ptexRects[base + 1] = rect.y;
  texture.ptexRects[base + 2] = rect.width;
  texture.ptexRects[base + 3] = rect.height;
  rebuildPtexRectBuffer();
  return true;
}

bool GLRenderer::updateTextureRegions(
    int slot, const std::vector<TextureRegionUpdate>& updates) {
  if (updates.empty() || slot < 0 || static_cast<size_t>(slot) >= textures_.size())
    return false;
  const TextureRegionUpdate& first = updates.front();
  if (first.compressedFormat == DrawCompressedFormat::None) {
    for (const TextureRegionUpdate& update : updates) {
      if (!update.rgba.empty() &&
          !updateTextureRegion(slot, update.x, update.y, update.width,
                               update.height, update.rgba.data(),
                               update.rowBytes)) return false;
    }
    return true;
  }
  GLTexture& texture = textures_[static_cast<size_t>(slot)];
  if (!texture.tex2d || !texture.regionUpdatable || first.x % 4 != 0 ||
      first.y % 4 != 0) return false;
  const GLenum fmt = GLCompressedFormat(first.compressedFormat, texture.srgb);
  if (fmt == 0) return false;
  auto blockBytes = [](DrawCompressedFormat f) -> size_t {
    return (f == DrawCompressedFormat::BC1 || f == DrawCompressedFormat::BC5)
               ? 8u : 16u;
  };
  glBindTexture(GL_TEXTURE_2D, texture.tex2d);
  for (const TextureRegionUpdate& update : updates) {
    if (update.compressedFormat != first.compressedFormat || update.x % 4 != 0 ||
        update.y % 4 != 0 || update.width <= 0 || update.height <= 0 ||
        update.x + update.width > texture.width ||
        update.y + update.height > texture.height) {
      glBindTexture(GL_TEXTURE_2D, 0);
      return false;
    }
    const size_t bytes = ((static_cast<size_t>(update.width) + 3u) / 4u) *
                         ((static_cast<size_t>(update.height) + 3u) / 4u) *
                         blockBytes(update.compressedFormat);
    if (update.compressed.size() < bytes) {
      glBindTexture(GL_TEXTURE_2D, 0);
      return false;
    }
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, update.x, update.y,
                              update.width, update.height, fmt,
                              static_cast<GLsizei>(bytes),
                              update.compressed.data());
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  return glGetError() == GL_NO_ERROR;
}

void GLRenderer::destroyIblTextures() {
  if (iblIrrTex_) glDeleteTextures(1, &iblIrrTex_);
  if (iblSpecTex_) glDeleteTextures(1, &iblSpecTex_);
  if (iblLutTex_) glDeleteTextures(1, &iblLutTex_);
  iblIrrTex_ = iblSpecTex_ = iblLutTex_ = 0;
  iblSpecLods_ = 0;
  iblActive_ = false;
}

void GLRenderer::setLights(const std::vector<DrawLightCPU>& lights,
                           size_t meshCount) {
  rasterLights_ = PackRasterLights(lights, meshCount);
  if (rasterLights_.omittedGeometryLights > 0) {
    std::fprintf(stderr,
                 "[lusdview] raster lighting: omitted %d GeometryLight(s); "
                 "triangle-source sampling is available in Vulkan RT\n",
                 rasterLights_.omittedGeometryLights);
  }
  if (rasterLights_.truncated > 0) {
    std::fprintf(stderr,
                 "[lusdview] raster lighting: evaluating first %d direct lights; "
                 "%d additional light(s) omitted\n",
                 kMaxRasterLights, rasterLights_.truncated);
  }
  destroyIblTextures();
  const DrawLightCPU* dome = nullptr;
  for (const DrawLightCPU& l : lights) {
    if (l.type == DrawLightCPU::Type::Dome && l.ibl.valid) {
      dome = &l;
      break;
    }
  }
  if (!dome) return;
  const DomeIblCPU& ibl = dome->ibl;

  // GGX-prefiltered specular chain: one cube level per roughness step.
  glGenTextures(1, &iblSpecTex_);
  glBindTexture(GL_TEXTURE_CUBE_MAP, iblSpecTex_);
  for (size_t l = 0; l < ibl.specLevels.size(); ++l) {
    const int fs = std::max(1, ibl.specFaceSize >> l);
    const float* data = ibl.specLevels[l].data();
    for (int f = 0; f < 6; ++f) {
      glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(f),
                   static_cast<GLint>(l), GL_RGB16F, fs, fs, 0, GL_RGB,
                   GL_FLOAT, data + static_cast<size_t>(f) * fs * fs * 3);
    }
  }
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL,
                  static_cast<GLint>(ibl.specLevels.size()) - 1);

  // Diffuse irradiance cube (single level).
  glGenTextures(1, &iblIrrTex_);
  glBindTexture(GL_TEXTURE_CUBE_MAP, iblIrrTex_);
  for (int f = 0; f < 6; ++f) {
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(f), 0,
                 GL_RGB16F, ibl.irrFaceSize, ibl.irrFaceSize, 0, GL_RGB,
                 GL_FLOAT,
                 ibl.irradiance.data() +
                     static_cast<size_t>(f) * ibl.irrFaceSize * ibl.irrFaceSize * 3);
  }
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

  // Split-sum BRDF LUT (scale, bias).
  glGenTextures(1, &iblLutTex_);
  glBindTexture(GL_TEXTURE_2D, iblLutTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, ibl.lutSize, ibl.lutSize, 0, GL_RG,
               GL_FLOAT, ibl.brdfLut.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  iblSpecLods_ = static_cast<int>(ibl.specLevels.size());
  iblColor_[0] = dome->effectiveColor[0];
  iblColor_[1] = dome->effectiveColor[1];
  iblColor_[2] = dome->effectiveColor[2];
  // World -> environment: transpose of the dome transform's normalized
  // rotation block (column-major 4x4 -> column-major 3x3).
  for (int c = 0; c < 3; ++c) {
    float col[3] = {dome->transform[c * 4 + 0], dome->transform[c * 4 + 1],
                    dome->transform[c * 4 + 2]};
    const float len =
        std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
    const float inv = (len > 1e-12f) ? 1.0f / len : 1.0f;
    // Row c of the inverse rotation = normalized column c of the transform.
    iblRotation_[0 * 3 + c] = col[0] * inv;
    iblRotation_[1 * 3 + c] = col[1] * inv;
    iblRotation_[2 * 3 + c] = col[2] * inv;
  }
  iblActive_ = true;
}

void GLRenderer::uploadSkinningFrame(const SkinningFrameCPU& skin) {
  if (!boneTex_) return;
  const bool valid = skin.enabled && skin.matrixCount > 0 &&
                     skin.rgba32f.size() >= static_cast<size_t>(skin.matrixCount) * 16;
  skinningFrameEnabled_ = valid;
  const float ident[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1};
  const int matrixCount = valid ? skin.matrixCount : 1;
  const size_t texels = static_cast<size_t>(matrixCount) * 4;
  int w = static_cast<int>(std::min<size_t>(texels, 1024));
  w = std::max(4, std::min(w, maxTextureSize_));
  int h = static_cast<int>((texels + static_cast<size_t>(w) - 1) /
                           static_cast<size_t>(w));
  if (h > maxTextureSize_) {
    w = maxTextureSize_;
    h = static_cast<int>((texels + static_cast<size_t>(w) - 1) /
                         static_cast<size_t>(w));
  }
  const size_t packedFloats = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  if (boneUploadScratch_.size() != packedFloats) {
    boneUploadScratch_.assign(packedFloats, 0.0f);
  } else if (packedFloats > texels * 4) {
    std::fill(boneUploadScratch_.data() + texels * 4,
              boneUploadScratch_.data() + packedFloats, 0.0f);
  }
  const float* src = valid ? skin.rgba32f.data() : ident;
  std::copy(src, src + texels * 4, boneUploadScratch_.begin());
  glBindTexture(GL_TEXTURE_2D, boneTex_);
  if (w != boneTexWidth_ || h != boneTexHeight_) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT,
                 boneUploadScratch_.data());
    boneTexWidth_ = w;
    boneTexHeight_ = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT,
                    boneUploadScratch_.data());
  }
  boneMatrixCount_ = matrixCount;
  glBindTexture(GL_TEXTURE_2D, 0);
}

void GLRenderer::updateMeshVertices(int meshIndex,
                                    const std::vector<DrawVertex>& verts) {
  if (meshIndex < 0 || meshIndex >= static_cast<int>(meshes_.size())) return;
  GLMesh& gm = meshes_[static_cast<size_t>(meshIndex)];
  if (!gm.vbo || verts.size() != gm.vertexCount) return;  // count must match
  glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0,
                  static_cast<GLsizeiptr>(verts.size() * sizeof(DrawVertex)),
                  verts.data());
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  if (!verts.empty()) {
    float lo[3] = {verts[0].px, verts[0].py, verts[0].pz};
    float hi[3] = {lo[0], lo[1], lo[2]};
    for (const DrawVertex& v : verts) {
      lo[0] = std::min(lo[0], v.px); hi[0] = std::max(hi[0], v.px);
      lo[1] = std::min(lo[1], v.py); hi[1] = std::max(hi[1], v.py);
      lo[2] = std::min(lo[2], v.pz); hi[2] = std::max(hi[2], v.pz);
    }
    for (int axis = 0; axis < 3; ++axis) {
      gm.localCentroid[axis] = 0.5f * (lo[axis] + hi[axis]);
    }
  }
}

void GLRenderer::updateMorphWeights(int meshIndex,
                                    const std::vector<float>& coeffs) {
  if (meshIndex < 0 || meshIndex >= static_cast<int>(meshes_.size())) return;
  GLMesh& gm = meshes_[static_cast<size_t>(meshIndex)];
  if (!gm.hasMorph || !gm.morphCoeffBuf || coeffs.empty()) return;
  const size_t n = std::min(coeffs.size(),
                            static_cast<size_t>(gm.morphChannelCount));
  glBindBuffer(GL_TEXTURE_BUFFER, gm.morphCoeffBuf);
  glBufferSubData(GL_TEXTURE_BUFFER, 0,
                  static_cast<GLsizeiptr>(n * sizeof(float)), coeffs.data());
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void GLRenderer::updateMeshWorld(int meshIndex, const float world[16]) {
  if (meshIndex < 0 || meshIndex >= static_cast<int>(meshes_.size())) return;
  // GL recomputes the normal matrix from `world` each draw, so storing the new
  // world is enough.
  std::memcpy(meshes_[static_cast<size_t>(meshIndex)].world, world,
              sizeof(float) * 16);
}

bool GLRenderer::applyMeshSlotUpdatesTransactional(
    const std::vector<MeshSlotUpdate>& updates, std::string* error) {
  for (const MeshSlotUpdate& update : updates) {
    if (update.meshIndex >= meshes_.size()) {
      if (error) *error = "OpenGL mesh transaction index is out of range";
      return false;
    }
    const GLMesh& mesh = meshes_[update.meshIndex];
    if (update.vertices &&
        (!mesh.vbo || update.vertices->size() != mesh.vertexCount)) {
      if (error) *error = "OpenGL mesh transaction vertex layout changed";
      return false;
    }
    if (update.instanceXforms &&
        (!mesh.instanceVbo || mesh.instanceCount <= 0 ||
         update.instanceXforms->size() !=
             static_cast<size_t>(mesh.instanceCount) * 12u)) {
      if (error) *error = "OpenGL instance transaction layout changed";
      return false;
    }
    if (update.instanceColors && !update.instanceColors->empty() &&
        (!mesh.instanceColorVbo || update.instanceColors->size() !=
             static_cast<size_t>(mesh.instanceCount) * 3u)) {
      if (error) *error = "OpenGL instance color layout changed";
      return false;
    }
    if (update.instanceOpacities && !update.instanceOpacities->empty() &&
        (!mesh.instanceOpacityVbo || update.instanceOpacities->size() !=
             static_cast<size_t>(mesh.instanceCount))) {
      if (error) *error = "OpenGL instance opacity layout changed";
      return false;
    }
  }
  // No allocation occurs below: all buffer sizes and slots were validated, so
  // the batch cannot fail halfway through its commit.
  for (const MeshSlotUpdate& update : updates) {
    if (update.vertices) {
      updateMeshVertices(static_cast<int>(update.meshIndex), *update.vertices);
    }
    if (update.world) {
      updateMeshWorld(static_cast<int>(update.meshIndex), update.world);
    }
    if (update.instanceXforms) {
      updateInstanceVisibility(update.meshIndex, update.instanceXforms->data(),
                               update.instanceColors && !update.instanceColors->empty()
                                   ? update.instanceColors->data() : nullptr,
                               update.instanceOpacities && !update.instanceOpacities->empty()
                                   ? update.instanceOpacities->data() : nullptr,
                               static_cast<uint32_t>(
                                   update.instanceXforms->size() / 12u));
      GLMesh& mesh = meshes_[update.meshIndex];
      if (update.instanceOpacities) {
        mesh.hasTranslucentInstances = mesh.flatOpacity < 1.0f - 1.0e-6f;
        for (float opacity : *update.instanceOpacities) {
          if (opacity < 1.0f - 1.0e-6f) {
            mesh.hasTranslucentInstances = true;
            break;
          }
        }
      }
    }
  }
  return true;
}

bool GLRenderer::replaceMeshSlotsTransactional(
    const std::vector<MeshSlotReplacement>& replacements,
    std::string* error) {
  size_t append_count = 0;
  for (size_t i = 0; i < replacements.size(); ++i) {
    const MeshSlotReplacement& replacement = replacements[i];
    const bool append = replacement.meshIndex >= meshes_.size();
    if (!replacement.mesh ||
        (append && replacement.meshIndex != meshes_.size() + append_count)) {
      if (error) *error = "OpenGL mesh replacement slot is invalid";
      return false;
    }
    if (append) ++append_count;
    for (size_t j = 0; j < i; ++j) {
      if (replacements[j].meshIndex == replacement.meshIndex) {
        if (error) *error = "OpenGL mesh replacement contains a duplicate slot";
        return false;
      }
    }
    std::string validation_error;
    if (!ValidateDrawMesh(*replacement.mesh, materials_.size(),
                          &validation_error)) {
      if (error) *error = validation_error;
      return false;
    }
  }

  while (glGetError() != GL_NO_ERROR) {}
  std::vector<GLMesh> staged;
  staged.reserve(replacements.size());
  for (const MeshSlotReplacement& replacement : replacements) {
    const size_t before = meshes_.size();
    appendMeshImpl(*replacement.mesh, true);
    if (meshes_.size() != before + 1) {
      for (GLMesh& mesh : staged) destroyMesh(mesh);
      if (error) *error = "OpenGL mesh replacement was not staged";
      return false;
    }
    staged.push_back(std::move(meshes_.back()));
    meshes_.pop_back();
  }
  const GLenum gl_error = glGetError();
  if (gl_error != GL_NO_ERROR) {
    for (GLMesh& mesh : staged) destroyMesh(mesh);
    if (error) *error = "OpenGL mesh replacement allocation failed";
    return false;
  }

  for (size_t i = 0; i < replacements.size(); ++i) {
    if (replacements[i].meshIndex == meshes_.size()) {
      meshes_.push_back(std::move(staged[i]));
    } else {
      GLMesh& resident = meshes_[replacements[i].meshIndex];
      destroyMesh(resident);
      resident = std::move(staged[i]);
    }
  }
  return true;
}

void GLRenderer::appendMesh(const DrawMeshCPU& sm) {
  std::string validationError;
  if (!ValidateDrawMesh(sm, materials_.size(), &validationError)) {
    std::fprintf(stderr, "OpenGL rejected unsafe draw mesh: %s\n",
                 validationError.c_str());
    reportSceneUploadError(validationError);
    meshes_.push_back(GLMesh{});
    return;
  }
  appendMeshImpl(sm, true);
}

void GLRenderer::appendMeshSurface(const DrawMeshCPU& sm) {
  std::string validationError;
  if (!ValidateDrawMesh(sm, materials_.size(), &validationError)) {
    std::fprintf(stderr, "OpenGL rejected unsafe draw mesh: %s\n",
                 validationError.c_str());
    reportSceneUploadError(validationError);
    meshes_.push_back(GLMesh{});
    return;
  }
  appendMeshImpl(sm, false);
}

void GLRenderer::appendPoints(const DrawPointsCPU& src) {
  const size_t n = src.points.size() / 3;
  if (!nonMeshProgram_ || n == 0) return;
  const GLMaterial* mat =
      src.materialId >= 0 && static_cast<size_t>(src.materialId) < materials_.size()
          ? &materials_[static_cast<size_t>(src.materialId)] : nullptr;
  const float sx = std::sqrt(src.world[0]*src.world[0]+src.world[1]*src.world[1]+src.world[2]*src.world[2]);
  const float sy = std::sqrt(src.world[4]*src.world[4]+src.world[5]*src.world[5]+src.world[6]*src.world[6]);
  const float sz = std::sqrt(src.world[8]*src.world[8]+src.world[9]*src.world[9]+src.world[10]*src.world[10]);
  const float worldScale = std::max(sx, std::max(sy, sz));
  std::vector<float> data;
  data.reserve(n * 11);
  bool translucent = false;
  for (size_t i = 0; i < n; ++i) {
    const float x=src.points[i*3],y=src.points[i*3+1],z=src.points[i*3+2];
    const float p[3] = {
      src.world[0]*x+src.world[4]*y+src.world[8]*z+src.world[12],
      src.world[1]*x+src.world[5]*y+src.world[9]*z+src.world[13],
      src.world[2]*x+src.world[6]*y+src.world[10]*z+src.world[14]};
    const float width = (src.widths.empty()?1.0f:(src.widths.size()==1?src.widths[0]:src.widths[std::min(i,src.widths.size()-1)]))*worldScale;
    float c[4] = {mat?mat->baseColor[0]:0.8f,mat?mat->baseColor[1]:0.8f,
                  mat?mat->baseColor[2]:0.8f,mat?mat->alpha:1.0f};
    if (src.colors.size() >= 3) {
      const size_t ci = src.colors.size() >= (i + 1) * 3 ? i * 3 : 0;
      c[0]*=src.colors[ci]; c[1]*=src.colors[ci+1]; c[2]*=src.colors[ci+2];
    }
    if (!src.opacities.empty()) {
      const size_t oi = src.opacities.size() > i ? i : 0;
      c[3] *= src.opacities[oi];
    }
    translucent |= c[3] < 0.999f;
    data.insert(data.end(), {p[0],p[1],p[2],p[0],p[1],p[2],
                             std::max(width,1e-6f),c[0],c[1],c[2],c[3]});
  }
  GLNonMeshBatch b;
  b.count=static_cast<GLsizei>(n); b.kind=0; b.materialId=src.materialId;
  b.carrierId=static_cast<int>(meshes_.size()+nonMeshBatches_.size());
  b.carrierIndex = static_cast<int>(nonMeshBatches_.size());
  b.absPath = src.absPath;
  b.purposeId=PurposeId(src.purpose); b.translucent=translucent;
  glGenVertexArrays(1,&b.vao); glGenBuffers(1,&b.vbo);
  glBindVertexArray(b.vao); glBindBuffer(GL_ARRAY_BUFFER,b.vbo);
  glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(data.size()*sizeof(float)),data.data(),GL_STATIC_DRAW);
  const GLsizei stride=11*sizeof(float);
  for(int a=0;a<4;++a){glEnableVertexAttribArray(a);glVertexAttribDivisor(a,1);}
  glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,stride,(void*)0);
  glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,stride,(void*)(3*sizeof(float)));
  glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,stride,(void*)(6*sizeof(float)));
  glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,stride,(void*)(7*sizeof(float)));
  glBindVertexArray(0); nonMeshBatches_.push_back(b);
}

void GLRenderer::appendCurves(const DrawCurvesCPU& src) {
  if (!nonMeshProgram_ || src.points.size() < 6) return;
  const GLMaterial* mat =
      src.materialId >= 0 && static_cast<size_t>(src.materialId) < materials_.size()
          ? &materials_[static_cast<size_t>(src.materialId)] : nullptr;
  const float sx=std::sqrt(src.world[0]*src.world[0]+src.world[1]*src.world[1]+src.world[2]*src.world[2]);
  const float sy=std::sqrt(src.world[4]*src.world[4]+src.world[5]*src.world[5]+src.world[6]*src.world[6]);
  const float sz=std::sqrt(src.world[8]*src.world[8]+src.world[9]*src.world[9]+src.world[10]*src.world[10]);
  const float worldScale=std::max(sx,std::max(sy,sz));
  const size_t np=src.points.size()/3;
  std::vector<float> data;
  bool translucent=mat&&mat->alpha<0.999f;
  auto wp=[&](size_t i,float p[3]){float x=src.points[i*3],y=src.points[i*3+1],z=src.points[i*3+2];p[0]=src.world[0]*x+src.world[4]*y+src.world[8]*z+src.world[12];p[1]=src.world[1]*x+src.world[5]*y+src.world[9]*z+src.world[13];p[2]=src.world[2]*x+src.world[6]*y+src.world[10]*z+src.world[14];};
  size_t base=0;
  const std::vector<uint32_t> counts = src.vertexCounts.empty()
      ? std::vector<uint32_t>{static_cast<uint32_t>(np)} : src.vertexCounts;
  for(uint32_t count:counts){
    size_t end=std::min(np,base+static_cast<size_t>(count));
    for(size_t i=base;i+1<end;++i){
      float p0[3],p1[3];wp(i,p0);wp(i+1,p1);
      const float dx = p1[0] - p0[0];
      const float dy = p1[1] - p0[1];
      const float dz = p1[2] - p0[2];
      if (dx * dx + dy * dy + dz * dz <= 1.0e-16f) continue;
      float width=(src.widths.empty()?1.0f:(src.widths.size()==1?src.widths[0]:0.5f*(src.widths[std::min(i,src.widths.size()-1)]+src.widths[std::min(i+1,src.widths.size()-1)])))*worldScale;
      if (!std::isfinite(width) || width < 0.0f) continue;
      float c[4]={mat?mat->baseColor[0]:0.8f,mat?mat->baseColor[1]:0.8f,mat?mat->baseColor[2]:0.8f,mat?mat->alpha:1.0f};
      if(src.colors.size()>=np*3){c[0]*=.5f*(src.colors[i*3]+src.colors[(i+1)*3]);c[1]*=.5f*(src.colors[i*3+1]+src.colors[(i+1)*3+1]);c[2]*=.5f*(src.colors[i*3+2]+src.colors[(i+1)*3+2]);}
      if (!src.opacities.empty()) {
        const float o0 = src.opacities[src.opacities.size() > i ? i : 0];
        const float o1 = src.opacities[src.opacities.size() > i + 1 ? i + 1 : 0];
        c[3] *= 0.5f * (o0 + o1);
      }
      translucent |= c[3] < 0.999f;
      data.insert(data.end(),{p0[0],p0[1],p0[2],p1[0],p1[1],p1[2],std::max(width,1e-6f),c[0],c[1],c[2],c[3]});
    }
    base=end;
  }
  if(data.empty())return;
  GLNonMeshBatch b;b.count=static_cast<GLsizei>(data.size()/11);b.kind=1;b.materialId=src.materialId;b.carrierId=static_cast<int>(meshes_.size()+nonMeshBatches_.size());b.carrierIndex=static_cast<int>(nonMeshBatches_.size());b.absPath=src.absPath;b.purposeId=PurposeId(src.purpose);b.translucent=translucent;
  glGenVertexArrays(1,&b.vao);glGenBuffers(1,&b.vbo);glBindVertexArray(b.vao);glBindBuffer(GL_ARRAY_BUFFER,b.vbo);glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(data.size()*sizeof(float)),data.data(),GL_STATIC_DRAW);
  const GLsizei stride=11*sizeof(float);for(int a=0;a<4;++a){glEnableVertexAttribArray(a);glVertexAttribDivisor(a,1);}glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,stride,(void*)0);glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,stride,(void*)(3*sizeof(float)));glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,stride,(void*)(6*sizeof(float)));glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,stride,(void*)(7*sizeof(float)));glBindVertexArray(0);nonMeshBatches_.push_back(b);
}

void GLRenderer::appendMeshImpl(const DrawMeshCPU& sm, bool includeAux) {
  GLMesh gm;
  gm.submeshes = sm.submeshes;
  std::memcpy(gm.world, sm.world, sizeof(gm.world));
  gm.doubleSided = sm.doubleSided;
  gm.purposeId = PurposeId(sm.purpose);
  gm.kindId = sm.kindId;
  gm.skinned = sm.jointIdx.size() == sm.vertices.size() * 4 &&
               sm.jointWt.size() == sm.vertices.size() * 4;
  gm.extendedSkinned =
      gm.skinned && sm.influenceOffsetCount.size() == sm.vertices.size() * 2 &&
      !sm.influenceTexels.empty() && sm.influenceTexWidth > 0 &&
      sm.influenceTexHeight > 0 && sm.maxInfluencesPerVertex > 4;
  gm.influenceTexWidth = sm.influenceTexWidth;
  gm.vertexCount = sm.vertices.size();
  gm.indexCount = sm.indices.size();
  // Mesh-space bbox center for the translucency back-to-front sort.
  if (!sm.vertices.empty()) {
    float lo[3] = {sm.vertices[0].px, sm.vertices[0].py, sm.vertices[0].pz};
    float hi[3] = {lo[0], lo[1], lo[2]};
    for (const DrawVertex& v : sm.vertices) {
      lo[0] = std::min(lo[0], v.px); hi[0] = std::max(hi[0], v.px);
      lo[1] = std::min(lo[1], v.py); hi[1] = std::max(hi[1], v.py);
      lo[2] = std::min(lo[2], v.pz); hi[2] = std::max(hi[2], v.pz);
    }
    gm.localCentroid[0] = 0.5f * (lo[0] + hi[0]);
    gm.localCentroid[1] = 0.5f * (lo[1] + hi[1]);
    gm.localCentroid[2] = 0.5f * (lo[2] + hi[2]);
  }

  glGenVertexArrays(1, &gm.vao);
  glBindVertexArray(gm.vao);
  glGenBuffers(1, &gm.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
  const std::vector<DrawVertex>& rasterVertices =
      sm.rasterDisplacementBaked &&
              sm.rtDisplacedVertices.size() == sm.vertices.size()
          ? sm.rtDisplacedVertices
          : sm.vertices;
  glBufferData(GL_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(rasterVertices.size() * sizeof(DrawVertex)),
               rasterVertices.data(), GL_STATIC_DRAW);
  glGenBuffers(1, &gm.ebo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(sm.indices.size() * sizeof(uint32_t)),
               sm.indices.data(), GL_STATIC_DRAW);
  const GLsizei stride = sizeof(DrawVertex);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
  // Skin attribute locations differ by program: the mesh program takes joints at 3
  // and weights at 4, but in the INSTANCED program those slots (and 5) carry the
  // per-instance o2w rows, so a skinned prototype binds 6/7 instead. The extended
  // (>4 influence) stream needs attrib 5 and therefore has no instanced form --
  // instanced prototypes use the 4-influence path only.
  const bool skinInstanced = !sm.instanceXforms.empty();
  const GLuint jointLoc = skinInstanced ? 6u : 3u;
  const GLuint weightLoc = skinInstanced ? 7u : 4u;
  if (skinInstanced) gm.extendedSkinned = false;
  if (gm.skinned) {
    glGenBuffers(1, &gm.jointVbo);
    glBindBuffer(GL_ARRAY_BUFFER, gm.jointVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sm.jointIdx.size() * sizeof(uint32_t)),
                 sm.jointIdx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(jointLoc);
    glVertexAttribIPointer(jointLoc, 4, GL_UNSIGNED_INT, 4 * sizeof(uint32_t),
                           (void*)0);
    glVertexAttribDivisor(jointLoc, 0);
    glGenBuffers(1, &gm.weightVbo);
    glBindBuffer(GL_ARRAY_BUFFER, gm.weightVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sm.jointWt.size() * sizeof(float)),
                 sm.jointWt.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(weightLoc);
    glVertexAttribPointer(weightLoc, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)0);
    glVertexAttribDivisor(weightLoc, 0);
    if (gm.extendedSkinned) {
      glGenBuffers(1, &gm.influenceVbo);
      glBindBuffer(GL_ARRAY_BUFFER, gm.influenceVbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(sm.influenceOffsetCount.size() *
                                           sizeof(uint32_t)),
                   sm.influenceOffsetCount.data(), GL_STATIC_DRAW);
      glEnableVertexAttribArray(5);
      glVertexAttribIPointer(5, 2, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)0);

      glGenTextures(1, &gm.influenceTex);
      glBindTexture(GL_TEXTURE_2D, gm.influenceTex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, sm.influenceTexWidth,
                   sm.influenceTexHeight, 0, GL_RGBA, GL_FLOAT,
                   sm.influenceTexels.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
      glDisableVertexAttribArray(5);
      glVertexAttribI2ui(5, 0, 0);
    }
  } else if (skinInstanced) {
    // Unskinned prototype: constant zero weights, so the instanced shader passes
    // every vertex through. Slots 3/4/5 belong to the instance rows here and are
    // set up below -- do NOT touch them.
    glDisableVertexAttribArray(6);
    glDisableVertexAttribArray(7);
    glVertexAttribI4ui(6, 0, 0, 0, 0);
    glVertexAttrib4f(7, 0.0f, 0.0f, 0.0f, 0.0f);
  } else {
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glDisableVertexAttribArray(5);
    glVertexAttribI4ui(3, 0, 0, 0, 0);
    glVertexAttrib4f(4, 0.0f, 0.0f, 0.0f, 0.0f);
    glVertexAttribI2ui(5, 0, 0);
  }

  gm.geometricNormal = sm.geometricNormal;
  gm.rasterDisplacementBaked = sm.rasterDisplacementBaked;
  gm.purposeId = PurposeId(sm.purpose);
  gm.kindId = sm.kindId;
  // Per-vertex displayColor/displayOpacity. Non-instanced material draws use
  // attrib 9; instanced meshes use attrib 10 because attrib 9 carries the
  // per-instance color in the separate flat shader.
  const bool gmInstanced = !sm.instanceXforms.empty();
  const GLuint vtxColorAttrib = gmInstanced ? 10u : 9u;
  const bool hasVtxColor = sm.vertexColors.size() == sm.vertices.size() * 3;
  const bool hasVtxAlpha = sm.vertexAlpha.size() == sm.vertices.size();
  gm.hasVertexOpacity = hasVtxAlpha;
  if (gmInstanced && hasVtxAlpha) {
    for (float opacity : sm.vertexAlpha) {
      if (opacity < 1.0f - 1.0e-6f) {
        gm.hasTranslucentInstances = true;
        break;
      }
    }
  }
  if ((hasVtxColor || hasVtxAlpha) && !sm.vertices.empty()) {
    std::vector<float> rgba(sm.vertices.size() * 4, 1.0f);
    for (size_t i = 0; i < sm.vertices.size(); ++i) {
      if (hasVtxColor) {
        rgba[i * 4 + 0] = sm.vertexColors[i * 3 + 0];
        rgba[i * 4 + 1] = sm.vertexColors[i * 3 + 1];
        rgba[i * 4 + 2] = sm.vertexColors[i * 3 + 2];
      }
      if (hasVtxAlpha) rgba[i * 4 + 3] = sm.vertexAlpha[i];
    }
    glGenBuffers(1, &gm.vertexColorVbo);
    glBindBuffer(GL_ARRAY_BUFFER, gm.vertexColorVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(rgba.size() * sizeof(float)),
                 rgba.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(vtxColorAttrib);
    glVertexAttribPointer(vtxColorAttrib, 4, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void*)0);
    glVertexAttribDivisor(vtxColorAttrib, 0);
  } else {
    if (gmInstanced) {
      // The instanced shader consumes this as a per-vertex value at location 10.
      // A one-element divisor-0 array would run out of bounds on every vertex;
      // the disabled-array generic value is constant across all instances.
      glDisableVertexAttribArray(vtxColorAttrib);
      glVertexAttrib4f(vtxColorAttrib, 1.0f, 1.0f, 1.0f, 1.0f);
    } else {
      // A disabled array's generic value is constant for every vertex and is
      // recorded in the mesh VAO while it is bound here. This avoids an
      // out-of-range fetch from a one-element divisor-0 stream.
      glDisableVertexAttribArray(vtxColorAttrib);
      glVertexAttrib4f(vtxColorAttrib, 1.0f, 1.0f, 1.0f, 1.0f);
    }
  }
  // Multi-UV (attrib 6) + blendshape influence (attrib 7), non-instanced only --
  // instanced meshes reuse attribs 6-8 for the per-instance rows (separate
  // program). Default 0 when the mesh lacks the data so modes read as zero.
  if (!gmInstanced) {
    if (sm.uv1.size() == sm.vertices.size() * 2 && !sm.uv1.empty()) {
      glGenBuffers(1, &gm.uv1Vbo);
      glBindBuffer(GL_ARRAY_BUFFER, gm.uv1Vbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(sm.uv1.size() * sizeof(float)),
                   sm.uv1.data(), GL_STATIC_DRAW);
      glEnableVertexAttribArray(6);
      glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
      glVertexAttribDivisor(6, 0);
    } else {
      glDisableVertexAttribArray(6);
      glVertexAttrib2f(6, 0.0f, 0.0f);
    }
    if (sm.morphInfluence.size() == sm.vertices.size() && !sm.morphInfluence.empty()) {
      glGenBuffers(1, &gm.morphInflVbo);
      glBindBuffer(GL_ARRAY_BUFFER, gm.morphInflVbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(sm.morphInfluence.size() * sizeof(float)),
                   sm.morphInfluence.data(), GL_STATIC_DRAW);
      glEnableVertexAttribArray(7);
      glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
      glVertexAttribDivisor(7, 0);
    } else {
      glDisableVertexAttribArray(7);
      glVertexAttrib1f(7, 0.0f);
    }
    // Per-triangle source face id as a texture buffer (source-face-id AOV),
    // fetched in the FS by gl_PrimitiveID + the submesh's first-triangle offset.
    if (includeAux && sm.sourceFaceId.size() == sm.indices.size() / 3 &&
        !sm.sourceFaceId.empty()) {
      glGenBuffers(1, &gm.faceIdBuf);
      glBindBuffer(GL_TEXTURE_BUFFER, gm.faceIdBuf);
      glBufferData(GL_TEXTURE_BUFFER,
                   static_cast<GLsizeiptr>(sm.sourceFaceId.size() * sizeof(uint32_t)),
                   sm.sourceFaceId.data(), GL_STATIC_DRAW);
      glGenTextures(1, &gm.faceIdTex);
      glBindTexture(GL_TEXTURE_BUFFER, gm.faceIdTex);
      glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, gm.faceIdBuf);
      glBindBuffer(GL_TEXTURE_BUFFER, 0);
      glBindTexture(GL_TEXTURE_BUFFER, 0);
    }
  }  // end !gmInstanced (uv1 / morphInfl / faceId)

  // GPU blendshape morph: per-vertex (offset,count) attr 8 + a static delta
  // texture-buffer (RGBA16F: channelId,dx,dy,dz half-precision) + a per-frame
  // coefficient texture-buffer (R32F). The vertex shader sums coeff*delta before
  // the per-instance transform. Shared by non-instanced AND instanced meshes:
  // instance rows live at locations 3/4/5, leaving attr 8 free for the morph CSR.
    if (sm.morphChannelCount > 0 &&
        sm.morphOffsetCount.size() == sm.vertices.size() * 2 &&
        !sm.morphDeltaHalf.empty()) {
      glGenBuffers(1, &gm.morphOffsetVbo);
      glBindBuffer(GL_ARRAY_BUFFER, gm.morphOffsetVbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(sm.morphOffsetCount.size() * sizeof(uint32_t)),
                   sm.morphOffsetCount.data(), GL_STATIC_DRAW);
      glEnableVertexAttribArray(8);
      glVertexAttribIPointer(8, 2, GL_UNSIGNED_INT, 2 * sizeof(uint32_t), (void*)0);
      glVertexAttribDivisor(8, 0);
      glGenBuffers(1, &gm.morphDeltaBuf);
      glBindBuffer(GL_TEXTURE_BUFFER, gm.morphDeltaBuf);
      glBufferData(GL_TEXTURE_BUFFER,
                   static_cast<GLsizeiptr>(sm.morphDeltaHalf.size() * sizeof(uint16_t)),
                   sm.morphDeltaHalf.data(), GL_STATIC_DRAW);
      glGenTextures(1, &gm.morphDeltaTex);
      glBindTexture(GL_TEXTURE_BUFFER, gm.morphDeltaTex);
      glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA16F, gm.morphDeltaBuf);
      // Per-entry channelId (R16UI) for the in-shader active-channel skip.
      if (sm.morphChannelId.size() == sm.morphDeltaHalf.size() / 4) {
        glGenBuffers(1, &gm.morphChanBuf);
        glBindBuffer(GL_TEXTURE_BUFFER, gm.morphChanBuf);
        glBufferData(GL_TEXTURE_BUFFER,
                     static_cast<GLsizeiptr>(sm.morphChannelId.size() * sizeof(uint16_t)),
                     sm.morphChannelId.data(), GL_STATIC_DRAW);
        glGenTextures(1, &gm.morphChanTex);
        glBindTexture(GL_TEXTURE_BUFFER, gm.morphChanTex);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, gm.morphChanBuf);
      }
      const std::vector<float> zeroCoeff(static_cast<size_t>(sm.morphChannelCount), 0.0f);
      glGenBuffers(1, &gm.morphCoeffBuf);
      glBindBuffer(GL_TEXTURE_BUFFER, gm.morphCoeffBuf);
      glBufferData(GL_TEXTURE_BUFFER,
                   static_cast<GLsizeiptr>(zeroCoeff.size() * sizeof(float)),
                   zeroCoeff.data(), GL_DYNAMIC_DRAW);
      glGenTextures(1, &gm.morphCoeffTex);
      glBindTexture(GL_TEXTURE_BUFFER, gm.morphCoeffTex);
      glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, gm.morphCoeffBuf);
      glBindBuffer(GL_TEXTURE_BUFFER, 0);
      glBindTexture(GL_TEXTURE_BUFFER, 0);
      gm.morphChannelCount = sm.morphChannelCount;
      // The skip shader reads channelId from morphChanTex; without it (the
      // size-match guard above failing) binding texture 0 would feed channelId 0
      // for every entry -> wrong morph. Gate on it, mirroring the VK path.
      gm.hasMorph = (gm.morphChanTex != 0);
    } else {
      glDisableVertexAttribArray(8);
      glVertexAttribI2ui(8, 0u, 0u);
    }

  // GPU instancing: upload per-instance 3x4 object-to-world matrices (3 vec4
  // rows = 48 B/instance) into a second VBO; bind as instanced attribs 3-5
  // (divisor 1) -- off the morph/uv slots so an instanced prototype can morph.
  if (!sm.instanceXforms.empty()) {
    gm.instanceCount = static_cast<int>(sm.instanceXforms.size() / 12);
    gm.drawInstanceCount = gm.instanceCount;
    glGenBuffers(1, &gm.instanceVbo);
    glBindBuffer(GL_ARRAY_BUFFER, gm.instanceVbo);
    // GL_DYNAMIC_DRAW: per-instance culling re-uploads the visible subset.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sm.instanceXforms.size() * sizeof(float)),
                 sm.instanceXforms.data(), GL_DYNAMIC_DRAW);
    const GLsizei mstride = 12 * sizeof(float);
    for (int r = 0; r < 3; ++r) {
      const GLuint loc = 3 + r;
      glEnableVertexAttribArray(loc);
      glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, mstride,
                            (void*)(static_cast<uintptr_t>(r * 4 * sizeof(float))));
      glVertexAttribDivisor(loc, 1);
    }
    // Per-instance displayColor/displayOpacity (attribs 9/11, divisor 1) when
    // present; otherwise per-draw constants are set at draw time.
    std::memcpy(gm.flatColor, sm.flatColor, sizeof(gm.flatColor));
    gm.flatOpacity = sm.flatOpacity;
    if (sm.instanceColors.size() == sm.instanceXforms.size() / 12 * 3 &&
        !sm.instanceColors.empty()) {
      gm.hasInstanceColors = true;
      glGenBuffers(1, &gm.instanceColorVbo);
      glBindBuffer(GL_ARRAY_BUFFER, gm.instanceColorVbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(sm.instanceColors.size() * sizeof(float)),
                   sm.instanceColors.data(), GL_DYNAMIC_DRAW);
      glEnableVertexAttribArray(9);
      glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
      glVertexAttribDivisor(9, 1);
    } else {
      glDisableVertexAttribArray(9);  // constant color set per draw
    }
    if (sm.instanceOpacities.size() == sm.instanceXforms.size() / 12 &&
        !sm.instanceOpacities.empty()) {
      gm.hasInstanceOpacities = true;
      for (float opacity : sm.instanceOpacities) {
        if (opacity < 1.0f - 1.0e-6f) {
          gm.hasTranslucentInstances = true;
          break;
        }
      }
      glGenBuffers(1, &gm.instanceOpacityVbo);
      glBindBuffer(GL_ARRAY_BUFFER, gm.instanceOpacityVbo);
      glBufferData(GL_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(sm.instanceOpacities.size() * sizeof(float)),
                   sm.instanceOpacities.data(), GL_DYNAMIC_DRAW);
      glEnableVertexAttribArray(11);
      glVertexAttribPointer(11, 1, GL_FLOAT, GL_FALSE, sizeof(float), (void*)0);
      glVertexAttribDivisor(11, 1);
    } else {
      gm.hasTranslucentInstances = gm.hasTranslucentInstances ||
                                   gm.flatOpacity < 1.0f - 1.0e-6f;
      glDisableVertexAttribArray(11);  // constant opacity set per draw
    }
  }

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  // Wireframe edges. Prefer the loader-provided original-polygon perimeter edges
  // (sm.wireframeIndices: quads/ngons, correct for double-sided meshes). Fall
  // back to deriving edges from triangles + source face ids (drop triangulation
  // diagonals). Without authored face ids, retain only the mesh boundary instead
  // of exposing every generated triangle. Always use the base (coarse) indices,
  // so wireframe shows the pre-tessellation mesh.
  if (!includeAux) {
    // Surface-critical data is resident. Wire/source-face data is uploaded by
    // uploadMeshAux after the first useful frame.
  } else if (!sm.wireframeIndices.empty()) {
    glGenBuffers(1, &gm.wireEbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.wireEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sm.wireframeIndices.size() * sizeof(uint32_t)),
                 sm.wireframeIndices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gm.wireCount = static_cast<GLsizei>(sm.wireframeIndices.size());
  } else {
    const size_t triCount = sm.indices.size() / 3;
    const bool haveFaceIds =
        sm.sourceFaceId.size() == triCount && !sm.sourceFaceId.empty();
    // edge key -> (first face id seen, keep?)
    std::unordered_map<uint64_t, std::pair<uint32_t, bool>> edges;
    edges.reserve(triCount * 3);
    auto key = [](uint32_t a, uint32_t b) -> uint64_t {
      if (a > b) { uint32_t t = a; a = b; b = t; }
      return (static_cast<uint64_t>(a) << 32) | b;
    };
    for (size_t t = 0; t < triCount; ++t) {
      const uint32_t v[3] = {sm.indices[t * 3], sm.indices[t * 3 + 1],
                             sm.indices[t * 3 + 2]};
      const uint32_t f = haveFaceIds ? sm.sourceFaceId[t] : 0u;
      const uint32_t e[3][2] = {{v[0], v[1]}, {v[1], v[2]}, {v[2], v[0]}};
      for (int k = 0; k < 3; ++k) {
        const uint64_t ek = key(e[k][0], e[k][1]);
        auto it = edges.find(ek);
        if (it == edges.end()) {
          edges.emplace(ek, std::make_pair(f, true));  // boundary until proven interior
        } else {
          // With authored ids, an edge shared by different faces is a real cage
          // edge; sharing within one face means a triangulation diagonal. With
          // no ids, only an unshared mesh-boundary edge is knowably authored.
          it->second.second = haveFaceIds && it->second.first != f;
        }
      }
    }
    std::vector<uint32_t> wire;
    wire.reserve(edges.size() * 2);
    for (const auto& kv : edges) {
      if (!kv.second.second) continue;
      wire.push_back(static_cast<uint32_t>(kv.first >> 32));
      wire.push_back(static_cast<uint32_t>(kv.first & 0xffffffffu));
    }
    if (!wire.empty()) {
      glGenBuffers(1, &gm.wireEbo);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.wireEbo);  // no VAO bound: not VAO state
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(wire.size() * sizeof(uint32_t)),
                   wire.data(), GL_STATIC_DRAW);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
      gm.wireCount = static_cast<GLsizei>(wire.size());
    }
  }

  meshes_.push_back(gm);
}

void GLRenderer::uploadMeshAux(size_t meshIndex, const DrawMeshCPU& sm) {
  if (meshIndex >= meshes_.size()) return;
  GLMesh& gm = meshes_[meshIndex];
  if (gm.instanceCount == 0 && !gm.faceIdTex &&
      sm.sourceFaceId.size() == gm.indexCount / 3 &&
      !sm.sourceFaceId.empty()) {
    glGenBuffers(1, &gm.faceIdBuf);
    glBindBuffer(GL_TEXTURE_BUFFER, gm.faceIdBuf);
    glBufferData(GL_TEXTURE_BUFFER,
                 static_cast<GLsizeiptr>(sm.sourceFaceId.size() *
                                        sizeof(uint32_t)),
                 sm.sourceFaceId.data(), GL_STATIC_DRAW);
    glGenTextures(1, &gm.faceIdTex);
    glBindTexture(GL_TEXTURE_BUFFER, gm.faceIdTex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, gm.faceIdBuf);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
  }

  // Instanced prototypes use the same authored/local edge index buffer and a
  // dedicated instanced wire vertex shader. Skipping them here made payload
  // foliage and other progressively loaded instances permanently wireless.
  if (gm.wireEbo) return;
  if (!sm.wireframeIndices.empty()) {
    glGenBuffers(1, &gm.wireEbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.wireEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(sm.wireframeIndices.size() *
                                        sizeof(uint32_t)),
                 sm.wireframeIndices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    gm.wireCount = static_cast<GLsizei>(sm.wireframeIndices.size());
    return;
  }
  std::vector<uint32_t> wire;
  {
    const size_t triCount = sm.indices.size() / 3;
    const bool haveFaceIds =
        sm.sourceFaceId.size() == triCount && !sm.sourceFaceId.empty();
    std::unordered_map<uint64_t, std::pair<uint32_t, bool>> edges;
    edges.reserve(triCount * 3);
    auto key = [](uint32_t a, uint32_t b) -> uint64_t {
      if (a > b) std::swap(a, b);
      return (static_cast<uint64_t>(a) << 32) | b;
    };
    for (size_t t = 0; t < triCount; ++t) {
      const uint32_t v[3] = {sm.indices[t * 3], sm.indices[t * 3 + 1],
                             sm.indices[t * 3 + 2]};
      const uint32_t f = haveFaceIds ? sm.sourceFaceId[t] : 0u;
      const uint32_t e[3][2] = {{v[0], v[1]}, {v[1], v[2]}, {v[2], v[0]}};
      for (int k = 0; k < 3; ++k) {
        const uint64_t ek = key(e[k][0], e[k][1]);
        auto it = edges.find(ek);
        if (it == edges.end()) {
          edges.emplace(ek, std::make_pair(f, true));
        } else {
          it->second.second = haveFaceIds && it->second.first != f;
        }
      }
    }
    wire.reserve(edges.size() * 2);
    for (const auto& edge : edges) {
      if (!edge.second.second) continue;
      wire.push_back(static_cast<uint32_t>(edge.first >> 32));
      wire.push_back(static_cast<uint32_t>(edge.first & 0xffffffffu));
    }
  }
  if (wire.empty()) return;
  glGenBuffers(1, &gm.wireEbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.wireEbo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               static_cast<GLsizeiptr>(wire.size() * sizeof(uint32_t)),
               wire.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  gm.wireCount = static_cast<GLsizei>(wire.size());
}

void GLRenderer::updateInstanceVisibility(size_t meshIndex, const float* xforms,
                                          const float* colors,
                                          const float* opacities,
                                          uint32_t count) {
  if (meshIndex >= meshes_.size()) return;
  GLMesh& m = meshes_[meshIndex];
  if (m.instanceCount <= 0) return;
  if (count > static_cast<uint32_t>(m.instanceCount))
    count = static_cast<uint32_t>(m.instanceCount);
  m.drawInstanceCount = static_cast<int>(count);
  if (count == 0) return;
  // Re-upload the compacted visible subset to the front of the dynamic buffers.
  if (m.instanceVbo && xforms) {
    glBindBuffer(GL_ARRAY_BUFFER, m.instanceVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(count) * 12 * sizeof(float), xforms);
  }
  if (m.instanceColorVbo && colors) {
    glBindBuffer(GL_ARRAY_BUFFER, m.instanceColorVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(count) * 3 * sizeof(float), colors);
  }
  if (m.instanceOpacityVbo && opacities) {
    glBindBuffer(GL_ARRAY_BUFFER, m.instanceOpacityVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(count) * sizeof(float), opacities);
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLRenderer::ensureFbo(int w, int h) {
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (fbo_ && w == vpW_ && h == vpH_) return;
  vpW_ = w;
  vpH_ = h;
  if (!fbo_) glGenFramebuffers(1, &fbo_);
  if (!colorTex_) glGenTextures(1, &colorTex_);
  if (!depthTex_) glGenTextures(1, &depthTex_);
  if (!dofFbo_) glGenFramebuffers(1, &dofFbo_);
  if (!dofTex_) glGenTextures(1, &dofTex_);

  glBindTexture(GL_TEXTURE_2D, colorTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindTexture(GL_TEXTURE_2D, depthTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0,
               GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                         GL_TEXTURE_2D, depthTex_, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr,
                 "[lusdview][warn] OpenGL scene framebuffer is incomplete\n");
  }
  glBindTexture(GL_TEXTURE_2D, dofTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindFramebuffer(GL_FRAMEBUFFER, dofFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         dofTex_, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    std::fprintf(stderr,
                 "[lusdview][warn] OpenGL raster DOF framebuffer is incomplete\n");
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  dofApplied_ = false;
}

void GLRenderer::resizeViewport(int width, int height) { ensureFbo(width, height); }

void GLRenderer::newFrame() { ImGui_ImplOpenGL3_NewFrame(); }

void GLRenderer::buildWireProgram() {
  // Anti-aliased thin wireframe: the VS transforms an edge endpoint to clip space
  // without changing its depth; the GS expands each edge into a screen-space quad
  // uHalfWidth pixels to each side; the FS applies an
  // analytic distance-to-center falloff for a crisp ~1 px AA line (usdview look,
  // resolution-independent -- the same edge-distance idea the RT path can use).
  static const char* kWireFS =
      "#version 330 core\n"
      "in float vDist;\n"            // signed pixel distance from the line center
      "flat in float vEdgeAlpha;\n"  // projected-length density fade
      "out vec4 FragColor;\n"
      "uniform vec3 uWireColor;\n"
      "uniform float uHalfWidth;\n"  // pixels: half the expanded quad width
      "void main(){\n"
      "  float d = abs(vDist);\n"
      // 1 px-wide analytic AA: opaque core, feather to 0 over the last pixel.
      "  float a = vEdgeAlpha * (1.0 - smoothstep(uHalfWidth - 1.0, uHalfWidth, d));\n"
      "  if (a <= 0.0) discard;\n"
      "  FragColor = vec4(uWireColor, a);\n"
      "}\n";
  // Geometry shader (shared): line -> screen-space quad. Endpoints come in clip
  // space; convert to pixels, offset along the screen-space normal by uHalfWidth,
  // convert back to clip (scaling by w for perspective). vDist feathers the edge.
  static const char* kWireGS =
      "#version 330 core\n"
      "layout(lines) in;\n"
      "layout(triangle_strip, max_vertices=4) out;\n"
      "uniform vec2 uViewport;\n"     // pixels
      "uniform float uHalfWidth;\n"   // pixels
      "out float vDist;\n"
      "flat out float vEdgeAlpha;\n"
      "void main(){\n"
      "  vec4 c0 = gl_in[0].gl_Position;\n"
      "  vec4 c1 = gl_in[1].gl_Position;\n"
      // Clip the segment against the GL near plane in homogeneous space before
      // dividing by w. Dropping the whole primitive when either endpoint moved
      // behind the eye made long authored edges pop during dolly.
      "  float d0 = c0.z + c0.w;\n"
      "  float d1 = c1.z + c1.w;\n"
      "  if (d0 < 0.0 && d1 < 0.0) return;\n"
      "  if (d0 < 0.0) { float t=d0/(d0-d1); c0=mix(c0,c1,t); }\n"
      "  if (d1 < 0.0) { float t=d1/(d1-d0); c1=mix(c1,c0,t); }\n"
      "  if (c0.w <= 1e-6 || c1.w <= 1e-6) return;\n"
      "  vec2 s0 = (c0.xy / c0.w) * 0.5 * uViewport;\n"
      "  vec2 s1 = (c1.xy / c1.w) * 0.5 * uViewport;\n"
      "  vec2 dir = s1 - s0;\n"
      "  float len = length(dir);\n"
      // Suppress sub-pixel topology and fade short edges over a broad range.
      // Without this, every distant authored edge expands to a full-width quad,
      // turning detailed buildings/foliage into a solid green mass and popping
      // abruptly as projected length crosses a pixel.
      "  vEdgeAlpha = smoothstep(1.0, 7.0, len);\n"
      "  if (vEdgeAlpha <= 0.001) return;\n"
      "  dir = len > 1e-5 ? dir / len : vec2(1.0, 0.0);\n"
      "  vec2 nrm = vec2(-dir.y, dir.x);\n"
      "  vec2 off = nrm / (0.5 * uViewport) * uHalfWidth;\n"  // pixels -> NDC
      "  vec4 e0 = c0; vec4 e1 = c1;\n"
      "  gl_Position = vec4(e0.xy + off * e0.w, e0.zw); vDist =  uHalfWidth; EmitVertex();\n"
      "  gl_Position = vec4(e0.xy - off * e0.w, e0.zw); vDist = -uHalfWidth; EmitVertex();\n"
      "  gl_Position = vec4(e1.xy + off * e1.w, e1.zw); vDist =  uHalfWidth; EmitVertex();\n"
      "  gl_Position = vec4(e1.xy - off * e1.w, e1.zw); vDist = -uHalfWidth; EmitVertex();\n"
      "  EndPrimitive();\n"
      "}\n";
  static const char* kWireVS =
      "#version 330 core\n"
      "layout(location=0) in vec3 aPosition;\n"
      "layout(location=3) in uvec4 aJoint;\n"
      "layout(location=4) in vec4 aWeight;\n"
      "layout(location=5) in uvec2 aInfluence;\n"
      "layout(location=8) in uvec2 aMorphOffsetCount;\n"
      "uniform mat4 uModelViewProj;\n"
      "uniform float uDepthBias;\n"
      "uniform bool uHasMorph;\n"
      "uniform samplerBuffer uMorphDeltaTex;\n"
      "uniform samplerBuffer uMorphCoeffTex;\n"
      "uniform usamplerBuffer uMorphChanTex;\n"
      "uniform sampler2D uBoneTex;\n"
      "uniform sampler2D uInfluenceTex;\n"
      "uniform bool uSkinningEnabled;\n"
      "uniform bool uExtendedSkinningEnabled;\n"
      "uniform int uBoneTexWidth;\n"
      "uniform int uBoneMatrixCount;\n"
      "uniform int uInfluenceTexWidth;\n"
      "mat4 fetchBone(uint idx){\n"
      "  int base=int(idx)*4;\n"
      "  return mat4(\n"
      "    texelFetch(uBoneTex,ivec2((base+0)%uBoneTexWidth,(base+0)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+1)%uBoneTexWidth,(base+1)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+2)%uBoneTexWidth,(base+2)/uBoneTexWidth),0),\n"
      "    texelFetch(uBoneTex,ivec2((base+3)%uBoneTexWidth,(base+3)/uBoneTexWidth),0));\n"
      "}\n"
      "void main(){\n"
      "  vec3 pos=aPosition;\n"
      "  if(uHasMorph && aMorphOffsetCount.y>0u){\n"
      "    int base=int(aMorphOffsetCount.x), count=min(int(aMorphOffsetCount.y),256);\n"
      "    for(int i=0;i<256;++i){ if(i>=count) break;\n"
      "      int ch=int(texelFetch(uMorphChanTex,base+i).r);\n"
      "      float c=texelFetch(uMorphCoeffTex,ch).r;\n"
      "      if(abs(c)>=1e-6) pos+=c*texelFetch(uMorphDeltaTex,base+i).yzw; }\n"
      "  }\n"
      "  float wsum=dot(aWeight,vec4(1.0));\n"
      "  uint maxJoint=max(max(aJoint.x,aJoint.y),max(aJoint.z,aJoint.w));\n"
      "  if(uSkinningEnabled && uExtendedSkinningEnabled && aInfluence.y>0u && uInfluenceTexWidth>0){\n"
      "    mat4 skin=mat4(0.0); float total=0.0; int base=int(aInfluence.x);\n"
      "    int count=min(int(aInfluence.y),256);\n"
      "    for(int i=0;i<256;++i){ if(i>=count) break; int n=base+i;\n"
      "      vec4 iw=texelFetch(uInfluenceTex,ivec2(n%uInfluenceTexWidth,n/uInfluenceTexWidth),0);\n"
      "      uint j=uint(iw.x+0.5); float w=iw.y;\n"
      "      if(w>0.0 && int(j)<uBoneMatrixCount){skin+=fetchBone(j)*w; total+=w;} }\n"
      "    if(total>0.0) pos=((skin/total)*vec4(pos,1.0)).xyz;\n"
      "  } else if(uSkinningEnabled && wsum>0.0 && int(maxJoint)<uBoneMatrixCount){\n"
      "    mat4 skin=fetchBone(aJoint.x)*aWeight.x+fetchBone(aJoint.y)*aWeight.y+\n"
      "              fetchBone(aJoint.z)*aWeight.z+fetchBone(aJoint.w)*aWeight.w;\n"
      "    pos=(skin*vec4(pos,1.0)).xyz;\n"
      "  }\n"
      "  vec4 p = uModelViewProj * vec4(pos, 1.0);\n"
      "  p.z -= uDepthBias * p.w;\n"
      "  gl_Position = p;\n"
      "}\n";
  static const char* kWireInstVS =
      "#version 330 core\n"
      "layout(location=0) in vec3 aPosition;\n"
      "layout(location=3) in vec4 aRow0;\n"
      "layout(location=4) in vec4 aRow1;\n"
      "layout(location=5) in vec4 aRow2;\n"
      "uniform mat4 uViewProj;\n"
      "uniform float uDepthBias;\n"
      "void main(){\n"
      "  vec4 lp = vec4(aPosition, 1.0);\n"
      "  vec3 wp = vec3(dot(lp, aRow0), dot(lp, aRow1), dot(lp, aRow2));\n"
      "  vec4 p = uViewProj * vec4(wp, 1.0);\n"
      "  p.z -= uDepthBias * p.w;\n"
      "  gl_Position = p;\n"
      "}\n";
  std::string werr;
  wireProgram_ = glutil::CompileProgramGeom(kWireVS, kWireGS, kWireFS, &werr);
  if (wireProgram_) {
    wMVP_ = glGetUniformLocation(wireProgram_, "uModelViewProj");
    wWireColor_ = glGetUniformLocation(wireProgram_, "uWireColor");
    wDepthBias_ = glGetUniformLocation(wireProgram_, "uDepthBias");
    wViewport_ = glGetUniformLocation(wireProgram_, "uViewport");
    wHalfWidth_ = glGetUniformLocation(wireProgram_, "uHalfWidth");
    wHasMorph_ = glGetUniformLocation(wireProgram_, "uHasMorph");
    wSkinningEnabled_ = glGetUniformLocation(wireProgram_, "uSkinningEnabled");
    wExtendedSkinningEnabled_ =
        glGetUniformLocation(wireProgram_, "uExtendedSkinningEnabled");
    wBoneTexWidth_ = glGetUniformLocation(wireProgram_, "uBoneTexWidth");
    wBoneMatrixCount_ = glGetUniformLocation(wireProgram_, "uBoneMatrixCount");
    wInfluenceTexWidth_ = glGetUniformLocation(wireProgram_, "uInfluenceTexWidth");
    glUseProgram(wireProgram_);
    glUniform1i(glGetUniformLocation(wireProgram_, "uBoneTex"), 4);
    glUniform1i(glGetUniformLocation(wireProgram_, "uInfluenceTex"), 5);
    glUniform1i(glGetUniformLocation(wireProgram_, "uMorphDeltaTex"), 8);
    glUniform1i(glGetUniformLocation(wireProgram_, "uMorphCoeffTex"), 9);
    glUniform1i(glGetUniformLocation(wireProgram_, "uMorphChanTex"), 10);
  } else if (!werr.empty()) {
    fprintf(stderr, "[lusdview] wire program: %s\n", werr.c_str());
  }
  werr.clear();
  wireInstProgram_ = glutil::CompileProgramGeom(kWireInstVS, kWireGS, kWireFS, &werr);
  if (wireInstProgram_) {
    wiViewProj_ = glGetUniformLocation(wireInstProgram_, "uViewProj");
    wiWireColor_ = glGetUniformLocation(wireInstProgram_, "uWireColor");
    wiDepthBias_ = glGetUniformLocation(wireInstProgram_, "uDepthBias");
    wiViewport_ = glGetUniformLocation(wireInstProgram_, "uViewport");
    wiHalfWidth_ = glGetUniformLocation(wireInstProgram_, "uHalfWidth");
  } else if (!werr.empty()) {
    fprintf(stderr, "[lusdview] wire inst program: %s\n", werr.c_str());
  }
}

void GLRenderer::drawWireframe(const RenderFrameParams& params, const float wireColor[3]) {
  if (!wireProgram_ && !wireInstProgram_) return;
  // Do not apply a constant NDC lift: under perspective it can represent meters
  // of world-space pull at interior-scene distances and expose edges through
  // foreground walls. LEQUAL resolves exact surface matches against the unbiased
  // wire-only depth prepass.
  const float kBias = 0.0f;
  light3d::Mat4 P = ToMat4(params.proj);
  light3d::Mat4 V = ToMat4(params.view);
  glDisable(GL_CULL_FACE);
  // The GS expands each edge into a thin screen-space quad and the FS feathers it
  // analytically, so we get sub-pixel-thin AA lines (no GL_LINE_SMOOTH). Alpha
  // blend for the feathered edge; LEQUAL accepts the edge's exact surface depth,
  // while depth writes stay off so overlapping edges don't fight.
  glDisable(GL_LINE_SMOOTH);
  glDepthFunc(GL_LEQUAL);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  const float vpx = static_cast<float>(vpW_), vpy = static_cast<float>(vpH_);
  const float kHalfWidth = 1.3f;  // pixels: ~1 px visible line with a 1 px AA edge

  // Non-instanced meshes: per-mesh MVP.
  if (wireProgram_) {
    glUseProgram(wireProgram_);
    glUniform3fv(wWireColor_, 1, wireColor);
    glUniform1f(wDepthBias_, kBias);
    glUniform2f(wViewport_, vpx, vpy);
    glUniform1f(wHalfWidth_, kHalfWidth);
    for (size_t mi = 0; mi < meshes_.size(); ++mi) {
      const GLMesh& mesh = meshes_[mi];
      if (mesh.instanceCount > 0 || mesh.wireEbo == 0 || mesh.wireCount == 0) continue;
      if (params.meshVisible && mi < static_cast<size_t>(params.meshVisibleCount) &&
          !params.meshVisible[mi]) {
        continue;
      }
      light3d::Mat4 MVP = P * V * ToMat4(mesh.world);
      glUniformMatrix4fv(wMVP_, 1, GL_FALSE, MVP.m);
      const bool skinOn = mesh.skinned && skinningFrameEnabled_;
      glUniform1i(wSkinningEnabled_, skinOn ? 1 : 0);
      glUniform1i(wExtendedSkinningEnabled_,
                  (mesh.extendedSkinned && skinningFrameEnabled_) ? 1 : 0);
      glUniform1i(wBoneTexWidth_, boneTexWidth_ > 0 ? boneTexWidth_ : 4);
      glUniform1i(wBoneMatrixCount_, skinningFrameEnabled_ ? boneMatrixCount_ : 1);
      glUniform1i(wInfluenceTexWidth_,
                  (mesh.extendedSkinned && skinningFrameEnabled_)
                      ? mesh.influenceTexWidth : 0);
      glActiveTexture(GL_TEXTURE4);
      glBindTexture(GL_TEXTURE_2D, boneTex_ ? boneTex_ : whiteTex_);
      glActiveTexture(GL_TEXTURE5);
      glBindTexture(GL_TEXTURE_2D, mesh.influenceTex ? mesh.influenceTex : whiteTex_);
      glUniform1i(wHasMorph_, mesh.hasMorph ? 1 : 0);
      if (mesh.hasMorph) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_BUFFER, mesh.morphDeltaTex);
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_BUFFER, mesh.morphCoeffTex);
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_BUFFER, mesh.morphChanTex);
      }
      glActiveTexture(GL_TEXTURE0);
      glBindVertexArray(mesh.vao);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.wireEbo);  // override the VAO's tri EBO
      glDrawElements(GL_LINES, mesh.wireCount, GL_UNSIGNED_INT, nullptr);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);      // restore the VAO state
    }
  }

  // Instanced prototypes: per-instance 3x4 rows (attribs 3/4/5) + view-proj.
  if (wireInstProgram_) {
    light3d::Mat4 VP = P * V;
    glUseProgram(wireInstProgram_);
    glUniformMatrix4fv(wiViewProj_, 1, GL_FALSE, VP.m);
    glUniform3fv(wiWireColor_, 1, wireColor);
    glUniform1f(wiDepthBias_, kBias);
    glUniform2f(wiViewport_, vpx, vpy);
    glUniform1f(wiHalfWidth_, kHalfWidth);
    for (size_t mi = 0; mi < meshes_.size(); ++mi) {
      const GLMesh& mesh = meshes_[mi];
      if (mesh.instanceCount <= 0 || mesh.drawInstanceCount <= 0) continue;
      if (mesh.wireEbo == 0 || mesh.wireCount == 0) continue;
      if (params.meshVisible && mi < static_cast<size_t>(params.meshVisibleCount) &&
          !params.meshVisible[mi]) {
        continue;
      }
      glBindVertexArray(mesh.vao);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.wireEbo);
      glDrawElementsInstanced(GL_LINES, mesh.wireCount, GL_UNSIGNED_INT, nullptr,
                              mesh.drawInstanceCount);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    }
  }
  glBindVertexArray(0);
  // Restore default state for the rest of the frame (ImGui / next passes).
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_LESS);
  glDisable(GL_BLEND);
  glDisable(GL_LINE_SMOOTH);
  glUseProgram(program_);  // restore for callers that assume program_ is bound
}

void GLRenderer::drawMeshes(const RenderFrameParams& params, bool wireframe,
                            const float* overrideEmissive, AlphaPass alphaPass) {
  static const GLMaterial kDefault;
  glActiveTexture(GL_TEXTURE0 + 32);
  glBindTexture(GL_TEXTURE_BUFFER, ptexRectTex_);
  glActiveTexture(GL_TEXTURE0);
  light3d::Mat4 P = ToMat4(params.proj);
  light3d::Mat4 V = ToMat4(params.view);
  glUseProgram(program_);
  UploadClippingPlanes(program_, params);

  auto ptexGrid = [&](const DrawTexSampleCPU& sample) {
    float offset = static_cast<float>(sample.ptexRectTexelOffset);
    if (sample.tex >= 0 && static_cast<size_t>(sample.tex) < textures_.size() &&
        !textures_[static_cast<size_t>(sample.tex)].ptexRects.empty()) {
      offset = static_cast<float>(textures_[static_cast<size_t>(sample.tex)]
                                      .ptexRectOffset);
    }
    return std::array<float, 2>{offset,
                                static_cast<float>(sample.ptexFaceCount)};
  };

  auto matTranslucent = [&](int materialId) -> bool {
    if (materialId < 0 || static_cast<size_t>(materialId) >= materials_.size())
      return false;
    return materials_[static_cast<size_t>(materialId)].alphaMode == 2;  // Blend
  };

  // Draw order. For the translucent pass, sort meshes back-to-front by world
  // centroid distance to the camera so alpha "over" composites correctly.
  std::vector<size_t> order(meshes_.size());
  for (size_t i = 0; i < meshes_.size(); ++i) order[i] = i;
  if (alphaPass == AlphaPass::Translucent) {
    std::vector<float> dist(meshes_.size(), 0.0f);
    for (size_t mi = 0; mi < meshes_.size(); ++mi) {
      const GLMesh& m = meshes_[mi];
      const float* c = m.localCentroid;
      // world * centroid (column-major world[16]).
      const float wx = m.world[0]*c[0] + m.world[4]*c[1] + m.world[8]*c[2] + m.world[12];
      const float wy = m.world[1]*c[0] + m.world[5]*c[1] + m.world[9]*c[2] + m.world[13];
      const float wz = m.world[2]*c[0] + m.world[6]*c[1] + m.world[10]*c[2] + m.world[14];
      const float dx = wx - params.cameraPos[0];
      const float dy = wy - params.cameraPos[1];
      const float dz = wz - params.cameraPos[2];
      dist[mi] = dx*dx + dy*dy + dz*dz;
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return dist[a] > dist[b]; });
  }

  int cullState = -1;  // -1 unknown; dedup glEnable/glDisable(GL_CULL_FACE)
  for (size_t oi = 0; oi < order.size(); ++oi) {
    const size_t mi = order[oi];
    if (params.meshVisible && mi < static_cast<size_t>(params.meshVisibleCount) &&
        !params.meshVisible[mi]) {
      continue;  // hidden by the viewer's per-mesh visibility mask
    }
    const GLMesh& mesh = meshes_[mi];
    if (mesh.instanceCount > 0) continue;  // drawn in the instanced pass below
    light3d::Mat4 W = ToMat4(mesh.world);
    light3d::Mat4 MVP = P * V * W;
    float nmat[9];
    NormalMatrix3(mesh.world, nmat);
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, MVP.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, W.m);
    glUniformMatrix3fv(uNormalMat_, 1, GL_FALSE, nmat);
    // Morphed meshes shade with geometric normals (the rest normals no longer
    // match the GPU-morphed surface), like the displacement path.
    glUniform1i(uGeometricNormal_, (mesh.geometricNormal || mesh.hasMorph) ? 1 : 0);
    glUniform1i(uRenderMode_, static_cast<int>(params.mode));
    glUniform1f(uDepthScale_, params.depthScale > 1e-4f ? params.depthScale : 1.0f);
    glUniform3fv(uSceneMin_, 1, params.sceneMin);
    glUniform3fv(uSceneExtent_, 1, params.sceneExtent);
    glUniform1i(uMeshId_, static_cast<int>(mi));
    UploadRasterLightMask(program_, rasterLights_, static_cast<int>(mi));
    glUniform1i(uDoubleSided_, mesh.doubleSided ? 1 : 0);
    glUniform1i(uPurpose_, mesh.purposeId);
    glUniform1i(uKind_, mesh.kindId);
    glUniform1i(uHasFaceId_, mesh.faceIdTex ? 1 : 0);
    if (mesh.faceIdTex) {
      glActiveTexture(GL_TEXTURE6);
      glBindTexture(GL_TEXTURE_BUFFER, mesh.faceIdTex);
      glActiveTexture(GL_TEXTURE0);
    }
    const bool skinOn = mesh.skinned && skinningFrameEnabled_;
    glUniform1i(uSkinningEnabled_, skinOn ? 1 : 0);
    glUniform1i(uExtendedSkinningEnabled_,
                (mesh.extendedSkinned && skinningFrameEnabled_) ? 1 : 0);
    glUniform1i(uBoneTexWidth_, boneTexWidth_ > 0 ? boneTexWidth_ : 4);
    glUniform1i(uBoneMatrixCount_, skinningFrameEnabled_ ? boneMatrixCount_ : 1);
    glUniform1i(uInfluenceTexWidth_,
                (mesh.extendedSkinned && skinningFrameEnabled_) ? mesh.influenceTexWidth : 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, boneTex_ ? boneTex_ : whiteTex_);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, mesh.influenceTex ? mesh.influenceTex : whiteTex_);
    // GPU blendshape morph: bind the per-vertex delta + per-frame coefficient
    // texture buffers (units 8/9). The VAO already supplies attr 8 (offset,count).
    glUniform1i(uHasMorph_, mesh.hasMorph ? 1 : 0);
    if (mesh.hasMorph) {
      glActiveTexture(GL_TEXTURE8);
      glBindTexture(GL_TEXTURE_BUFFER, mesh.morphDeltaTex);
      glActiveTexture(GL_TEXTURE9);
      glBindTexture(GL_TEXTURE_BUFFER, mesh.morphCoeffTex);
      glActiveTexture(GL_TEXTURE10);
      glBindTexture(GL_TEXTURE_BUFFER, mesh.morphChanTex);
      glActiveTexture(GL_TEXTURE0);
    }

    // Avoid compositing both sides of a uniform translucent closed shell. This
    // is especially visible on convex lenses; varying-opacity cloth/cornea
    // meshes retain authored two-sided folds.
    const bool translucentFrontSurface =
        alphaPass == AlphaPass::Translucent && mesh.doubleSided &&
        !mesh.hasVertexOpacity;
    const int wantCull =
        (mesh.doubleSided && !translucentFrontSurface) || wireframe ? 0 : 1;
    if (wantCull != cullState) {
      if (wantCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
      else glDisable(GL_CULL_FACE);
      cullState = wantCull;
    }

    glBindVertexArray(mesh.vao);
    if (!mesh.vertexColorVbo) {
      // Generic attribute values are context state rather than VAO state on
      // several GL implementations. Re-assert the white fallback after the
      // mesh VAO is bound so a preceding instanced draw cannot leak its color.
      glVertexAttrib4f(9, 1.0f, 1.0f, 1.0f, 1.0f);
    }
    for (const auto& sub : mesh.submeshes) {
      const bool splitBack =
          !wireframe && sub.backfaceMaterialId >= 0 &&
          sub.backfaceMaterialId != sub.materialId;
      const int sideCount = splitBack ? 2 : 1;
      for (int side = 0; side < sideCount; ++side) {
      const int materialId = side == 0 ? sub.materialId
                                       : sub.backfaceMaterialId;
      if (splitBack) {
        // A purpose-bound back material turns one logical submesh into two
        // complementary draws. This also works when doubleSided is false: the
        // explicit back binding is an authored request to render that side.
        const int wantSideCull = side == 0 ? 1 : 2;
        if (wantSideCull != cullState) {
          glEnable(GL_CULL_FACE);
          glCullFace(side == 0 ? GL_BACK : GL_FRONT);
          cullState = wantSideCull;
        }
      } else if (wantCull != cullState) {
        // A preceding split-back submesh may have left GL_FRONT selected.
        // Restore this mesh's ordinary cull state before drawing an unsplit
        // submesh in the same mesh.
        if (wantCull) {
          glEnable(GL_CULL_FACE);
          glCullFace(GL_BACK);
        } else {
          glDisable(GL_CULL_FACE);
        }
        cullState = wantCull;
      }
      // Alpha-class filtering: the opaque pass skips Blend submeshes, the
      // translucent pass skips the rest. `All` (AOV/wireframe) draws everything.
      if (alphaPass == AlphaPass::Opaque && matTranslucent(materialId)) continue;
      if (alphaPass == AlphaPass::Translucent && !matTranslucent(materialId)) continue;
      const GLMaterial& mat =
          (materialId >= 0 && static_cast<size_t>(materialId) < materials_.size())
              ? materials_[static_cast<size_t>(materialId)]
              : kDefault;
      glUniform1i(uMatId_, materialId);
      glUniform1i(uFaceBase_, static_cast<int>(sub.indexOffset / 3));
      // Resolve a material texture slot to a GPU texture; white if the slot is out
      // of range or not yet uploaded (lazy texture streaming).
      auto slotTex = [&](int slot) -> GLuint {
        if (slot >= 0 && static_cast<size_t>(slot) < textures_.size() &&
            textures_[static_cast<size_t>(slot)].tex2d) {
          return textures_[static_cast<size_t>(slot)].tex2d;
        }
        return whiteTex_;
      };
      auto slotGpuTex = [&](int slot) -> const GLTexture* {
        if (slot >= 0 && static_cast<size_t>(slot) < textures_.size()) {
          const GLTexture& tex = textures_[static_cast<size_t>(slot)];
          if (tex.tex2d || (tex.isUdim && tex.arrayTex)) return &tex;
        }
        return nullptr;
      };
      // Coarse displacement: applied identically in the normal and highlight passes
      // (the deformed positions must match) and forces geometric normals so shading
      // follows the displaced surface. Unit 7 always bound (white when disabled) to
      // keep the sampler complete.
      const bool displaced =
          params.displacement && !mesh.rasterDisplacementBaked &&
          ((materialId >= 0 &&
            static_cast<size_t>(materialId) < materials_.size())
               ? materials_[static_cast<size_t>(materialId)].hasDisplacement()
               : false);
      // GPU tessellation detail path: subdivide displaced triangles on the GPU and
      // displace each generated sample. Only in the Shaded view, for non-skinned
      // displaced meshes, when the slider asks for >1x. The tess program is restored
      // to the coarse program immediately after so following submeshes draw normally.
      if (displaced && tessAvailable_ && !overrideEmissive &&
          params.maxTessLevel > 1 &&
          params.mode == RenderMode::Shaded) {
        const GLMaterial& dmat = materials_[static_cast<size_t>(materialId)];
        glUseProgram(tessProgram_);
        UploadRasterLightArray(tessProgram_, rasterLights_);
        UploadRasterLightMask(tessProgram_, rasterLights_, static_cast<int>(mi));
        UploadClippingPlanes(tessProgram_, params);
        glUniformMatrix4fv(tMVP_, 1, GL_FALSE, MVP.m);
        glUniformMatrix4fv(tModel_, 1, GL_FALSE, W.m);
        glUniformMatrix3fv(tNormalMat_, 1, GL_FALSE, nmat);
        glUniform3fv(tCameraPos_, 1, params.cameraPos);
        glUniform1f(tExposure_, params.exposure);
        glUniform3fv(tLightDir_, 1, params.lightDir);
        glUniform1i(tHasIbl_, iblActive_ ? 1 : 0);
        if (iblActive_) {
          glUniform3fv(tIblColor_, 1, iblColor_);
          glUniformMatrix3fv(tEnvRotation_, 1, GL_FALSE, iblRotation_);
        }
        glUniform3fv(tLightColor_, 1, params.lightColor);
        glUniform3fv(tBaseColor_, 1, dmat.baseColor);
        glUniform1i(tHasBaseColorTex_, dmat.baseColorTex >= 0 ? 1 : 0);
        glUniform1i(tHasDisplacementTex_, dmat.displacementTex >= 0 ? 1 : 0);
        glUniform1f(tDisplacementConst_, dmat.displacementConst);
        glUniform1f(tDisplacementScale_, params.displacementScale);
        glUniform1f(tDisplacementTexScale_, dmat.displacementTexScale);
        glUniform1f(tDisplacementTexBias_, dmat.displacementTexBias);
        glUniform1f(tMaxTessLevel_, static_cast<float>(params.maxTessLevel));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, slotTex(dmat.baseColorTex));
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, slotTex(dmat.displacementTex));
        // GPU blendshape morph in the tess vertex stage (the VAO already supplies
        // attr 8); bind the morph delta/coeff/channelId texture buffers (units
        // 8/9/10) so the tessellated surface is the morphed one.
        glUniform1i(tHasMorph_, mesh.hasMorph ? 1 : 0);
        if (mesh.hasMorph) {
          glActiveTexture(GL_TEXTURE8);
          glBindTexture(GL_TEXTURE_BUFFER, mesh.morphDeltaTex);
          glActiveTexture(GL_TEXTURE9);
          glBindTexture(GL_TEXTURE_BUFFER, mesh.morphCoeffTex);
          glActiveTexture(GL_TEXTURE10);
          glBindTexture(GL_TEXTURE_BUFFER, mesh.morphChanTex);
          glActiveTexture(GL_TEXTURE0);
        }
        // Skinning in the tess vertex stage (mirrors the coarse program); bone
        // matrices (unit 4) + extended-influence (unit 5) texture buffers.
        const bool tSkinOn = mesh.skinned && skinningFrameEnabled_;
        glUniform1i(tSkinningEnabled_, tSkinOn ? 1 : 0);
        glUniform1i(tExtendedSkinningEnabled_,
                    (mesh.extendedSkinned && skinningFrameEnabled_) ? 1 : 0);
        glUniform1i(tBoneTexWidth_, boneTexWidth_ > 0 ? boneTexWidth_ : 4);
        glUniform1i(tBoneMatrixCount_, skinningFrameEnabled_ ? boneMatrixCount_ : 1);
        glUniform1i(tInfluenceTexWidth_,
                    (mesh.extendedSkinned && skinningFrameEnabled_) ? mesh.influenceTexWidth : 0);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, boneTex_ ? boneTex_ : whiteTex_);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, mesh.influenceTex ? mesh.influenceTex : whiteTex_);
        glActiveTexture(GL_TEXTURE0);
        glPatchParameteri(GL_PATCH_VERTICES, 3);
        glDrawElements(GL_PATCHES, static_cast<GLsizei>(sub.indexCount),
                       GL_UNSIGNED_INT,
                       (void*)(static_cast<uintptr_t>(sub.indexOffset) * sizeof(uint32_t)));
        glUseProgram(program_);  // restore coarse program for following submeshes
        continue;
      }
      glUniform1i(uHasDisplacement_, displaced ? 1 : 0);
      glActiveTexture(GL_TEXTURE7);
      if (displaced) {
        const GLMaterial& dmat = materials_[static_cast<size_t>(materialId)];
        glUniform1i(uHasDisplacementTex_, dmat.displacementTex >= 0 ? 1 : 0);
        glUniform1f(uDisplacementConst_, dmat.displacementConst);
        glUniform1f(uDisplacementScale_, params.displacementScale);
        glUniform1f(uDisplacementTexScale_, dmat.displacementTexScale);
        glUniform1f(uDisplacementTexBias_, dmat.displacementTexBias);
        glBindTexture(GL_TEXTURE_2D, slotTex(dmat.displacementTex));
        glUniform1i(uGeometricNormal_, 1);
      } else {
        glBindTexture(GL_TEXTURE_2D, whiteTex_);
        glUniform1i(uGeometricNormal_, (mesh.geometricNormal || mesh.hasMorph) ? 1 : 0);
      }
      if (overrideEmissive) {
        glUniform3f(uBaseColor_, 0.f, 0.f, 0.f);
        glUniform1f(uMetallic_, 0.f);
        glUniform1f(uRoughness_, 1.f);
        glUniform1i(uUseSpecularWorkflow_, 0);
        glUniform1i(uOpenPbrSpecularModel_, 0);
        glUniform3f(uSpecularColor_, 0.f, 0.f, 0.f);
        glUniform1f(uIor_, 1.5f);
        glUniform1f(uOcclusion_, 1.0f);
        glUniform1f(uCoatWeight_, 0.0f);
        glUniform3f(uCoatColor_, 1.0f, 1.0f, 1.0f);
        glUniform1f(uCoatRoughness_, 0.1f);
        glUniform1f(uCoatIor_, 1.5f);
        glUniform1f(uTransmission_, 0.0f);
        glUniform3f(uTransmissionColor_, 1.0f, 1.0f, 1.0f);
        glUniform1f(uTransmissionDepth_, 0.0f);
        glUniform3f(uTransmissionScatter_, 0.0f, 0.0f, 0.0f);
        glUniform1f(uTransmissionScatterAnisotropy_, 0.0f);
        glUniform1f(uVolumeDensity_, 0.0f);
        glUniform3f(uVolumeAlbedo_, 1.0f, 1.0f, 1.0f);
        glUniform3f(uVolumeEmission_, 0.0f, 0.0f, 0.0f);
        glUniform1f(uVolumeEmissionScale_, 0.0f);
        glUniform1f(uDiffuseRoughness_, 0.0f);
        glUniform1f(uTransmissionDispersionAbbeNumber_, 0.0f);
        glUniform1f(uSubsurfaceAnisotropy_, 0.0f);
        glUniform1f(uSubsurfaceScatterAnisotropy_, 0.0f);
        glUniform1f(uSpecularAnisotropy_, 0.0f);
        glUniform1f(uSpecularRotation_, 0.0f);
        glUniform1f(uSpecularRoughnessAnisotropy_, 0.0f);
        glUniform1f(uCoatAnisotropy_, 0.0f);
        glUniform1f(uCoatRotation_, 0.0f);
        glUniform1f(uCoatRoughnessAnisotropy_, 0.0f);
        glUniform1f(uTransmissionDispersion_, 0.0f);
        glUniform1f(uTransmissionDispersionScale_, 0.0f);
        glUniform1f(uCoatAffectColor_, 0.0f);
        glUniform1f(uCoatAffectRoughness_, 0.0f);
        glUniform1f(uCoatDarkening_, 0.0f);
        glUniform1f(uBaseWeight_, 1.0f);
        glUniform1f(uSpecularWeight_, 1.0f);
        glUniform1f(uSubsurface_, 0.0f);
        glUniform3f(uSubsurfaceColor_, 0.8f, 0.8f, 0.8f);
        glUniform1f(uSubsurfaceScale_, 1.0f);
        glUniform1f(uSubsurfaceRadius_, 1.0f);
        glUniform1f(uSheenWeight_, 0.0f);
        glUniform3f(uSheenColor_, 1.0f, 1.0f, 1.0f);
        glUniform1f(uSheenRoughness_, 0.3f);
        glUniform1f(uThinFilmWeight_, 0.0f);
        glUniform1f(uThinFilmThickness_, 0.0f);
        glUniform1f(uThinFilmIor_, 1.5f);
        glUniform3fv(uEmissive_, 1, overrideEmissive);
        glUniform1f(uAlpha_, 1.f);
        glUniform1i(uAlphaMode_, 0);
        glUniform1f(uAlphaCutoff_, 0.5f);
        glUniform1i(uHasBaseColorTex_, 0);
        glUniform1i(uBasePtex_, 0);
        glUniform2f(uBasePtexGrid_, 1.0f, 1.0f);
        glUniform1i(uHasMetallicTex_, 0);
        glUniform1i(uHasRoughnessTex_, 0);
        glUniform1i(uHasNormalTex_, 0);
        glUniform1i(uHasEmissiveTex_, 0);
        glUniform1i(uHasOpacityTex_, 0);
        glUniform1i(uHasOcclusionTex_, 0);
        glUniform1i(uHasSpecularColorTex_, 0);
        glUniform1i(uHasCoatWeightTex_, 0);
        glUniform1i(uHasCoatColorTex_, 0);
        glUniform1i(uHasCoatRoughnessTex_, 0);
        glUniform1i(uHasCoatNormalTex_, 0);
        glUniform4i(uAdvancedTexIsUdim_, 0, 0, 0, 0);
        glUniform4i(uAdvancedUdimRoutes_, -1, -1, -1, -1);
        glUniform4i(uAdvancedUdimSlots_, -1, -1, -1, -1);
        glUniform1i(uCoatNormalTexIsUdim_, 0);
        glUniform1i(uCoatNormalUdimRoute_, -1);
        glUniform1i(uCoatNormalUdimSlot_, -1);
        glUniform1i(uBaseColorTexIsUdim_, 0);
        glUniform1i(uMetallicTexIsUdim_, 0);
        glUniform1i(uRoughnessTexIsUdim_, 0);
        glUniform1i(uNormalTexIsUdim_, 0);
        glUniform1i(uEmissiveTexIsUdim_, 0);
        glUniform1i(uOpacityTexIsUdim_, 0);
        glUniform1i(uOcclusionTexIsUdim_, 0);
      } else {
        glUniform3fv(uBaseColor_, 1, mat.baseColor);
        glUniform1f(uMetallic_, mat.metallic);
        glUniform1f(uRoughness_, mat.roughness);
        glUniform1i(uUseSpecularWorkflow_, mat.useSpecularWorkflow ? 1 : 0);
        glUniform1i(uOpenPbrSpecularModel_, mat.openPbrSpecularModel ? 1 : 0);
        glUniform3fv(uSpecularColor_, 1, mat.specularColor);
        glUniform1f(uIor_, mat.ior);
        glUniform1f(uOcclusion_, mat.occlusion);
        glUniform1f(uCoatWeight_, mat.coatWeight);
        glUniform3fv(uCoatColor_, 1, mat.coatColor);
        glUniform1f(uCoatRoughness_, mat.coatRoughness);
        glUniform1f(uCoatIor_, mat.coatIor);
        const DrawLightRtOpenPBRCPU& p = mat.lightRtOpenPBR;
        glUniform1f(uBaseWeight_, p.baseWeight);
        glUniform1f(uSpecularWeight_, p.specularWeight);
        glUniform1f(uTransmission_, p.transmission);
        glUniform3fv(uTransmissionColor_, 1, p.transmissionColor);
        glUniform1f(uTransmissionDepth_, p.transmissionDepth);
        glUniform3fv(uTransmissionScatter_, 1, p.transmissionScatter);
        glUniform1f(uTransmissionScatterAnisotropy_,
                    p.transmissionScatterAnisotropy);
        glUniform1f(uVolumeDensity_, p.volumeDensity);
        glUniform3fv(uVolumeAlbedo_, 1, p.volumeAlbedo);
        glUniform3fv(uVolumeEmission_, 1, p.volumeEmission);
        glUniform1f(uVolumeEmissionScale_, p.volumeEmissionScale);
        glUniform1f(uDiffuseRoughness_, p.diffuseRoughness);
        glUniform1f(uTransmissionDispersionAbbeNumber_,
                    p.transmissionDispersionAbbeNumber);
        glUniform1f(uSubsurfaceAnisotropy_, p.subsurfaceAnisotropy);
        glUniform1f(uSubsurfaceScatterAnisotropy_,
                    p.subsurfaceScatterAnisotropy);
        glUniform1f(uSpecularAnisotropy_, p.specularAnisotropy);
        glUniform1f(uSpecularRotation_, p.specularRotation);
        glUniform1f(uSpecularRoughnessAnisotropy_, p.specularRoughnessAnisotropy);
        glUniform1f(uCoatAnisotropy_, p.coatAnisotropy);
        glUniform1f(uCoatRotation_, p.coatRotation);
        glUniform1f(uCoatRoughnessAnisotropy_, p.coatRoughnessAnisotropy);
        glUniform1f(uTransmissionDispersion_, p.transmissionDispersion);
        glUniform1f(uTransmissionDispersionScale_, p.transmissionDispersionScale);
        glUniform1f(uCoatAffectColor_, p.coatAffectColor);
        glUniform1f(uCoatAffectRoughness_, p.coatAffectRoughness);
        glUniform1f(uCoatDarkening_, p.coatDarkening);
        glUniform1f(uSubsurface_, p.subsurface);
        glUniform3fv(uSubsurfaceColor_, 1, p.subsurfaceColor);
        glUniform1f(uSubsurfaceScale_, p.subsurfaceScale);
        glUniform1f(uSubsurfaceRadius_, p.subsurfaceRadius[0]);
        glUniform1f(uSheenWeight_, p.sheenWeight);
        glUniform3fv(uSheenColor_, 1, p.sheenColor);
        glUniform1f(uSheenRoughness_, p.sheenRoughness);
        glUniform1f(uThinFilmWeight_, p.thinFilmWeight);
        glUniform1f(uThinFilmThickness_, p.thinFilmThicknessNm);
        glUniform1f(uThinFilmIor_, p.thinFilmIor);
        glUniform3fv(uEmissive_, 1, mat.emissive);
        glUniform1f(uAlpha_, mat.alpha);
        glUniform1i(uAlphaMode_, mat.alphaMode);
        glUniform1f(uAlphaCutoff_, mat.alphaCutoff);
        SetUvUniform(uBaseColorUv0_, uBaseColorUv1_, mat.baseColorSample.uv);
        SetUvUniform(uMetallicUv0_, uMetallicUv1_, mat.metallicSample.uv);
        SetUvUniform(uRoughnessUv0_, uRoughnessUv1_, mat.roughnessSample.uv);
        SetUvUniform(uNormalUv0_, uNormalUv1_, mat.normalSample.uv);
        SetUvUniform(uEmissiveUv0_, uEmissiveUv1_, mat.emissiveSample.uv);
        SetUvUniform(uOpacityUv0_, uOpacityUv1_, mat.opacitySample.uv);
        SetUvUniform(uOcclusionUv0_, uOcclusionUv1_, mat.occlusionSample.uv);
        SetUvUniform(uSpecularColorUv0_, uSpecularColorUv1_,
                     mat.specularColorSample.uv);
        SetUvUniform(uCoatWeightUv0_, uCoatWeightUv1_, mat.coatWeightSample.uv);
        SetUvUniform(uCoatColorUv0_, uCoatColorUv1_, mat.coatColorSample.uv);
        SetUvUniform(uCoatRoughnessUv0_, uCoatRoughnessUv1_,
                     mat.coatRoughnessSample.uv);
        SetUvUniform(uCoatNormalUv0_, uCoatNormalUv1_,
                     mat.coatNormalSample.uv);
        glUniform1i(uCoatWeightUvSet_, mat.coatWeightSample.uvSet);
        glUniform1i(uSpecularColorUvSet_, mat.specularColorSample.uvSet);
        glUniform1i(uCoatColorUvSet_, mat.coatColorSample.uvSet);
        glUniform1i(uCoatRoughnessUvSet_, mat.coatRoughnessSample.uvSet);
        glUniform1i(uCoatNormalUvSet_, mat.coatNormalSample.uvSet);
        // Sample.channel is -1 for whole-value reads; scalar coat slots default
        // to R in that case.
        glUniform1i(uCoatWeightChannel_,
                    mat.coatWeightSample.channel < 0
                        ? 0 : mat.coatWeightSample.channel);
        glUniform1i(uCoatRoughnessChannel_,
                    mat.coatRoughnessSample.channel < 0
                        ? 0 : mat.coatRoughnessSample.channel);
        glUniform4fv(uCoatWeightScale_, 1, mat.coatWeightSample.scale);
        glUniform4fv(uCoatWeightBias_, 1, mat.coatWeightSample.bias);
        glUniform4fv(uCoatColorScale_, 1, mat.coatColorSample.scale);
        glUniform4fv(uCoatColorBias_, 1, mat.coatColorSample.bias);
        glUniform4fv(uCoatRoughnessScale_, 1, mat.coatRoughnessSample.scale);
        glUniform4fv(uCoatRoughnessBias_, 1, mat.coatRoughnessSample.bias);
        glUniform4fv(uSpecularColorScale_, 1, mat.specularColorSample.scale);
        glUniform4fv(uSpecularColorBias_, 1, mat.specularColorSample.bias);
        glUniform4fv(uCoatNormalScale_, 1, mat.coatNormalSample.scale);
        glUniform4fv(uCoatNormalBias_, 1, mat.coatNormalSample.bias);
        {
          const GLint uvSets[4] = {mat.baseColorSample.uvSet,
                                   mat.metallicSample.uvSet,
                                   mat.normalSample.uvSet,
                                   mat.emissiveSample.uvSet};
          glUniform4iv(uUvSet_, 1, uvSets);
        }
        glUniform1i(uRoughnessUvSet_, mat.roughnessSample.uvSet);
        glUniform4fv(uBaseColorTexScale_, 1, mat.baseColorSample.scale);
        glUniform4fv(uBaseColorTexBias_, 1, mat.baseColorSample.bias);
        glUniform1i(uBasePtex_, mat.baseColorSample.isPtex ? 1 : 0);
        const auto baseGrid = ptexGrid(mat.baseColorSample);
        const auto metallicGrid = ptexGrid(mat.metallicSample);
        const auto roughnessGrid = ptexGrid(mat.roughnessSample);
        const auto normalGrid = ptexGrid(mat.normalSample);
        const auto emissiveGrid = ptexGrid(mat.emissiveSample);
        const auto opacityGrid = ptexGrid(mat.opacitySample);
        const auto occlusionGrid = ptexGrid(mat.occlusionSample);
        const auto specularGrid = ptexGrid(mat.specularColorSample);
        const auto coatWeightGrid = ptexGrid(mat.coatWeightSample);
        const auto coatColorGrid = ptexGrid(mat.coatColorSample);
        const auto coatRoughnessGrid = ptexGrid(mat.coatRoughnessSample);
        glUniform2fv(uBasePtexGrid_, 1, baseGrid.data());
        glUniform2fv(uMetallicPtexGrid_, 1, metallicGrid.data());
        glUniform2fv(uRoughnessPtexGrid_, 1, roughnessGrid.data());
        glUniform2fv(uNormalPtexGrid_, 1, normalGrid.data());
        glUniform2fv(uEmissivePtexGrid_, 1, emissiveGrid.data());
        glUniform2fv(uOpacityPtexGrid_, 1, opacityGrid.data());
        glUniform2fv(uOcclusionPtexGrid_, 1, occlusionGrid.data());
        glUniform2fv(uSpecularColorPtexGrid_, 1, specularGrid.data());
        glUniform2fv(uCoatWeightPtexGrid_, 1, coatWeightGrid.data());
        glUniform2fv(uCoatColorPtexGrid_, 1, coatColorGrid.data());
        glUniform2fv(uCoatRoughnessPtexGrid_, 1, coatRoughnessGrid.data());
        glUniform4fv(uNormalTexScale_, 1, mat.normalSample.scale);
        glUniform4fv(uNormalTexBias_, 1, mat.normalSample.bias);
        glUniform4fv(uEmissiveTexScale_, 1, mat.emissiveSample.scale);
        glUniform4fv(uEmissiveTexBias_, 1, mat.emissiveSample.bias);
        glUniform1i(uMetallicChannel_, mat.metallicChannel);
        glUniform1i(uRoughnessChannel_, mat.roughnessChannel);
        glUniform1f(uMetallicTexScale_, mat.metallicTexScale);
        glUniform1f(uMetallicTexBias_, mat.metallicTexBias);
        glUniform1f(uRoughnessTexScale_, mat.roughnessTexScale);
        glUniform1f(uRoughnessTexBias_, mat.roughnessTexBias);
        glUniform1i(uOpacityUvSet_, mat.opacitySample.uvSet);
        glUniform1i(uOpacityChannel_, mat.opacityChannel);
        glUniform1f(uOpacityTexScale_, mat.opacityTexScale);
        glUniform1f(uOpacityTexBias_, mat.opacityTexBias);
        glUniform1i(uOcclusionUvSet_, mat.occlusionSample.uvSet);
        glUniform1i(uOcclusionChannel_, mat.occlusionChannel);
        glUniform1f(uOcclusionTexScale_, mat.occlusionTexScale);
        glUniform1f(uOcclusionTexBias_, mat.occlusionTexBias);
        const GLint udimSlots[4] = {mat.baseColorTex, mat.metallicTex,
                                    mat.normalTex, mat.emissiveTex};
        glUniform4iv(uUdimSlots_, 1, udimSlots);
        glUniform1i(uOpacityUdimSlot_, mat.opacityTex);
        glUniform1i(uRoughnessUdimSlot_, mat.roughnessTex);
        glUniform1i(uOcclusionUdimSlot_, mat.occlusionTex);
        glActiveTexture(GL_TEXTURE12);
        glBindTexture(GL_TEXTURE_2D, udimLutAtlas_);
        auto bindMaterialTexture = [&](int slot, GLenum texUnit2D,
                                       GLenum texUnitArray,
                                       GLint hasLoc, GLint isUdimLoc) {
          const GLTexture* tex = slotGpuTex(slot);
          const bool hasTex = tex != nullptr;
          const bool isUdim = hasTex && tex->isUdim && tex->arrayTex;
          glUniform1i(hasLoc, hasTex ? 1 : 0);
          glUniform1i(isUdimLoc, isUdim ? 1 : 0);
          glActiveTexture(texUnit2D);
          glBindTexture(GL_TEXTURE_2D,
                        (hasTex && tex->tex2d) ? tex->tex2d : whiteTex_);
          if (isUdim) {
            glActiveTexture(texUnitArray);
            glBindTexture(GL_TEXTURE_2D_ARRAY, tex->arrayTex);
          }
        };
        bindMaterialTexture(mat.baseColorTex, GL_TEXTURE0, GL_TEXTURE11,
                            uHasBaseColorTex_,
                            uBaseColorTexIsUdim_);
        bindMaterialTexture(mat.metallicTex, GL_TEXTURE1, GL_TEXTURE13,
                            uHasMetallicTex_, uMetallicTexIsUdim_);
        bindMaterialTexture(mat.roughnessTex, GL_TEXTURE18, GL_TEXTURE22,
                            uHasRoughnessTex_, uRoughnessTexIsUdim_);
        bindMaterialTexture(mat.normalTex, GL_TEXTURE2, GL_TEXTURE15,
                            uHasNormalTex_,
                            uNormalTexIsUdim_);
        bindMaterialTexture(mat.emissiveTex, GL_TEXTURE3, GL_TEXTURE17,
                            uHasEmissiveTex_,
                            uEmissiveTexIsUdim_);
        bindMaterialTexture(mat.opacityTex, GL_TEXTURE14, GL_TEXTURE16,
                            uHasOpacityTex_, uOpacityTexIsUdim_);
        const int coreSlots[7] = {
            mat.baseColorTex, mat.metallicTex, mat.normalTex, mat.opacityTex,
            mat.emissiveTex, mat.roughnessTex, mat.occlusionTex};
        const GLenum routeUnits[7] = {
            GL_TEXTURE11, GL_TEXTURE13, GL_TEXTURE15, GL_TEXTURE16,
            GL_TEXTURE17, GL_TEXTURE22, GL_TEXTURE24};
        int routeSlots[7];
        for (int route = 0; route < 7; ++route) {
          const GLTexture* tex = slotGpuTex(coreSlots[route]);
          routeSlots[route] =
              (tex && tex->isUdim && tex->arrayTex) ? coreSlots[route] : -1;
        }
        auto bindAdvanced = [&](int slot, GLenum texUnit2D, GLint hasLoc,
                                int* isUdimOut, int* routeOut) {
          const GLTexture* tex = slotGpuTex(slot);
          bool hasTex = tex != nullptr && tex->tex2d != 0;
          *isUdimOut = 0;
          *routeOut = -1;
          if (tex && tex->isUdim && tex->arrayTex) {
            int route = -1;
            for (int i = 0; i < 7; ++i) {
              if (routeSlots[i] == slot) { route = i; break; }
            }
            if (route < 0) {
              for (int i = 0; i < 7; ++i) {
                if (routeSlots[i] < 0) { route = i; break; }
              }
            }
            if (route >= 0) {
              routeSlots[route] = slot;
              glActiveTexture(routeUnits[route]);
              glBindTexture(GL_TEXTURE_2D_ARRAY, tex->arrayTex);
              *isUdimOut = 1;
              *routeOut = route;
              hasTex = true;
            } else {
              hasTex = false;
            }
          }
          glUniform1i(hasLoc, hasTex ? 1 : 0);
          glActiveTexture(texUnit2D);
          glBindTexture(GL_TEXTURE_2D,
                        (hasTex && !*isUdimOut) ? tex->tex2d : whiteTex_);
        };
        int advancedIsUdim[4]{}, advancedRoutes[4]{-1, -1, -1, -1};
        bindAdvanced(mat.specularColorTex, GL_TEXTURE26,
                     uHasSpecularColorTex_, &advancedIsUdim[0],
                     &advancedRoutes[0]);
        bindAdvanced(mat.coatWeightTex, GL_TEXTURE27, uHasCoatWeightTex_,
                     &advancedIsUdim[1], &advancedRoutes[1]);
        bindAdvanced(mat.coatColorTex, GL_TEXTURE28, uHasCoatColorTex_,
                     &advancedIsUdim[2], &advancedRoutes[2]);
        bindAdvanced(mat.coatRoughnessTex, GL_TEXTURE29,
                     uHasCoatRoughnessTex_, &advancedIsUdim[3],
                     &advancedRoutes[3]);
        int coatNormalIsUdim = 0, coatNormalRoute = -1;
        bindAdvanced(mat.coatNormalTex, GL_TEXTURE30, uHasCoatNormalTex_,
                     &coatNormalIsUdim, &coatNormalRoute);
        glUniform4iv(uAdvancedTexIsUdim_, 1, advancedIsUdim);
        glUniform4iv(uAdvancedUdimRoutes_, 1, advancedRoutes);
        const int advancedSlots[4] = {
            mat.specularColorTex, mat.coatWeightTex, mat.coatColorTex,
            mat.coatRoughnessTex};
        glUniform4iv(uAdvancedUdimSlots_, 1, advancedSlots);
        glUniform1i(uCoatNormalTexIsUdim_, coatNormalIsUdim);
        glUniform1i(uCoatNormalUdimRoute_, coatNormalRoute);
        glUniform1i(uCoatNormalUdimSlot_, mat.coatNormalTex);
        bindMaterialTexture(mat.occlusionTex, GL_TEXTURE23, GL_TEXTURE24,
                            uHasOcclusionTex_, uOcclusionTexIsUdim_);
        glActiveTexture(GL_TEXTURE0);
      }
      // Native MaterialX graph upload. Keep ordinary graph images in a separate
      // high texture-unit range so semantic bindings remain valid for mixed
      // graphs. A graph UDIM image reuses the matching semantic UDIM array route
      // (GL 3.3 has no portable bindless sampler); graph-only UDIMs use the
      // deterministic semantic fallback.
      {
        const MaterialXGraphRuntimeCPU& graph = mat.materialXGraph;
        std::vector<int> graphTextureIds;
        std::array<int, 8> graphUdimRoutes{};
        graphUdimRoutes.fill(-1);
        const int graphCoreSlots[7] = {
            mat.baseColorTex, mat.metallicTex, mat.normalTex, mat.opacityTex,
            mat.emissiveTex, mat.roughnessTex, mat.occlusionTex};
        auto graphTextureIsUdim = [&](int slot) {
          return slot >= 0 && static_cast<size_t>(slot) < textures_.size() &&
                 textures_[static_cast<size_t>(slot)].isUdim &&
                 textures_[static_cast<size_t>(slot)].arrayTex != 0;
        };
        auto graphUdimRoute = [&](int slot) {
          for (int route = 0; route < 7; ++route) {
            if (graphCoreSlots[route] == slot &&
                graphTextureIsUdim(slot)) return route;
          }
          return -1;
        };
        // The native graph interpreter is for MaterialX/OpenPBR graphs. A
        // UsdPreviewSurface network is already lowered into the semantic slots
        // above (including its named primvar reader); interpreting its retained
        // diagnostic graph a second time bypasses per-slot UV routing.
        bool graphUsable = mat.openPbrSpecularModel && graph.valid &&
                           graph.nodes.size() <= 64;
        if (graphUsable) {
          for (const MaterialXGraphNodeCPU& node : graph.nodes) {
            if (node.textureId < 0) continue;
            if (std::find(graphTextureIds.begin(), graphTextureIds.end(),
                          node.textureId) == graphTextureIds.end()) {
              graphTextureIds.push_back(node.textureId);
              // The GL 3.3 shader reserves four ordinary graph samplers so it
              // stays within the 32-fragment-sampler limit after semantic,
              // UDIM, shadow, and Ptex bindings are linked. Vulkan supports
              // the full eight-image graph budget independently.
              if (graphTextureIds.size() > 4) { graphUsable = false; break; }
            }
            const bool nodeUdim = node.isUdim ||
                                  graphTextureIsUdim(node.textureId);
            if (nodeUdim) {
              const size_t local = static_cast<size_t>(
                  std::find(graphTextureIds.begin(), graphTextureIds.end(),
                            node.textureId) - graphTextureIds.begin());
              const int route = graphUdimRoute(node.textureId);
              if (route < 0) { graphUsable = false; break; }
              graphUdimRoutes[local] = route;
            }
          }
        }
        for (int i = 0; i < 8; ++i) {
          glActiveTexture(GL_TEXTURE0 + 33 + i);
          GLuint tex = whiteTex_;
          if (graphUsable && i < static_cast<int>(graphTextureIds.size())) {
            const int slot = graphTextureIds[static_cast<size_t>(i)];
            if (graphUdimRoutes[static_cast<size_t>(i)] >= 0) {
              // The corresponding semantic sampler2DArray was bound above.
              tex = whiteTex_;
            } else if (slot >= 0 && static_cast<size_t>(slot) < textures_.size() &&
                       textures_[static_cast<size_t>(slot)].tex2d)
              tex = textures_[static_cast<size_t>(slot)].tex2d;
            else
              graphUsable = false;
          }
          glBindTexture(GL_TEXTURE_2D, tex);
        }
        if (uGraphUdimRoutes_ >= 0)
          glUniform1iv(uGraphUdimRoutes_, 8, graphUdimRoutes.data());
        glUniform1i(uGraphUsable_, graphUsable ? 1 : 0);
        glUniform1i(uGraphNodeCount_, graphUsable ? static_cast<GLint>(graph.nodes.size()) : 0);
        std::array<GLint, MaterialXGraphRuntimeCPU::kOutputCount> graphOutputs{};
        for (size_t i = 0; i < graphOutputs.size(); ++i)
          graphOutputs[i] = graph.output[i];
        glUniform1iv(uGraphOutputs_,
                     static_cast<GLsizei>(graphOutputs.size()),
                     graphOutputs.data());
        if (graphUsable) {
          std::vector<GLfloat> n0(graph.nodes.size() * 4, 0.0f);
          std::vector<GLfloat> n1(graph.nodes.size() * 4, 0.0f);
          std::vector<GLfloat> n2(graph.nodes.size() * 4, 0.0f);
          std::vector<GLfloat> n3(graph.nodes.size() * 4, 0.0f);
          std::vector<GLfloat> n4(graph.nodes.size() * 4, 0.0f);
          std::vector<GLfloat> n5(graph.nodes.size() * 4, 0.0f);
          for (size_t i = 0; i < graph.nodes.size(); ++i) {
            const MaterialXGraphNodeCPU& node = graph.nodes[i];
            n0[i * 4 + 0] = static_cast<float>(node.op);
            n0[i * 4 + 1] = static_cast<float>(node.input[0]);
            n0[i * 4 + 2] = static_cast<float>(node.input[1]);
            n0[i * 4 + 3] = static_cast<float>(node.input[2]);
            for (int lane = 0; lane < 4; ++lane) {
              n1[i * 4 + lane] = node.value[0][lane];
              n2[i * 4 + lane] = node.value[1][lane];
              n3[i * 4 + lane] = node.value[2][lane];
            }
            int local = -1;
            if (node.textureId >= 0) {
              auto it = std::find(graphTextureIds.begin(), graphTextureIds.end(),
                                  node.textureId);
              local = it == graphTextureIds.end()
                          ? -1
                          : static_cast<int>(it - graphTextureIds.begin());
            }
            const bool nodeUdim = local >= 0 &&
                                  graphUdimRoutes[static_cast<size_t>(local)] >= 0;
            n4[i * 4 + 0] = nodeUdim ? -static_cast<float>(local + 1)
                                     : static_cast<float>(local);
            n4[i * 4 + 1] = node.uvScale[0];
            n4[i * 4 + 2] = node.uvScale[1];
            n4[i * 4 + 3] = node.uvOffset[0];
            n5[i * 4 + 0] = node.uvOffset[1];
            n5[i * 4 + 1] = node.value[2][3];
            n5[i * 4 + 2] = nodeUdim ? static_cast<float>(node.textureId) : -1.0f;
          }
          glUniform4fv(uGraphNode0_, static_cast<GLsizei>(graph.nodes.size()), n0.data());
          glUniform4fv(uGraphNode1_, static_cast<GLsizei>(graph.nodes.size()), n1.data());
          glUniform4fv(uGraphNode2_, static_cast<GLsizei>(graph.nodes.size()), n2.data());
          glUniform4fv(uGraphNode3_, static_cast<GLsizei>(graph.nodes.size()), n3.data());
          glUniform4fv(uGraphNode4_, static_cast<GLsizei>(graph.nodes.size()), n4.data());
          glUniform4fv(uGraphNode5_, static_cast<GLsizei>(graph.nodes.size()), n5.data());
        }
        glActiveTexture(GL_TEXTURE0);
      }
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.indexCount), GL_UNSIGNED_INT,
                     (void*)(static_cast<uintptr_t>(sub.indexOffset) * sizeof(uint32_t)));
      }
    }
  }

  // Instanced pass: PointInstancer prototypes drawn with glDrawElementsInstanced.
  // Flat-shaded (default gray material, hardcoded headlight in the fragment
  // shader); each instance's model matrix comes from vertex attribs 3-5, color
  // from attrib 9, opacity from attrib 11. Batches with any instance opacity < 1
  // draw in the translucent pass; the rest draw in opaque/All.
  bool anyInstanced = false;
  for (const GLMesh& m : meshes_) {
    if (m.instanceCount > 0) { anyInstanced = true; break; }
  }
  if (anyInstanced && instProgram_) {
    light3d::Mat4 VP = P * V;
    glUseProgram(instProgram_);
    UploadClippingPlanes(instProgram_, params);
    glUniformMatrix4fv(iUViewProj_, 1, GL_FALSE, VP.m);
    glUniform3fv(iCameraPos_, 1, params.cameraPos);
    glUniform1f(iExposure_, params.exposure);
    const bool hasPointShadow = pointShadowCameras_.lightSlot >= 0 &&
                                pointShadowDepthTex_ != 0;
    const bool hasPlanarShadow = shadowCamera_.lightSlot >= 0 && shadowDepthTex_ != 0;
    const int shadowLightSlot = hasPointShadow ? pointShadowCameras_.lightSlot
                                                : shadowCamera_.lightSlot;
    glUniform1i(iHasShadowMap_, hasPlanarShadow ? 1 : 0);
    glUniform1i(iShadowLightSlot_, shadowLightSlot);
    glUniformMatrix4fv(iShadowViewProj_, 1, GL_FALSE, shadowCamera_.viewProj.m);
    glUniform1i(iHasPointShadowMap_, hasPointShadow ? 1 : 0);
    if (hasPointShadow) {
      const RasterLightGPU& point = rasterLights_.lights[
          static_cast<size_t>(pointShadowCameras_.lightSlot)];
      glUniform3fv(iPointShadowLightPos_, 1, point.positionType);
      glUniformMatrix4fv(iPointShadowViewProj_, 6, GL_FALSE,
                         pointShadowCameras_.viewProj[0].m);
      glActiveTexture(GL_TEXTURE31);
      glBindTexture(GL_TEXTURE_CUBE_MAP, pointShadowDepthTex_);
    }
    if (hasPlanarShadow) {
      glActiveTexture(GL_TEXTURE25);
      glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    }
    glUniform3fv(iLightDir_, 1, params.lightDir);
    glUniform1i(iHasIbl_, iblActive_ ? 1 : 0);
    if (iblActive_) {
      glUniform3fv(iIblColor_, 1, iblColor_);
      glUniformMatrix3fv(iEnvRotation_, 1, GL_FALSE, iblRotation_);
    }
    glUniform3fv(iLightColor_, 1, params.lightColor);
    UploadRasterLightArray(instProgram_, rasterLights_);
    const float black[3] = {0.0f, 0.0f, 0.0f};
    glUniform3fv(iEmissive_, 1, overrideEmissive ? overrideEmissive : black);
    glUniform1i(iRenderMode_, static_cast<int>(params.mode));
    glUniform1f(iDepthScale_, params.depthScale > 1e-4f ? params.depthScale : 1.0f);
    glUniform3fv(iSceneMin_, 1, params.sceneMin);
    glUniform3fv(iSceneExtent_, 1, params.sceneExtent);
    // Scene-wide bone texture (unit 4), shared with the mesh program.
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, boneTex_ ? boneTex_ : whiteTex_);
    glActiveTexture(GL_TEXTURE0);
    cullState = -1;  // reset across the program switch; dedup within this pass
    for (size_t mi = 0; mi < meshes_.size(); ++mi) {
      if (params.meshVisible && mi < static_cast<size_t>(params.meshVisibleCount) &&
          !params.meshVisible[mi]) {
        continue;
      }
      const GLMesh& mesh = meshes_[mi];
      if (mesh.instanceCount <= 0 || mesh.drawInstanceCount <= 0) continue;
      if (alphaPass == AlphaPass::Opaque && mesh.hasTranslucentInstances) continue;
      if (alphaPass == AlphaPass::Translucent && !mesh.hasTranslucentInstances) continue;
      glUniform1i(iMeshId_, static_cast<int>(mi));
      UploadRasterLightMask(instProgram_, rasterLights_, static_cast<int>(mi));
      glUniform1i(iGeometricNormal_, mesh.geometricNormal ? 1 : 0);
      glUniform1i(iDoubleSided_, mesh.doubleSided ? 1 : 0);
      glUniform1i(iPurpose_, mesh.purposeId);
      glUniform1i(iKind_, mesh.kindId);
      // Skeletal skinning of the PROTOTYPE (all its instances share one pose).
      // The bone texture is scene-wide and stays bound for the whole pass; only
      // the enable flag is per-draw. Unskinned prototypes carry zero weights, so
      // the shader would pass them through even without this, but skip the fetch.
      const bool instSkinOn = mesh.skinned && skinningFrameEnabled_;
      glUniform1i(iSkinningEnabled_, instSkinOn ? 1 : 0);
      glUniform1i(iBoneTexWidth_, boneTexWidth_ > 0 ? boneTexWidth_ : 4);
      glUniform1i(iBoneMatrixCount_, boneMatrixCount_);
      // Only touch cull state on a transition: across tens of thousands of
      // prototypes the back-face cull flag almost never changes, so the per-draw
      // glEnable/glDisable/glCullFace was pure redundant driver traffic.
      const int wantCull = (mesh.doubleSided || wireframe) ? 0 : 1;
      if (wantCull != cullState) {
        if (wantCull) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
        else glDisable(GL_CULL_FACE);
        cullState = wantCull;
      }
      glBindVertexArray(mesh.vao);
      if (!mesh.jointVbo) {  // no skin attrs: constant zero weights
        glVertexAttribI4ui(6, 0, 0, 0, 0);
        glVertexAttrib4f(7, 0.0f, 0.0f, 0.0f, 0.0f);
      }
      // Constant per-draw color when there is no per-instance color array (the
      // generic vertex-attribute value feeds aColor for every instance).
      if (!mesh.hasInstanceColors) glVertexAttrib4f(
          9, mesh.flatColor[0], mesh.flatColor[1], mesh.flatColor[2], 1.0f);
      if (!mesh.vertexColorVbo) {
        // Attribute 10 is the per-vertex color input of the instanced shader;
        // keep its generic fallback local to this VAO/draw as well.
        glVertexAttrib4f(10, 1.0f, 1.0f, 1.0f, 1.0f);
      }
      if (!mesh.hasInstanceOpacities) glVertexAttrib1f(11, mesh.flatOpacity);
      // GPU blendshape morph for a morphed prototype: bind its delta/coeff/chan
      // texture-buffers (units 8/9/10, matching the instanced program's samplers)
      // and enable the in-shader morph. Same as the non-instanced material pass.
      glUniform1i(iHasMorph_, mesh.hasMorph ? 1 : 0);
      if (mesh.hasMorph) {
        glActiveTexture(GL_TEXTURE8);
        glBindTexture(GL_TEXTURE_BUFFER, mesh.morphDeltaTex);
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_BUFFER, mesh.morphCoeffTex);
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_BUFFER, mesh.morphChanTex);
        glActiveTexture(GL_TEXTURE0);
      }
      for (const auto& sub : mesh.submeshes) {
        glDrawElementsInstanced(
            GL_TRIANGLES, static_cast<GLsizei>(sub.indexCount), GL_UNSIGNED_INT,
            (void*)(static_cast<uintptr_t>(sub.indexOffset) * sizeof(uint32_t)),
            mesh.drawInstanceCount);
      }
    }
    // LOD box proxies (optimization B): one instanced draw of the shared unit cube
    // for every distant instance the cull collapsed. Flat geometric-normal shading;
    // tint from the per-instance color (attrib 9), white per-vertex color.
    if (boxProxyCount_ > 0 && alphaPass != AlphaPass::Translucent) {
      glUniform1i(iMeshId_, -1);
      UploadRasterLightMask(instProgram_, rasterLights_, -1);
      glUniform1i(iGeometricNormal_, 1);
      glUniform1i(iDoubleSided_, 0);
      glUniform1i(iPurpose_, 0);
      glUniform1i(iKind_, 0);
      glUniform1i(iHasMorph_, 0);
      if (cullState != 1) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        cullState = 1;
      }
      glBindVertexArray(boxProxyVao_);
      glVertexAttrib3f(1, 0.0f, 1.0f, 0.0f);    // constant normal (FS uses geometric)
      glVertexAttribI2ui(8, 0u, 0u);            // no morph
      glVertexAttrib3f(10, 1.0f, 1.0f, 1.0f);   // white per-vertex color (tint=attr 9)
      glVertexAttrib1f(11, 1.0f);               // box proxies are opaque
      glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr,
                              static_cast<GLsizei>(boxProxyCount_));
    }
    glUseProgram(program_);  // restore for any caller that assumes program_
  }

  glBindVertexArray(0);
}

void GLRenderer::appendVolume(const DrawVolumeCPU& v) {
  if (v.density.empty() || v.dim[0] <= 0 || v.dim[1] <= 0 || v.dim[2] <= 0)
    return;

  GLVolume gv;
  std::memcpy(gv.world, v.world, sizeof(gv.world));
  light3d::Mat4 W;
  std::memcpy(W.m, v.world, sizeof(W.m));
  light3d::Mat4 inv = W.inverse();
  std::memcpy(gv.invWorld, inv.m, sizeof(gv.invWorld));
  for (int a = 0; a < 3; a++) {
    gv.bmin[a] = v.aabbMin[a];
    gv.bmax[a] = v.aabbMax[a];
    gv.albedo[a] = v.albedo[a];
    gv.emission[a] = v.emission[a];
  }
  gv.densityScale = v.densityScale;
  gv.background = v.background;

  glGenTextures(1, &gv.tex3d);
  glBindTexture(GL_TEXTURE_3D, gv.tex3d);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, v.dim[0], v.dim[1], v.dim[2], 0,
               GL_RED, GL_FLOAT, v.density.data());
  glBindTexture(GL_TEXTURE_3D, 0);

  auto uploadAux = [&](const std::vector<float>& field, GLuint* tex) {
    if (field.size() != v.density.size()) return;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_3D, *tex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, v.dim[0], v.dim[1], v.dim[2], 0,
                 GL_RED, GL_FLOAT, field.data());
  };
  uploadAux(v.emissionField, &gv.emissionTex3d);
  uploadAux(v.temperatureField, &gv.temperatureTex3d);
  glBindTexture(GL_TEXTURE_3D, 0);

  volumes_.push_back(gv);
}

void GLRenderer::drawNonMesh(const RenderFrameParams& params) {
  if (!nonMeshProgram_ || nonMeshBatches_.empty()) return;
  const light3d::Mat4 vp = ToMat4(params.proj) * ToMat4(params.view);
  const float right[3] = {params.view[0], params.view[4], params.view[8]};
  const float up[3] = {params.view[1], params.view[5], params.view[9]};
  glUseProgram(nonMeshProgram_);
  glUniformMatrix4fv(nmViewProj_,1,GL_FALSE,vp.m);
  glUniform3fv(nmCameraPos_,1,params.cameraPos);
  glUniform3fv(nmCameraRight_,1,right);
  glUniform3fv(nmCameraUp_,1,up);
  glUniform3fv(nmLightDir_,1,params.lightDir);
  glUniform3fv(nmLightColor_,1,params.lightColor);
  UploadRasterLightArray(nonMeshProgram_, rasterLights_);
  glUniform1f(nmExposure_,params.exposure);
  glUniform1i(nmRenderMode_,static_cast<int>(params.mode));
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  auto pass=[&](bool translucent){
    for(const GLNonMeshBatch& b:nonMeshBatches_){
      if(b.translucent!=translucent)continue;
      if((params.purposeVisibleMask&(1u<<static_cast<unsigned>(b.purposeId)))==0)continue;
      if (params.carrierVisible && b.carrierIndex >= 0 &&
          b.carrierIndex < params.carrierVisibleCount &&
          !params.carrierVisible[b.carrierIndex]) continue;
      UploadRasterLightMaskValue(nonMeshProgram_,
                                 RasterLightMaskForPath(rasterLights_, b.absPath));
      glUniform1i(nmKind_,b.kind);glUniform1i(nmMaterialId_,b.materialId);
      glUniform1i(nmCarrierId_,b.carrierId);glUniform1i(nmPurpose_,b.purposeId);
      glBindVertexArray(b.vao);
      glDrawArraysInstanced(GL_TRIANGLE_STRIP,0,4,b.count);
    }
  };
  pass(false);
  if(static_cast<int>(params.mode)==0){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);pass(true);glDepthMask(GL_TRUE);glDisable(GL_BLEND);}else pass(true);
  glBindVertexArray(0);
  glUseProgram(program_);
}

void GLRenderer::renderShadowMap(const RenderFrameParams& params) {
  shadowCamera_.lightSlot = -1;
  pointShadowCameras_.lightSlot = -1;
  if (!shadowProgram_ || !shadowFbo_) return;
  const bool point = BuildRasterPointShadowCameras(
      rasterLights_, params.sceneMin, params.sceneExtent, false, &pointShadowCameras_);
  if (!point && !BuildRasterShadowCamera(rasterLights_, params.sceneMin,
                                         params.sceneExtent, false, &shadowCamera_)) return;
  const int faceCount = point ? 6 : 1;
  for (int face = 0; face < faceCount; ++face) {
    const light3d::Mat4& shadowViewProj =
        point ? pointShadowCameras_.viewProj[static_cast<size_t>(face)]
              : shadowCamera_.viewProj;
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    if (point) {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                             pointShadowDepthTex_, 0);
    } else {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                             shadowDepthTex_, 0);
    }
    glViewport(0, 0, 2048, 2048);
    glClear(GL_DEPTH_BUFFER_BIT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glUseProgram(shadowProgram_);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, boneTex_ ? boneTex_ : whiteTex_);
    for (size_t mi = 0; mi < meshes_.size(); ++mi) {
    if (params.meshVisible && mi < static_cast<size_t>(params.meshVisibleCount) &&
        !params.meshVisible[mi]) continue;
    if (!RasterShadowIncludesMesh(rasterLights_, static_cast<int>(mi))) continue;
    const GLMesh& mesh = meshes_[mi];
    if (mesh.instanceCount > 0) continue;
    const light3d::Mat4 world = ToMat4(mesh.world);
    const light3d::Mat4 mvp = shadowViewProj * world;
    float normal[9];
    NormalMatrix3(mesh.world, normal);
    glUniformMatrix4fv(sMVP_, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(sModel_, 1, GL_FALSE, world.m);
    glUniformMatrix3fv(sNormalMat_, 1, GL_FALSE, normal);
    glUniform1i(sSkinningEnabled_, mesh.skinned && skinningFrameEnabled_);
    glUniform1i(sExtendedSkinningEnabled_,
                mesh.extendedSkinned && skinningFrameEnabled_);
    glUniform1i(sBoneTexWidth_, boneTexWidth_ > 0 ? boneTexWidth_ : 4);
    glUniform1i(sBoneMatrixCount_, skinningFrameEnabled_ ? boneMatrixCount_ : 1);
    glUniform1i(sInfluenceTexWidth_, mesh.influenceTexWidth);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, mesh.influenceTex ? mesh.influenceTex : whiteTex_);
    glUniform1i(sHasMorph_, mesh.hasMorph);
    if (mesh.hasMorph) {
      glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_BUFFER, mesh.morphDeltaTex);
      glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_BUFFER, mesh.morphCoeffTex);
      glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_BUFFER, mesh.morphChanTex);
    }
    glBindVertexArray(mesh.vao);
    for (const DrawSubmesh& sub : mesh.submeshes) {
      static const GLMaterial fallback;
      const GLMaterial& mat = sub.materialId >= 0 &&
                                      static_cast<size_t>(sub.materialId) < materials_.size()
                                  ? materials_[static_cast<size_t>(sub.materialId)]
                                  : fallback;
      const bool displaced = params.displacement &&
                             !mesh.rasterDisplacementBaked &&
                             mat.hasDisplacement();
      glUniform1i(sHasDisplacement_, displaced);
      glUniform1i(sHasDisplacementTex_, displaced && mat.displacementTex >= 0);
      glUniform1f(sDisplacementConst_, mat.displacementConst);
      glUniform1f(sDisplacementScale_, params.displacementScale);
      glUniform1f(sDisplacementTexScale_, mat.displacementTexScale);
      glUniform1f(sDisplacementTexBias_, mat.displacementTexBias);
      glActiveTexture(GL_TEXTURE7);
      GLuint disp = whiteTex_;
      if (mat.displacementTex >= 0 &&
          static_cast<size_t>(mat.displacementTex) < textures_.size() &&
          textures_[static_cast<size_t>(mat.displacementTex)].tex2d)
        disp = textures_[static_cast<size_t>(mat.displacementTex)].tex2d;
      glBindTexture(GL_TEXTURE_2D, disp);
      glUniform1i(sAlphaMode_, mat.alphaMode);
      glUniform1f(sAlpha_, mat.alpha);
      glUniform1f(sAlphaCutoff_, mat.alphaCutoff);
      SetUvUniform(sBaseAlphaUv0_, sBaseAlphaUv1_, mat.baseColorSample.uv);
      SetUvUniform(sOpacityUv0_, sOpacityUv1_, mat.opacitySample.uv);
      glUniform1i(sBaseAlphaUvSet_, mat.baseColorSample.uvSet);
      glUniform1i(sOpacityUvSet_, mat.opacitySample.uvSet);
      glUniform4fv(sBaseAlphaScale_, 1, mat.baseColorSample.scale);
      glUniform4fv(sBaseAlphaBias_, 1, mat.baseColorSample.bias);
      glUniform1i(sOpacityChannel_, mat.opacityChannel);
      glUniform1f(sOpacityScale_, mat.opacityTexScale);
      glUniform1f(sOpacityBias_, mat.opacityTexBias);
      auto shadowTexture = [&](int slot) -> const GLTexture* {
        if (slot < 0 || static_cast<size_t>(slot) >= textures_.size()) return nullptr;
        return &textures_[static_cast<size_t>(slot)];
      };
      const GLTexture* baseAlpha = shadowTexture(mat.baseColorTex);
      const GLTexture* opacity = shadowTexture(mat.opacityTex);
      const bool baseUdim = baseAlpha && baseAlpha->isUdim && baseAlpha->arrayTex;
      const bool opacityUdim = opacity && opacity->isUdim && opacity->arrayTex;
      glUniform1i(sHasBaseAlphaTex_, baseAlpha ? 1 : 0);
      glUniform1i(sHasOpacityTex_, opacity ? 1 : 0);
      glUniform1i(sBaseAlphaIsUdim_, baseUdim ? 1 : 0);
      glUniform1i(sOpacityIsUdim_, opacityUdim ? 1 : 0);
      glUniform1i(sBaseAlphaUdimSlot_, std::max(mat.baseColorTex, 0));
      glUniform1i(sOpacityUdimSlot_, std::max(mat.opacityTex, 0));
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D,
                    baseAlpha && baseAlpha->tex2d ? baseAlpha->tex2d : whiteTex_);
      glActiveTexture(GL_TEXTURE14);
      glBindTexture(GL_TEXTURE_2D,
                    opacity && opacity->tex2d ? opacity->tex2d : whiteTex_);
      if (baseUdim) {
        glActiveTexture(GL_TEXTURE11);
        glBindTexture(GL_TEXTURE_2D_ARRAY, baseAlpha->arrayTex);
      }
      if (opacityUdim) {
        glActiveTexture(GL_TEXTURE16);
        glBindTexture(GL_TEXTURE_2D_ARRAY, opacity->arrayTex);
      }
      glActiveTexture(GL_TEXTURE12);
      glBindTexture(GL_TEXTURE_2D, udimLutAtlas_);
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.indexCount),
                     GL_UNSIGNED_INT,
                     reinterpret_cast<void*>(static_cast<uintptr_t>(sub.indexOffset) *
                                             sizeof(uint32_t)));
    }
  }
    if (shadowInstProgram_) {
    glUseProgram(shadowInstProgram_);
    glUniformMatrix4fv(siViewProj_, 1, GL_FALSE, shadowViewProj.m);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, boneTex_ ? boneTex_ : whiteTex_);
    for (size_t mi = 0; mi < meshes_.size(); ++mi) {
      if (params.meshVisible && mi < static_cast<size_t>(params.meshVisibleCount) &&
          !params.meshVisible[mi]) continue;
      if (!RasterShadowIncludesMesh(rasterLights_, static_cast<int>(mi))) continue;
      const GLMesh& mesh = meshes_[mi];
      if (mesh.instanceCount <= 0 || mesh.drawInstanceCount <= 0) continue;
      glUniform1i(siSkinningEnabled_, mesh.skinned && skinningFrameEnabled_);
      glUniform1i(siBoneTexWidth_, boneTexWidth_ > 0 ? boneTexWidth_ : 4);
      glUniform1i(siBoneMatrixCount_, skinningFrameEnabled_ ? boneMatrixCount_ : 1);
      glUniform1i(siHasMorph_, mesh.hasMorph);
      if (mesh.hasMorph) {
        glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_BUFFER, mesh.morphDeltaTex);
        glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_BUFFER, mesh.morphCoeffTex);
        glActiveTexture(GL_TEXTURE10); glBindTexture(GL_TEXTURE_BUFFER, mesh.morphChanTex);
      }
      glBindVertexArray(mesh.vao);
      for (const DrawSubmesh& sub : mesh.submeshes) {
        glDrawElementsInstanced(
            GL_TRIANGLES, static_cast<GLsizei>(sub.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<void*>(static_cast<uintptr_t>(sub.indexOffset) *
                                    sizeof(uint32_t)),
            mesh.drawInstanceCount);
      }
    }
    }
    if (nonMeshProgram_ && !nonMeshBatches_.empty()) {
      const light3d::Vec3& eye = point ? pointShadowCameras_.eye
                                       : shadowCamera_.eye;
      const light3d::Vec3& right = point
          ? pointShadowCameras_.right[static_cast<size_t>(face)]
          : shadowCamera_.right;
      const light3d::Vec3& up = point
          ? pointShadowCameras_.up[static_cast<size_t>(face)]
          : shadowCamera_.up;
      const float eyeData[3] = {eye.x, eye.y, eye.z};
      const float rightData[3] = {right.x, right.y, right.z};
      const float upData[3] = {up.x, up.y, up.z};
      glUseProgram(nonMeshProgram_);
      glUniformMatrix4fv(nmViewProj_, 1, GL_FALSE, shadowViewProj.m);
      glUniform3fv(nmCameraPos_, 1, eyeData);
      glUniform3fv(nmCameraRight_, 1, rightData);
      glUniform3fv(nmCameraUp_, 1, upData);
      glUniform1i(nmRenderMode_, 1);
      glDisable(GL_CULL_FACE);
      for (const GLNonMeshBatch& batch : nonMeshBatches_) {
        if ((params.purposeVisibleMask &
             (1u << static_cast<unsigned>(batch.purposeId))) == 0)
          continue;
        if (params.carrierVisible && batch.carrierIndex >= 0 &&
            batch.carrierIndex < params.carrierVisibleCount &&
            !params.carrierVisible[batch.carrierIndex])
          continue;
        if (!RasterShadowIncludesPath(rasterLights_, batch.absPath)) continue;
        glUniform1i(nmKind_, batch.kind);
        glBindVertexArray(batch.vao);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, batch.count);
      }
      glEnable(GL_CULL_FACE);
    }
  }
  glBindVertexArray(0);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glCullFace(GL_BACK);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
}

void GLRenderer::applyRasterDof(const RenderFrameParams& params) {
  if (params.mode != RenderMode::Shaded || !params.cameraLens.enabled() ||
      !colorTex_ || !depthTex_ || !dofFbo_) {
    return;
  }
  if (!dofProgram_) {
    static const char* kVs = R"glsl(#version 330 core
out vec2 vUv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";
    static const char* kFs = R"glsl(#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uColor;
uniform sampler2D uDepth;
uniform mat4 uInvProj;
uniform float uProjY;
uniform float uFocusDistance;
uniform float uApertureRadius;
uniform vec2 uExtent;

float viewDepth(vec2 uv) {
  float d = texture(uDepth, uv).r;
  if (d >= 0.999999) return 1.0e30;
  vec4 p = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
  return abs(p.z / max(abs(p.w), 1.0e-8));
}
float cocPixels(float z) {
  if (z > 1.0e20) return 0.0;
  float angular = uApertureRadius * abs(1.0 / max(z, 1.0e-5) -
                                             1.0 / uFocusDistance);
  return clamp(0.5 * uExtent.y * abs(uProjY) * angular, 0.0, 14.0);
}
vec3 toLinear(vec3 c) {
  bvec3 lo = lessThanEqual(c, vec3(0.04045));
  return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, lo);
}
vec3 toSrgb(vec3 c) {
  bvec3 lo = lessThanEqual(c, vec3(0.0031308));
  return mix(1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
             c * 12.92, lo);
}
void main() {
  const vec2 disk[16] = vec2[16](
    vec2(0.000, 0.000), vec2(0.527, 0.085), vec2(-0.368, 0.284),
    vec2(0.145, -0.547), vec2(-0.705, -0.184), vec2(0.724, -0.438),
    vec2(-0.192, 0.765), vec2(0.423, 0.681), vec2(-0.817, 0.409),
    vec2(0.891, 0.214), vec2(-0.521, -0.691), vec2(0.218, 0.934),
    vec2(-0.941, -0.307), vec2(0.649, -0.724), vec2(-0.070, -0.947),
    vec2(0.982, -0.121));
  float radius = cocPixels(viewDepth(vUv));
  if (radius < 0.5) {
    fragColor = texture(uColor, vUv);
    return;
  }
  vec3 sum = vec3(0.0);
  float alpha = 0.0;
  float weight = 0.0;
  for (int i = 0; i < 16; ++i) {
    vec2 uv = clamp(vUv + disk[i] * radius / uExtent,
                    vec2(0.0), vec2(1.0));
    vec4 sampleColor = texture(uColor, uv);
    float sampleCoc = cocPixels(viewDepth(uv));
    float support = smoothstep(length(disk[i]) * radius - 1.0,
                               length(disk[i]) * radius + 1.0,
                               max(radius, sampleCoc));
    sum += toLinear(sampleColor.rgb) * support;
    alpha += sampleColor.a * support;
    weight += support;
  }
  fragColor = vec4(toSrgb(sum / max(weight, 1.0e-5)),
                   alpha / max(weight, 1.0e-5));
}
)glsl";
    std::string error;
    dofProgram_ = glutil::CompileProgram(kVs, kFs, &error);
    if (!dofProgram_) {
      std::fprintf(stderr,
                   "[lusdview][warn] OpenGL raster depth of field disabled: %s\n",
                   error.c_str());
      return;
    }
    glGenVertexArrays(1, &dofVao_);
    glUseProgram(dofProgram_);
    glUniform1i(glGetUniformLocation(dofProgram_, "uColor"), 0);
    glUniform1i(glGetUniformLocation(dofProgram_, "uDepth"), 1);
  }

  const light3d::Mat4 projection = ToMat4(params.proj);
  const light3d::Mat4 inverseProjection = projection.inverse();
  glBindFramebuffer(GL_FRAMEBUFFER, dofFbo_);
  glViewport(0, 0, vpW_, vpH_);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glUseProgram(dofProgram_);
  glUniformMatrix4fv(glGetUniformLocation(dofProgram_, "uInvProj"), 1,
                     GL_FALSE, inverseProjection.m);
  glUniform1f(glGetUniformLocation(dofProgram_, "uProjY"), projection.m[5]);
  glUniform1f(glGetUniformLocation(dofProgram_, "uFocusDistance"),
              params.cameraLens.focusDistance);
  glUniform1f(glGetUniformLocation(dofProgram_, "uApertureRadius"),
              params.cameraLens.apertureRadius);
  glUniform2f(glGetUniformLocation(dofProgram_, "uExtent"),
              static_cast<float>(vpW_), static_cast<float>(vpH_));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, colorTex_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, depthTex_);
  glBindVertexArray(dofVao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  dofApplied_ = true;
}

void GLRenderer::renderFrame(const RenderFrameParams& params) {
  if (!fbo_ || !program_ || !params.view || !params.proj) return;
  dofApplied_ = false;
  renderShadowMap(params);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, vpW_, vpH_);
  glClearColor(params.clearColor[0], params.clearColor[1], params.clearColor[2],
               params.clearColor[3]);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);

  glUseProgram(program_);
  glUniform3fv(uCameraPos_, 1, params.cameraPos);
  glUniform1f(uExposure_, params.exposure);
  glUniform3fv(uLightDir_, 1, params.lightDir);
  glUniform3fv(uLightColor_, 1, params.lightColor);
  UploadRasterLightArray(program_, rasterLights_);
  const bool hasShadow = shadowCamera_.lightSlot >= 0 && shadowDepthTex_ != 0;
  const bool hasPointShadow = pointShadowCameras_.lightSlot >= 0 &&
                              pointShadowDepthTex_ != 0;
  const int shadowLightSlot = hasPointShadow ? pointShadowCameras_.lightSlot
                                              : shadowCamera_.lightSlot;
  glUniform1i(uHasShadowMap_, hasShadow ? 1 : 0);
  glUniform1i(uShadowLightSlot_, shadowLightSlot);
  glUniformMatrix4fv(uShadowViewProj_, 1, GL_FALSE, shadowCamera_.viewProj.m);
  glUniform1i(uHasPointShadowMap_, hasPointShadow ? 1 : 0);
  if (hasPointShadow) {
    const RasterLightGPU& point = rasterLights_.lights[
        static_cast<size_t>(pointShadowCameras_.lightSlot)];
    glUniform3fv(uPointShadowLightPos_, 1, point.positionType);
    glUniformMatrix4fv(uPointShadowViewProj_, 6, GL_FALSE,
                       pointShadowCameras_.viewProj[0].m);
    glActiveTexture(GL_TEXTURE31);
    glBindTexture(GL_TEXTURE_CUBE_MAP, pointShadowDepthTex_);
  }
  if (hasShadow) {
    glActiveTexture(GL_TEXTURE25);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTex_);
    glActiveTexture(GL_TEXTURE0);
  }
  glUniform1i(uHasIbl_, iblActive_ ? 1 : 0);
  if (iblActive_) {
    glUniform3fv(uIblColor_, 1, iblColor_);
    glUniformMatrix3fv(uEnvRotation_, 1, GL_FALSE, iblRotation_);
    glUniform1i(uPrefilteredLods_, iblSpecLods_);
    glActiveTexture(GL_TEXTURE19);
    glBindTexture(GL_TEXTURE_CUBE_MAP, iblIrrTex_);
    glActiveTexture(GL_TEXTURE20);
    glBindTexture(GL_TEXTURE_CUBE_MAP, iblSpecTex_);
    glActiveTexture(GL_TEXTURE21);
    glBindTexture(GL_TEXTURE_2D, iblLutTex_);
    glActiveTexture(GL_TEXTURE0);
  }

  // Resolve the wireframe state: explicit params.wireMode wins; the legacy
  // RenderMode::Wireframe maps to "wireframe only".
  int wireMode = params.wireMode;
  if (wireMode == 0 && params.mode == RenderMode::Wireframe) wireMode = 1;
  const bool haveWire = (wireProgram_ != 0 || wireInstProgram_ != 0);
  const bool wire = (wireMode != 0);  // disables back-face cull in fill passes
  // Wireframe is a diagnostic overlay, independent of the preview light and
  // material colors. Keep it legible against both the light and dark themes.
  const float wireCol[3] = {0.04f, 0.24f, 0.10f};  // dark green

  if (wireMode == 1 && haveWire) {
    // Wireframe only (hidden-line removed): render the fill into DEPTH ONLY so
    // occluded edges are hidden, then draw the polygon edges over the background.
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    drawMeshes(params, /*wireframe=*/false, nullptr);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    drawWireframe(params, wireCol);
  } else {
    // Shaded fill. In the lit mode (RenderMode 0), draw opaque geometry first,
    // then translucent (Blend) materials in a separate back-to-front, blended,
    // depth-write-off pass so transparency composites over what's behind it.
    // AOV / debug modes draw everything in one unblended pass (blending would
    // corrupt the encoded AOV values).
    const bool shaded = (static_cast<int>(params.mode) == 0);
    if (shaded) {
      drawMeshes(params, /*wireframe=*/false, nullptr, AlphaPass::Opaque);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      // Transparent meshes are sorted back-to-front below. Keeping depth
      // testing enabled here rejects transparent layers against one another
      // and loses valid middle layers; the opaque pass has already populated
      // the depth buffer.
      glDisable(GL_DEPTH_TEST);
      glDepthMask(GL_FALSE);
      drawMeshes(params, /*wireframe=*/false, nullptr, AlphaPass::Translucent);
      glDepthMask(GL_TRUE);
      glEnable(GL_DEPTH_TEST);
      glDisable(GL_BLEND);
    } else {
      drawMeshes(params, /*wireframe=*/false, nullptr, AlphaPass::All);
    }
    if (wireMode == 2 && haveWire) drawWireframe(params, wireCol);
  }
  if (wireMode != 1) drawNonMesh(params);

  // Highlight overlay (wireframe, emissive orange) on the selected mesh.
  if (params.highlightMeshIndex >= 0 &&
      static_cast<size_t>(params.highlightMeshIndex) < meshes_.size() && !wire) {
    // drawNonMesh() leaves its own shader bound. Uniform locations below belong
    // to the material mesh program; without rebinding it the outline was drawn
    // by the helper shader from rest positions and all skin uniforms were
    // ignored. This is especially visible on the Elephant pose.
    glUseProgram(program_);
    if (params.highlightXray) glDisable(GL_DEPTH_TEST);
    else glEnable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glEnable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(-1.0f, -1.0f);
    const float orange[3] = {1.0f, 0.55f, 0.1f};
    const GLMesh& mesh = meshes_[static_cast<size_t>(params.highlightMeshIndex)];
    light3d::Mat4 P = ToMat4(params.proj), V = ToMat4(params.view), W = ToMat4(mesh.world);
    light3d::Mat4 MVP = P * V * W;
    glUniformMatrix4fv(uMVP_, 1, GL_FALSE, MVP.m);
    glUniformMatrix4fv(uModel_, 1, GL_FALSE, W.m);
    glUniform1i(uSkinningEnabled_, (mesh.skinned && skinningFrameEnabled_) ? 1 : 0);
    glUniform1i(uExtendedSkinningEnabled_,
                (mesh.extendedSkinned && skinningFrameEnabled_) ? 1 : 0);
    glUniform1i(uBoneTexWidth_, boneTexWidth_ > 0 ? boneTexWidth_ : 4);
    glUniform1i(uBoneMatrixCount_, skinningFrameEnabled_ ? boneMatrixCount_ : 1);
    glUniform1i(uInfluenceTexWidth_,
                (mesh.extendedSkinned && skinningFrameEnabled_) ? mesh.influenceTexWidth : 0);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, boneTex_ ? boneTex_ : whiteTex_);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, mesh.influenceTex ? mesh.influenceTex : whiteTex_);
    glUniform1i(uHasMorph_, mesh.hasMorph ? 1 : 0);
    if (mesh.hasMorph) {
      glActiveTexture(GL_TEXTURE8);
      glBindTexture(GL_TEXTURE_BUFFER, mesh.morphDeltaTex);
      glActiveTexture(GL_TEXTURE9);
      glBindTexture(GL_TEXTURE_BUFFER, mesh.morphCoeffTex);
      glActiveTexture(GL_TEXTURE10);
      glBindTexture(GL_TEXTURE_BUFFER, mesh.morphChanTex);
      glActiveTexture(GL_TEXTURE0);
    }
    glUniform3f(uBaseColor_, 0, 0, 0);
    glUniform3fv(uEmissive_, 1, orange);
    glUniform1f(uAlpha_, 1.f);
    glUniform1i(uAlphaMode_, 0);
    glUniform1f(uAlphaCutoff_, 0.5f);
    glUniform1i(uHasBaseColorTex_, 0);
    glUniform1i(uHasMetallicTex_, 0);
    glUniform1i(uHasRoughnessTex_, 0);
    glUniform1i(uHasNormalTex_, 0);
    glUniform1i(uHasEmissiveTex_, 0);
    glUniform1i(uHasOpacityTex_, 0);
    glUniform1i(uHasOcclusionTex_, 0);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(mesh.vao);
    if (params.highlightIndices && params.highlightIndexCount > 0) {
      // Highlight only a selected GeomSubset's triangles: draw a dynamic index
      // buffer over the mesh's vertices. Restore the VAO's element buffer after.
      if (!highlightEbo_) glGenBuffers(1, &highlightEbo_);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, highlightEbo_);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   static_cast<GLsizeiptr>(params.highlightIndexCount * sizeof(uint32_t)),
                   params.highlightIndices, GL_STREAM_DRAW);
      glDrawElements(GL_TRIANGLES, params.highlightIndexCount, GL_UNSIGNED_INT, nullptr);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);  // restore VAO state
    } else {
      for (const auto& sub : mesh.submeshes) {
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(sub.indexCount), GL_UNSIGNED_INT,
                       (void*)(static_cast<uintptr_t>(sub.indexOffset) * sizeof(uint32_t)));
      }
    }
    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_LINE);
    glEnable(GL_DEPTH_TEST);
  }

  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

  // UsdVol volume raymarch pass (proxy-box; emission/absorption). Drawn after
  // opaque geometry with premultiplied-alpha "over" blending. Back faces only
  // (cull front) so each covered pixel marches once and the camera may be inside
  // the box. Depth-tested (no depth write) so opaque geometry in front occludes.
  if (volumeProgram_ && !volumes_.empty()) {
    const light3d::Mat4 VP = ToMat4(params.proj) * ToMat4(params.view);
    glUseProgram(volumeProgram_);
    glUniformMatrix4fv(uVolVP_, 1, GL_FALSE, VP.m);
    glUniform3fv(uVolCameraPos_, 1, params.cameraPos);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uVolDensity_, 0);
    glUniform1i(uVolEmissionField_, 1);
    glUniform1i(uVolTemperatureField_, 2);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied "over"
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    glBindVertexArray(volumeCubeVao_);
    for (const GLVolume& gv : volumes_) {
      glUniformMatrix4fv(uVolModel_, 1, GL_FALSE, gv.world);
      glUniformMatrix4fv(uVolInvModel_, 1, GL_FALSE, gv.invWorld);
      glUniform3fv(uVolBmin_, 1, gv.bmin);
      glUniform3fv(uVolBmax_, 1, gv.bmax);
      glUniform1f(uVolDensityScale_, gv.densityScale);
      glUniform3fv(uVolAlbedo_, 1, gv.albedo);
      glUniform3fv(uVolEmission_, 1, gv.emission);
      glUniform1f(uVolBackground_, gv.background);
      glUniform1i(uVolHasEmissionField_, gv.emissionTex3d != 0);
      glUniform1i(uVolHasTemperatureField_, gv.temperatureTex3d != 0);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_3D, gv.emissionTex3d);
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_3D, gv.temperatureTex3d);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_3D, gv.tex3d);
      glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_3D, 0);

    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
  }

  // Debug helper lines (grid/axes/bbox), world space, depth-tested so they are
  // occluded by geometry.
  if (params.helperLines && params.helperLineVertexCount > 0 && lineProgram_) {
    glUseProgram(lineProgram_);
    const light3d::Mat4 VP = ToMat4(params.proj) * ToMat4(params.view);
    glUniformMatrix4fv(uLineVP_, 1, GL_FALSE, VP.m);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    const size_t bytes =
        static_cast<size_t>(params.helperLineVertexCount) * sizeof(HelperVertex);
    if (bytes > lineVboCap_) {
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), params.helperLines,
                   GL_DYNAMIC_DRAW);
      lineVboCap_ = bytes;
    } else {
      glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes),
                      params.helperLines);
    }
    glDisable(GL_CULL_FACE);
    glDrawArrays(GL_LINES, 0, params.helperLineVertexCount);
    glBindVertexArray(0);
  }

  // Mesh selections already used the GPU-deformed polygon overlay above. The
  // same params also carry CPU-built world-space lines for Vulkan (which lacks
  // that overlay); drawing them here duplicates the highlight in rest pose for
  // skinned/morphed meshes. Keep this line path only for native Points/Curves,
  // represented by highlightMeshIndex < 0.
  if (params.highlightMeshIndex < 0 && params.highlightLines &&
      params.highlightLineVertexCount > 0 && lineProgram_) {
    glUseProgram(lineProgram_);
    const light3d::Mat4 VP = ToMat4(params.proj) * ToMat4(params.view);
    glUniformMatrix4fv(uLineVP_, 1, GL_FALSE, VP.m);
    glBindVertexArray(lineVao_); glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    const size_t bytes = static_cast<size_t>(params.highlightLineVertexCount) * sizeof(HelperVertex);
    if (bytes > lineVboCap_) {
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), params.highlightLines, GL_DYNAMIC_DRAW);
      lineVboCap_ = bytes;
    } else {
      glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), params.highlightLines);
    }
    glDisable(GL_CULL_FACE);
    if (params.highlightXray) glDisable(GL_DEPTH_TEST);
    else glEnable(GL_DEPTH_TEST);
    glDrawArrays(GL_LINES, 0, params.highlightLineVertexCount);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
  }

  // Overlay lines (skeleton bones): drawn on top with depth testing disabled so
  // they remain visible through the mesh (X-ray). Reuses the line program/VBO.
  if (params.overlayLines && params.overlayLineVertexCount > 0 && lineProgram_) {
    glUseProgram(lineProgram_);
    const light3d::Mat4 VP = ToMat4(params.proj) * ToMat4(params.view);
    glUniformMatrix4fv(uLineVP_, 1, GL_FALSE, VP.m);
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    const size_t bytes =
        static_cast<size_t>(params.overlayLineVertexCount) * sizeof(HelperVertex);
    if (bytes > lineVboCap_) {
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes), params.overlayLines,
                   GL_DYNAMIC_DRAW);
      lineVboCap_ = bytes;
    } else {
      glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes),
                      params.overlayLines);
    }
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_LINES, 0, params.overlayLineVertexCount);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
  }

  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  applyRasterDof(params);
}

ViewportTexHandle GLRenderer::viewportTexture() const {
  return static_cast<ViewportTexHandle>(dofApplied_ ? dofTex_ : colorTex_);
}

bool GLRenderer::uploadViewportImage(const uint8_t* rgba, int w, int h) {
  // Composite an externally-traced RGBA8 image (CUDA/HIP/CPU RT) into the
  // offscreen viewport target for this frame, mirroring
  // VulkanRenderer::uploadViewportImage. Called instead of drawMeshes()/the
  // raster 3D pass -- the per-frame dispatch in App::run() is either/or, so
  // there is no concurrent write to colorTex_ from renderFrame() to guard
  // against.
  if (!rgba || w < 1 || h < 1) return false;
  ensureFbo(w, h);
  dofApplied_ = false;
  // colorTex_ is bottom-up (GL framebuffer convention; see captureViewport's
  // flip and caps_.flipViewportV=true, which tells gui.cc to flip the display
  // UVs to compensate). `rgba` is top-down, so flip rows going in rather than
  // leaving the mismatch to the display-time flip, which would otherwise show
  // this image upside down relative to the normal raster content.
  const size_t rowBytes = static_cast<size_t>(w) * 4;
  flippedUploadScratch_.resize(rowBytes * static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    std::memcpy(&flippedUploadScratch_[static_cast<size_t>(y) * rowBytes],
                rgba + static_cast<size_t>(h - 1 - y) * rowBytes, rowBytes);
  }
  glBindTexture(GL_TEXTURE_2D, colorTex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                  flippedUploadScratch_.data());
  glBindTexture(GL_TEXTURE_2D, 0);
  return true;
}

#if defined(LUSDVIEW_ENABLE_GL_THREAD)
// Shared composite: draw `drawData` to the default framebuffer + swap. Used by
// present() (live, main thread) and presentThreaded() (render thread).
void GLRenderer::presentImpl(ImDrawData* drawData, int fbw, int fbh) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, fbw, fbh);
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(drawData);
  if (wantWindowCapture_) {
    winCapW_ = fbw;
    winCapH_ = fbh;
    windowCapture_.resize(static_cast<size_t>(fbw) * static_cast<size_t>(fbh) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, windowCapture_.data());
    wantWindowCapture_ = false;
  }
  glfwSwapBuffers(window_);
}

void GLRenderer::presentThreaded(ImDrawData* drawData, int fbW, int fbH) {
  presentImpl(drawData, fbW, fbH);  // fb size queried on the main thread
}

void GLRenderer::present() {
  int fbw = 0, fbh = 0;
  glfwGetFramebufferSize(window_, &fbw, &fbh);
  presentImpl(ImGui::GetDrawData(), fbw, fbh);
}
#else
void GLRenderer::present() {
  int fbw = 0, fbh = 0;
  glfwGetFramebufferSize(window_, &fbw, &fbh);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, fbw, fbh);
  glDisable(GL_DEPTH_TEST);
  glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

  // Grab the composited window from the back buffer before swapping (reliable
  // under headless/Xvfb, unlike reading GL_FRONT after the swap).
  if (wantWindowCapture_) {
    winCapW_ = fbw;
    winCapH_ = fbh;
    windowCapture_.resize(static_cast<size_t>(fbw) * static_cast<size_t>(fbh) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, windowCapture_.data());
    wantWindowCapture_ = false;
  }

  glfwSwapBuffers(window_);
}
#endif

bool GLRenderer::gpuMemoryMB(size_t* usedMB, size_t* totalMB) const {
  // GL_NVX_gpu_memory_info (NVIDIA, and exposed by Mesa for AMD/Intel): total
  // dedicated VRAM + currently-available; used = total - available.
  constexpr GLenum kDedicated = 0x9047;  // GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
  constexpr GLenum kAvailable = 0x9049;  // GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
  GLint totalKB = 0, availKB = 0;
  while (glGetError() != GL_NO_ERROR) {}  // clear prior errors
  glGetIntegerv(kDedicated, &totalKB);
  glGetIntegerv(kAvailable, &availKB);
  if (glGetError() != GL_NO_ERROR || totalKB <= 0) return false;
  if (totalMB) *totalMB = static_cast<size_t>(totalKB) / 1024u;
  if (usedMB) {
    const GLint usedKB = totalKB > availKB ? (totalKB - availKB) : 0;
    *usedMB = static_cast<size_t>(usedKB) / 1024u;
  }
  return true;
}

bool GLRenderer::captureViewport(std::vector<uint8_t>* rgba, int* w, int* h) {
  if (!fbo_ || vpW_ < 1 || vpH_ < 1) return false;
  *w = vpW_;
  *h = vpH_;
  const size_t rowBytes = static_cast<size_t>(vpW_) * 4;
  std::vector<uint8_t> tmp(rowBytes * static_cast<size_t>(vpH_));
  glBindFramebuffer(GL_FRAMEBUFFER, dofApplied_ ? dofFbo_ : fbo_);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, vpW_, vpH_, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  // GL is bottom-up; return top-down rows (consistent with the Vulkan backend).
  rgba->resize(tmp.size());
  for (int y = 0; y < vpH_; ++y) {
    std::memcpy(&(*rgba)[static_cast<size_t>(y) * rowBytes],
                &tmp[static_cast<size_t>(vpH_ - 1 - y) * rowBytes], rowBytes);
  }
  return true;
}

bool GLRenderer::captureWindow(std::vector<uint8_t>* rgba, int* w, int* h) {
  if (windowCapture_.empty() || winCapW_ < 1 || winCapH_ < 1) return false;
  *w = winCapW_;
  *h = winCapH_;
  const size_t rowBytes = static_cast<size_t>(winCapW_) * 4;
  rgba->resize(windowCapture_.size());
  // Stored bottom-up; emit top-down.
  for (int y = 0; y < winCapH_; ++y) {
    std::memcpy(&(*rgba)[static_cast<size_t>(y) * rowBytes],
                &windowCapture_[static_cast<size_t>(winCapH_ - 1 - y) * rowBytes],
                rowBytes);
  }
  return true;
}

void GLRenderer::shutdown() {
  destroyScene();
  if (whiteTex_) { glDeleteTextures(1, &whiteTex_); whiteTex_ = 0; }
  if (boneTex_) { glDeleteTextures(1, &boneTex_); boneTex_ = 0; }
  if (colorTex_) { glDeleteTextures(1, &colorTex_); colorTex_ = 0; }
  if (depthTex_) { glDeleteTextures(1, &depthTex_); depthTex_ = 0; }
  if (dofTex_) { glDeleteTextures(1, &dofTex_); dofTex_ = 0; }
  if (dofFbo_) { glDeleteFramebuffers(1, &dofFbo_); dofFbo_ = 0; }
  if (dofProgram_) { glDeleteProgram(dofProgram_); dofProgram_ = 0; }
  if (dofVao_) { glDeleteVertexArrays(1, &dofVao_); dofVao_ = 0; }
  if (fbo_) { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
  if (program_) { glDeleteProgram(program_); program_ = 0; }
  if (shadowProgram_) { glDeleteProgram(shadowProgram_); shadowProgram_ = 0; }
  if (shadowInstProgram_) { glDeleteProgram(shadowInstProgram_); shadowInstProgram_ = 0; }
  if (shadowDepthTex_) { glDeleteTextures(1, &shadowDepthTex_); shadowDepthTex_ = 0; }
  if (pointShadowDepthTex_) { glDeleteTextures(1, &pointShadowDepthTex_); pointShadowDepthTex_ = 0; }
  if (shadowFbo_) { glDeleteFramebuffers(1, &shadowFbo_); shadowFbo_ = 0; }
  if (tessProgram_) { glDeleteProgram(tessProgram_); tessProgram_ = 0; }
  if (instProgram_) { glDeleteProgram(instProgram_); instProgram_ = 0; }
  if (wireProgram_) { glDeleteProgram(wireProgram_); wireProgram_ = 0; }
  if (wireInstProgram_) { glDeleteProgram(wireInstProgram_); wireInstProgram_ = 0; }
  if (lineProgram_) { glDeleteProgram(lineProgram_); lineProgram_ = 0; }
  if (nonMeshProgram_) { glDeleteProgram(nonMeshProgram_); nonMeshProgram_ = 0; }
  if (lineVbo_) { glDeleteBuffers(1, &lineVbo_); lineVbo_ = 0; }
  if (lineVao_) { glDeleteVertexArrays(1, &lineVao_); lineVao_ = 0; }
  for (GLVolume& gv : volumes_) {
    if (gv.tex3d) glDeleteTextures(1, &gv.tex3d);
    if (gv.emissionTex3d) glDeleteTextures(1, &gv.emissionTex3d);
    if (gv.temperatureTex3d) glDeleteTextures(1, &gv.temperatureTex3d);
  }
  volumes_.clear();
  if (volumeProgram_) { glDeleteProgram(volumeProgram_); volumeProgram_ = 0; }
  if (volumeCubeVbo_) { glDeleteBuffers(1, &volumeCubeVbo_); volumeCubeVbo_ = 0; }
  if (volumeCubeEbo_) { glDeleteBuffers(1, &volumeCubeEbo_); volumeCubeEbo_ = 0; }
  if (volumeCubeVao_) { glDeleteVertexArrays(1, &volumeCubeVao_); volumeCubeVao_ = 0; }
  if (highlightEbo_) { glDeleteBuffers(1, &highlightEbo_); highlightEbo_ = 0; }
  if (imguiInited_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    imguiInited_ = false;
  }
}

std::unique_ptr<Renderer> CreateGLRenderer() {
  return std::unique_ptr<Renderer>(new GLRenderer());
}

}  // namespace lusdview
