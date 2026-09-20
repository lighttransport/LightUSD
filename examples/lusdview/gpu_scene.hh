// SPDX-License-Identifier: Apache-2.0
// lusdview - backend-neutral GPU scene representation.
//
// `DrawScene` is the hand-off between the USD/Tydra side (mesh_build.cc) and the
// graphics backends (gl_renderer / vk_renderer). It contains nothing
// backend-specific: interleaved vertices, a triangulated index buffer grouped
// into per-material submeshes, materials (mapped from UsdPreviewSurface), and
// decoded RGBA8 textures. Both the OpenGL and Vulkan backends consume this
// identical structure.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "light3d/texture.h"  // light3d::Image (CPU texel container)
#include "tydra/openpbr-params.hh"

namespace lusdview {

namespace tydra = lightusd::tydra;
using DrawLightRtOpenPBRCPU = tydra::LightRtOpenPBRParams;

// Halton(2,3) sub-pixel jitter offset in [-0.5, 0.5) for supersampled ray-traced
// screenshots (the CUDA/HIP trace path). Sample 0 is the pixel center (0,0), so
// spp==1 reproduces the un-jittered image exactly. Shared by both GPU tracers.
inline void RtPixelJitter(int s, int spp, float* jx, float* jy) {
  if (spp <= 1 || s == 0) { *jx = 0.0f; *jy = 0.0f; return; }
  auto radical = [](int i, int base) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= float(base); r += f * float(i % base); i /= base; }
    return r;
  };
  *jx = radical(s, 2) - 0.5f;
  *jy = radical(s, 3) - 0.5f;
}


// Interleaved vertex: matches the GL330 / VK450 shader attribute layout
//   location 0: vec3 aPosition  (offset 0)
//   location 1: vec3 aNormal    (offset 12)
//   location 2: vec2 aUV        (offset 24)  -> shader declares vec3 aUV, reads .xy
struct DrawVertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
};

// Generic MaterialX geomprop stream. Values are expanded to DrawVertex order
// by the scene loader, so interpolation and authored index indirection are
// resolved once on the host before a backend consumes the mesh.
struct DrawGeomPropCPU {
  std::string name;
  // Scalar/vector streams use 1..4; matrix33/matrix44 streams use 9/16 and
  // are exposed to graph matrix operators one column at a time.
  uint32_t components{0};
  std::vector<float> values;  // components floats per DrawVertex
};

// One draw call: a contiguous run of indices sharing a single material.
struct DrawSubmesh {
  uint32_t indexOffset{0};  // offset into DrawMeshCPU::indices (in indices)
  uint32_t indexCount{0};
  int materialId{-1};  // index into DrawScene::materials (-1 = default material)
  // Optional material for triangles viewed from behind. -1 means use
  // materialId, so ordinary scenes pay only one extra integer per draw range
  // and every backend has the same explicit fallback rule.
  int backfaceMaterialId{-1};
};

// Generated index-only raster LOD for an instanced prototype. All levels reuse
// DrawMeshCPU::vertices, so materials, UVs, normals and picking identity remain
// identical to the authored mesh while the submitted triangle count falls.
struct DrawPrototypeLodCPU {
  std::vector<uint32_t> indices;
  std::vector<DrawSubmesh> submeshes;
  float objectError{0.0f};  // meshoptimizer normalized geometric/attribute error
  float triangleRatio{1.0f};
  uint32_t level{1};  // requested logical level (1..3); LOD0 is authored
};

// One blendshape target in DrawVertex order (sparse), for per-frame GPU morph.
// `vtx[i]` is the affected DrawVertex; `dpos` holds its position offset (3
// floats, parallel to `vtx`). Normals are regenerated from the morphed
// positions (RegenNormalsOriented), so authored normal offsets are not stored.
// One in-between shape sample of a blendshape: its position-offset deltas at a
// weight in (0,1). Parallel to MorphTargetCPU::vtx (same affected DrawVertices).
struct MorphInbetweenCPU {
  float weight{0.5f};        // USD inbetween weight (0 < w < 1, ascending order)
  std::vector<float> dpos;   // 3 floats per MorphTargetCPU::vtx entry
};

struct MorphTargetCPU {
  std::string name;  // BlendShape prim name == SkelAnimation weight key
  std::vector<uint32_t> vtx;
  std::vector<float> dpos;   // primary (weight == 1.0) offsets, 3 per vtx entry
  // In-between shapes, sorted ascending by weight. Empty = simple linear morph
  // (rest at w=0 -> primary at w=1). With inbetweens the morph piecewise-lerps
  // through (0, rest), each (weight, sample), and (1, primary).
  std::vector<MorphInbetweenCPU> inbetweens;
};

// GPU-morph "channels" for one blendshape target: the primary and each in-between
// sample become a channel with a mesh-local id and its own sparse per-vertex delta
// (in DrawMeshCPU::morphDeltaHalf). Per frame the CPU computes one coefficient
// per channel (reproducing the piecewise-lerp), and the vertex shader sums
// coeff[channel] * delta. `usdWeights`/`channelIds` are parallel and ascending:
// the in-between weights then 1.0 (primary). The implicit rest sample (weight 0)
// has no channel.
struct MorphTargetChannelsCPU {
  std::string name;               // BlendShape name == weight key
  std::vector<float> usdWeights;  // ascending: inbetween weights..., then 1.0
  std::vector<int> channelIds;    // mesh-local channel id per usdWeights entry
};

struct DrawMeshCPU {
  std::string name;
  std::string absPath;
  std::string purpose{"default"};  // USD purpose token: default/render/proxy/guide
  // A proxy with no render-purpose alternative in its model is the only usable
  // representation and is therefore visible in the default viewer profile.
  // Keep this separate from `purpose`: callers and the Purpose UI still see
  // the authored proxy token.
  bool proxyFallback{false};

  std::vector<DrawVertex> vertices;  // rest pose (GPU morph re-derives from this)
  // Upload identities survive release of the large CPU arrays. They let a
  // post-composition transaction prove topology compatibility and avoid
  // touching renderer slots whose vertex stream is unchanged.
  uint64_t uploadedVertexFingerprint{0};
  uint64_t uploadedTopologyFingerprint{0};
  size_t uploadedVertexCount{0};
  // Optional per-vertex displayColor (rgb, parallel to `vertices`); empty = none.
  // Used by the flat --next preview to tint geometry; the material shader
  // multiplies baseColor by it (default white when absent).
  std::vector<float> vertexColors;
  // Optional per-vertex alpha (parallel to `vertices`); empty = fully opaque.
  // Carries USD `primvars:displayOpacity` when it is authored per-point or
  // per-face-vertex; constant/uniform opacity folds into DrawMaterialCPU::alpha
  // instead and leaves this empty.
  std::vector<float> vertexAlpha;
  // Optional tangent-space basis, parallel to `vertices`; each entry is xyz.
  // Populated from USD primvars or Tydra-computed tangents/binormals. Current
  // renderers ignore these until full normal-map/anisotropy evaluation lands.
  std::vector<float> tangents;
  std::vector<float> binormals;
  // Float/vector primvars retained from the next loader for MaterialX
  // geompropvalue nodes. Empty until a generic primvar is authored.
  std::vector<DrawGeomPropCPU> geomProps;
  // True when the mesh has no authored normals: the shader shades it with the
  // geometric (screen-derivative) normal instead of the per-vertex normal, so
  // hard-surface geometry isn't smeared by averaged smooth normals.
  bool geometricNormal{false};
  // Winding sign used when regenerating smooth normals from triangle topology.
  // Tydra preserves source handedness; CPU repacks apply this sign after
  // skinning/blendshape deformation.
  float normalSign{1.0f};
  // Blendshape targets remapped to DrawVertex order; empty = no blendshapes.
  // The CPU/RT path morphs `vertices` (ApplyMorphTarget) for baked geometry.
  std::vector<MorphTargetCPU> morphs;
  // GPU-morph buffers (raster path): the morph is applied in the vertex shader
  // from a static sparse per-vertex delta list + a tiny per-frame coefficient
  // array, so weight changes upload only the coefficients (no VBO re-upload).
  // Mirrors the extended-skinning influence layout.
  //   morphOffsetCount: 2 uints/vertex (offset, count) into morphDeltaHalf.
  //   morphDeltaHalf: 4 halfs/entry (channelId, dx, dy, dz), half-precision to
  //     halve both the buffer (facial rigs: 64+ targets x 64k+ points) and the
  //     per-vertex shader fetch bandwidth. GL binds it as an RGBA16F texture
  //     buffer (hardware f16->f32 on texelFetch); VK reads uvec2 + unpackHalf2x16.
  //     The packed channelId (slot 0) is a legacy fallback -- the active-channel
  //     skip reads channelId from the exact uint16 `morphChannelId` side buffer,
  //     so only dx,dy,dz are live and the channel count is bounded by uint16, not
  //     by half-exactness (<= 2048).
  //   morphChannelCount: number of channels (size of the per-frame coeff array).
  //   morphTargetChannels: per-target channel metadata for the per-frame eval.
  std::vector<uint32_t> morphOffsetCount;
  std::vector<uint16_t> morphDeltaHalf;
  // Parallel to the morphDeltaHalf entries (one uint16 channelId each). A small
  // side buffer the vertex shader fetches FIRST (GL: R16UI texture buffer; VK:
  // widened to a uint SSBO, set 9), so it can skip the wide delta fetch when that
  // channel's coefficient is ~0 (facial animation: only a handful of 64+
  // expressions are active per frame). Channels are assigned per target in a fixed
  // order shared by every vertex, so the skip branch stays warp-coherent.
  std::vector<uint16_t> morphChannelId;
  int morphChannelCount{0};
  std::vector<MorphTargetChannelsCPU> morphTargetChannels;
  // Optional GPU skinning attributes, parallel to `vertices`.
  // `jointIdx` stores four absolute bone-matrix texture row indices per vertex;
  // `jointWt` stores the corresponding normalized weights. Empty = unskinned.
  std::vector<uint32_t> jointIdx;
  std::vector<float> jointWt;
  // Optional GL3-compatible full influence stream. `influenceOffsetCount` stores
  // two uints per vertex: texel offset and influence count. `influenceTexels`
  // stores one influence per RGBA32F texel: (absoluteJointRow, weight, 0, 0).
  std::vector<uint32_t> influenceOffsetCount;
  std::vector<float> influenceTexels;
  int influenceTexWidth{0};
  int influenceTexHeight{0};
  int maxInfluencesPerVertex{0};
  std::vector<uint32_t> indices;  // triangulated, grouped by submesh/material
  // Compact static triangle copy retained after the full CPU vertex/index data
  // is released. Viewport picking decodes these positions and intersects the
  // clicked pixel ray with actual triangles instead of treating the AABB as a
  // solid object. Positions are UNORM16 within pickQuantMin/Extent: 6 bytes per
  // vertex versus the complete DrawVertex, while indices retain exact topology.
  std::vector<uint16_t> pickPositions;
  std::vector<uint32_t> pickIndices;
  float pickQuantMin[3]{0, 0, 0};
  float pickQuantExtent[3]{0, 0, 0};
  std::vector<DrawSubmesh> submeshes;
  // Generated asynchronously after the authored mesh has been uploaded. LOD0
  // is the authored indices/submeshes above; entries here are LOD1..LOD3.
  std::vector<DrawPrototypeLodCPU> prototypeLods;

  // GPU instancing: per-instance 3x4 object-to-world matrices, 12 floats each
  // (3 rows of (x,y,z,translate); the constant bottom row is implicit). When
  // non-empty, the mesh is drawn with glDrawElementsInstanced and `world` is
  // ignored (each instance carries its own placement). Empty = a single
  // non-instanced draw.
  std::vector<float> instanceXforms;
  size_t instanceCount() const { return instanceXforms.size() / 12; }
  // Instance/prototype displayColor/displayOpacity for the flat instanced path.
  // When instanceColors is non-empty (3 floats/instance) each instance is tinted
  // individually; otherwise the whole instanced draw uses flatColor (e.g. the
  // prototype's average displayColor). instanceOpacities follows the same rule
  // with one float/instance, falling back to flatOpacity. Ignored for
  // non-instanced draws.
  std::vector<float> instanceColors;
  std::vector<float> instanceOpacities;
  float flatColor[3]{0.8f, 0.8f, 0.8f};
  float flatOpacity{1.0f};

  float world[16];  // column-major (light3d::Mat4 layout), world transform
  bool animatedWorld{false};
  // USD row-vector matrix copied with the same convention as `world`.
  float skinGeomBind[16]{1, 0, 0, 0, 0, 1, 0, 0,
                         0, 0, 1, 0, 0, 0, 0, 1};
  // Current Skeleton-prim local-to-world transform. Skinning produces points
  // in skeleton space; the per-joint object-space matrices fold this transform
  // in and cancel `world` before the renderer reapplies it.
  float skinSkeletonWorld[16]{1, 0, 0, 0, 0, 1, 0, 0,
                              0, 0, 1, 0, 0, 0, 0, 1};
  std::string skinSkeletonPath;
  int skelId{-1};
  int skinMatrixBase{-1};  // first matrix row in DrawScene's bone texture layout
  // Optional local-space skinned point samples for dense point-joint helper
  // display. Stored as xyz triples and updated by GPU skinning frame builds.
  std::vector<float> skinnedHelperPoints;
  float aabbMin[3]{0, 0, 0};
  float aabbMax[3]{0, 0, 0};
  // Prototype-LOCAL bbox over `vertices` (object space, before any instance
  // transform). For instanced meshes aabbMin/Max is the union over all instance
  // placements (scene-spanning); this local box is what per-instance frustum
  // culling (raster) and the CUDA instance world-AABBs transform per instance.
  float protoAabbMin[3]{0, 0, 0};
  float protoAabbMax[3]{0, 0, 0};
  // Max per-axis morph displacement (object space), used to pad protoAabb so a
  // GPU-morphed instanced prototype is not wrongly frustum-culled when the morph
  // pushes geometry past the rest box. 0 = no morph. See BuildMorphChannelsNext.
  float morphExtent[3]{0, 0, 0};
  // --next deform: this mesh's OWN rest-pose world box, and the absolute bone-row
  // range its jointIdx references. aabbMin/Max cannot serve: a skinned batch keeps
  // the conservative whole-scene box there (a tight rest box would pop the mesh out
  // of view as the rig moves). These let BuildNextPosedSceneBounds re-derive the
  // scene box at a new time code from 8 corners per bone, instead of re-skinning
  // every vertex -- which is what the Tydra path does, and what the next loader is
  // built to avoid. boneLo < 0 = unskinned.
  float restAabbMin[3]{0, 0, 0};
  float restAabbMax[3]{0, 0, 0};
  float posedPickAabbMin[3]{0, 0, 0};
  float posedPickAabbMax[3]{0, 0, 0};
  bool hasPosedPickAabb{false};
  int boneLo{-1};
  int boneHi{-1};
  bool doubleSided{false};
  int kindId{0};  // USD model kind AOV (resolved up ancestors); see KindId()
  // Optional 2nd texcoord set (2 floats/vertex, parallel to `vertices`); empty =
  // none. Drives the UV-set-1 (multi-UV) debug AOV. V is flipped like uv set 0.
  std::vector<float> uv1;
  // Optional per-vertex blendshape "influence": the largest displacement (world
  // units) any single morph target applies to that vertex. Computed at load from
  // `morphs`; empty when the mesh has no blendshapes. Drives the influence AOV.
  std::vector<float> morphInfluence;
  // Optional per-TRIANGLE original USD face id (the polygon each triangle was
  // triangulated from), parallel to the triangles of `indices` (grouped order).
  // Empty when triangulation face counts were unavailable. Drives SourceFaceId.
  std::vector<uint32_t> sourceFaceId;
  // Original-polygon wireframe edges as GL_LINES vertex-index pairs (perimeter of
  // each USD face, deduped). Built from the pre-triangulation topology so the
  // wireframe shows quads/ngons -- correct even for double-sided (doubled-tri)
  // meshes where a per-triangle approach fails. Empty => fall back to tri edges.
  std::vector<uint32_t> wireframeIndices;
  // Authored control points used by wireframeIndices when subdivision rewrites
  // the render mesh. Empty means the main vertex buffer owns the indices.
  std::vector<DrawVertex> wireframeVertices;
  // Coarse displacement baked into geometry for the ray-tracing backends (which
  // intersect real triangles, so displacement can't be a vertex/tess-shader effect
  // like the raster path). Parallel to `vertices`; empty when the mesh has no
  // displaced material. The raster path keeps using `vertices` (shader displacement
  // with live sliders); the VK ray-query BLAS/hit reads these instead.
  std::vector<DrawVertex> rtDisplacedVertices;
  // True when rtDisplacedVertices contains Ptex displacement that raster
  // backends must upload as the base geometry (their vertex stages cannot
  // identify a polygon face before primitive assembly).
  bool rasterDisplacementBaked{false};
};

// USD purpose token -> compact id used by the Purpose debug AOV (consistent across
// all backends): 0=default, 1=render, 2=proxy, 3=guide.
inline int PurposeId(const std::string& p) {
  if (p == "render") return 1;
  if (p == "proxy") return 2;
  if (p == "guide") return 3;
  return 0;  // default / unknown
}

// USD model kind token -> compact id for the Kind debug AOV (consistent across all
// backends): 0=none/unknown, 1=component, 2=group, 3=assembly, 4=subcomponent.
inline int KindId(const std::string& k) {
  if (k == "component") return 1;
  if (k == "group") return 2;
  if (k == "assembly") return 3;
  if (k == "subcomponent") return 4;
  return 0;  // no authored kind on this prim or any ancestor
}

enum class AlphaMode : int { Opaque = 0, Mask = 1, Blend = 2 };

enum class UdimMode : int { Sparse = 0, Atlas = 1 };
// Requested texture compression. Astc/Etc2 are mobile formats; Auto picks the
// best available for the device (BC7 desktop, ASTC/ETC2 mobile, else BCn/off).
// The requested mode is cap-gated against RendererCaps before CPU encoding.
enum class TextureCompressionMode : int {
  Off = 0,
  BCn = 1,
  BC7 = 2,
  Astc = 3,
  Etc2 = 4,
  Auto = 5,
};

enum class TextureGpuBackend : int { Off = 0, Vulkan = 1, CUDA = 2, HIP = 3 };

// GPU compressed-format capabilities, mirrored from RendererCaps so the CPU-side
// texture build (ApplyTextureRuntimeOptions) can cap-gate the requested format
// without a renderer dependency. Default = BC-only desktop assumption.
struct TextureCompressCaps {
  bool bc{true};
  bool astc{false};
  bool etc2{false};
  bool bc5{false};
  bool bc6h{false};
};

struct TextureRuntimeOptions {
  int maxTextureSize{0};       // longest edge cap in texels; 0 = no cap
  int textureBudgetMB{0};      // decoded/upload budget; 0 = no budget
  UdimMode udimMode{UdimMode::Sparse};
  TextureCompressionMode compression{TextureCompressionMode::Off};
  TextureCompressCaps caps{};  // populated from renderer->caps() before build
  // Keep already-compressed .ktx2 textures compressed (upload/transcode the GPU
  // blocks directly instead of decoding + re-encoding). Requires textools.
  bool keepCompressed{false};
  // Build content-aware CPU mip chains (sRGB/alpha-coverage/normal-map aware;
  // needs the vendored textools; no-op otherwise). Backends upload the
  // precomputed levels instead of glGenerateMipmap (GL) / no mips (VK).
  bool generateMips{false};
  // Optional GPU preprocessing. Vulkan is currently the viewer integration;
  // unsupported formats or initialization failures fall back to CPU encoding.
  TextureGpuBackend gpuBackend{TextureGpuBackend::Off};
  std::string gpuDevice;
  // DomeLight image-based lighting bake at load (vendored envmap lib):
  // 0 = off, 1 = low (32px faces), 2 = high (64px faces). Needs textools.
  int domeIbl{2};
};

struct DrawUvXformCPU {
  // Affine UV transform:
  //   u' = m00*u + m01*v + tx
  //   v' = m10*u + m11*v + ty
  float m00{1.0f};
  float m01{0.0f};
  float m10{0.0f};
  float m11{1.0f};
  float tx{0.0f};
  float ty{0.0f};
};

// Wrap modes (match light3d / GL semantics).
enum class WrapMode : int { ClampToEdge = 0, Repeat = 1, Mirror = 2, ClampToBorder = 3 };

// Source color space of a sampled texture. `Auto` defers to the image's own
// metadata/heuristic, which is what every slot did implicitly before this was
// carried per-slot.
enum class DrawColorSpace : int { Auto = 0, Raw = 1, sRGB = 2 };

// One self-contained description of how a material slot samples a texture.
//
// This used to hold only the UV transform, so a slot's image id, packed-channel
// select, wrap modes, and color space lived either as parallel per-slot fields
// on DrawMaterialCPU or were dropped entirely at the Draw boundary (wrap/color
// space were recoverable only from the shared DrawTextureCPU, which is wrong
// for two slots sampling one image with different intent -- e.g. an ORM map
// read as Raw for roughness). Keeping them here makes every slot uniform and
// lets a new slot be added without another five parallel arrays.
//
// The legacy parallel fields on DrawMaterialCPU are still populated for the
// slots that had them, so existing backend code keeps working unchanged.
struct DrawTexSampleCPU {
  DrawUvXformCPU uv;
  float scale[4]{1.0f, 1.0f, 1.0f, 1.0f};
  float bias[4]{0.0f, 0.0f, 0.0f, 0.0f};
  // Which UV set this texture samples: 0 = texcoords_0, 1 = texcoords_1. Resolved
  // from the texture's UsdPrimvarReader varname against the bound mesh's UV-set
  // names. Meshes with one UV set (the overwhelming majority) always leave it 0.
  int uvSet{0};
  // DrawScene::textures index, -1 when this slot samples no texture. Mirrors the
  // per-slot `*Tex` field on DrawMaterialCPU.
  int tex{-1};
  // Selected packed channel (0=R..3=A), -1 when the whole value is used.
  int channel{-1};
  WrapMode wrapS{WrapMode::Repeat};
  WrapMode wrapT{WrapMode::Repeat};
  DrawColorSpace colorSpace{DrawColorSpace::Auto};
  bool isUdim{false};
  // Native Ptex atlas sampling metadata. `isPtex` distinguishes the slot;
  // ptexFaceCount may be zero when the residency budget selected the
  // representative-face fallback.
  bool isPtex{false};
  uint16_t ptexAtlasCols{0};
  uint16_t ptexAtlasRows{0};
  uint32_t ptexTileEdge{0};
  // Linear texel offset of the embedded Ptex face-rectangle table and the
  // number of records. Each record occupies eight alpha texels (little-endian
  // uint16 x/y/width/height), so it survives sRGB texture uploads unchanged.
  uint32_t ptexRectTexelOffset{0};
  uint32_t ptexFaceCount{0};
};

enum class DrawMaterialParamType : int { Float = 0, Vec2 = 1, Vec3 = 2, Vec4 = 3 };

struct DrawMaterialParamCPU {
  // Neutral USD/MaterialX shader input record for future evaluators. Existing
  // preview shaders intentionally ignore this and keep using the legacy fields.
  std::string shader;  // "UsdPreviewSurface" or "OpenPBRSurface"
  std::string name;    // input name without the "inputs:" namespace
  DrawMaterialParamType type{DrawMaterialParamType::Float};
  float value[4]{0.0f, 0.0f, 0.0f, 1.0f};
  int texture{-1};        // DrawScene::textures index, -1 when unconnected/unloaded
  int renderTexture{-1};  // Tydra RenderScene::textures index, preserves connection
  int channel{-1};        // selected packed channel, -1 when whole value is used
  DrawTexSampleCPU sample;
};

enum class MaterialXGraphOpCPU : uint32_t {
  Constant = 0,
  Image,
  TiledImage,
  NormalMap,
  Add,
  Subtract,
  Multiply,
  Divide,
  Mix,
  Clamp,
  Dot,
  Normalized,
  Power,
  Minimum,
  Maximum,
  Absolute,
  SquareRoot,
  Sine,
  Cosine,
  Luminance,
  Select,
  Texcoord,
  Floor,
  Ceil,
  Fract,
  Step,
  Smoothstep,
  Cross,
  Length,
  Noise2D,
  Tangent,
  Exponential,
  Logarithm,
  Modulo,
  Invert,
  Remap,
  GeometricNormal,
  GeometricTangent,
  Rotate3D,
  Transform2D,
  // MaterialX noise3d keeps the authored z coordinate instead of degrading
  // to the legacy two-dimensional noise approximation.
  Noise3D = 40,
  Atan2 = 41,
  Sign = 42,
  Round = 43,
  Combine,
  Extract,
  Convert,
  Position,
  HsvAdjust,
  HeightToNormal,
  Arcsine,
  Arccosine,
  Contrast,
  Swizzle,
  Arctangent,
  Screen,
  Overlay,
  Burn,
  Dodge,
  RampLR,
  RampTB,
  SplitLR,
  SplitTB,
  Saturate,
  IfGreater,
  IfGreaterEqual,
  IfEqual,
  RgbToHsv,
  HsvToRgb,
  Rotate2D,
  Distance,
  Reflect,
  Refract,
  Premult,
  Unpremult,
  MinComponent,
  MaxComponent,
  LogicalAnd,
  LogicalOr,
  LogicalXor,
  LogicalNot,
  Inside,
  Outside,
  GeomColor,
  Bitangent,
  Difference,
  In,
  Mask,
  Matte,
  Out,
  Over,
  DisjointOver,
  SetAlpha,
  CellNoise2D,
  CellNoise3D,
  Fractal2D,
  WorleyNoise2D,
  WorleyNoise3D,
  Fractal3D,
  Cloverleaf,
  Hexagon,
  Grid,
  Crosshatch,
  TiledCircles,
  TiledCloverleafs,
  TiledHexagons,
  RampCoordinate,
  Ramp,
  RampGradient,
  Flake,
  MatrixTransform,
  MatrixTranspose,
  MatrixInverse,
  MatrixDeterminant,
  ViewDirection,
  Time,
  Frame,
  Blackbody,
  RoughnessAnisotropy,
  RoughnessDual,
  ArtisticIor,
  ChiangHairAbsorption,
  Unknown,
  GeomProp,
};

// Canonical, backend-neutral MaterialX graph record. Input indices refer to
// nodes in the same material graph; -1 means the corresponding constant lane
// in `value` is used. `imagePath` remains the authored asset-relative path until
// the Vulkan descriptor-indexed image table resolves it to a resident texture.
struct MaterialXGraphNodeCPU {
  MaterialXGraphOpCPU op{MaterialXGraphOpCPU::Unknown};
  int input[3]{-1, -1, -1};
  // Independent fallback values for the three possible inputs.  A single
  // shared fallback loses vector-valued MaterialX constants (for example the
  // two bounds of a clamp node), so the runtime record preserves all lanes.
  float value[3][4]{{0.0f, 0.0f, 0.0f, 1.0f},
                    {0.0f, 0.0f, 0.0f, 1.0f},
                    {0.0f, 0.0f, 0.0f, 1.0f}};
  int textureId{-1};  // resolved DrawScene texture slot, when resident
  // Fourth graph dependency for operators such as splitlr/splittb. Packed in
  // the texture-id lane, which those non-image operators otherwise never use.
  int auxInput{-1};
  float auxValue[4]{0.0f, 0.0f, 0.0f, 1.0f};
  bool isUdim{false}; // textureId names a UDIM atlas, not a plain 2D image
  float uvScale[2]{1.0f, 1.0f};
  float uvOffset[2]{0.0f, 0.0f};
  // Authored name for GeomProp nodes. The packed runtime stores a stable
  // 32-bit hash in the otherwise-unused UV-transform lane.
  std::string geomPropName;
  std::string imagePath;
  std::string name;
};

struct MaterialXGraphRuntimeCPU {
  std::vector<MaterialXGraphNodeCPU> nodes;
  // OpenPBR output node indices: base, metalness, roughness, opacity,
  // emission, normal, subsurface weight/color/radius, specular weight/color,
  // transmission weight/color, coat weight/color/roughness, sheen
  // weight/color/roughness, specular IOR, and the advanced OpenPBR controls
  // through volume emission scale. Route 48 is the distinct
  // subsurface-scatter-anisotropy lane; -1 means no graph connection.
  static constexpr int kOutputCount = 49;
  std::array<int, kOutputCount> output = [] {
    std::array<int, kOutputCount> routes{};
    routes.fill(-1);
    return routes;
  }();
  bool valid{false};
  bool hasImages{false};
};

// Baked constant fallback for raster/RT backends. Texture-connected inputs keep
// their texture ids in DrawMaterialParamCPU and the legacy material slots.


struct DrawMaterialCPU {
  std::string name;
  std::string absPath;
  std::string displayName;
  bool hasUsdPreviewSurface{false};
  bool hasOpenPBRSurface{false};
  bool hasDisplacementOutput{false};
  bool hasVolumeOutput{false};
  std::string displacementShaderPath;
  std::string volumeShaderPath;
  float volumeDensity{0.0f};
  float volumeAlbedo[3]{0.5f, 0.5f, 0.5f};
  float volumeAnisotropy{0.0f};
  float volumeEmission[3]{0.0f, 0.0f, 0.0f};
  float volumeEmissionScale{0.0f};
  std::string materialXNodeGraphJson;
  std::string volumeMaterialXNodeGraphJson;
  bool materialXVolumeGraph{false};
  MaterialXGraphRuntimeCPU materialXGraph;
  std::vector<DrawMaterialParamCPU> params;
  bool hasLightRtOpenPBR{false};
  DrawLightRtOpenPBRCPU lightRtOpenPBR;
  float baseColor[3]{0.8f, 0.8f, 0.8f};
  float metallic{0.0f};
  float roughness{0.5f};
  float emissive[3]{0.0f, 0.0f, 0.0f};
  float alpha{1.0f};
  int alphaMode{static_cast<int>(AlphaMode::Opaque)};
  float alphaCutoff{0.5f};
  // True when Mask was inferred only because an opacity map is JPEG coverage,
  // rather than authored through opacityThreshold. A fractional
  // displayOpacity must be allowed to promote this heuristic to Blend.
  bool alphaMaskHeuristic{false};
  // UsdPreviewSurface specular workflow (T12): when true, F0 is specularColor
  // directly; else F0 is the dielectric reflectance from `ior` lerped toward the
  // base color by metalness (ior 1.5 -> the fixed 0.04 the metallic path used).
  bool useSpecularWorkflow{false};
  // OpenPBR/MaterialX Standard Surface uses specularColor as a tint on the
  // dielectric IOR-derived F0, rather than Preview Surface's direct-F0 mode.
  bool openPbrSpecularModel{false};
  float specularColor[3]{0.0f, 0.0f, 0.0f};
  float ior{1.5f};
  // Real-time PBR core shared by raster and RT. These are populated from the
  // full LightRT/OpenPBR fallback even when the source is UsdPreviewSurface.
  float occlusion{1.0f};
  float coatWeight{0.0f};
  float coatColor[3]{1.0f, 1.0f, 1.0f};
  float coatRoughness{0.1f};
  float coatIor{1.5f};
  // Indices into DrawScene::textures (-1 = no texture)
  int baseColorTex{-1};
  // Keep metallic and roughness independent. They may alias the same packed
  // ORM texture, but USD commonly authors them as separate images/UV streams.
  int metallicTex{-1};
  int roughnessTex{-1};
  int normalTex{-1};
  int coatNormalTex{-1};
  int emissiveTex{-1};
  // Separate scalar opacity map. This is distinct from baseColorTex's alpha:
  // DCCs commonly connect an independent grayscale/UDIM mask to
  // UsdPreviewSurface inputs:opacity.
  int opacityTex{-1};
  // Ambient-occlusion scalar map. It modulates only indirect/ambient light.
  int occlusionTex{-1};
  // Specular-workflow F0 map. Only consulted when useSpecularWorkflow is set;
  // in the metallic workflow F0 comes from ior/baseColor as before.
  int specularColorTex{-1};
  // Coat lobe maps. These were constant-only, so an authored coat weight/tint/
  // roughness texture silently collapsed to its fallback constant.
  int coatWeightTex{-1};
  int coatColorTex{-1};
  int coatRoughnessTex{-1};
  DrawTexSampleCPU baseColorSample;
  DrawTexSampleCPU metallicSample;
  DrawTexSampleCPU roughnessSample;
  DrawTexSampleCPU normalSample;
  DrawTexSampleCPU coatNormalSample;
  DrawTexSampleCPU emissiveSample;
  DrawTexSampleCPU opacitySample;
  DrawTexSampleCPU occlusionSample;
  DrawTexSampleCPU specularColorSample;
  DrawTexSampleCPU coatWeightSample;
  DrawTexSampleCPU coatColorSample;
  DrawTexSampleCPU coatRoughnessSample;
  // Full sample for the displacement map. `displacementUv` below is the older
  // UV-only form kept for the existing displacement code paths; this carries the
  // UV set and scale/bias so displacement can use UV set 1 like every other slot.
  DrawTexSampleCPU displacementSample;
  int opacityChannel{0};
  float opacityTexScale{1.0f};
  float opacityTexBias{0.0f};
  int occlusionChannel{0};
  float occlusionTexScale{1.0f};
  float occlusionTexBias{0.0f};
  int metallicChannel{2};  // glTF ORM default: B
  int roughnessChannel{1}; // glTF ORM default: G
  float metallicTexScale{1.0f};
  float metallicTexBias{0.0f};
  float roughnessTexScale{1.0f};
  float roughnessTexBias{0.0f};
  // UsdPreviewSurface inputs:displacement. The surface is offset along its normal
  // by (displacementConst, or the displacement texture's red channel when present)
  // times the global displacement scale. The displacement map is linear (raw).
  int displacementTex{-1};
  DrawUvXformCPU displacementUv;
  float displacementConst{0.0f};
  // UsdUVTexture inputs:scale/inputs:bias for the displacement map's sampled
  // channel: effective height = texel*scale + bias (bias commonly centers a [0,1]
  // map). Identity (1, 0) when unauthored or for constant displacement.
  float displacementTexScale{1.0f};
  float displacementTexBias{0.0f};
  // A material terminal is an authored displacement output even when its
  // resolved constant happens to be zero. Keep that distinction from an
  // absent terminal so the geometry path does not silently skip the output.
  bool hasDisplacement() const {
    return hasDisplacementOutput || displacementTex >= 0 ||
           displacementConst != 0.0f;
  }
};

enum class DrawCompressedFormat : int {
  None = 0,
  BC1 = 1,
  BC3 = 3,
  BC5 = 5,       // RGTC two-channel (normal maps)
  BC6H = 6,      // BPTC float (HDR)
  BC7 = 7,
  ETC2_RGB = 10,
  ETC2_RGBA = 11,
  ASTC_4x4 = 20,
};

struct DrawCompressedMipCPU {
  int width{0};
  int height{0};
  std::vector<uint8_t> data;
};

struct DrawCompressedImageCPU {
  DrawCompressedFormat format{DrawCompressedFormat::None};
  int width{0};
  int height{0};
  std::vector<uint8_t> data;
  // Optional precomputed mip payloads for levels 1..N (level 0 is
  // width/height/data above), same block format. Empty = single level.
  std::vector<DrawCompressedMipCPU> mips;
};

// Inner texel rectangle for one Ptex face in DrawTextureCPU::image. The atlas
// may contain padding around this rectangle; sampling maps intrinsic face UVs
// between the first and last inner texel centers.
struct DrawPtexFaceRectCPU {
  uint32_t x{0};
  uint32_t y{0};
  uint32_t width{0};
  uint32_t height{0};
  uint16_t mipLevel{0};
  uint16_t reserved{0};
};

struct DrawUdimTileCPU {
  uint32_t udim{1001};
  uint32_t u{0};
  uint32_t v{0};
  std::string assetIdentifier;
  int renderImageId{-1};
  light3d::Image image;  // RGBA8 tile layer
  // Linear RGB float source for HDR/EXR UDIM tiles. `image` remains the
  // tonemapped RGBA8 fallback used by renderers without BC6H array support.
  std::vector<float> hdrRGB;
  bool isHDR{false};
  DrawCompressedImageCPU compressed;
  // Optional precomputed RGBA8 mips for levels 1..N (level 0 = `image`).
  // NormalizeUdimTiles equalizes tile dims, so all tiles of a texture carry
  // the same level count/dims when mips are generated.
  std::vector<light3d::Image> mipImages;
};

struct DrawTextureCPU {
  light3d::Image image;  // always normalized to RGBA8 (channels == 4) on the CPU side
  // Identity of the complete ordinary 2D upload. This survives release of the
  // pixel/mip payload and lets a later recomposition retain an unchanged GPU
  // texture slot. Zero means the identity has not been captured yet.
  uint64_t uploadedFingerprint{0};
  // Linear RGB float source retained for HDR/EXR GPU BC6H processing. The
  // image field is a tonemapped RGBA8 fallback when a backend cannot use it.
  std::vector<float> hdrRGB;
  bool isHDR{false};
  std::string assetIdentifier;  // Tydra TextureImage::asset_identifier, if known
  bool deferredDecode{false};  // slot exists; pixels arrive asynchronously
  // Native Ptex source. `image` is a bounded face atlas when residency permits,
  // or a representative-face fallback after the cumulative budget is spent.
  bool isPtex{false};
  // Allocate mutable RGBA8 storage even when `image.data` is empty. Streaming
  // Ptex caches use this to reserve only their fixed physical atlas without a
  // same-sized zero-filled CPU upload.
  bool streamingMutable{false};
  bool ptexForceResidency{false};  // diagnostic: stream even fitting faces
  // Only refine faces referenced by admitted meshes. Enabled when the initial
  // atlas deliberately leaves a tail of faces as reserved placeholders.
  bool ptexDemandDriven{false};
  uint32_t ptexFaces{0};
  uint16_t ptexLevels{0};
  uint16_t ptexChannels{0};
  uint32_t ptexMaxFaceEdge{0};
  uint32_t ptexDownsampledFaces{0};
  uint64_t ptexAtlasBytes{0};
  uint64_t ptexPageCacheHits{0};
  uint64_t ptexPageCacheMisses{0};
  uint64_t ptexPageCacheEvictions{0};
  uint64_t ptexPageCachePeakBytes{0};
  uint64_t ptexPageDecodedBytes{0};
  uint64_t ptexGpuPageUploads{0};
  uint64_t ptexGpuPageHits{0};
  uint64_t ptexGpuPageMisses{0};
  uint64_t ptexGpuPageEvictions{0};
  uint16_t ptexAtlasCols{0};
  uint16_t ptexAtlasRows{0};
  uint32_t ptexTileEdge{0};
  uint32_t ptexGutter{0};
  uint32_t ptexRectTexelOffset{0};
  uint32_t ptexPhysicalCacheOffsetY{0};
  uint32_t ptexPhysicalCacheSlotEdge{0};
  uint32_t ptexPhysicalCacheSlots{0};
  std::vector<DrawPtexFaceRectCPU> ptexFaceRects;
  // Compressed native source retained only when physical cache slots exist;
  // pages are decoded on demand and the much larger full-resolution atlas is
  // never materialized.
  std::vector<uint8_t> ptexSourceData;
  std::shared_ptr<const std::vector<uint8_t>> ptexSourceDataShared;

  const std::vector<uint8_t>& PtexSourceData() const {
    return ptexSourceDataShared ? *ptexSourceDataShared : ptexSourceData;
  }
  bool HasPtexSourceData() const { return !PtexSourceData().empty(); }
  int renderImageId{-1};        // source RenderScene::images index, or -1
  int renderUdimId{-1};         // source RenderScene::udim_textures index, or -1
  bool srgb{false};      // sRGB color data (baseColor/emissive) vs linear scalar/normal data
  int wrapS{static_cast<int>(WrapMode::Repeat)};
  int wrapT{static_cast<int>(WrapMode::Repeat)};
  bool requestedCompressed{false};
  // The compressed payload is already final (kept-compressed KTX2 passthrough):
  // skip re-encoding and mip generation from `image`. `image` may be empty.
  bool compressedFinal{false};
  DrawCompressedImageCPU compressed;
  bool isUdim{false};
  std::vector<DrawUdimTileCPU> udimTiles;
  std::array<int, 100> udimLayer{};
  int udimTileWidth{0};
  int udimTileHeight{0};
  // Texture usage, classified from the materials after BuildDrawMaterials
  // (FinalizeDrawTextures). Drives content-aware mip generation.
  bool isNormalMap{false};
  bool isAlphaTested{false};
  float alphaCutoff{0.5f};
  // Per-channel downsample rule for packed maps (0=linear, 1=majority,
  // 2=roughness variance-aware); indices match RGBA.
  int channelOp[4]{0, 0, 0, 0};
  // Optional precomputed RGBA8 mip images for levels 1..N (level 0 is
  // `image`). Empty = backends fall back to GPU mip generation (GL) or a
  // single level (VK).
  std::vector<light3d::Image> mipImages;
};

// A UsdVol volume (OpenVDB) as a dense scalar (density) grid, for GPU 3D-texture
// raymarching. `data` is the dense float grid (x-contiguous), `world` maps the
// object-space box [aabbMin, aabbMax] into world space.
struct DrawVolumeCPU {
  std::string name;
  int dim[3]{0, 0, 0};
  std::vector<float> density;  // dense, length dim[0]*dim[1]*dim[2]
  // Optional scalar grids aligned with density. Raster backends use these for
  // spatial emission and blackbody fire shading.
  std::vector<float> emissionField;
  std::vector<float> temperatureField;
  float world[16];             // column-major 4x4 (light3d::Mat4 layout)
  float aabbMin[3]{0, 0, 0};   // object-space grid bounds
  float aabbMax[3]{0, 0, 0};
  float densityScale{1.0f};
  float albedo[3]{0.6f, 0.6f, 0.65f};
  float emission[3]{0.0f, 0.0f, 0.0f};
  float background{0.0f};
};

// Precomputed split-sum IBL for a DomeLight (baked at load from the HDR float
// envmap by the vendored envmap library). Cube data is face-major float RGB in
// the KTX/D3D/GL face order +X,-X,+Y,-Y,+Z,-Z (matches GL cube upload).
struct DomeIblCPU {
  bool valid{false};
  int specFaceSize{0};  // level 0 face dim; level l is specFaceSize >> l
  // specLevels[l]: 6 * (specFaceSize>>l)^2 * 3 floats, GGX-prefiltered at
  // roughness l/(N-1); level 0 = mirror.
  std::vector<std::vector<float>> specLevels;
  int irrFaceSize{0};
  std::vector<float> irradiance;  // 6 * irrFaceSize^2 * 3, stored as E/pi
  int lutSize{0};
  std::vector<float> brdfLut;  // lutSize^2 * 2 (scale, bias)
  // Full-resolution (pre-firefly-clamp) source cube for env backgrounds (RT
  // miss); 6 * envCubeSize^2 * 3 floats, same face order as specLevels.
  int envCubeSize{0};
  std::vector<float> envCube;
  // Compact HDR latlong used by CUDA/HIP RT miss and glossy-environment
  // sampling. RGB stores a shared mantissa and A stores a biased base-2
  // exponent (Radiance RGBE). Keeping this beside the float cube avoids
  // clamping HDR domes to the generic RGBA8 material-texture representation.
  int envLatlongWidth{0};
  int envLatlongHeight{0};
  std::vector<uint8_t> envLatlongRgbe;
  // Order-2 real-SH projection of the irradiance (E/pi), 9 coefficients x RGB
  // (coefficient-major: coeff k channel c at [k*3+c]). Evaluated by the RT
  // shaders as the surface ambient term; the basis polynomial set matches
  // ShIrradianceBasis in texture_tools.cc / the RT kernels.
  std::vector<float> shIrradiance;
};

struct DrawLightCPU {
  enum class Type : int {
    Point = 0,
    Sphere = 1,
    Disk = 2,
    Rect = 3,
    Cylinder = 4,
    Distant = 5,
    Dome = 6,
    Geometry = 7,
    Portal = 8,
  };
  enum class DomeTextureFormat : int {
    Automatic = 0,
    Latlong = 1,
    MirroredBall = 2,
    Angular = 3,
  };

  std::string name;
  std::string absPath;
  std::string displayName;
  Type type{Type::Point};
  float color[3]{1.0f, 1.0f, 1.0f};
  float intensity{1.0f};
  float exposure{0.0f};
  // color * colorTemperature(if enabled) * intensity * 2^exposure.
  float effectiveColor[3]{1.0f, 1.0f, 1.0f};
  // effectiveColor divided by shape area when normalize=true and area > 0.
  float normalizedColor[3]{1.0f, 1.0f, 1.0f};
  float effectiveIntensity{1.0f};
  float diffuse{1.0f};
  float specular{1.0f};
  bool normalize{false};
  bool enableColorTemperature{false};
  float colorTemperature{6500.0f};
  float transform[16]{};
  float position[3]{0.0f, 0.0f, 0.0f};
  float direction[3]{0.0f, -1.0f, 0.0f};
  float radius{0.5f};
  float width{1.0f};
  float height{1.0f};
  float length{1.0f};
  float area{0.0f};
  float invArea{0.0f};
  float angle{0.53f};
  std::string textureFile;
  int envmapTexture{-1};  // DrawScene::textures index when decoded/uploadable
  int renderEnvmapImage{-1};  // Tydra RenderScene::images index for DomeLight
  DomeTextureFormat domeTextureFormat{DomeTextureFormat::Automatic};
  DomeIblCPU ibl;  // split-sum IBL bake (Dome only; empty when disabled)
  float guideRadius{1.0e5f};
  float shapingConeAngle{90.0f};
  float shapingConeSoftness{0.0f};
  float shapingFocus{0.0f};
  float shapingFocusTint[3]{0.0f, 0.0f, 0.0f};
  std::string shapingIesFile;
  float shapingIesAngleScale{0.0f};
  bool shapingIesNormalize{false};
  // Parsed IES photometric profile. The profile is kept on the canonical light
  // record so all backends can consume the same angular data; empty vectors
  // mean that the authored profile was unavailable or invalid.
  bool iesValid{false};
  float iesMaxCandela{0.0f};
  std::vector<float> iesVerticalAngles;
  std::vector<float> iesHorizontalAngles;
  std::vector<float> iesCandela;
  bool hasShaping{false};
  bool shadowEnable{true};
  float shadowColor[3]{0.0f, 0.0f, 0.0f};
  float shadowDistance{-1.0f};
  float shadowFalloff{-1.0f};
  float shadowFalloffGamma{1.0f};
  int geometryMesh{-1};
  std::string geometryTargetPath;
  // Resolved RT geometry-light payload. These are filled by BuildHostScene;
  // geometryMesh remains the source DrawScene mesh id for diagnostics.
  int geometryTriOffset{-1};
  int geometryTriCount{0};
  int geometryInstance{-1};
  // Deterministic world-space samples used by bounded raster lighting. RT keeps
  // using the complete triangle range above. Three floats per sample.
  int geometryRasterSampleCount{0};
  float geometryRasterSamples[24]{};
  std::string materialSyncMode;
  bool lightLinksAll{true};
  std::vector<int> lightLinkMeshIndices;
  // Authored carrier/collection targets retained alongside resolved mesh ids.
  // Native point/curve carriers do not have a mesh index, so RT/raster carrier
  // paths use these targets when applying the same collection mask.
  std::vector<std::string> lightLinkPaths;
  bool shadowLinksAll{true};
  std::vector<int> shadowLinkMeshIndices;
  std::vector<std::string> shadowLinkPaths;
  bool hasSpectralEmission{false};
};

// Render-ready non-mesh geometry retained from the next-core converter. These
// carriers deliberately preserve world-space placement and authored/tessellated
// attributes instead of prematurely expanding them into camera-dependent mesh
// proxies; raster backends consume them as billboards/ribbons and RT backends
// build their solid proxy geometry from the same records.
struct DrawPointsCPU {
  std::string name;
  std::string absPath;
  std::string purpose{"default"};
  std::vector<float> points;   // local xyz
  std::vector<float> normals;  // optional per-point disk/oval normals
  std::vector<float> widths;   // empty, constant, or per-point
  std::vector<float> colors;   // empty, constant rgb, or per-point rgb
  std::vector<float> opacities;  // empty, constant, or per-point
  // Gaussian splat covariance carrier. Radii are diameters in local space;
  // normals/major_axes are optional and parallel to points.
  bool gaussian{false};
  std::vector<float> ellipseRadii;
  std::vector<float> ellipseNormals;
  std::vector<float> ellipseMajorAxes;
  int colorsInterpolation{0};
  int opacitiesInterpolation{0};
  int materialId{-1};
  uint32_t directLightMask{0xffffffffu};
  uint32_t shadowLightMask{0xffffffffu};
  float world[16]{};            // column-major local-to-world
  float aabbMin[3]{0, 0, 0};
  float aabbMax[3]{0, 0, 0};
};

struct DrawCurvesCPU {
  std::string name;
  std::string absPath;
  std::string purpose{"default"};
  std::vector<uint32_t> vertexCounts;  // tessellated polyline counts
  std::vector<float> points;           // tessellated local xyz
  std::vector<float> widths;           // empty, constant, or per-point
  std::vector<float> colors;           // empty or per-point rgb
  std::vector<float> opacities;        // empty, constant, or per-point
  int materialId{-1};
  float world[16]{};                    // column-major local-to-world
  float aabbMin[3]{0, 0, 0};
  float aabbMax[3]{0, 0, 0};
};

struct DrawCameraCPU {
  std::string name;
  std::string absPath;
  std::string displayName;
  enum class Projection : int { Perspective = 0, Orthographic = 1 };
  Projection projection{Projection::Perspective};
  float eye[3]{0.0f, 0.0f, 0.0f};
  float up[3]{0.0f, 1.0f, 0.0f};
  float forward[3]{0.0f, 0.0f, -1.0f};
  float focalLength{50.0f};
  float horizontalAperture{20.955f};
  float verticalAperture{15.2908f};
  float horizontalApertureOffset{0.0f};
  float verticalApertureOffset{0.0f};
  float exposure{0.0f};
  float focusDistance{0.0f};
  float fStop{0.0f};
  double shutterOpen{0.0};
  double shutterClose{0.0};
  enum class StereoRole : int { Mono = 0, Left = 1, Right = 2 };
  StereoRole stereoRole{StereoRole::Mono};
  // World-space clipping-plane equations (a,b,c,d), retained for backends
  // that support arbitrary user clipping.
  std::vector<float> clippingPlanes;
  float zNear{0.1f};
  float zFar{10000.0f};
  float fovYDeg{45.0f};
};

struct DrawScene {
  struct OptimizationStats {
    size_t sourceMaterials{0};
    size_t uniqueMaterials{0};
    // Number of distinct raster payloads after identity-preserving
    // canonicalization. `materials` itself retains every logical authored
    // material so picking, diagnostics and live edits keep stable ids.
    size_t canonicalMaterials{0};
    size_t deduplicatedMaterials{0};
  } optimization;
  std::vector<DrawMeshCPU> meshes;
  std::vector<DrawPointsCPU> points;
  std::vector<DrawCurvesCPU> curves;
  std::vector<DrawMaterialCPU> materials;
  std::vector<DrawTextureCPU> textures;
  std::vector<DrawVolumeCPU> volumes;  // UsdVol volumes (OpenVDB)
  std::vector<DrawLightCPU> lights;    // USD light parameters for later shading
  std::vector<DrawCameraCPU> cameras;  // USD camera records for loader-equivalence testing
  bool hasPreviewLight{false};
  // A single derived key light used by today's simple preview shaders. Full
  // multi-light evaluation will consume DrawLightCPU directly later.
  float previewLightDir[3]{0.40160966f, 0.64257544f, 0.48193160f};
  float previewLightColor[3]{1.0f, 1.0f, 1.0f};
  int boneMatrixCount{0};  // height of the per-frame 4xN RGBA32F bone texture

  // --next per-frame GPU skinning. The Tydra path re-poses from
  // RenderScene::skeletons; the next path has no RenderScene, so each skinned
  // source mesh records here how to re-pose itself straight from the retained
  // next Stage. One entry per skinned source mesh (not per DrawMesh: the next
  // loader merges meshes into material batches, so one batch may draw vertices
  // from several of these).
  //
  // The mesh's vertices are already world-baked into its batch, so the bone rows
  // this entry owns ([matrixBase, matrixBase + numJoints)) are pre-composed with
  // both `geomBind` and the mesh's world transform -- see BuildNextSkinningFrame.
  // Vertices reference them by ABSOLUTE row index through DrawMeshCPU::jointIdx.
  struct NextSkelBinding {
    std::string skelPath;  // Skeleton prim
    std::string animPath;  // bound SkelAnimation ("" = rest pose)
    std::string meshPath;  // the skinned mesh (diagnostics)
    int numJoints{0};
    int matrixBase{0};
    double geomBind[16];  // primvars:skel:geomBindTransform (row-vector)
    double world[16];     // mesh world transform baked into the vertices
    double renderWorld[16];  // transform applied by the draw after skinning
    double skeletonWorld[16];  // Skeleton prim local-to-world transform
  };
  std::vector<NextSkelBinding> nextSkels;

  // World-space bounds over all meshes.
  float aabbMin[3]{-1, -1, -1};
  float aabbMax[3]{1, 1, 1};
  bool hasBounds{false};

  // Stage up axis ("Y" or "Z"); drives camera orbit + grid orientation. Set by
  // the loader (the Tydra path uses RenderScene.meta.upAxis directly instead).
  std::string upAxis{"Y"};
  // Physical scale used to convert CLI-specified 35 mm-equivalent camera
  // optics into world-space lens radii for auto-framed scenes.
  double metersPerUnit{0.01};

  // Diagnostics surfaced in the GUI (skipped meshes/textures, UDIM, etc.)
  std::vector<std::string> skipped;
  size_t triangleCount{0};
  // Total vertex count, captured at load time. Stored separately because the
  // --next path frees per-mesh CPU geometry after GPU upload (so summing
  // meshes[].vertices later would report 0).
  size_t vertexCount{0};

  // True when a render budget (triangles / VRAM) was hit and the scene was only
  // partially built to avoid freezing / VRAM thrashing.
  bool truncated{false};

  bool empty() const {
    return meshes.empty() && points.empty() && curves.empty() &&
           volumes.empty() && lights.empty() && cameras.empty();
  }
};

struct SkinningFrameCPU {
  int matrixCount{0};  // texture height; width is always 4 RGBA texels
  std::vector<float> rgba32f;  // matrixCount * 4 texels * 4 floats
  bool enabled{false};
};

}  // namespace lusdview
