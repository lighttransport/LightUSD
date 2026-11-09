#include "light3d/material.h"
#include "light3d/mesh_data.h"
#include <algorithm>
#include <cassert>
#ifdef _MSC_VER
#include <windows.h>
#include "material_resource.h"
#endif

namespace light3d {

// --- MaterialLibrary ---

int MaterialLibrary::addMaterial(Material material) {
    int id = static_cast<int>(materials_.size());
    material.id = id;
    if (!material.name.empty()) {
        nameToIndex_[material.name] = id;
    }
    materials_.push_back(std::move(material));
    return id;
}

const Material* MaterialLibrary::getMaterial(int id) const {
    if (id < 0 || id >= static_cast<int>(materials_.size())) return nullptr;
    return &materials_[id];
}

Material* MaterialLibrary::getMaterial(int id) {
    if (id < 0 || id >= static_cast<int>(materials_.size())) return nullptr;
    return &materials_[id];
}

const Material* MaterialLibrary::findByName(const std::string& name) const {
    auto it = nameToIndex_.find(name);
    if (it == nameToIndex_.end()) return nullptr;
    return &materials_[it->second];
}

// --- Triangulation helper ---

// Fan-triangulate a single face, appending triangle vertex indices to `out`.
// For a face with vertices [v0, v1, v2, ..., vN-1], produces triangles:
//   (v0, v1, v2), (v0, v2, v3), ..., (v0, vN-2, vN-1)
static void triangulateFace(const int* faceIndices, int vertexCount,
                            std::vector<uint32_t>& out) {
    for (int i = 1; i + 1 < vertexCount; ++i) {
        out.push_back(static_cast<uint32_t>(faceIndices[0]));
        out.push_back(static_cast<uint32_t>(faceIndices[i]));
        out.push_back(static_cast<uint32_t>(faceIndices[i + 1]));
    }
}

// --- buildSubmeshes ---

SubmeshData buildSubmeshes(const MeshGeometry& geometry) {
    struct TriangleRecord {
        int materialId;
        uint32_t idx[3];
    };

    std::vector<TriangleRecord> triangles;
    bool hasMatIds = geometry.hasPerFaceMaterials();

    int offset = 0;
    for (size_t face = 0; face < geometry.faceVertexCounts.size(); ++face) {
        int count = geometry.faceVertexCounts[face];
        int matId = hasMatIds ? geometry.faceMaterialIds[face] : 0;
        const int* faceVerts = &geometry.faceVertexIndices[offset];

        for (int i = 1; i + 1 < count; ++i) {
            TriangleRecord rec;
            rec.materialId = matId;
            rec.idx[0] = static_cast<uint32_t>(faceVerts[0]);
            rec.idx[1] = static_cast<uint32_t>(faceVerts[i]);
            rec.idx[2] = static_cast<uint32_t>(faceVerts[i + 1]);
            triangles.push_back(rec);
        }

        offset += count;
    }

    // Stable sort by material ID (preserves face order within each material group)
    std::stable_sort(triangles.begin(), triangles.end(),
                     [](const TriangleRecord& a, const TriangleRecord& b) {
                         return a.materialId < b.materialId;
                     });

    // Build output
    SubmeshData result;
    result.triangleIndices.reserve(triangles.size() * 3);

    if (triangles.empty()) return result;

    int currentMatId = triangles[0].materialId;
    int groupStart = 0;

    for (size_t i = 0; i < triangles.size(); ++i) {
        result.triangleIndices.push_back(triangles[i].idx[0]);
        result.triangleIndices.push_back(triangles[i].idx[1]);
        result.triangleIndices.push_back(triangles[i].idx[2]);

        bool last = (i + 1 == triangles.size());
        bool matChange = !last && (triangles[i + 1].materialId != currentMatId);

        if (last || matChange) {
            Submesh sub;
            sub.materialId = currentMatId;
            sub.triangleOffset = groupStart;
            sub.triangleCount = static_cast<int>(i) - groupStart + 1;
            result.submeshes.push_back(sub);

            if (matChange) {
                currentMatId = triangles[i + 1].materialId;
                groupStart = static_cast<int>(i) + 1;
            }
        }
    }

    return result;
}

// --- packMaterialsToBuffer ---

std::vector<float> packMaterialsToBuffer(const MaterialLibrary& library) {
    std::vector<float> buf;
    buf.reserve(library.count() * kPackedMaterialFloats);

    for (const auto& mat : library.materials()) {
        // vec4(baseColor.rgb, metallic)
        buf.push_back(mat.baseColor.x);
        buf.push_back(mat.baseColor.y);
        buf.push_back(mat.baseColor.z);
        buf.push_back(mat.metallic);

        // vec4(emissive.rgb, roughness)
        buf.push_back(mat.emissive.x);
        buf.push_back(mat.emissive.y);
        buf.push_back(mat.emissive.z);
        buf.push_back(mat.roughness);

        // vec4(alpha, alphaCutoff, doubleSided, 0)
        buf.push_back(mat.alpha);
        buf.push_back(mat.alphaCutoff);
        buf.push_back(mat.doubleSided ? 1.0f : 0.0f);
        buf.push_back(0.0f);

        // vec4(baseColorTexIdx, metalRoughTexIdx, normalTexIdx, emissiveTexIdx)
        buf.push_back(static_cast<float>(mat.baseColorTexture));
        buf.push_back(static_cast<float>(mat.metallicRoughnessTexture));
        buf.push_back(static_cast<float>(mat.normalTexture));
        buf.push_back(static_cast<float>(mat.emissiveTexture));
    }

    return buf;
}

// --- buildTriangleMaterialIds ---

TriangleMaterialIdData buildTriangleMaterialIds(const MeshGeometry& geometry) {
    TriangleMaterialIdData result;
    bool hasMatIds = geometry.hasPerFaceMaterials();

    int offset = 0;
    for (size_t face = 0; face < geometry.faceVertexCounts.size(); ++face) {
        int count = geometry.faceVertexCounts[face];
        int matId = hasMatIds ? geometry.faceMaterialIds[face] : 0;
        const int* faceVerts = &geometry.faceVertexIndices[offset];

        for (int i = 1; i + 1 < count; ++i) {
            result.triangleIndices.push_back(static_cast<uint32_t>(faceVerts[0]));
            result.triangleIndices.push_back(static_cast<uint32_t>(faceVerts[i]));
            result.triangleIndices.push_back(static_cast<uint32_t>(faceVerts[i + 1]));
            result.materialIds.push_back(static_cast<uint32_t>(matId));
        }

        offset += count;
    }

    return result;
}

// --- Shader Source Strings ---

// ==================== GL330 Submesh Shaders ====================

const char* getMaterialVertexShaderGL330() {
    return R"glsl(#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aUV;
layout(location = 3) in uvec4 aJoint;
layout(location = 4) in vec4 aWeight;
layout(location = 5) in uvec2 aInfluence;
layout(location = 6) in vec2 aUV1;        // 2nd texcoord set (multi-UV AOV; default 0)
layout(location = 7) in float aMorphInfl; // blendshape influence magnitude (default 0)
layout(location = 8) in uvec2 aMorphOffsetCount; // GPU morph (offset,count); default 0
layout(location = 9) in vec4 aColor;  // displayColor.rgb + displayOpacity (default 1)
uniform mat4 uModelViewProj;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;
uniform sampler2D uBoneTex;
uniform sampler2D uInfluenceTex;
uniform bool uSkinningEnabled;
uniform bool uExtendedSkinningEnabled;
uniform int uBoneTexWidth;
uniform int uBoneMatrixCount;
uniform int uInfluenceTexWidth;

// UsdPreviewSurface displacement (coarse per-vertex). The surface is offset along
// its normal by (height-map red, or uDisplacementConst) * uDisplacementScale. No
// derivatives in the vertex stage, so the map is sampled with textureLod(...,0).
uniform bool uHasDisplacement;
uniform bool uHasDisplacementTex;
uniform sampler2D uDisplacementTex;
uniform float uDisplacementConst;
uniform float uDisplacementScale;
uniform float uDisplacementTexScale;  // UsdUVTexture scale/bias (height = t*s + b)
uniform float uDisplacementTexBias;

// GPU blendshape morph: per-vertex (offset,count) into uMorphDeltaTex (RGBA16F:
// channelId, dx,dy,dz); uMorphCoeffTex (R32F) holds the per-frame coefficient per
// channel. pos += sum_i coeff[channel_i] * delta_i, applied before skinning.
// uMorphChanTex (R16UI) duplicates each entry's channelId so the loop can read the
// coefficient and skip the wide delta fetch when the channel is inactive.
uniform bool uHasMorph;
uniform samplerBuffer uMorphDeltaTex;
uniform samplerBuffer uMorphCoeffTex;
uniform usamplerBuffer uMorphChanTex;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
flat out int vDomJoint;    // dominant skin joint (SkinWeights AOV); -1 = unskinned
out float vDomWeight;      // its weight
out vec2 vUV1;             // 2nd texcoord set (multi-UV AOV)
out float vMorphInfl;      // blendshape influence (world units)

mat4 fetchBone(uint idx) {
    int base = int(idx) * 4;
    return mat4(
        texelFetch(uBoneTex, ivec2((base + 0) % uBoneTexWidth, (base + 0) / uBoneTexWidth), 0),
        texelFetch(uBoneTex, ivec2((base + 1) % uBoneTexWidth, (base + 1) / uBoneTexWidth), 0),
        texelFetch(uBoneTex, ivec2((base + 2) % uBoneTexWidth, (base + 2) / uBoneTexWidth), 0),
        texelFetch(uBoneTex, ivec2((base + 3) % uBoneTexWidth, (base + 3) / uBoneTexWidth), 0));
}

void main() {
    vec3 pos = aPosition;
    vec3 nrm = aNormal;
    // GPU blendshape morph (before skinning): sum coeff*delta over this vertex's
    // sparse channel list. Loop cap mirrors the extended-skinning influence loop.
    if (uHasMorph && aMorphOffsetCount.y > 0u) {
        int mbase = int(aMorphOffsetCount.x);
        int mcount = min(int(aMorphOffsetCount.y), 256);
        for (int i = 0; i < 256; ++i) {
            if (i >= mcount) break;
            // Cheap channelId fetch first; skip the wide delta fetch when this
            // channel is inactive (facial animation: most coeffs are 0). The
            // channel order is shared across vertices, so the branch is coherent.
            int ch = int(texelFetch(uMorphChanTex, mbase + i).r);
            float c = texelFetch(uMorphCoeffTex, ch).r;
            if (abs(c) < 1e-6) continue;
            pos += c * texelFetch(uMorphDeltaTex, mbase + i).yzw;
        }
    }
    float wsum = aWeight.x + aWeight.y + aWeight.z + aWeight.w;
    uint maxJoint = max(max(aJoint.x, aJoint.y), max(aJoint.z, aJoint.w));
    if (uSkinningEnabled && uExtendedSkinningEnabled && aInfluence.y > 0u && uInfluenceTexWidth > 0) {
        mat4 skin = mat4(0.0);
        float fullWeightSum = 0.0;
        int boneRows = uBoneMatrixCount;
        int base = int(aInfluence.x);
        int count = min(int(aInfluence.y), 256);
        for (int i = 0; i < 256; ++i) {
            if (i >= count) break;
            int linear = base + i;
            vec4 iw = texelFetch(uInfluenceTex,
                                 ivec2(linear % uInfluenceTexWidth,
                                       linear / uInfluenceTexWidth),
                                 0);
            uint joint = uint(iw.x + 0.5);
            float weight = iw.y;
            if (weight > 0.0 && int(joint) < boneRows) {
                skin += fetchBone(joint) * weight;
                fullWeightSum += weight;
            }
        }
        if (fullWeightSum > 0.0) {
            skin *= 1.0 / fullWeightSum;
            pos = (skin * vec4(pos, 1.0)).xyz;  // skin the morphed position
            nrm = normalize((skin * vec4(aNormal, 0.0)).xyz);
        }
    } else if (uSkinningEnabled && wsum > 0.0 && int(maxJoint) < uBoneMatrixCount) {
        mat4 skin =
            fetchBone(aJoint.x) * aWeight.x +
            fetchBone(aJoint.y) * aWeight.y +
            fetchBone(aJoint.z) * aWeight.z +
            fetchBone(aJoint.w) * aWeight.w;
        pos = (skin * vec4(pos, 1.0)).xyz;  // skin the morphed position
        nrm = normalize((skin * vec4(aNormal, 0.0)).xyz);
    }
    // Coarse displacement: offset along the (object-space) normal before the world
    // transform, so the displacement scales with the model like the geometry does.
    if (uHasDisplacement) {
        float d = uHasDisplacementTex
                      ? textureLod(uDisplacementTex, aUV.xy, 0.0).r *
                                uDisplacementTexScale +
                            uDisplacementTexBias
                      : uDisplacementConst;
        pos += normalize(nrm) * (d * uDisplacementScale);
    }
    vec4 worldPos = uModel * vec4(pos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = normalize(uNormalMatrix * nrm);
    vUV = aUV.xy;
    vColor = aColor;
    vUV1 = aUV1;
    vMorphInfl = aMorphInfl;
    // Dominant skin joint (SkinWeights AOV) from the base 4-weight set.
    vDomJoint = -1;
    vDomWeight = 0.0;
    if (aWeight.x > vDomWeight) { vDomWeight = aWeight.x; vDomJoint = int(aJoint.x); }
    if (aWeight.y > vDomWeight) { vDomWeight = aWeight.y; vDomJoint = int(aJoint.y); }
    if (aWeight.z > vDomWeight) { vDomWeight = aWeight.z; vDomJoint = int(aJoint.z); }
    if (aWeight.w > vDomWeight) { vDomWeight = aWeight.w; vDomJoint = int(aJoint.w); }
    gl_Position = uModelViewProj * vec4(pos, 1.0);
}
)glsl";
}

const char* getMaterialFragmentShaderGL330() {
#ifndef _MSC_VER
    return R"glsl(#version 330 core

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
flat in int vDomJoint;   // dominant skin joint (SkinWeights AOV)
in float vDomWeight;
in vec2 vUV1;            // 2nd texcoord set (multi-UV AOV)
in float vMorphInfl;     // blendshape influence (world units)

uniform int uClippingPlaneCount;
uniform vec4 uClippingPlanes[8];

// Material uniforms (one draw call per submesh)
uniform vec3 uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform vec3 uEmissive;
uniform float uAlpha;
uniform int uAlphaMode;       // 0=opaque, 1=mask, 2=blend
uniform float uAlphaCutoff;
// Specular F0 (T12): specular workflow -> specularColor directly; else the
// dielectric reflectance from ior. Unified with the Vulkan mesh.frag.
uniform int uUseSpecularWorkflow;
uniform int uOpenPbrSpecularModel;
uniform vec3 uSpecularColor;
uniform sampler2D uSpecularColorTex;
uniform bool uHasSpecularColorTex;
uniform vec3 uSpecularColorUv0;
uniform vec3 uSpecularColorUv1;
uniform int uSpecularColorUvSet;
uniform vec4 uSpecularColorScale;
uniform vec4 uSpecularColorBias;
uniform sampler2D uCoatNormalTex;
uniform bool uHasCoatNormalTex;
uniform vec3 uCoatNormalUv0;
uniform vec3 uCoatNormalUv1;
uniform int uCoatNormalUvSet;
uniform vec4 uCoatNormalScale;
uniform vec4 uCoatNormalBias;
// Advanced slots share otherwise-unused core UDIM array units per draw. The
// route indices select base/metallic/normal/opacity/emissive/roughness/
// occlusion respectively; -1 degrades to the neutral material constant when a
// material exhausts all seven routes.
uniform bvec4 uAdvancedTexIsUdim; // specular, coat weight/color/roughness
uniform ivec4 uAdvancedUdimRoutes;
uniform ivec4 uAdvancedUdimSlots;
uniform bool uCoatNormalTexIsUdim;
uniform int uCoatNormalUdimRoute;
uniform int uCoatNormalUdimSlot;
uniform float uIor;
uniform float uOcclusion;
uniform float uCoatWeight;
uniform vec3 uCoatColor;
uniform float uCoatRoughness;
uniform float uCoatIor;
uniform float uTransmission;
uniform vec3 uTransmissionColor;
uniform float uTransmissionDepth;
uniform vec3 uTransmissionScatter;
uniform float uTransmissionScatterAnisotropy;
uniform float uVolumeDensity;
uniform vec3 uVolumeAlbedo;
uniform vec3 uVolumeEmission;
uniform float uVolumeEmissionScale;
uniform float uDiffuseRoughness;
uniform float uTransmissionDispersionAbbeNumber;
uniform float uSubsurfaceAnisotropy;
uniform float uSubsurfaceScatterAnisotropy;
uniform float uSpecularAnisotropy;
uniform float uSpecularRotation;
uniform float uSpecularRoughnessAnisotropy;
uniform float uCoatAnisotropy;
uniform float uCoatRotation;
uniform float uCoatRoughnessAnisotropy;
uniform float uTransmissionDispersion;
uniform float uTransmissionDispersionScale;
uniform float uCoatAffectColor;
uniform float uCoatAffectRoughness;
uniform float uCoatDarkening;
uniform float uBaseWeight;
uniform float uSpecularWeight;
uniform float uSubsurface;
uniform vec3 uSubsurfaceColor;
uniform float uSubsurfaceScale;
uniform float uSubsurfaceRadius;
uniform float uSheenWeight;
uniform vec3 uSheenColor;
uniform float uSheenRoughness;
uniform float uThinFilmWeight;
uniform float uThinFilmThickness;
uniform float uThinFilmIor;
// Native OpenGL MaterialX graph path. The node table mirrors lusdview's
// bounded runtime IR. UDIM graph images reuse the semantic UDIM array routes
// when the source image is one of the material's resident semantic textures;
// unsupported graph-only UDIM sources use the deterministic fallback.
uniform bool uGraphUsable;
uniform int uGraphNodeCount;
uniform int uGraphOutputs[49]; // canonical OpenPBR route -> node index
uniform vec4 uGraphNode0[64]; // op, input0, input1, input2
uniform vec4 uGraphNode1[64];
uniform vec4 uGraphNode2[64];
uniform vec4 uGraphNode3[64];
uniform vec4 uGraphNode4[64]; // texture slot, scale.x, scale.y, offset.x
uniform vec4 uGraphNode5[64]; // offset.y, uv-input index
uniform sampler2D uGraphTex0;
uniform sampler2D uGraphTex1;
uniform sampler2D uGraphTex2;
uniform sampler2D uGraphTex3;
// GL 3.3 guarantees only 16 fragment samplers and common software drivers
// expose 32. Four ordinary graph images leave enough headroom for the
// semantic/UDIM/Ptex bindings; Vulkan retains the full eight-image budget.
uniform int uGraphUdimRoutes[8]; // local graph image -> semantic UDIM route
// When set, shade with the geometric (screen-derivative) normal -- used for
// meshes without authored normals so hard surfaces aren't smeared by smooth
// (averaged) normals.
uniform bool uGeometricNormal;

uniform vec3 uCameraPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
const int kMaxRasterLights = 16;
uniform int uLightCount;
uniform uint uLightMask;
uniform vec4 uLightPositionType[kMaxRasterLights];
uniform vec4 uLightDirectionAngle[kMaxRasterLights];
uniform vec4 uLightColorDiffuse[kMaxRasterLights];
uniform vec4 uLightSpecularShape[kMaxRasterLights];
uniform vec4 uLightAreaParams[kMaxRasterLights];
uniform vec4 uLightIesAxisX[kMaxRasterLights];
uniform vec4 uLightIesAxisY[kMaxRasterLights];
uniform vec4 uLightIesProfile[kMaxRasterLights * 6];

// DomeLight split-sum IBL (precomputed at load; replaces the constant ambient
// floor when present).
uniform bool uHasIbl;
uniform vec3 uIblColor;             // dome effectiveColor (intensity baked in)
uniform float uExposure;            // authored Camera exposure, in stops
uniform mat3 uEnvRotation;          // world -> environment direction
uniform samplerCube uIrradianceMap; // cosine-convolved env, stored E/pi
uniform samplerCube uPrefilteredMap;// GGX chain, lod = roughness*(lods-1)
uniform int uPrefilteredLods;
uniform bool uHasShadowMap;
uniform int uShadowLightSlot;
uniform mat4 uShadowViewProj;
uniform sampler2D uShadowMap;
uniform bool uHasPointShadowMap;
uniform vec3 uPointShadowLightPos;
uniform mat4 uPointShadowViewProj[6];
uniform samplerCube uPointShadowMap;

// Texture samplers
uniform sampler2D uBaseColorTex;
uniform sampler2DArray uBaseColorUdimTex;
uniform bool uHasBaseColorTex;
uniform bool uBaseColorTexIsUdim;
uniform sampler2D uMetallicTex;
uniform sampler2DArray uMetallicUdimTex;
uniform bool uHasMetallicTex;
uniform bool uMetallicTexIsUdim;
uniform sampler2D uRoughnessTex;
uniform sampler2DArray uRoughnessUdimTex;
uniform bool uHasRoughnessTex;
uniform bool uRoughnessTexIsUdim;
uniform sampler2D uNormalTex;
uniform sampler2DArray uNormalUdimTex;
uniform bool uHasNormalTex;
uniform bool uNormalTexIsUdim;
uniform sampler2D uEmissiveTex;
uniform sampler2DArray uEmissiveUdimTex;
uniform bool uHasEmissiveTex;
uniform bool uEmissiveTexIsUdim;
uniform sampler2D uOpacityTex;
uniform sampler2DArray uOpacityUdimTex;
uniform bool uHasOpacityTex;
uniform bool uOpacityTexIsUdim;
uniform sampler2D uOcclusionTex;
uniform sampler2DArray uOcclusionUdimTex;
uniform bool uHasOcclusionTex;
uniform bool uOcclusionTexIsUdim;
// One scene-wide 100 x texture-count atlas. Each row maps UDIM 1001..1100 to
// an array layer; -1 means the tile is absent. Consolidating four independent
// LUT samplers avoids the GL 3.3 fragment-sampler ceiling.
uniform isampler2D uUdimLutAtlas;
uniform ivec4 uUdimSlots;  // base, metallic, normal, emissive texture rows
uniform int uOpacityUdimSlot;
uniform int uRoughnessUdimSlot;
uniform int uOcclusionUdimSlot;
// Per-slot UV set: 0 = vUV (texcoords_0), 1 = vUV1 (texcoords_1).
// x = base color, y = metal/rough, z = normal, w = emissive.
uniform ivec4 uUvSet;
uniform int uRoughnessUvSet;
uniform vec3 uBaseColorUv0;   // m00,m01,tx
uniform vec3 uBaseColorUv1;   // m10,m11,ty
uniform vec3 uMetallicUv0;
uniform vec3 uMetallicUv1;
uniform vec3 uRoughnessUv0;
uniform vec3 uRoughnessUv1;
uniform vec3 uNormalUv0;
uniform vec3 uNormalUv1;
uniform vec3 uEmissiveUv0;
uniform vec3 uEmissiveUv1;
uniform vec3 uOpacityUv0;
uniform vec3 uOpacityUv1;
uniform vec3 uOcclusionUv0;
uniform vec3 uOcclusionUv1;
uniform vec4 uBaseColorTexScale;
uniform vec4 uBaseColorTexBias;
uniform vec4 uNormalTexScale;
uniform vec4 uNormalTexBias;
uniform vec4 uEmissiveTexScale;
uniform vec4 uEmissiveTexBias;
uniform int uMetallicChannel;
uniform int uRoughnessChannel;
uniform float uMetallicTexScale;
uniform float uMetallicTexBias;
uniform float uRoughnessTexScale;
uniform float uRoughnessTexBias;
uniform int uOpacityUvSet;
uniform int uOpacityChannel;
uniform float uOpacityTexScale;
uniform float uOpacityTexBias;
uniform int uOcclusionUvSet;
uniform int uOcclusionChannel;
uniform float uOcclusionTexScale;
uniform float uOcclusionTexBias;

// Coat lobe maps. Ordinary images use their dedicated 2D units. UDIM images
// route through a free core sampler2DArray unit selected per draw, avoiding an
// increase beyond the GL 3.3 32-fragment-unit floor.
uniform sampler2D uCoatWeightTex;
uniform sampler2D uCoatColorTex;
uniform sampler2D uCoatRoughnessTex;
uniform bool uHasCoatWeightTex;
uniform bool uHasCoatColorTex;
uniform bool uHasCoatRoughnessTex;
uniform vec3 uCoatWeightUv0;
uniform vec3 uCoatWeightUv1;
uniform vec3 uCoatColorUv0;
uniform vec3 uCoatColorUv1;
uniform vec3 uCoatRoughnessUv0;
uniform vec3 uCoatRoughnessUv1;
uniform int uCoatWeightUvSet;
uniform int uCoatColorUvSet;
uniform int uCoatRoughnessUvSet;
uniform int uCoatWeightChannel;
uniform int uCoatRoughnessChannel;
uniform vec4 uCoatWeightScale;
uniform vec4 uCoatWeightBias;
uniform vec4 uCoatColorScale;
uniform vec4 uCoatColorBias;
uniform vec4 uCoatRoughnessScale;
uniform vec4 uCoatRoughnessBias;

out vec4 fragColor;

uniform int uRenderMode;     // RenderMode (see renderer.hh)
uniform int uMatId;          // per-draw material id (material-id viz; -1 = none)
uniform float uDepthScale;   // depth AOV: camera-distance normalizer (scene extent)
uniform vec3 uSceneMin;      // position AOV: scene bbox min
uniform vec3 uSceneExtent;   // position AOV: scene bbox size (max-min)
uniform int uMeshId;         // mesh-id AOV (per-draw mesh index)
uniform bool uDoubleSided;   // double-sided AOV flag
uniform int uPurpose;        // purpose AOV: 0=default/1=render/2=proxy/3=guide
uniform int uKind;           // kind AOV: 0=none/1=component/2=group/3=assembly/4=subcomponent
uniform usamplerBuffer uFaceIdTex;  // per-triangle source face id (source-face-id AOV)
uniform int uFaceBase;       // first triangle of this submesh (gl_PrimitiveID is submesh-local)
uniform bool uHasFaceId;
// Base-color Ptex uses an embedded face-rectangle table. The source-face buffer
// supplies the record for the current triangle; UVs are intrinsic face-local.
uniform bool uBasePtex;
uniform usamplerBuffer uPtexRectTex;
uniform vec2 uBasePtexGrid; // rectangle texel offset, face count
uniform vec2 uMetallicPtexGrid;
uniform vec2 uRoughnessPtexGrid;
uniform vec2 uNormalPtexGrid;
uniform vec2 uEmissivePtexGrid;
uniform vec2 uOpacityPtexGrid;
uniform vec2 uOcclusionPtexGrid;
uniform vec2 uSpecularColorPtexGrid;
uniform vec2 uCoatWeightPtexGrid;
uniform vec2 uCoatColorPtexGrid;
uniform vec2 uCoatRoughnessPtexGrid;

// Stable distinct color per material id (-1 -> neutral gray).
vec3 idColor(int id) {
    if (id < 0) return vec3(0.45);
    uint h = (uint(id) + 1u) * 2654435761u;
    return vec3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) * (1.0 / 255.0);
}

// USD purpose -> distinct color (shared scheme across all backends).
vec3 purposeColor(int p) {
    if (p == 1) return vec3(0.2, 0.8, 0.3);    // render: green
    if (p == 2) return vec3(0.2, 0.45, 0.95);  // proxy: blue
    if (p == 3) return vec3(0.95, 0.75, 0.1);  // guide: amber
    return vec3(0.5);                          // default: gray
}

// USD model kind -> distinct color (shared scheme across all backends).
vec3 kindColor(int k) {
    if (k == 1) return vec3(0.2, 0.8, 0.8);    // component: cyan
    if (k == 2) return vec3(0.85, 0.3, 0.85);  // group: magenta
    if (k == 3) return vec3(0.95, 0.6, 0.15);  // assembly: orange
    if (k == 4) return vec3(0.5, 0.85, 0.4);   // subcomponent: green
    return vec3(0.35);                         // no kind: dark gray
}

vec4 sampleUdim(sampler2DArray tex, int slot, vec2 uv, vec4 missing) {
    ivec2 tile = ivec2(floor(uv));
    int idx = tile.x + tile.y * 10;
    if (idx < 0 || idx >= 100) {
        return missing;
    }
    int layer = texelFetch(uUdimLutAtlas, ivec2(idx, slot), 0).r;
    if (layer < 0) {
        return missing;
    }
    return texture(tex, vec3(fract(uv), float(layer)));
}

vec4 sampleRoutedUdim(int route, int slot, vec2 uv, vec4 missing) {
    if (route == 0) return sampleUdim(uBaseColorUdimTex, slot, uv, missing);
    if (route == 1) return sampleUdim(uMetallicUdimTex, slot, uv, missing);
    if (route == 2) return sampleUdim(uNormalUdimTex, slot, uv, missing);
    if (route == 3) return sampleUdim(uOpacityUdimTex, slot, uv, missing);
    if (route == 4) return sampleUdim(uEmissiveUdimTex, slot, uv, missing);
    if (route == 5) return sampleUdim(uRoughnessUdimTex, slot, uv, missing);
    if (route == 6) return sampleUdim(uOcclusionUdimTex, slot, uv, missing);
    return missing;
}

vec2 xformUv(vec2 uv, vec3 row0, vec3 row1) {
    return vec2(dot(vec3(uv, 1.0), row0), dot(vec3(uv, 1.0), row1));
}

vec4 sampleGraphImage(int slot, vec2 uv, vec4 missing) {
    if (slot < 0 || slot >= 4) return missing;
    if (slot == 0) return texture(uGraphTex0, uv);
    if (slot == 1) return texture(uGraphTex1, uv);
    if (slot == 2) return texture(uGraphTex2, uv);
    if (slot == 3) return texture(uGraphTex3, uv);
    return missing;
}

vec4 sampleGraphImageUdim(int slot, float row, vec2 uv, vec4 missing) {
    if (slot < 0 || slot >= 8 || row < 0.0) return missing;
    int route = uGraphUdimRoutes[slot];
    return route >= 0 ? sampleRoutedUdim(route, int(row + 0.5), uv, missing)
                      : missing;
}

int graphOutput(int route) {
    if (route < 0 || route >= 49) return -1;
    return uGraphOutputs[route];
}

bool hasGraphRoute(int route) {
    if (!uGraphUsable || route < 0 || route >= 49) return false;
    int wanted = graphOutput(route);
    return wanted >= 0 && wanted < uGraphNodeCount;
}

vec4 gGraphResults[49];

void evalMaterialXGraph(vec2 uv) {
    for (int route = 0; route < 49; ++route) gGraphResults[route] = vec4(0.0);
    if (!uGraphUsable || uGraphNodeCount <= 0) return;
    vec4 values[64];
    for (int i = 0; i < 64; ++i) values[i] = vec4(0.0);
    // CompileMaterialXGraphRuntime stores dependencies before consumers, so
    // one bounded pass replaces the old 64-pass fixed-point interpreter.
    for (int i = 0; i < 64; ++i) {
            if (i >= uGraphNodeCount) break;
            int op = int(uGraphNode0[i].x + 0.5);
            int ia = int(uGraphNode0[i].y + (uGraphNode0[i].y < 0.0 ? -0.5 : 0.5));
            int ib = int(uGraphNode0[i].z + (uGraphNode0[i].z < 0.0 ? -0.5 : 0.5));
            int ic = int(uGraphNode0[i].w + (uGraphNode0[i].w < 0.0 ? -0.5 : 0.5));
            vec4 a = ia >= 0 && ia < uGraphNodeCount ? values[ia] : uGraphNode1[i];
            vec4 b = ib >= 0 && ib < uGraphNodeCount ? values[ib] : uGraphNode2[i];
            vec4 c = ic >= 0 && ic < uGraphNodeCount ? values[ic] : uGraphNode3[i];
            vec4 value = a;
            if (op == 0) values[i] = a;
            else if (op == 1 || op == 2) {
                vec2 guv = uv;
                int ui = int(uGraphNode5[i].y + 0.5);
                if (ui == 0) guv = a.xy;
                else if (ui == 1) guv = b.xy;
                else if (ui == 2) guv = c.xy;
                guv = guv * uGraphNode4[i].yz +
                      vec2(uGraphNode4[i].w, uGraphNode5[i].x);
                float encodedSlot = uGraphNode4[i].x;
                if (encodedSlot < 0.0) {
                    int slot = int(-encodedSlot - 0.5);
                    values[i] = sampleGraphImageUdim(slot, uGraphNode5[i].z,
                                                     guv, value);
                } else {
                    values[i] = sampleGraphImage(int(encodedSlot + 0.5),
                                                 guv, value);
                }
            } else if (op == 3) values[i] = vec4(normalize(a.xyz * 2.0 - 1.0) * 0.5 + 0.5, a.w);
            else if (op == 4) values[i] = a + b;
            else if (op == 5) values[i] = a - b;
            else if (op == 6) values[i] = a * b;
            else if (op == 7) values[i] = a / max(abs(b), vec4(1e-6));
            else if (op == 8) values[i] = mix(a, b, c);
            else if (op == 9) values[i] = clamp(a, b, c);
            else if (op == 10) values[i] = vec4(dot(a.xyz, b.xyz));
            else if (op == 11) values[i] = vec4(normalize(a.xyz), a.w);
            else if (op == 12) values[i] = pow(max(a, vec4(0.0)), b);
            else if (op == 13) values[i] = min(a, b);
            else if (op == 14) values[i] = max(a, b);
            else if (op == 15) values[i] = abs(a);
            else if (op == 16) values[i] = sqrt(max(a, vec4(0.0)));
            else if (op == 17) values[i] = sin(a);
            else if (op == 18) values[i] = cos(a);
            else if (op == 19) values[i] = vec4(dot(a.xyz, vec3(0.2126, 0.7152, 0.0722)));
            else if (op == 20) values[i] = a.x >= 0.5 ? b : c;
            else if (op == 21) {
                vec2 tc = uGraphNode3[i].z > 0.5 ? vUV1 : uv;
                values[i] = vec4(tc, 0.0, 1.0);
            }
            else if (op == 22) values[i] = floor(a);
            else if (op == 23) values[i] = ceil(a);
            else if (op == 24) values[i] = fract(a);
            else if (op == 25) values[i] = step(a, b);
            else if (op == 26) values[i] = smoothstep(b, c, a);
            else if (op == 27) values[i] = vec4(cross(a.xyz, b.xyz), a.w);
            else if (op == 28) values[i] = vec4(length(a.xyz));
            else if (op == 29) values[i] = vec4(fract(sin(dot(a.xy + uv, vec2(127.1, 311.7))) * 43758.5453));
            else if (op == 40) values[i] = vec4(fract(sin(dot(a.xyz + vec3(uv, uv.x + uv.y),
                                                                        vec3(127.1, 311.7, 74.7))) *
                                                           43758.5453));
            else if (op == 30) values[i] = tan(a);
            else if (op == 31) values[i] = exp(a);
            else if (op == 32) values[i] = log(max(a, vec4(1e-6)));
            else if (op == 33) values[i] = mod(a, max(abs(b), vec4(1e-6)));
            else if (op == 41) values[i] = atan(a, b);
            else if (op == 42) values[i] = sign(a);
            else if (op == 43) values[i] = round(a);
            else if (op == 34) values[i] = vec4(1.0) - a;
            else if (op == 35) values[i] = (a - b) / max(c - b, vec4(1e-6));
            else if (op == 36) values[i] = vec4(0.0, 0.0, 1.0, 1.0);
            else if (op == 37) values[i] = vec4(1.0, 0.0, 0.0, 1.0);
            else if (op == 38) {
                vec3 axis = normalize(b.xyz);
                float angle = radians(a.x);
                vec3 q = c.xyz;
                values[i] = vec4(q * cos(angle) + cross(axis, q) * sin(angle) +
                                  axis * dot(axis, q) * (1.0 - cos(angle)), c.w);
            }
            else if (op == 39) {
                vec2 t = a.xy * uGraphNode4[i].yz +
                         vec2(uGraphNode4[i].w, uGraphNode5[i].x);
                float a2 = radians(uGraphNode3[i].w);
                values[i] = vec4(vec2(t.x * cos(a2) - t.y * sin(a2),
                                      t.x * sin(a2) + t.y * cos(a2)), a.zw);
            }
            else values[i] = value;
    }
    for (int route = 0; route < 49; ++route) {
        int wanted = graphOutput(route);
        if (wanted >= 0 && wanted < uGraphNodeCount)
            gGraphResults[route] = values[wanted];
    }
}

vec2 ptexUv(sampler2D tex, vec2 uv, vec2 grid) {
    if (grid.y <= 0.5 || !uHasFaceId) return uv;
    int face = int(texelFetch(uFaceIdTex, uFaceBase + gl_PrimitiveID).r);
    int count = int(grid.y + 0.5);
    if (face < 0 || face >= count) return uv;
    ivec2 size = textureSize(tex, 0);
    int base = int(grid.x + 0.5) + face * 4;
    uint value[4];
    for (int component = 0; component < 4; ++component)
        value[component] = texelFetch(uPtexRectTex, base + component).r;
    if (value[2] == 0u || value[3] == 0u) return uv;
    vec2 t = clamp(uv, 0.0, 1.0);
    vec2 px = vec2(value[0], value[1]) +
              vec2(t.x, 1.0 - t.y) * vec2(value[2] - 1u, value[3] - 1u);
    return (px + vec2(0.5)) / vec2(size);
}

// Linear -> sRGB OETF for the final shaded output. sRGB base-color textures are
// uploaded as GL_SRGB8_ALPHA8 (linearized on sample) and the scene is lit in
// linear space, so the encode happens here (the FBO is plain RGBA8). Lit path
// only -- AOVs stay raw. Matches the Vulkan mesh.frag.
vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, vec3(greaterThan(c, vec3(0.0031308))));
}

// Specular F0 (T12), matching the Vulkan mesh.frag: specular workflow ->
// specularColor; else the dielectric reflectance from ior lerped to base by
// metalness. ior 1.5 (the default) gives exactly 0.04.
vec3 computeF0(vec3 base, float metallic, vec3 specularColor,
               float iorInput, int useSpecularWorkflow,
               int openPbrSpecularModel) {
    if (useSpecularWorkflow != 0) return specularColor;
    float ior = max(1.0, iorInput);
    float d = (ior - 1.0) / (ior + 1.0);
    vec3 dielectric = vec3(d * d);
    if (openPbrSpecularModel != 0) dielectric *= specularColor;
    return mix(dielectric, base, clamp(metallic, 0.0, 1.0));
}

const float kPi = 3.14159265358979323846;

float distributionGGX(float NoH, float roughness) {
    float a = max(roughness * roughness, 0.002);
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-6);
}

float distributionAnisotropicGGX(float NoH, float ToH, float BoH,
                                 float roughness, float anisotropy) {
    float a = max(roughness * roughness, 0.002);
    float aspect = sqrt(max(1.0 - clamp(anisotropy, -0.95, 0.95) * 0.8,
                            0.05));
    float ax = max(a / aspect, 0.002);
    float ay = max(a * aspect, 0.002);
    float q = (ToH * ToH) / ax + (BoH * BoH) / ay + NoH * NoH;
    return 1.0 / max(kPi * sqrt(ax * ay) * q * q, 1e-6);
}

float geometrySchlickGGX(float NoX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return NoX / max(NoX * (1.0 - k) + k, 1e-6);
}
)glsl" R"glsl(
vec3 fresnelSchlick(float VoH, vec3 f0) {
    float f = pow(1.0 - clamp(VoH, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

// Compact thin-film approximation for the raster preview.  It preserves the
// authored base F0 at zero thickness and adds the familiar view-dependent
// spectral shift without requiring a second shader variant or an LUT.
vec3 applyThinFilm(vec3 f0, float cosTheta, float filmWeight,
                   float filmThickness, float filmIor) {
    float w = clamp(filmWeight, 0.0, 1.0);
    if (w <= 0.0 || filmThickness <= 0.0) return f0;
    float eta = max(filmIor, 1.0);
    float phase = 6.28318530718 * eta * filmThickness *
                  (1.0 - clamp(cosTheta, 0.0, 1.0));
    vec3 film = 0.5 + 0.5 * cos(phase * vec3(1.0, 1.17, 1.35) +
                                vec3(0.0, 2.1, 4.2));
    return mix(f0, clamp(f0 + (vec3(1.0) - f0) * film * 0.65, 0.0, 1.0), w);
}

float channelOf(vec4 c, int ch) {
    if (ch == 1) return c.g;
    if (ch == 2) return c.b;
    if (ch == 3) return c.a;
    return c.r;
}

float sampleOcclusion() {
    if (!uHasOcclusionTex) return 1.0;
    vec2 uv = (uOcclusionUvSet == 1) ? vUV1 : vUV;
    vec2 tuv = xformUv(uv, uOcclusionUv0, uOcclusionUv1);
    tuv = ptexUv(uOcclusionTex, tuv, uOcclusionPtexGrid);
    vec4 c = uOcclusionTexIsUdim
        ? sampleUdim(uOcclusionUdimTex, uOcclusionUdimSlot, tuv, vec4(1.0))
        : texture(uOcclusionTex, tuv);
    return clamp(channelOf(c, uOcclusionChannel) * uOcclusionTexScale +
                 uOcclusionTexBias, 0.0, 1.0);
}

// Scalar/color fetches for the coat and specular-workflow slots. The material
// constant is neutralized to 1 by the loaders whenever a texture is bound, so
// the caller multiplies the two unconditionally.
float sampleCoatScalar(sampler2D tex, bool has, bool isUdim, int udimRoute,
                       int udimSlot, vec3 uv0, vec3 uv1, int uvSet, int ch,
                       vec4 scale, vec4 bias, vec2 ptexGrid) {
    if (!has) return 1.0;
    vec2 uv = (uvSet == 1) ? vUV1 : vUV;
    uv = xformUv(uv, uv0, uv1);
    uv = ptexUv(tex, uv, ptexGrid);
    vec4 texel = isUdim ? sampleRoutedUdim(udimRoute, udimSlot, uv, vec4(1.0))
                        : texture(tex, uv);
    vec4 c = texel * scale + bias;
    return clamp(channelOf(c, ch), 0.0, 1.0);
}

vec3 sampleCoatColor(sampler2D tex, bool has, bool isUdim, int udimRoute,
                     int udimSlot, vec3 uv0, vec3 uv1, int uvSet, vec4 scale,
                     vec4 bias, vec2 ptexGrid) {
    if (!has) return vec3(1.0);
    vec2 uv = (uvSet == 1) ? vUV1 : vUV;
    uv = xformUv(uv, uv0, uv1);
    uv = ptexUv(tex, uv, ptexGrid);
    vec4 texel = isUdim ? sampleRoutedUdim(udimRoute, udimSlot, uv, vec4(1.0))
                        : texture(tex, uv);
    return (texel * scale + bias).rgb;
}

float sampleShadow(vec3 worldPos, vec3 normal, vec3 lightDir) {
    if (uHasPointShadowMap) {
    vec3 d = worldPos - uPointShadowLightPos;
    vec3 a = abs(d);
    int face;
    if (a.x >= a.y && a.x >= a.z) face = d.x >= 0.0 ? 0 : 1;
    else if (a.y >= a.z) face = d.y >= 0.0 ? 2 : 3;
    else face = d.z >= 0.0 ? 4 : 5;
    vec4 clip = uPointShadowViewProj[face] * vec4(worldPos, 1.0);
    vec3 p = clip.xyz / clip.w;
    p = p * 0.5 + 0.5;
    if (p.z <= 0.0 || p.z >= 1.0) return 1.0;
    float bias = max(0.00035, 0.0015 * (1.0 - max(dot(normal, lightDir), 0.0)));
    return (p.z - bias <= texture(uPointShadowMap, normalize(d)).r) ? 1.0 : 0.0;
    }
    if (!uHasShadowMap) return 1.0;
    vec4 clip = uShadowViewProj * vec4(worldPos, 1.0);
    vec3 p = clip.xyz / clip.w;
    p = p * 0.5 + 0.5;
    if (p.z <= 0.0 || p.z >= 1.0 || any(lessThan(p.xy, vec2(0.0))) ||
        any(greaterThan(p.xy, vec2(1.0)))) return 1.0;
    float bias = max(0.00035, 0.0015 * (1.0 - max(dot(normal, lightDir), 0.0)));
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x)
        visible += p.z - bias <= texture(uShadowMap, p.xy + vec2(x, y) * texel).r
                       ? 1.0 : 0.0;
    return visible / 9.0;
}

void main() {
    for (int i = 0; i < uClippingPlaneCount; ++i) {
        if (dot(vec4(vWorldPos, 1.0), uClippingPlanes[i]) < 0.0) discard;
    }
    vec3 displayColor = vColor.rgb;
    float displayOpacity = vColor.a;
    vec3 baseColor = uBaseColor * displayColor;  // displayColor defaults to white
    // base_weight is an OpenPBR input. UsdPreviewSurface has no equivalent and
    // must retain its full diffuse contribution; its shared CPU payload leaves
    // the OpenPBR-only field at zero.
    float baseWeight = uOpenPbrSpecularModel != 0
                           ? clamp(uBaseWeight, 0.0, 1.0)
                           : 1.0;
    float metallic = uMetallic;
    float roughness = uRoughness;
    vec3 emissive = uEmissive;
    float volumeDensity = uVolumeDensity;
    vec3 volumeAlbedo = uVolumeAlbedo;
    vec3 volumeEmission = uVolumeEmission;
    float volumeEmissionScale = uVolumeEmissionScale;
    float opacity = clamp(uAlpha * displayOpacity, 0.0, 1.0);
    // The graph interpreter is dependency-first and produces every route in
    // one pass. This is intentionally called once even when only the six
    // legacy semantic lanes are used; it keeps advanced OpenPBR routes on the
    // same precedence and fallback rules as Vulkan.
    evalMaterialXGraph(vUV);
    vec4 graphBase = gGraphResults[0];
    vec4 graphMetal = gGraphResults[1];
    vec4 graphRough = gGraphResults[2];
    vec4 graphOpacity = gGraphResults[3];
    vec4 graphEmission = gGraphResults[4];
    vec4 graphNormal = gGraphResults[5];
    vec4 graphSubsurface = gGraphResults[6];
    vec4 graphSubsurfaceColor = gGraphResults[7];
    vec4 graphSubsurfaceRadius = gGraphResults[8];
    float specularWeight = uSpecularWeight;
    vec3 specularColor = uSpecularColor;
    float specularIor = uIor;
    float diffuseRoughness = uDiffuseRoughness;
    float transmissionWeight = uTransmission;
    vec3 transmissionColor = uTransmissionColor;
    float transmissionDepth = uTransmissionDepth;
    vec3 transmissionScatter = uTransmissionScatter;
    float transmissionScatterAnisotropy = uTransmissionScatterAnisotropy;
    float subsurfaceScale = uSubsurfaceScale;
    float subsurfaceAnisotropy = uSubsurfaceAnisotropy;
    float subsurfaceScatterAnisotropy = uSubsurfaceScatterAnisotropy;
    float coatWeight = uCoatWeight;
    vec3 coatColor = uCoatColor;
    float coatRoughness = uCoatRoughness;
    float coatIor = uCoatIor;
    float sheenWeight = uSheenWeight;
    vec3 sheenColor = uSheenColor;
    float sheenRoughness = uSheenRoughness;
    float thinFilmWeight = uThinFilmWeight;
    float thinFilmThickness = uThinFilmThickness;
    float thinFilmIor = uThinFilmIor;
    float specularAnisotropy = uSpecularAnisotropy;
    float specularRotation = uSpecularRotation;
    float specularRoughnessAnisotropy = uSpecularRoughnessAnisotropy;
    float transmissionDispersion = uTransmissionDispersion;
    float transmissionDispersionAbbeNumber = uTransmissionDispersionAbbeNumber;
    float transmissionDispersionScale = uTransmissionDispersionScale;
    float coatAnisotropy = uCoatAnisotropy;
    float coatRotation = uCoatRotation;
    float coatRoughnessAnisotropy = uCoatRoughnessAnisotropy;
    float coatAffectColor = uCoatAffectColor;
    float coatAffectRoughness = uCoatAffectRoughness;
    float coatDarkening = uCoatDarkening;
    float emissionLuminance = 1.0;
    float subsurfaceWeight = clamp(uSubsurface, 0.0, 1.0);
    vec3 subsurfaceColor = uSubsurfaceColor;
    if (hasGraphRoute(6)) subsurfaceWeight = clamp(graphSubsurface.x, 0.0, 1.0);
    if (hasGraphRoute(7)) subsurfaceColor = max(graphSubsurfaceColor.rgb, vec3(0.0));
    float subsurfaceRadius = 1.0;
    if (hasGraphRoute(8)) subsurfaceRadius = max(dot(graphSubsurfaceRadius.rgb,
                                                     vec3(0.2126, 0.7152, 0.0722)), 0.0);

    // Advanced OpenPBR routes are resolved here once for both the direct-light
    // and IBL paths. A graph route is authoritative over the packed constant;
    // the CPU bridge disables routes that require a deterministic fallback.
    if (hasGraphRoute(9)) specularWeight = clamp(gGraphResults[9].x, 0.0, 1.0);
    if (hasGraphRoute(10)) specularColor = max(gGraphResults[10].rgb, vec3(0.0));
    if (hasGraphRoute(11)) transmissionWeight = clamp(gGraphResults[11].x, 0.0, 1.0);
    if (hasGraphRoute(12)) transmissionColor = max(gGraphResults[12].rgb, vec3(0.0));
    if (hasGraphRoute(13)) coatWeight = clamp(gGraphResults[13].x, 0.0, 1.0);
    if (hasGraphRoute(14)) coatColor = max(gGraphResults[14].rgb, vec3(0.0));
    if (hasGraphRoute(15)) coatRoughness = clamp(gGraphResults[15].x, 0.02, 1.0);
    if (hasGraphRoute(16)) sheenWeight = clamp(gGraphResults[16].x, 0.0, 1.0);
    if (hasGraphRoute(17)) sheenColor = max(gGraphResults[17].rgb, vec3(0.0));
    if (hasGraphRoute(18)) sheenRoughness = clamp(gGraphResults[18].x, 0.02, 1.0);
    if (hasGraphRoute(19)) specularIor = max(gGraphResults[19].x, 1.0);
    if (hasGraphRoute(20)) baseWeight = clamp(gGraphResults[20].x, 0.0, 1.0);
    if (hasGraphRoute(21)) diffuseRoughness = clamp(gGraphResults[21].x, 0.0, 1.0);
    if (hasGraphRoute(22)) transmissionScatter = max(gGraphResults[22].rgb, vec3(0.0));
    if (hasGraphRoute(23)) transmissionDepth = max(gGraphResults[23].x, 0.0);
    if (hasGraphRoute(24)) transmissionScatterAnisotropy = clamp(gGraphResults[24].x, -1.0, 1.0);
    if (hasGraphRoute(25)) subsurfaceScale = max(gGraphResults[25].x, 0.0);
    if (hasGraphRoute(26)) subsurfaceAnisotropy = clamp(gGraphResults[26].x, -1.0, 1.0);
    if (hasGraphRoute(27)) coatIor = max(gGraphResults[27].x, 1.0);
    if (hasGraphRoute(28)) thinFilmWeight = clamp(gGraphResults[28].x, 0.0, 1.0);
    if (hasGraphRoute(29)) thinFilmThickness = max(gGraphResults[29].x, 0.0);
    if (hasGraphRoute(30)) thinFilmIor = max(gGraphResults[30].x, 1.0);
    if (hasGraphRoute(31)) specularAnisotropy = clamp(gGraphResults[31].x, -1.0, 1.0);
    if (hasGraphRoute(32)) specularRotation = gGraphResults[32].x;
    if (hasGraphRoute(33)) specularRoughnessAnisotropy = clamp(gGraphResults[33].x, -1.0, 1.0);
    if (hasGraphRoute(34)) transmissionDispersion = max(gGraphResults[34].x, 0.0);
    if (hasGraphRoute(35)) transmissionDispersionAbbeNumber = max(gGraphResults[35].x, 0.0);
    if (hasGraphRoute(36)) transmissionDispersionScale = max(gGraphResults[36].x, 0.0);
    if (hasGraphRoute(37)) coatAnisotropy = clamp(gGraphResults[37].x, -1.0, 1.0);
    if (hasGraphRoute(38)) coatRotation = gGraphResults[38].x;
    if (hasGraphRoute(39)) coatRoughnessAnisotropy = clamp(gGraphResults[39].x, -1.0, 1.0);
    if (hasGraphRoute(40)) volumeDensity = max(gGraphResults[40].x, 0.0);
    if (hasGraphRoute(41)) volumeAlbedo = max(gGraphResults[41].rgb, vec3(0.0));
    if (hasGraphRoute(42)) volumeEmission = max(gGraphResults[42].rgb, vec3(0.0));
    if (hasGraphRoute(43)) volumeEmissionScale = max(gGraphResults[43].x, 0.0);
    if (hasGraphRoute(44)) emissionLuminance = max(gGraphResults[44].x, 0.0);
    if (hasGraphRoute(45)) coatAffectColor = clamp(gGraphResults[45].x, 0.0, 1.0);
    if (hasGraphRoute(46)) coatAffectRoughness = clamp(gGraphResults[46].x, 0.0, 1.0);
    if (hasGraphRoute(47)) coatDarkening = clamp(gGraphResults[47].x, 0.0, 1.0);
    if (hasGraphRoute(48)) subsurfaceScatterAnisotropy = clamp(gGraphResults[48].x, -1.0, 1.0);

    if (hasGraphRoute(0)) baseColor = graphBase.rgb * displayColor;
    if (!hasGraphRoute(0) && uHasBaseColorTex) {
        vec2 uv = xformUv(uUvSet.x == 1 ? vUV1 : vUV, uBaseColorUv0, uBaseColorUv1);
        if (uBasePtex && uHasFaceId) {
            int face = int(texelFetch(uFaceIdTex, uFaceBase + gl_PrimitiveID).r);
            int count = int(uBasePtexGrid.y + 0.5);
            if (face >= 0 && face < count) {
                ivec2 size = textureSize(uBaseColorTex, 0);
                int base = int(uBasePtexGrid.x + 0.5) + face * 4;
                uint value[4];
                for (int component = 0; component < 4; ++component)
                    value[component] = texelFetch(uPtexRectTex,
                                                  base + component).r;
                vec2 px = vec2(float(value[0]), float(value[1])) +
                          vec2(clamp(uv.x, 0.0, 1.0),
                               1.0 - clamp(uv.y, 0.0, 1.0)) *
                          vec2(float(max(value[2], 1u) - 1u),
                               float(max(value[3], 1u) - 1u));
                uv = (px + vec2(0.5)) / vec2(size);
            }
        }
        vec4 texel = uBaseColorTexIsUdim
                         ? sampleUdim(uBaseColorUdimTex, uUdimSlots.x, uv,
                                      vec4(1.0, 0.0, 1.0, 1.0))
                         : texture(uBaseColorTex, uv);
        vec4 sample = texel * uBaseColorTexScale + uBaseColorTexBias;
        baseColor *= sample.rgb;
        opacity *= clamp(sample.a, 0.0, 1.0);
    }
    if (hasGraphRoute(1)) metallic = clamp(graphMetal.x, 0.0, 1.0);
    if (!hasGraphRoute(1) && uHasMetallicTex) {
        vec2 uv = xformUv(uUvSet.y == 1 ? vUV1 : vUV, uMetallicUv0, uMetallicUv1);
        uv = ptexUv(uMetallicTex, uv, uMetallicPtexGrid);
        vec4 texel = uMetallicTexIsUdim
                      ? sampleUdim(uMetallicUdimTex, uUdimSlots.y, uv, vec4(1.0))
                      : texture(uMetallicTex, uv);
        metallic *= channelOf(texel, uMetallicChannel) * uMetallicTexScale + uMetallicTexBias;
    }
    if (hasGraphRoute(2)) roughness = clamp(graphRough.x, 0.02, 1.0);
    if (!hasGraphRoute(2) && uHasRoughnessTex) {
        vec2 uv = xformUv(uRoughnessUvSet == 1 ? vUV1 : vUV, uRoughnessUv0, uRoughnessUv1);
        uv = ptexUv(uRoughnessTex, uv, uRoughnessPtexGrid);
        vec4 texel = uRoughnessTexIsUdim
                      ? sampleUdim(uRoughnessUdimTex, uRoughnessUdimSlot, uv, vec4(1.0))
                      : texture(uRoughnessTex, uv);
        roughness *= channelOf(texel, uRoughnessChannel) * uRoughnessTexScale + uRoughnessTexBias;
    }
    if (hasGraphRoute(4)) emissive = max(graphEmission.rgb, vec3(0.0));
    if (!hasGraphRoute(4) && uHasEmissiveTex) {
        vec2 uv = xformUv(uUvSet.w == 1 ? vUV1 : vUV, uEmissiveUv0, uEmissiveUv1);
        uv = ptexUv(uEmissiveTex, uv, uEmissivePtexGrid);
        vec4 texel = uEmissiveTexIsUdim
                         ? sampleUdim(uEmissiveUdimTex, uUdimSlots.w, uv,
                                      vec4(1.0, 0.0, 1.0, 1.0))
                         : texture(uEmissiveTex, uv);
        emissive *= (texel * uEmissiveTexScale + uEmissiveTexBias).rgb;
    }
    float volumeOpacity = clamp(1.0 - exp(-max(volumeDensity, 0.0) * 0.1),
                                0.0, 1.0);
    baseColor = mix(baseColor, baseColor * volumeAlbedo, volumeOpacity);
    emissive = emissive * emissionLuminance +
               volumeEmission * volumeEmissionScale * volumeOpacity;

    vec3 N = uGeometricNormal
                 ? normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)))
                 : normalize(vNormal);
    if (uHasNormalTex) {
        vec2 uv = xformUv(uUvSet.z == 1 ? vUV1 : vUV, uNormalUv0, uNormalUv1);
        uv = ptexUv(uNormalTex, uv, uNormalPtexGrid);
        vec3 tangentNormal = ((uNormalTexIsUdim
                                  ? sampleUdim(uNormalUdimTex, uUdimSlots.z, uv,
                                               vec4(0.5, 0.5, 1.0, 1.0))
                                  : texture(uNormalTex, uv)) * uNormalTexScale +
                              uNormalTexBias).xyz;
        // Full derivative TBN (unified with the Vulkan backend). The old
        // `normalize(N + tangentNormal*0.1)` was a weak non-TBN perturbation
        // that made relief nearly flat on GL and pronounced on VK; build the
        // tangent frame from the screen-space position/UV gradients so the
        // tangent-space sample perturbs N at full strength in the right basis.
        vec3 dp1 = dFdx(vWorldPos);
        vec3 dp2 = dFdy(vWorldPos);
        vec2 du1 = dFdx(uv);
        vec2 du2 = dFdy(uv);
        float r = du1.x * du2.y - du2.x * du1.y;
        vec3 t = dp1 * du2.y - dp2 * du1.y;
        if (abs(r) > 1e-8) {
            t /= r;
        } else {
            // A constant/degenerate UV derivative must not feed a zero or
            // normal-parallel tangent into normalize(): that produces NaNs,
            // which encode as black in the AOV. Pick a stable tangent instead.
            vec3 ref = (abs(N.z) < 0.9) ? vec3(0.0, 0.0, 1.0)
                                       : vec3(0.0, 1.0, 0.0);
            t = cross(ref, N);
        }
        t = normalize(t - N * dot(N, t));
        vec3 b = normalize(cross(N, t)) * (r < 0.0 ? -1.0 : 1.0);
        N = normalize(mat3(t, b, N) * tangentNormal);
    }
    if (hasGraphRoute(5)) N = normalize(graphNormal.xyz * 2.0 - 1.0);
    vec3 coatN = N;
    if (uHasCoatNormalTex) {
        vec2 uv = xformUv(uCoatNormalUvSet == 1 ? vUV1 : vUV,
                          uCoatNormalUv0, uCoatNormalUv1);
        vec4 coatTexel = uCoatNormalTexIsUdim
                             ? sampleRoutedUdim(uCoatNormalUdimRoute,
                                                uCoatNormalUdimSlot, uv,
                                                vec4(0.5, 0.5, 1.0, 1.0))
                             : texture(uCoatNormalTex, uv);
        vec3 tn = (coatTexel * uCoatNormalScale +
                   uCoatNormalBias).xyz;
        vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
        vec2 du1 = dFdx(uv), du2 = dFdy(uv);
        float rr = du1.x * du2.y - du2.x * du1.y;
        vec3 t = dp1 * du2.y - dp2 * du1.y;
        if (abs(rr) > 1e-8) {
            t /= rr;
        } else {
            vec3 ref = (abs(N.z) < 0.9) ? vec3(0.0, 0.0, 1.0)
                                       : vec3(0.0, 1.0, 0.0);
            t = cross(ref, N);
        }
        t = normalize(t - N * dot(N, t));
        vec3 b = normalize(cross(N, t)) * (rr < 0.0 ? -1.0 : 1.0);
        coatN = normalize(mat3(t, b, N) * tn);
    }
    if (hasGraphRoute(3)) opacity = clamp(uAlpha * graphOpacity.x * displayOpacity, 0.0, 1.0);
    if (!hasGraphRoute(3) && uHasOpacityTex) {
        vec2 uv = xformUv(uOpacityUvSet == 1 ? vUV1 : vUV,
                          uOpacityUv0, uOpacityUv1);
        uv = ptexUv(uOpacityTex, uv, uOpacityPtexGrid);
        // Missing opacity UDIM tiles are opaque, not magenta/channel-dependent.
        vec4 ot = uOpacityTexIsUdim
                      ? sampleUdim(uOpacityUdimTex, uOpacityUdimSlot, uv, vec4(1.0))
                      : texture(uOpacityTex, uv);
        opacity *= clamp(channelOf(ot, uOpacityChannel) * uOpacityTexScale +
                         uOpacityTexBias, 0.0, 1.0);
    }
    opacity = clamp(opacity, 0.0, 1.0);
    if (uAlphaMode == 1) {
        opacity = (opacity >= uAlphaCutoff) ? 1.0 : 0.0;
    }

    // Debug AOVs: override the shaded output with the requested channel.
    if (uRenderMode != 0 && uRenderMode != 36 && uRenderMode != 37 &&
        uRenderMode != 38 && uRenderMode != 39 && uRenderMode != 40) {
        vec3 Ngeo = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
        if (uRenderMode == 2) {
            vec3 aovN = (dot(N, uCameraPos - vWorldPos) < 0.0) ? -N : N;
            fragColor = vec4(aovN * 0.5 + 0.5, 1.0);
            return;
        } // shading normal
        if (uRenderMode == 35) { fragColor = vec4(coatN * 0.5 + 0.5, 1.0); return; } // coat normal
        if (uRenderMode == 3) { fragColor = vec4(idColor(uMatId), 1.0); return; }     // material id
        if (uRenderMode == 4) { fragColor = vec4(Ngeo * 0.5 + 0.5, 1.0); return; }    // geometric normal
        if (uRenderMode == 6) {                                                       // depth
            float d = clamp(length(uCameraPos - vWorldPos) / uDepthScale, 0.0, 1.0);
            fragColor = vec4(vec3(1.0 - d), 1.0);
            return;
        }
        if (uRenderMode == 5) { fragColor = vec4(fract(vUV), 0.0, 1.0); return; }      // uv set 0
        if (uRenderMode == 7) { fragColor = vec4(baseColor, 1.0); return; }     // albedo (unlit)
        if (uRenderMode == 8) {                                                        // facing
            fragColor = gl_FrontFacing ? vec4(0.1, 0.7, 0.1, 1.0) : vec4(0.7, 0.1, 0.1, 1.0);
            return;
        }
        if (uRenderMode == 9)  { fragColor = vec4(vec3(roughness), 1.0); return; }     // roughness
        if (uRenderMode == 10) { fragColor = vec4(vec3(metallic), 1.0); return; }      // metallic
        if (uRenderMode == 11) { fragColor = vec4(emissive, 1.0); return; }            // emissive
        if (uRenderMode == 12) { fragColor = vec4(vec3(opacity), 1.0); return; }       // opacity
        if (uRenderMode == 13) {                                                       // world position
            fragColor = vec4(clamp((vWorldPos - uSceneMin) / uSceneExtent, 0.0, 1.0), 1.0);
            return;
        }
        if (uRenderMode == 23) {                                                       // uv checker
            vec2 c = floor(fract(vUV) * 16.0);
            float chk = mod(c.x + c.y, 2.0);
            fragColor = vec4(vec3(mix(0.25, 0.85, chk)), 1.0);
            return;
        }
        if (uRenderMode == 15) { fragColor = vec4(idColor(gl_PrimitiveID), 1.0); return; }  // prim id
        if (uRenderMode == 16) { fragColor = vec4(idColor(uMeshId), 1.0); return; }         // mesh id
        if (uRenderMode == 19) {                                                            // missing normals
            fragColor = uGeometricNormal ? vec4(0.95, 0.1, 0.85, 1.0) : vec4(0.2, 0.2, 0.2, 1.0);
            return;
        }
        if (uRenderMode == 20) {                                                            // double-sided
            fragColor = uDoubleSided ? vec4(0.95, 0.55, 0.1, 1.0) : vec4(0.2, 0.2, 0.2, 1.0);
            return;
        }
        if (uRenderMode == 18) { fragColor = vec4(purposeColor(uPurpose), 1.0); return; }    // purpose
        if (uRenderMode == 29) { fragColor = vec4(kindColor(uKind), 1.0); return; }          // kind
        if (uRenderMode == 30) {                                                             // udim tile
            int tile = int(floor(vUV.x)) + 10 * int(floor(vUV.y));
            fragColor = vec4(idColor(tile), 1.0); return;
        }
)glsl" R"glsl(
        if (uRenderMode == 34) {                                                             // source USD face id
            int fid = uHasFaceId ? int(texelFetch(uFaceIdTex, uFaceBase + gl_PrimitiveID).r) : -1;
            fragColor = vec4(idColor(fid), 1.0); return;
        }
        if (uRenderMode == 33) {                                                             // texel density (UV/world area ratio)
            vec2 du = dFdx(vUV), dv = dFdy(vUV);
            float uvArea = abs(du.x * dv.y - dv.x * du.y);
            float worldArea = length(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
            float td = sqrt(uvArea / max(worldArea, 1e-12));
            float c = clamp(td * uDepthScale * 0.5, 0.0, 1.0);  // scene-relative
            fragColor = vec4(c, 1.0 - abs(c - 0.5) * 2.0, 1.0 - c, 1.0); return;
        }
        if (uRenderMode == 31) { fragColor = vec4(fract(vUV1), 0.0, 1.0); return; }          // uv set 1
        if (uRenderMode == 32) {                                                             // blendshape influence
            // Normalize by ~10% of the scene extent into a blue->red heatmap.
            float c = clamp(vMorphInfl / max(uDepthScale * 0.1, 1e-4), 0.0, 1.0);
            fragColor = vec4(c, 1.0 - abs(c - 0.5) * 2.0, 1.0 - c, 1.0); return;
        }
        if (uRenderMode == 21) {                                                             // skin weights
            vec3 jc = idColor(vDomJoint);                  // dominant joint -> hashed color
            fragColor = vec4(jc * (0.3 + 0.7 * clamp(vDomWeight, 0.0, 1.0)), 1.0);
            return;
        }
        if (uRenderMode == 22) {                                                             // tangent (from UV gradient)
            vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
            vec2 du1 = dFdx(vUV), du2 = dFdy(vUV);
            float r = du1.x * du2.y - du2.x * du1.y;
            vec3 T = dp1 * du2.y - dp2 * du1.y;
            T = (abs(r) > 1e-8) ? T / r : dp1;
            fragColor = vec4(normalize(T) * 0.5 + 0.5, 1.0);
            return;
        }
        if (uRenderMode == 25) {                                                             // curvature
            vec3 n = normalize(vNormal);
            float c = clamp((length(dFdx(n)) + length(dFdy(n))) * 8.0, 0.0, 1.0);
            // blue (flat) -> red (high curvature) heatmap
            fragColor = vec4(c, 1.0 - abs(c - 0.5) * 2.0, 1.0 - c, 1.0);
            return;
        }
        if (uRenderMode == 26) { fragColor = vec4(idColor(-1), 1.0); return; }  // instance id: non-instanced -> gray
    }
    vec3 V = normalize(uCameraPos - vWorldPos);

    // Real-time Cook-Torrance preview. Face two-sided shading normals toward the
    // camera, then evaluate an energy-conserving GGX direct lobe. The constant
    // ambient fallback below keeps scenes without authored lighting readable.
    vec3 Nf = (dot(N, V) < 0.0) ? -N : N;
    vec3 coatNf = (dot(coatN, V) < 0.0) ? -coatN : coatN;
    vec3 tangent = dFdx(vWorldPos);
    tangent = normalize(tangent - Nf * dot(tangent, Nf));
    if (dot(tangent, tangent) < 1e-8) tangent = normalize(cross(Nf, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = normalize(cross(Nf, tangent));
    float tangentAngle = radians(specularRotation);
    vec3 rotatedTangent = tangent * cos(tangentAngle) +
                          bitangent * sin(tangentAngle);
    vec3 rotatedBitangent = normalize(cross(Nf, rotatedTangent));
    float NoV = max(dot(Nf, V), 1e-4);
    float met = clamp(metallic, 0.0, 1.0);
    float rgh = clamp(roughness +
                      0.25 * clamp(diffuseRoughness, 0.0, 1.0) * (1.0 - met),
                      0.02, 1.0);
    vec3 F0 = computeF0(baseColor, met, specularColor, specularIor,
                        uUseSpecularWorkflow, uOpenPbrSpecularModel);
    if (uUseSpecularWorkflow != 0 || uOpenPbrSpecularModel != 0) {
        F0 *= sampleCoatColor(uSpecularColorTex, uHasSpecularColorTex,
                              uAdvancedTexIsUdim.x, uAdvancedUdimRoutes.x,
                              uAdvancedUdimSlots.x,
                              uSpecularColorUv0, uSpecularColorUv1,
                              uSpecularColorUvSet, uSpecularColorScale,
                              uSpecularColorBias, uSpecularColorPtexGrid);
    }
    F0 = applyThinFilm(F0, NoV, thinFilmWeight, thinFilmThickness,
                       thinFilmIor);
    if (uRenderMode == 39) { fragColor = vec4(F0, 1.0); return; }
    if (uRenderMode == 40) {
        float d = (max(specularIor, 1.0) - 1.0) /
                  (max(specularIor, 1.0) + 1.0);
        fragColor = vec4(vec3(d * d), 1.0); return;
    }
    float cw = clamp(coatWeight *
                     sampleCoatScalar(uCoatWeightTex, uHasCoatWeightTex,
                                      uAdvancedTexIsUdim.y,
                                      uAdvancedUdimRoutes.y,
                                      uAdvancedUdimSlots.y,
                                      uCoatWeightUv0, uCoatWeightUv1,
                                      uCoatWeightUvSet, uCoatWeightChannel,
                                      uCoatWeightScale, uCoatWeightBias,
                                      uCoatWeightPtexGrid),
                     0.0, 1.0);
    float cr = clamp(coatRoughness *
                     sampleCoatScalar(uCoatRoughnessTex, uHasCoatRoughnessTex,
                                      uAdvancedTexIsUdim.w,
                                      uAdvancedUdimRoutes.w,
                                      uAdvancedUdimSlots.w,
                                      uCoatRoughnessUv0, uCoatRoughnessUv1,
                                      uCoatRoughnessUvSet,
                                      uCoatRoughnessChannel,
                                      uCoatRoughnessScale, uCoatRoughnessBias,
                                      uCoatRoughnessPtexGrid),
                     0.02, 1.0);
    cr = mix(cr, clamp(roughness, 0.02, 1.0),
             clamp(coatAffectRoughness, 0.0, 1.0));
    vec3 sampledCoatTint = coatColor * sampleCoatColor(
        uCoatColorTex, uHasCoatColorTex, uAdvancedTexIsUdim.z,
        uAdvancedUdimRoutes.z, uAdvancedUdimSlots.z, uCoatColorUv0,
        uCoatColorUv1, uCoatColorUvSet, uCoatColorScale, uCoatColorBias,
        uCoatColorPtexGrid);
    if (uRenderMode == 36) { fragColor = vec4(vec3(cw), 1.0); return; }
    // The semantic AOV reports the authored/sampled coat color itself. The
    // coat_affect_color control belongs to BSDF shading and must not wash the
    // diagnostic texture toward white.
    if (uRenderMode == 37) { fragColor = vec4(sampledCoatTint, 1.0); return; }
    if (uRenderMode == 38) { fragColor = vec4(vec3(cr), 1.0); return; }
    vec3 coatTint = mix(vec3(1.0), sampledCoatTint,
                        clamp(coatAffectColor, 0.0, 1.0));
    float ci = max(coatIor, 1.0);
    float cd = (ci - 1.0) / (ci + 1.0);
    vec3 direct = vec3(0.0);
    for (int li = 0; li < kMaxRasterLights; ++li) {
        if (li >= uLightCount) break;
        if ((uLightMask & (1u << uint(li))) == 0u) continue;
        vec4 pt = uLightPositionType[li];
        vec4 da = uLightDirectionAngle[li];
        vec4 lc = uLightColorDiffuse[li];
        vec4 ss = uLightSpecularShape[li];
        int lightType = int(pt.w + 0.5);
        int sampleCount = (lightType == 2 || lightType == 3 ||
                           lightType == 4 || lightType == 7 ||
                           lightType == 8) ? 8 : 1;
        for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        vec3 samplePos = pt.xyz;
        int sampleBase = sampleIndex * 3;
        if (lightType == 7) {
            samplePos = vec3(
                uLightIesProfile[li * 6 + sampleBase / 4][sampleBase % 4],
                uLightIesProfile[li * 6 + (sampleBase + 1) / 4][(sampleBase + 1) % 4],
                uLightIesProfile[li * 6 + (sampleBase + 2) / 4][(sampleBase + 2) % 4]);
        } else if (lightType == 3 || lightType == 8) {
            float sx = (float(sampleIndex % 4) + 0.5) * 0.25 - 0.5;
            float sy = (float(sampleIndex / 4) + 0.5) * 0.5 - 0.5;
            samplePos += normalize(uLightIesAxisX[li].xyz) *
                         (sx * uLightAreaParams[li].y) +
                         normalize(uLightIesAxisY[li].xyz) *
                         (sy * uLightAreaParams[li].z);
        } else if (lightType == 2 || lightType == 4) {
            float angle = 6.28318530718 * (float(sampleIndex) + 0.5) / 8.0;
            samplePos += normalize(uLightIesAxisX[li].xyz) *
                         (cos(angle) * 0.5 * uLightAreaParams[li].x) +
                         normalize(uLightIesAxisY[li].xyz) *
                         (sin(angle) * 0.5 * uLightAreaParams[li].x);
        }
        vec3 L;
        float attenuation = 1.0;
        if (lightType == 5) {
            L = normalize(da.xyz);
        } else {
            vec3 toLight = samplePos - vWorldPos;
            float dist2 = max(dot(toLight, toLight), 1e-6);
            L = toLight * inversesqrt(dist2);
            attenuation = 1.0 / dist2;
        }
        float shape = 1.0;
        if (ss.w > 0.5 && lightType != 5) {
            float coneCos = dot(normalize(da.xyz), -L);
            float outer = cos(radians(clamp(da.w, 0.0, 180.0)));
            float innerAngle = da.w * (1.0 - clamp(ss.y, 0.0, 1.0));
            float inner = cos(radians(clamp(innerAngle, 0.0, 180.0)));
            shape = smoothstep(outer, max(inner, outer + 1e-5), coneCos);
            shape *= pow(max(coneCos, 0.0), max(ss.z, 0.0));
        }
        float NoL = max(dot(Nf, L), 0.0);
        float backNoL = max(dot(-Nf, L), 0.0);
        if ((NoL <= 0.0 && backNoL <= 0.0) || shape <= 0.0) continue;
        vec3 H = normalize(L + V);
        float NoH = max(dot(Nf, H), 0.0);
        float VoH = max(dot(V, H), 0.0);
        vec3 F = fresnelSchlick(VoH, F0);
        float D = distributionAnisotropicGGX(
            NoH, dot(H, rotatedTangent), dot(H, rotatedBitangent), rgh,
            specularAnisotropy + specularRoughnessAnisotropy * 0.5);
        float G = geometrySchlickGGX(NoV, rgh) *
                  geometrySchlickGGX(NoL, rgh);
        vec3 specular = D * G * F * clamp(specularWeight, 0.0, 1.0) /
                        max(4.0 * NoV * NoL, 1e-5);
        vec3 diffuse = (vec3(1.0) - F) * (1.0 - met) * baseColor *
                       baseWeight / kPi;
        // OpenPBR sheen: a rough retro-reflective lobe concentrated at the
        // grazing angles.  This Charlie-like approximation is intentionally
        // bounded so it can be layered with the existing GGX response.
        float sheenGrazing = pow(1.0 - max(NoL, 0.0), 5.0);
        vec3 sheen = sheenColor * clamp(sheenWeight, 0.0, 1.0) *
                      (0.5 + 0.5 * clamp(sheenRoughness, 0.0, 1.0)) *
                      sheenGrazing / kPi;
        // Subsurface is represented as wrapped diffuse plus a back-lighting
        // term.  Radius/anisotropy remain available to the path tracer; the
        // GL preview keeps the perceptual response stable across mesh scale.
        float wrap = clamp(subsurfaceWeight, 0.0, 1.0) *
                     (0.5 + 0.25 * abs(clamp(subsurfaceAnisotropy, -1.0, 1.0)) +
                      0.25 * abs(clamp(subsurfaceScatterAnisotropy, -1.0, 1.0)));
        float wrappedNoL = clamp((dot(Nf, L) + wrap) / (1.0 + wrap), 0.0, 1.0);
        float radiusGain = clamp(sqrt(max(subsurfaceScale *
                                          subsurfaceRadius * subsurfaceRadius, 0.0)) * 8.0,
                                 0.0, 1.0);
        float diffusionShape = mix(wrappedNoL + backNoL * 0.35,
                                   0.5 + 0.5 * sqrt(max(NoL, 0.0)),
                                   radiusGain);
        vec3 subsurface = subsurfaceColor * subsurfaceWeight *
                          diffusionShape / kPi;
        // Single-bounce transmission/backlighting.  It complements alpha
        // blending and remains visible for opaque thin-sheet OpenPBR assets.
        vec3 transmissionMedium = exp(-max(transmissionScatter, vec3(0.0)) *
                                      max(transmissionDepth, 0.0));
        float abbeScale = transmissionDispersionAbbeNumber > 0.0
                              ? 1.0 + 20.0 / max(transmissionDispersionAbbeNumber, 1.0)
                              : 1.0;
        float dispersion = clamp(transmissionDispersion *
                                 transmissionDispersionScale * abbeScale,
                                 0.0, 1.0);
        vec3 dispersionTint = vec3(1.0 + 0.35 * dispersion,
                                    1.0,
                                    1.0 - 0.25 * dispersion);
        vec3 transmitted = transmissionColor * dispersionTint * transmissionMedium *
                           clamp(transmissionWeight, 0.0, 1.0) * backNoL *
                           (1.0 - met) / kPi;
        float coatNoL = max(dot(coatNf, L), 0.0);
        float coatNoV = max(dot(coatNf, V), 1e-4);
        float coatNoH = max(dot(coatNf, H), 0.0);
        vec3 coatF = fresnelSchlick(VoH, vec3(cd * cd));
        float coatAngle = radians(coatRotation);
        vec3 coatT = tangent * cos(coatAngle) + bitangent * sin(coatAngle);
        vec3 coatB = normalize(cross(coatNf, coatT));
        float coatD = distributionAnisotropicGGX(
            coatNoH, dot(H, coatT), dot(H, coatB),
            cr * (1.0 + 0.25 * coatRoughnessAnisotropy), coatAnisotropy);
        float coatRgh = cr * (1.0 + 0.25 * coatRoughnessAnisotropy);
        float coatG = geometrySchlickGGX(coatNoV, coatRgh) *
                      geometrySchlickGGX(coatNoL, coatRgh);
        vec3 coatSpec = coatD * coatG * coatF /
                        max(4.0 * coatNoV * coatNoL, 1e-5);
        vec3 baseBrdf = (diffuse + sheen + subsurface) *
                        (1.0 - clamp(coatDarkening, 0.0, 1.0) * cw) * lc.w +
                        specular * (vec3(1.0) - coatF * cw) * ss.x;
        vec3 coatBrdf = coatSpec * coatTint * cw * ss.x;
        float visibility = (li == uShadowLightSlot)
                               ? sampleShadow(vWorldPos, Nf, L) : 1.0;
        direct += (baseBrdf * NoL + coatBrdf * coatNoL + transmitted) * lc.rgb *
                  (attenuation * shape * visibility) / float(sampleCount);
        }
    }
    // Scenes without authored direct lights retain the readable preview key.
    if (uLightCount == 0) {
        vec3 L = (dot(uLightDir, uLightDir) > 1e-8)
                     ? normalize(uLightDir)
                     : normalize(vec3(0.3, 0.5, 0.8));
        vec3 lightColor = (dot(uLightColor, uLightColor) > 1e-8)
                              ? uLightColor : vec3(1.0);
        float NoL = max(dot(Nf, L), 0.0);
        vec3 H = normalize(L + V);
        float NoH = max(dot(Nf, H), 0.0), VoH = max(dot(V, H), 0.0);
        vec3 F = fresnelSchlick(VoH, F0);
        vec3 specular = distributionAnisotropicGGX(
                         NoH, dot(H, rotatedTangent), dot(H, rotatedBitangent),
                         rgh, specularAnisotropy +
                         specularRoughnessAnisotropy * 0.5) *
                        geometrySchlickGGX(NoV, rgh) *
                        geometrySchlickGGX(NoL, rgh) * F /
                        max(4.0 * NoV * NoL, 1e-5);
        vec3 diffuse = (vec3(1.0) - F) * (1.0 - met) * baseColor *
                       baseWeight / kPi;
        specular *= clamp(specularWeight, 0.0, 1.0);
        direct = (diffuse + specular) * lightColor * NoL;
    }

    // Ambient: DomeLight split-sum IBL when baked, else a constant floor
    // (keeps unlit faces lifted off black).
    vec3 ambient;
    if (uHasIbl) {
        vec3 Ne = normalize(uEnvRotation * Nf);
        vec3 Re = normalize(uEnvRotation * reflect(-V, Nf));
        vec3 irr = texture(uIrradianceMap, Ne).rgb;
        float lod = clamp(roughness, 0.0, 1.0) * float(uPrefilteredLods - 1);
        vec3 pref = textureLod(uPrefilteredMap, Re, lod).rgb;
        // Analytic split-sum approximation keeps the GL 3.3 fragment sampler
        // count within the guaranteed 16 after adding independent roughness.
        float rr = clamp(roughness, 0.0, 1.0);
        vec4 r = rr * vec4(-1.0, -0.0275, -0.572, 0.022) +
                 vec4(1.0, 0.0425, 1.04, -0.04);
        float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
        vec2 dfg = vec2(-1.04, 1.04) * a004 + r.zw;
        ambient = (baseColor * (1.0 - met) * baseWeight * irr +
                   pref * (F0 * dfg.x + dfg.y)) * uIblColor;
        vec3 coatPref = textureLod(uPrefilteredMap, Re,
                                   cr * float(uPrefilteredLods - 1)).rgb;
        vec4 rc = cr * vec4(-1.0, -0.0275, -0.572, 0.022) +
                  vec4(1.0, 0.0425, 1.04, -0.04);
        float ca004 = min(rc.x * rc.x, exp2(-9.28 * NoV)) * rc.x + rc.y;
        vec2 coatDfg = vec2(-1.04, 1.04) * ca004 + rc.zw;
        vec3 coatF0 = vec3(cd * cd);
        vec3 coatIbl = coatPref * (coatF0 * coatDfg.x + coatDfg.y) *
                       coatTint * cw * uIblColor;
        ambient = ambient * (vec3(1.0) - coatF0 * cw) + coatIbl;
    } else {
        ambient = baseColor * 0.12;
    }
    // Keep the advanced lobes visible in the ambient-only preview as well.
    // With an IBL, transmission uses a refracted environment sample; without
    // one, the neutral floor still receives a bounded colored contribution.
    float lobeWeight = clamp(transmissionWeight, 0.0, 1.0) * (1.0 - met);
    if (uHasIbl && lobeWeight > 0.0) {
        vec3 T = refract(-V, Nf, 1.0 / max(specularIor, 1.001));
        vec3 transmittedIbl = textureLod(uPrefilteredMap,
                                         normalize(T),
                                         rgh * float(uPrefilteredLods - 1)).rgb;
        vec3 medium = exp(-max(transmissionScatter, vec3(0.0)) *
                          max(transmissionDepth, 0.0));
        float dispersion = clamp(transmissionDispersion *
                                 transmissionDispersionScale, 0.0, 1.0);
        vec3 dispersionTint = vec3(1.0 + 0.35 * dispersion,
                                    1.0,
                                    1.0 - 0.25 * dispersion);
        ambient += transmittedIbl * transmissionColor * dispersionTint * medium * lobeWeight *
                   uIblColor;
    } else {
        vec3 medium = exp(-max(transmissionScatter, vec3(0.0)) *
                          max(transmissionDepth, 0.0));
        ambient += transmissionColor * medium * lobeWeight * 0.04;
    }
    ambient += subsurfaceColor * clamp(subsurfaceWeight, 0.0, 1.0) *
               (uHasIbl ? 0.12 : 0.03);
    ambient += sheenColor * clamp(sheenWeight, 0.0, 1.0) * 0.025;
    if (uAlphaMode == 1 && opacity <= 0.0) {
        discard;
    }
    ambient *= clamp(uOcclusion, 0.0, 1.0) * sampleOcclusion();
    vec3 color = linearToSrgb((ambient + direct + emissive) * exp2(uExposure));
    fragColor = vec4(color, opacity);
}
)glsl";
#else
    static const std::string shader = []() {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_LIGHT3D_MATERIAL_CPP),
                                       MAKEINTRESOURCEW(RT_RCDATA));
        if (!resource) return std::string();
        HGLOBAL data = LoadResource(module, resource);
        const char* source = static_cast<const char*>(LockResource(data));
        const size_t size = static_cast<size_t>(SizeofResource(module, resource));
        if (!source || size == 0) return std::string();

        const std::string text(source, size);
        const std::string function = "const char* getMaterialFragmentShaderGL330()";
        const std::string begin = "return R\"glsl(";
        const std::string end = ")glsl\"";
        size_t cursor = text.find(function);
        cursor = cursor == std::string::npos ? cursor : text.find(begin, cursor);
        const size_t functionEnd = text.find("// ==================== GL430", cursor);
        std::string result;
        while (cursor != std::string::npos && cursor < functionEnd) {
            cursor += begin.size();
            const size_t endPos = text.find(end, cursor);
            if (endPos == std::string::npos || endPos >= functionEnd) break;
            result.append(text, cursor, endPos - cursor);
            cursor = text.find("R\"glsl(", endPos + end.size());
        }
        return result;
    }();
    return shader.c_str();
#endif
}

// ==================== GL430 Bindless Shaders ====================
//
// UNUSED scaffold for a future GL 4.3 path (no callers; the renderer uses the
// GL330 shaders). STALE: these predate GPU blendshape morph, GPU skinning, and
// displacement, so activating them as-is would render none of those. Re-derive
// from the GL330 shaders (which carry the current morph/skin/displacement) before
// wiring up a 4.3 path.

const char* getMaterialFragmentShaderGL330Fallback() {
    return R"glsl(#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec2 vUV1;
in vec4 vColor;
uniform int uClippingPlaneCount;
uniform vec4 uClippingPlanes[8];
uniform vec3 uCameraPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform float uExposure;
uniform vec3 uBaseColor;
uniform vec3 uEmissive;
uniform float uAlpha;
uniform int uAlphaMode;
uniform float uAlphaCutoff;
uniform float uMetallic;
uniform float uRoughness;
uniform int uUseSpecularWorkflow;
uniform int uOpenPbrSpecularModel;
uniform vec3 uSpecularColor;
uniform float uIor;
uniform float uBaseWeight;
uniform float uSpecularWeight;
uniform float uDiffuseRoughness;
uniform float uTransmission;
uniform vec3 uTransmissionColor;
uniform float uTransmissionDepth;
uniform vec3 uTransmissionScatter;
uniform float uTransmissionDispersion;
uniform float uTransmissionDispersionScale;
uniform float uVolumeDensity;
uniform vec3 uVolumeAlbedo;
uniform vec3 uVolumeEmission;
uniform float uVolumeEmissionScale;
uniform float uSubsurface;
uniform vec3 uSubsurfaceColor;
uniform float uSubsurfaceScale;
uniform float uSubsurfaceAnisotropy;
uniform float uSubsurfaceScatterAnisotropy;
uniform float uCoatWeight;
uniform vec3 uCoatColor;
uniform float uCoatRoughness;
uniform float uCoatIor;
uniform float uCoatAffectColor;
uniform float uCoatDarkening;
uniform float uSheenWeight;
uniform vec3 uSheenColor;
uniform float uSheenRoughness;
uniform float uSpecularAnisotropy;
uniform float uSpecularRoughnessAnisotropy;
uniform sampler2D uMetallicTex;
uniform sampler2D uRoughnessTex;
uniform sampler2D uNormalTex;
uniform sampler2D uEmissiveTex;
uniform sampler2D uOpacityTex;
uniform sampler2D uOcclusionTex;
uniform sampler2D uSpecularColorTex;
uniform sampler2D uCoatWeightTex;
uniform sampler2D uCoatColorTex;
uniform sampler2D uCoatRoughnessTex;
uniform sampler2D uCoatNormalTex;
uniform int uRenderMode;
uniform int uMatId;
uniform sampler2D uBaseColorTex;
uniform bool uHasBaseColorTex;
uniform bool uHasMetallicTex;
uniform bool uHasRoughnessTex;
uniform bool uHasNormalTex;
uniform bool uHasEmissiveTex;
uniform bool uHasOpacityTex;
uniform bool uHasOcclusionTex;
uniform bool uHasSpecularColorTex;
uniform bool uHasCoatWeightTex;
uniform bool uHasCoatColorTex;
uniform bool uHasCoatRoughnessTex;
uniform bool uHasCoatNormalTex;
uniform vec3 uBaseColorUv0;
uniform vec3 uBaseColorUv1;
uniform vec3 uMetallicUv0;
uniform vec3 uMetallicUv1;
uniform vec3 uRoughnessUv0;
uniform vec3 uRoughnessUv1;
uniform int uRoughnessUvSet;
uniform vec3 uNormalUv0;
uniform vec3 uNormalUv1;
uniform vec3 uEmissiveUv0;
uniform vec3 uEmissiveUv1;
uniform vec3 uOpacityUv0;
uniform vec3 uOpacityUv1;
uniform vec3 uOcclusionUv0;
uniform vec3 uOcclusionUv1;
uniform vec3 uSpecularColorUv0;
uniform vec3 uSpecularColorUv1;
uniform vec3 uCoatWeightUv0;
uniform vec3 uCoatWeightUv1;
uniform vec3 uCoatColorUv0;
uniform vec3 uCoatColorUv1;
uniform vec3 uCoatRoughnessUv0;
uniform vec3 uCoatRoughnessUv1;
uniform vec3 uCoatNormalUv0;
uniform vec3 uCoatNormalUv1;
uniform ivec4 uUvSet;
uniform int uOpacityUvSet;
uniform int uOcclusionUvSet;
uniform int uSpecularColorUvSet;
uniform int uCoatWeightUvSet;
uniform int uCoatColorUvSet;
uniform int uCoatRoughnessUvSet;
uniform int uCoatNormalUvSet;
uniform int uOpacityChannel;
uniform float uOpacityTexScale;
uniform float uOpacityTexBias;
uniform int uMetallicChannel;
uniform int uRoughnessChannel;
uniform int uOcclusionChannel;
uniform int uCoatWeightChannel;
uniform int uCoatRoughnessChannel;
uniform float uOcclusionTexScale;
uniform float uOcclusionTexBias;
uniform vec4 uSpecularColorScale;
uniform vec4 uSpecularColorBias;
uniform vec4 uCoatWeightScale;
uniform vec4 uCoatWeightBias;
uniform vec4 uCoatColorScale;
uniform vec4 uCoatColorBias;
uniform vec4 uCoatRoughnessScale;
uniform vec4 uCoatRoughnessBias;
uniform vec4 uCoatNormalScale;
uniform vec4 uCoatNormalBias;
uniform float uOcclusion;
uniform float uCoatAffectRoughness;
uniform float uMetallicTexScale;
uniform float uMetallicTexBias;
uniform float uRoughnessTexScale;
uniform float uRoughnessTexBias;
uniform vec4 uNormalTexScale;
uniform vec4 uNormalTexBias;
uniform vec4 uEmissiveTexScale;
uniform vec4 uEmissiveTexBias;
uniform bool uHasShadowMap;
uniform int uShadowLightSlot;
uniform mat4 uShadowViewProj;
uniform sampler2D uShadowMap;
uniform bool uHasPointShadowMap;
uniform vec3 uPointShadowLightPos;
uniform mat4 uPointShadowViewProj[6];
uniform samplerCube uPointShadowMap;
uniform int uLightCount;
uniform uint uLightMask;
uniform vec4 uLightPositionType[16];
uniform vec4 uLightDirectionAngle[16];
uniform vec4 uLightColorDiffuse[16];
uniform vec4 uLightSpecularShape[16];
uniform vec4 uLightAreaParams[16];
uniform vec4 uLightIesAxisX[16];
uniform vec4 uLightIesAxisY[16];
uniform vec4 uLightIesProfile[96];
out vec4 fragColor;
vec3 linearToSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, vec3(greaterThan(c, vec3(0.0031308))));
}
vec3 idColor(int id) {
    if (id < 0) return vec3(0.45);
    uint h = (uint(id) + 1u) * 2654435761u;
    return vec3(float(h & 255u), float((h >> 8) & 255u),
                float((h >> 16) & 255u)) * (1.0 / 255.0);
}
vec2 applyUv(vec2 uv, vec3 row0, vec3 row1) {
    return vec2(dot(vec3(uv, 1.0), row0),
                dot(vec3(uv, 1.0), row1));
}
float channelOf(vec4 c, int channel) {
    if (channel == 1) return c.g;
    if (channel == 2) return c.b;
    if (channel == 3) return c.a;
    return c.r;
}
float ggxD(float noH, float roughness) {
    float a = roughness * roughness;
    float d = noH * noH * (a - 1.0) + 1.0;
    return a / max(3.14159265 * d * d, 1e-5);
}
float ggxG1(float noX, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    return noX / max(noX * (1.0 - k) + k, 1e-5);
}
vec3 fresnel(float voH, vec3 f0) {
    return f0 + (vec3(1.0) - f0) * pow(1.0 - clamp(voH, 0.0, 1.0), 5.0);
}
float sampleChannel(vec4 value, int channel) {
    if (channel == 1) return value.g;
    if (channel == 2) return value.b;
    if (channel == 3) return value.a;
    return value.r;
}
float sampleShadow(vec3 worldPos, vec3 normal, vec3 lightDir) {
    if (uHasPointShadowMap) {
        vec3 d = worldPos - uPointShadowLightPos;
        vec3 a = abs(d);
        int face;
        if (a.x >= a.y && a.x >= a.z) face = d.x >= 0.0 ? 0 : 1;
        else if (a.y >= a.z) face = d.y >= 0.0 ? 2 : 3;
        else face = d.z >= 0.0 ? 4 : 5;
        vec4 clip = uPointShadowViewProj[face] * vec4(worldPos, 1.0);
        vec3 p = clip.xyz / clip.w;
        p = p * 0.5 + 0.5;
        if (p.z <= 0.0 || p.z >= 1.0) return 1.0;
        float bias = max(0.00035,
                         0.0015 * (1.0 - max(dot(normal, lightDir), 0.0)));
        return (p.z - bias <= texture(uPointShadowMap, normalize(d)).r)
                   ? 1.0
                   : 0.0;
    }
    if (!uHasShadowMap) return 1.0;
    vec4 clip = uShadowViewProj * vec4(worldPos, 1.0);
    vec3 p = clip.xyz / clip.w;
    p = p * 0.5 + 0.5;
    if (p.z <= 0.0 || p.z >= 1.0 || any(lessThan(p.xy, vec2(0.0))) ||
        any(greaterThan(p.xy, vec2(1.0)))) return 1.0;
    float bias = max(0.00035,
                     0.0015 * (1.0 - max(dot(normal, lightDir), 0.0)));
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float visible = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            visible += p.z - bias <=
                               texture(uShadowMap, p.xy + vec2(x, y) * texel).r
                           ? 1.0
                           : 0.0;
    return visible / 9.0;
}
void main() {
    for (int i = 0; i < uClippingPlaneCount; ++i) {
        if (dot(vec4(vWorldPos, 1.0), uClippingPlanes[i]) < 0.0) discard;
    }
    vec3 displayColor = vColor.rgb;
    float displayOpacity = vColor.a;
    vec3 N = normalize(vNormal);
    if (uRenderMode == 3) { fragColor = vec4(idColor(uMatId), 1.0); return; }
    vec3 base = uBaseColor * displayColor;
    vec3 emissive = uEmissive;
    float opacity = clamp(uAlpha * displayOpacity, 0.0, 1.0);
    float metallic = clamp(uMetallic, 0.0, 1.0);
    float roughness = clamp(uRoughness, 0.02, 1.0);
    if (uHasBaseColorTex) {
        vec2 uv = applyUv(uUvSet.x == 1 ? vUV1 : vUV,
                          uBaseColorUv0, uBaseColorUv1);
        base *= texture(uBaseColorTex, uv).rgb;
    }
    if (uHasNormalTex) {
        vec2 uv = applyUv(uUvSet.z == 1 ? vUV1 : vUV,
                          uNormalUv0, uNormalUv1);
        vec3 mapped = texture(uNormalTex, uv).rgb * uNormalTexScale.xyz +
                      uNormalTexBias.xyz;
        vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
        vec2 du1 = dFdx(uv), du2 = dFdy(uv);
        float determinant = du1.x * du2.y - du2.x * du1.y;
        vec3 tangent = dp1 * du2.y - dp2 * du1.y;
        if (abs(determinant) > 1e-8) tangent /= determinant;
        else tangent = cross(abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0)
                                            : vec3(0.0, 1.0, 0.0), N);
        tangent = normalize(tangent - N * dot(N, tangent));
        vec3 bitangent = normalize(cross(N, tangent)) *
                         (determinant < 0.0 ? -1.0 : 1.0);
        N = normalize(mat3(tangent, bitangent, N) * normalize(mapped));
    }
    if (uHasMetallicTex) {
        vec2 uv = applyUv(uUvSet.y == 1 ? vUV1 : vUV,
                          uMetallicUv0, uMetallicUv1);
        metallic = clamp(channelOf(texture(uMetallicTex, uv), uMetallicChannel) *
                              uMetallicTexScale + uMetallicTexBias, 0.0, 1.0);
    }
    if (uHasRoughnessTex) {
        vec2 uv = applyUv(uRoughnessUvSet == 1 ? vUV1 : vUV,
                          uRoughnessUv0, uRoughnessUv1);
        roughness = clamp(channelOf(texture(uRoughnessTex, uv), uRoughnessChannel) *
                              uRoughnessTexScale + uRoughnessTexBias, 0.02, 1.0);
    }
    if (uHasEmissiveTex) {
        vec2 uv = applyUv(uUvSet.w == 1 ? vUV1 : vUV,
                          uEmissiveUv0, uEmissiveUv1);
        emissive *= (texture(uEmissiveTex, uv) * uEmissiveTexScale +
                     uEmissiveTexBias).rgb;
    }
    if (uHasOpacityTex) {
        vec2 uv = applyUv(uOpacityUvSet == 1 ? vUV1 : vUV,
                          uOpacityUv0, uOpacityUv1);
        opacity *= clamp(sampleChannel(texture(uOpacityTex, uv),
                                       uOpacityChannel) * uOpacityTexScale +
                       uOpacityTexBias, 0.0, 1.0);
    }
    float occlusion = clamp(uOcclusion, 0.0, 1.0);
    if (uHasOcclusionTex) {
        vec2 uv = applyUv(uOcclusionUvSet == 1 ? vUV1 : vUV,
                          uOcclusionUv0, uOcclusionUv1);
        occlusion *= clamp(channelOf(texture(uOcclusionTex, uv),
                                     uOcclusionChannel) * uOcclusionTexScale +
                           uOcclusionTexBias, 0.0, 1.0);
    }
    float coatWeight = clamp(uCoatWeight, 0.0, 1.0);
    if (uHasCoatWeightTex) {
        vec2 uv = applyUv(uCoatWeightUvSet == 1 ? vUV1 : vUV,
                          uCoatWeightUv0, uCoatWeightUv1);
        coatWeight *= channelOf(texture(uCoatWeightTex, uv), uCoatWeightChannel) *
                      uCoatWeightScale.x + uCoatWeightBias.x;
    }
    vec3 coatColor = uCoatColor;
    if (uHasCoatColorTex) {
        vec2 uv = applyUv(uCoatColorUvSet == 1 ? vUV1 : vUV,
                          uCoatColorUv0, uCoatColorUv1);
        coatColor *= (texture(uCoatColorTex, uv) * uCoatColorScale +
                      uCoatColorBias).rgb;
    }
    float coatRoughness = clamp(uCoatRoughness, 0.02, 1.0);
    if (uHasCoatRoughnessTex) {
        vec2 uv = applyUv(uCoatRoughnessUvSet == 1 ? vUV1 : vUV,
                          uCoatRoughnessUv0, uCoatRoughnessUv1);
        coatRoughness *= channelOf(texture(uCoatRoughnessTex, uv),
                                   uCoatRoughnessChannel) *
                         uCoatRoughnessScale.x + uCoatRoughnessBias.x;
    }
    coatRoughness = mix(coatRoughness, roughness,
                        clamp(uCoatAffectRoughness, 0.0, 1.0));
    vec3 coatN = N;
    if (uHasCoatNormalTex) {
        vec2 uv = applyUv(uCoatNormalUvSet == 1 ? vUV1 : vUV,
                          uCoatNormalUv0, uCoatNormalUv1);
        vec3 mapped = (texture(uCoatNormalTex, uv) * uCoatNormalScale +
                       uCoatNormalBias).xyz;
        vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
        vec2 du1 = dFdx(uv), du2 = dFdy(uv);
        float determinant = du1.x * du2.y - du2.x * du1.y;
        vec3 tangent = dp1 * du2.y - dp2 * du1.y;
        if (abs(determinant) > 1e-8) tangent /= determinant;
        else tangent = cross(abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0)
                                            : vec3(0.0, 1.0, 0.0), N);
        tangent = normalize(tangent - N * dot(N, tangent));
        vec3 bitangent = normalize(cross(N, tangent)) *
                         (determinant < 0.0 ? -1.0 : 1.0);
        coatN = normalize(mat3(tangent, bitangent, N) * normalize(mapped));
    }
    if (uRenderMode == 2) { fragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (uRenderMode == 35) { fragColor = vec4(coatN * 0.5 + 0.5, 1.0); return; }
    if (uRenderMode == 36) { fragColor = vec4(vec3(clamp(coatWeight, 0.0, 1.0)), 1.0); return; }
    if (uRenderMode == 37) { fragColor = vec4(coatColor, 1.0); return; }
    if (uRenderMode == 38) { fragColor = vec4(vec3(coatRoughness), 1.0); return; }
    vec3 sampledSpecularColor = uSpecularColor;
    if (uHasSpecularColorTex) {
        vec2 uv = applyUv(uSpecularColorUvSet == 1 ? vUV1 : vUV,
                          uSpecularColorUv0, uSpecularColorUv1);
        sampledSpecularColor *= (texture(uSpecularColorTex, uv) *
                                 uSpecularColorScale + uSpecularColorBias).rgb;
    }
    float dielectric = pow((max(uIor, 1.0) - 1.0) /
                           (max(uIor, 1.0) + 1.0), 2.0);
    if (uRenderMode == 40) { fragColor = vec4(vec3(dielectric), 1.0); return; }
    if (uRenderMode == 39) {
        vec3 f0 = uUseSpecularWorkflow != 0 ? sampledSpecularColor :
                  mix(vec3(dielectric), base, metallic);
        if (uUseSpecularWorkflow == 0 && uOpenPbrSpecularModel != 0)
            f0 *= sampledSpecularColor;
        fragColor = vec4(f0, 1.0); return;
    }
    if (uAlphaMode == 1) opacity = opacity >= uAlphaCutoff ? 1.0 : 0.0;
    if (uAlphaMode == 1 && opacity <= 0.0) discard;
    float volumeOpacity = clamp(1.0 - exp(-max(uVolumeDensity, 0.0) * 0.1),
                                0.0, 1.0);
    base = mix(base, base * uVolumeAlbedo, volumeOpacity);
    emissive += uVolumeEmission * uVolumeEmissionScale * volumeOpacity;
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 Nf = dot(N, V) < 0.0 ? -N : N;
    float nv = max(dot(Nf, V), 1e-4);
    float baseWeight = clamp(uBaseWeight, 0.0, 1.0);
    float specularWeight = clamp(uSpecularWeight, 0.0, 1.0);
    vec3 f0;
    if (uUseSpecularWorkflow != 0) {
        f0 = max(uSpecularColor, vec3(0.0));
    } else {
        float ior = max(uIor, 1.0);
        float f = (ior - 1.0) / (ior + 1.0);
        f0 = vec3(f * f);
        if (uOpenPbrSpecularModel != 0) f0 *= max(uSpecularColor, vec3(0.0));
        f0 = mix(f0, base, metallic);
    }
    float diffuseRoughness = clamp(uDiffuseRoughness, 0.0, 1.0);
    float rough = clamp(roughness + 0.25 * diffuseRoughness * (1.0 - metallic),
                        0.02, 1.0);
    vec3 direct = vec3(0.0);
    for (int li = 0; li < 16; ++li) {
        if (li >= uLightCount) break;
        if ((uLightMask & (1u << uint(li))) == 0u) continue;
        vec4 pt = uLightPositionType[li];
        vec4 da = uLightDirectionAngle[li];
        vec4 lc = uLightColorDiffuse[li];
        vec4 ss = uLightSpecularShape[li];
        int lightType = int(pt.w + 0.5);
        int sampleCount = (lightType == 2 || lightType == 3 ||
                           lightType == 4 || lightType == 7 ||
                           lightType == 8) ? 8 : 1;
        for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        vec3 samplePos = pt.xyz;
        int sampleBase = sampleIndex * 3;
        if (lightType == 7) {
            samplePos = vec3(
                uLightIesProfile[li * 6 + sampleBase / 4][sampleBase % 4],
                uLightIesProfile[li * 6 + (sampleBase + 1) / 4][(sampleBase + 1) % 4],
                uLightIesProfile[li * 6 + (sampleBase + 2) / 4][(sampleBase + 2) % 4]);
        } else if (lightType == 3 || lightType == 8) {
            float sx = (float(sampleIndex % 4) + 0.5) * 0.25 - 0.5;
            float sy = (float(sampleIndex / 4) + 0.5) * 0.5 - 0.5;
            samplePos += normalize(uLightIesAxisX[li].xyz) *
                         (sx * uLightAreaParams[li].y) +
                         normalize(uLightIesAxisY[li].xyz) *
                         (sy * uLightAreaParams[li].z);
        } else if (lightType == 2 || lightType == 4) {
            float angle = 6.28318530718 * (float(sampleIndex) + 0.5) / 8.0;
            samplePos += normalize(uLightIesAxisX[li].xyz) *
                         (cos(angle) * 0.5 * uLightAreaParams[li].x) +
                         normalize(uLightIesAxisY[li].xyz) *
                         (sin(angle) * 0.5 * uLightAreaParams[li].x);
        }
        vec3 L;
        float attenuation = 1.0;
        if (lightType == 5) {
            L = normalize(da.xyz);
        } else {
            vec3 toLight = samplePos - vWorldPos;
            float dist2 = max(dot(toLight, toLight), 1e-6);
            L = toLight * inversesqrt(dist2);
            attenuation = 1.0 / dist2;
        }
        float shape = 1.0;
        if (ss.w > 0.5 && lightType != 5) {
            float coneCos = dot(normalize(da.xyz), -L);
            float outer = cos(radians(clamp(da.w, 0.0, 180.0)));
            float inner = cos(radians(clamp(da.w *
                                             (1.0 - clamp(ss.y, 0.0, 1.0)),
                                             0.0, 180.0)));
            shape = smoothstep(outer, max(inner, outer + 1e-5), coneCos) *
                    pow(max(coneCos, 0.0), max(ss.z, 0.0));
        }
        float nl = max(dot(Nf, L), 0.0);
        if (nl <= 0.0 || shape <= 0.0) continue;
        vec3 H = normalize(L + V);
        float nh = max(dot(Nf, H), 0.0);
        float vh = max(dot(V, H), 0.0);
        vec3 F = fresnel(vh, f0);
        float D = ggxD(nh, rough);
        float G = ggxG1(nv, rough) * ggxG1(nl, rough);
        vec3 spec = D * G * F * specularWeight /
                    max(4.0 * nv * nl, 1e-5);
        vec3 diff = (vec3(1.0) - F) * (1.0 - metallic) * base *
                    baseWeight / 3.14159265;
        float wrap = clamp(uSubsurface, 0.0, 1.0) *
                     (0.5 + 0.25 * abs(clamp(uSubsurfaceAnisotropy, -1.0, 1.0)) +
                      0.25 * abs(clamp(uSubsurfaceScatterAnisotropy, -1.0, 1.0)));
        float wrapped = clamp((dot(Nf, L) + wrap) / (1.0 + wrap), 0.0, 1.0);
        vec3 subsurface = uSubsurfaceColor * clamp(uSubsurface, 0.0, 1.0) *
                          wrapped * (0.35 + 0.65 *
                                    clamp(uSubsurfaceScale, 0.0, 1.0)) /
                          3.14159265;
        vec3 sheen = uSheenColor * clamp(uSheenWeight, 0.0, 1.0) *
                      (0.5 + 0.5 * clamp(uSheenRoughness, 0.0, 1.0)) *
                      pow(1.0 - nl, 5.0) / 3.14159265;
        float coat = clamp(uCoatWeight, 0.0, 1.0);
        float coatRough = clamp(uCoatRoughness, 0.02, 1.0);
        float coatIor = max(uCoatIor, 1.0);
        float coatF0v = (coatIor - 1.0) / (coatIor + 1.0);
        vec3 coatF = fresnel(vh, vec3(coatF0v * coatF0v));
        vec3 coatSpec = ggxD(nh, coatRough) *
                        ggxG1(nv, coatRough) * ggxG1(nl, coatRough) * coatF /
                        max(4.0 * nv * nl, 1e-5);
        vec3 brdf = (diff + subsurface + sheen) *
                    (1.0 - clamp(uCoatDarkening, 0.0, 1.0) * coat) +
                    spec * (vec3(1.0) - coatF * coat) +
                    coatSpec * mix(vec3(1.0), uCoatColor,
                                   clamp(uCoatAffectColor, 0.0, 1.0)) * coat;
        float dispersion = clamp(uTransmissionDispersion *
                                 uTransmissionDispersionScale, 0.0, 1.0);
        vec3 transmitted = uTransmissionColor *
                           vec3(1.0 + 0.35 * dispersion, 1.0,
                                1.0 - 0.25 * dispersion) *
                           exp(-max(uTransmissionScatter, vec3(0.0)) *
                               max(uTransmissionDepth, 0.0)) *
                           clamp(uTransmission, 0.0, 1.0) *
                           max(dot(-Nf, L), 0.0) / 3.14159265;
        float visibility = li == uShadowLightSlot
                               ? sampleShadow(vWorldPos, Nf, L)
                               : 1.0;
        direct += (brdf * nl + transmitted) * lc.rgb *
                  (attenuation * shape * visibility) / float(sampleCount);
        }
    }
    if (uLightCount == 0) {
        vec3 L = dot(uLightDir, uLightDir) > 1e-8
                     ? normalize(uLightDir)
                     : normalize(vec3(0.3, 0.5, 0.8));
        vec3 lc = dot(uLightColor, uLightColor) > 1e-8
                      ? uLightColor : vec3(1.0);
        float nl = max(dot(Nf, L), 0.0);
        vec3 H = normalize(L + V);
        vec3 F = fresnel(max(dot(V, H), 0.0), f0);
        vec3 diff = (vec3(1.0) - F) * (1.0 - metallic) * base *
                    baseWeight / 3.14159265;
        vec3 spec = ggxD(max(dot(Nf, H), 0.0), rough) *
                    ggxG1(nv, rough) * ggxG1(nl, rough) * F * specularWeight /
                    max(4.0 * nv * nl, 1e-5);
        direct = (diff + spec) * lc * nl;
    }
    vec3 color = base * 0.12 * occlusion + direct + emissive;
    color += uSubsurfaceColor * clamp(uSubsurface, 0.0, 1.0) * 0.03;
    color += uTransmissionColor * clamp(uTransmission, 0.0, 1.0) * 0.04;
    if (uRenderMode == 7) color = base;
    else if (uRenderMode == 9) color = vec3(roughness);
    else if (uRenderMode == 10) color = vec3(metallic);
    else if (uRenderMode == 11) color = emissive;
    else if (uRenderMode == 12) color = vec3(opacity);
    fragColor = vec4(linearToSrgb(color * exp2(uExposure)), opacity);
}
)glsl";
}

const char* getMaterialVertexShaderGL430() {
    return R"glsl(#version 430 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aUV;

uniform mat4 uModelViewProj;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;

void main() {
    vec4 worldPos = uModel * vec4(aPosition, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = normalize(uNormalMatrix * aNormal);
    vUV = aUV.xy;
    gl_Position = uModelViewProj * vec4(aPosition, 1.0);
}
)glsl";
}

const char* getMaterialFragmentShaderGL430() {
    return R"glsl(#version 430 core

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;

// Materials SSBO: each material is 4 x vec4 (16 floats)
layout(std430, binding = 1) readonly buffer Materials {
    vec4 materials[];
};

// Per-triangle material ID
layout(std430, binding = 2) readonly buffer FaceMaterialIds {
    uint faceMaterialId[];
};

uniform vec3 uCameraPos;
uniform sampler2DArray uTextureArray;

out vec4 fragColor;

void main() {
    int matId = int(faceMaterialId[gl_PrimitiveID]);
    int base = matId * 4;

    vec3 baseColor = materials[base + 0].xyz;
    float metallic = materials[base + 0].w;
    vec3 emissive  = materials[base + 1].xyz;
    float roughness = materials[base + 1].w;
    float alpha     = materials[base + 2].x;

    // Texture indices from 4th vec4
    float baseColorTexIdx  = materials[base + 3].x;
    float metalRoughTexIdx = materials[base + 3].y;
    float normalTexIdx     = materials[base + 3].z;
    float emissiveTexIdx   = materials[base + 3].w;

    if (baseColorTexIdx >= 0.0) {
        baseColor *= texture(uTextureArray, vec3(vUV, baseColorTexIdx)).rgb;
    }
    if (metalRoughTexIdx >= 0.0) {
        vec4 mr = texture(uTextureArray, vec3(vUV, metalRoughTexIdx));
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if (emissiveTexIdx >= 0.0) {
        emissive *= texture(uTextureArray, vec3(vUV, emissiveTexIdx)).rgb;
    }

    vec3 N = normalize(vNormal);
    if (normalTexIdx >= 0.0) {
        vec3 tangentNormal = texture(uTextureArray, vec3(vUV, normalTexIdx)).xyz * 2.0 - 1.0;
        N = normalize(N + tangentNormal * 0.1);
    }

    vec3 V = normalize(uCameraPos - vWorldPos);

    // Soft camera-headlight shading (USD-viewer look). Face the shading normal
    // toward the camera so back / grazing faces never read as pure black, then
    // combine a view-aligned headlight (N.V) with a gentle half-Lambert key and an
    // ambient floor -- soft, no hard terminator, no unlit black facets.
    vec3 Nf = (dot(N, V) < 0.0) ? -N : N;
    float facing = max(dot(Nf, V), 0.0);                 // N.V headlight
    vec3 L = normalize(vec3(0.3, 0.5, 0.8));
    float key = dot(Nf, L) * 0.5 + 0.5;                  // half-Lambert, never 0
    float shade = 0.6 * facing + 0.4 * key;              // [0,1]

    // Ambient floor + view-driven diffuse (keeps unlit faces lifted off black).
    vec3 ambient = baseColor * 0.25;
    vec3 diffuse = baseColor * (1.0 - metallic) * (0.75 * shade);

    vec3 H = normalize(L + V);
    float NdotH = max(dot(Nf, H), 0.0);
    float specPower = mix(16.0, 256.0, 1.0 - roughness);
    vec3 specColor = mix(vec3(0.04), baseColor, metallic);
    vec3 specular = specColor * pow(NdotH, specPower) * facing;

    vec3 color = ambient + diffuse + specular + emissive;
    fragColor = vec4(color, alpha);
}
)glsl";
}

// ==================== Vulkan 450 Shaders ====================
//
// UNUSED: the Vulkan backend compiles vk/shaders/*.vert|frag to SPIR-V (which
// carry the current morph/skin/displacement) rather than these strings. STALE
// (predate that work); kept only as a GLSL reference. Don't wire these up without
// re-deriving from vk/shaders/.

const char* getMaterialVertexShaderVK450() {
    return R"glsl(#version 450

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aUV;

layout(push_constant) uniform PushConstants {
    mat4 modelViewProj;
    mat4 model;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main() {
    vec4 worldPos = pc.model * vec4(aPosition, 1.0);
    vWorldPos = worldPos.xyz;
    // Approximate normal matrix (works for uniform scale)
    vNormal = normalize(mat3(pc.model) * aNormal);
    vUV = aUV.xy;
    gl_Position = pc.modelViewProj * vec4(aPosition, 1.0);
}
)glsl";
}

const char* getMaterialFragmentShaderVK450() {
    return R"glsl(#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

// Materials SSBO: each material is 4 x vec4 (16 floats)
layout(set = 0, binding = 1) readonly buffer Materials {
    vec4 materials[];
};

// Per-triangle material ID
layout(set = 0, binding = 2) readonly buffer FaceMaterialIds {
    uint faceMaterialId[];
};

layout(set = 0, binding = 0) uniform SceneUBO {
    vec3 cameraPos;
};

layout(set = 0, binding = 3) uniform sampler2DArray uTextureArray;

layout(location = 0) out vec4 fragColor;

void main() {
    int matId = int(faceMaterialId[gl_PrimitiveID]);
    int base = matId * 4;

    vec3 baseColor = materials[base + 0].xyz;
    float metallic = materials[base + 0].w;
    vec3 emissive  = materials[base + 1].xyz;
    float roughness = materials[base + 1].w;
    float alpha     = materials[base + 2].x;

    // Texture indices from 4th vec4
    float baseColorTexIdx  = materials[base + 3].x;
    float metalRoughTexIdx = materials[base + 3].y;
    float normalTexIdx     = materials[base + 3].z;
    float emissiveTexIdx   = materials[base + 3].w;

    if (baseColorTexIdx >= 0.0) {
        baseColor *= texture(uTextureArray, vec3(vUV, baseColorTexIdx)).rgb;
    }
    if (metalRoughTexIdx >= 0.0) {
        vec4 mr = texture(uTextureArray, vec3(vUV, metalRoughTexIdx));
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if (emissiveTexIdx >= 0.0) {
        emissive *= texture(uTextureArray, vec3(vUV, emissiveTexIdx)).rgb;
    }

    vec3 N = normalize(vNormal);
    if (normalTexIdx >= 0.0) {
        vec3 tangentNormal = texture(uTextureArray, vec3(vUV, normalTexIdx)).xyz * 2.0 - 1.0;
        N = normalize(N + tangentNormal * 0.1);
    }

    vec3 V = normalize(cameraPos - vWorldPos);

    // Soft camera-headlight shading (USD-viewer look). Face the shading normal
    // toward the camera so back / grazing faces never read as pure black, then
    // combine a view-aligned headlight (N.V) with a gentle half-Lambert key and an
    // ambient floor -- soft, no hard terminator, no unlit black facets.
    vec3 Nf = (dot(N, V) < 0.0) ? -N : N;
    float facing = max(dot(Nf, V), 0.0);                 // N.V headlight
    vec3 L = normalize(vec3(0.3, 0.5, 0.8));
    float key = dot(Nf, L) * 0.5 + 0.5;                  // half-Lambert, never 0
    float shade = 0.6 * facing + 0.4 * key;              // [0,1]

    // Ambient floor + view-driven diffuse (keeps unlit faces lifted off black).
    vec3 ambient = baseColor * 0.25;
    vec3 diffuse = baseColor * (1.0 - metallic) * (0.75 * shade);

    vec3 H = normalize(L + V);
    float NdotH = max(dot(Nf, H), 0.0);
    float specPower = mix(16.0, 256.0, 1.0 - roughness);
    vec3 specColor = mix(vec3(0.04), baseColor, metallic);
    vec3 specular = specColor * pow(NdotH, specPower) * facing;

    vec3 color = ambient + diffuse + specular + emissive;
    fragColor = vec4(color, alpha);
}
)glsl";
}

} // namespace light3d
