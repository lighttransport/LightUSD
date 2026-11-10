// LightUSD WebGPU + OpenPBR Demo
// High-performance USD viewer using WebGPU with:
// - Uber shader for OpenPBR materials
// - UBO-based material parameters for efficient multi-material rendering
// - Bindless textures via texture arrays
// - Minimal draw calls through material batching

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import {Matrix4,Vector3} from 'three';
import {createCoreRenderInspector,getCoreInspectorMesh} from './src/lightusd/CoreRenderInspector.js';
import {coreWebGPUImage} from './src/lightusd/CoreWebGPUTexture.js';

// ============================================================================
// WGSL Uber Shader for OpenPBR
// ============================================================================

const OPENPBR_SHADER = /* wgsl */`
// ============================================================================
// Uniform Structures
// ============================================================================

struct CameraUniforms {
    viewProjection: mat4x4<f32>,
    view: mat4x4<f32>,
    projection: mat4x4<f32>,
    cameraPosition: vec3<f32>,
    _pad0: f32,
    invViewProjection: mat4x4<f32>,
};

struct SceneUniforms {
    envIntensity: f32,
    exposure: f32,
    toneMapping: u32, // 0=linear, 1=reinhard, 2=aces, 3=agx
    time: f32,
    resolution: vec2<f32>,
    _pad: vec2<f32>,
};

// OpenPBR Material parameters (144 bytes, aligned to 16)
struct MaterialParams {
    // Base layer (48 bytes)
    baseColor: vec3<f32>,
    baseWeight: f32,
    baseMetalness: f32,
    baseDiffuseRoughness: f32,
    _pad0: vec2<f32>,

    // Specular layer (32 bytes)
    specularColor: vec3<f32>,
    specularWeight: f32,
    specularRoughness: f32,
    specularIOR: f32,
    specularAnisotropy: f32,
    _pad1: f32,

    // Transmission layer (16 bytes)
    transmissionWeight: f32,
    transmissionDepth: f32,
    transmissionDispersion: f32,
    _pad2: f32,

    // Coat layer (32 bytes)
    coatColor: vec3<f32>,
    coatWeight: f32,
    coatRoughness: f32,
    coatIOR: f32,
    _pad3: vec2<f32>,

    // Emission layer (16 bytes)
    emissionColor: vec3<f32>,
    emissionLuminance: f32,

    // Sheen/Fuzz layer (16 bytes)
    sheenColor: vec3<f32>,
    sheenWeight: f32,

    // Texture indices (-1 = no texture) (32 bytes)
    baseColorTexIdx: i32,
    normalTexIdx: i32,
    roughnessTexIdx: i32,
    metalnessTexIdx: i32,
    emissiveTexIdx: i32,
    aoTexIdx: i32,
    _texPad: vec2<i32>,
};

// Per-instance data for batched rendering
struct InstanceData {
    modelMatrix: mat4x4<f32>,
    normalMatrix: mat4x4<f32>,
    materialIndex: u32,
    _pad: vec3<u32>,
};

// ============================================================================
// Bindings
// ============================================================================

@group(0) @binding(0) var<uniform> camera: CameraUniforms;
@group(0) @binding(1) var<uniform> scene: SceneUniforms;

// Material UBO array (supports up to 256 materials)
@group(1) @binding(0) var<storage, read> materials: array<MaterialParams>;

// Instance data storage buffer
@group(1) @binding(1) var<storage, read> instances: array<InstanceData>;

// Texture arrays for bindless textures
@group(2) @binding(0) var textureSampler: sampler;
@group(2) @binding(1) var baseColorTextures: texture_2d_array<f32>;
@group(2) @binding(2) var normalTextures: texture_2d_array<f32>;
@group(2) @binding(3) var pbrTextures: texture_2d_array<f32>; // R=roughness, G=metalness, B=ao

// Environment map
@group(3) @binding(0) var envSampler: sampler;
@group(3) @binding(1) var envMap: texture_cube<f32>;
@group(3) @binding(2) var irradianceMap: texture_cube<f32>;
@group(3) @binding(3) var prefilteredMap: texture_cube<f32>;
@group(3) @binding(4) var brdfLUT: texture_2d<f32>;

// ============================================================================
// Vertex Shader
// ============================================================================

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) tangent: vec4<f32>,
    @builtin(instance_index) instanceIndex: u32,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldPosition: vec3<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) @interpolate(flat) materialIndex: u32,
    @location(4) worldTangent: vec3<f32>,
    @location(5) worldBitangent: vec3<f32>,
};

@vertex
fn vertexMain(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;

    let instance = instances[input.instanceIndex];
    let worldPos = instance.modelMatrix * vec4<f32>(input.position, 1.0);

    output.position = camera.viewProjection * worldPos;
    output.worldPosition = worldPos.xyz;
    output.worldNormal = normalize((instance.normalMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.uv = input.uv;
    output.materialIndex = instance.materialIndex;

    // Tangent space
    let worldTangent = normalize((instance.modelMatrix * vec4<f32>(input.tangent.xyz, 0.0)).xyz);
    output.worldTangent = worldTangent;
    output.worldBitangent = cross(output.worldNormal, worldTangent) * input.tangent.w;

    return output;
}

// ============================================================================
// PBR Helper Functions
// ============================================================================

const PI: f32 = 3.14159265359;
const INV_PI: f32 = 0.31830988618;

// Fresnel-Schlick approximation
fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

fn fresnelSchlickRoughness(cosTheta: f32, F0: vec3<f32>, roughness: f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// GGX Normal Distribution Function
fn distributionGGX(NdotH: f32, roughness: f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Smith's geometry function for GGX
fn geometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
    let r = roughness + 1.0;
    let k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn geometrySmith(NdotV: f32, NdotL: f32, roughness: f32) -> f32 {
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// Oren-Nayar diffuse for diffuse roughness
fn orenNayarDiffuse(NdotL: f32, NdotV: f32, LdotV: f32, roughness: f32) -> f32 {
    let sigma2 = roughness * roughness;
    let A = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    let B = 0.45 * sigma2 / (sigma2 + 0.09);

    let cosPhiDiff = LdotV - NdotL * NdotV;
    let sinNL = sqrt(saturate(1.0 - NdotL * NdotL));
    let sinNV = sqrt(saturate(1.0 - NdotV * NdotV));
    let s = max(sinNL, sinNV);
    let t = select(0.0, min(sinNL, sinNV) / max(NdotL, NdotV), s > 0.0);

    return A + B * max(0.0, cosPhiDiff) * s * t;
}

// Sample environment map with roughness-based mip level
fn sampleEnvMap(R: vec3<f32>, roughness: f32) -> vec3<f32> {
    // Use prefiltered environment map if available
    let mipLevel = roughness * 7.0; // Assume 8 mip levels
    return textureSampleLevel(prefilteredMap, envSampler, R, mipLevel).rgb;
}

// Sample irradiance map for diffuse IBL
fn sampleIrradiance(N: vec3<f32>) -> vec3<f32> {
    return textureSample(irradianceMap, envSampler, N).rgb;
}

// ============================================================================
// Tone Mapping
// ============================================================================

fn linearToneMap(color: vec3<f32>) -> vec3<f32> {
    return saturate(color);
}

fn reinhardToneMap(color: vec3<f32>) -> vec3<f32> {
    return color / (color + vec3<f32>(1.0));
}

fn acesToneMap(color: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

fn agxToneMap(color: vec3<f32>) -> vec3<f32> {
    // Simplified AgX tonemapping
    let agxMatrix = mat3x3<f32>(
        vec3<f32>(0.842479, 0.0423303, 0.0423765),
        vec3<f32>(0.0784336, 0.878469, 0.0784336),
        vec3<f32>(0.0792227, 0.0791407, 0.879142)
    );
    var result = agxMatrix * color;
    result = max(result, vec3<f32>(0.0));
    result = pow(result, vec3<f32>(1.0 / 2.2));
    return saturate(result);
}

fn applyToneMapping(color: vec3<f32>, mode: u32) -> vec3<f32> {
    switch(mode) {
        case 0u: { return linearToneMap(color); }
        case 1u: { return reinhardToneMap(color); }
        case 2u: { return acesToneMap(color); }
        case 3u: { return agxToneMap(color); }
        default: { return acesToneMap(color); }
    }
}

// sRGB gamma correction
fn linearToSRGB(color: vec3<f32>) -> vec3<f32> {
    let cutoff = color < vec3<f32>(0.0031308);
    let higher = vec3<f32>(1.055) * pow(color, vec3<f32>(1.0/2.4)) - vec3<f32>(0.055);
    let lower = color * vec3<f32>(12.92);
    return select(higher, lower, cutoff);
}

// ============================================================================
// Fragment Shader
// ============================================================================

@fragment
fn fragmentMain(input: VertexOutput) -> @location(0) vec4<f32> {
    let mat = materials[input.materialIndex];

    // Sample textures if available
    var baseColor = mat.baseColor;
    if (mat.baseColorTexIdx >= 0) {
        let texColor = textureSample(baseColorTextures, textureSampler, input.uv, mat.baseColorTexIdx);
        // Texture is in sRGB, convert to linear
        baseColor = pow(texColor.rgb, vec3<f32>(2.2)) * mat.baseColor;
    }

    var roughness = mat.specularRoughness;
    var metalness = mat.baseMetalness;
    var ao = 1.0;
    if (mat.roughnessTexIdx >= 0) {
        let pbrSample = textureSample(pbrTextures, textureSampler, input.uv, mat.roughnessTexIdx);
        roughness = pbrSample.r * mat.specularRoughness;
        metalness = pbrSample.g * mat.baseMetalness;
        ao = pbrSample.b;
    }

    // Normal mapping
    var N = normalize(input.worldNormal);
    if (mat.normalTexIdx >= 0) {
        let tangentNormal = textureSample(normalTextures, textureSampler, input.uv, mat.normalTexIdx).rgb * 2.0 - 1.0;
        let TBN = mat3x3<f32>(
            normalize(input.worldTangent),
            normalize(input.worldBitangent),
            N
        );
        N = normalize(TBN * tangentNormal);
    }

    // View and reflection vectors
    let V = normalize(camera.cameraPosition - input.worldPosition);
    let R = reflect(-V, N);

    let NdotV = max(dot(N, V), 0.001);

    // Calculate F0 (reflectance at normal incidence)
    let dielectricF0 = vec3<f32>(pow((mat.specularIOR - 1.0) / (mat.specularIOR + 1.0), 2.0));
    let F0 = mix(dielectricF0 * mat.specularColor, baseColor, metalness);

    // ========== IBL Lighting ==========

    // Diffuse IBL
    let irradiance = sampleIrradiance(N) * scene.envIntensity;
    let diffuseIBL = irradiance * baseColor * (1.0 - metalness) * mat.baseWeight;

    // Oren-Nayar diffuse roughness factor (simplified for IBL)
    let diffuseRoughnessFactor = 1.0 - mat.baseDiffuseRoughness * 0.5;

    // Specular IBL
    let prefilteredColor = sampleEnvMap(R, roughness) * scene.envIntensity;
    let brdf = textureSample(brdfLUT, textureSampler, vec2<f32>(NdotV, roughness)).rg;
    let F = fresnelSchlickRoughness(NdotV, F0, roughness);
    let specularIBL = prefilteredColor * (F * brdf.x + brdf.y) * mat.specularWeight;

    // ========== Coat Layer ==========
    var coatContrib = vec3<f32>(0.0);
    if (mat.coatWeight > 0.0) {
        let coatF0 = vec3<f32>(pow((mat.coatIOR - 1.0) / (mat.coatIOR + 1.0), 2.0));
        let coatF = fresnelSchlickRoughness(NdotV, coatF0, mat.coatRoughness);
        let coatSpecular = sampleEnvMap(R, mat.coatRoughness) * coatF;
        coatContrib = coatSpecular * mat.coatColor * mat.coatWeight * scene.envIntensity;
    }

    // ========== Sheen/Fuzz Layer ==========
    var sheenContrib = vec3<f32>(0.0);
    if (mat.sheenWeight > 0.0) {
        // Simple sheen approximation
        let sheenFresnel = pow(1.0 - NdotV, 5.0);
        sheenContrib = mat.sheenColor * sheenFresnel * mat.sheenWeight * irradiance;
    }

    // ========== Emission ==========
    let emission = mat.emissionColor * mat.emissionLuminance;

    // ========== Combine ==========
    var finalColor = (diffuseIBL * diffuseRoughnessFactor + specularIBL) * ao;
    finalColor += coatContrib;
    finalColor += sheenContrib;
    finalColor += emission;

    // ========== Transmission (simple approximation) ==========
    if (mat.transmissionWeight > 0.0) {
        // Simple refraction approximation
        let transmissionColor = sampleEnvMap(-V, roughness) * baseColor;
        finalColor = mix(finalColor, transmissionColor, mat.transmissionWeight * (1.0 - metalness));
    }

    // Apply exposure
    finalColor *= scene.exposure;

    // Tone mapping
    finalColor = applyToneMapping(finalColor, scene.toneMapping);

    // Gamma correction (linear to sRGB)
    finalColor = linearToSRGB(finalColor);

    return vec4<f32>(finalColor, 1.0);
}
`;

// Fallback simple shader when env maps are not loaded
const SIMPLE_SHADER = /* wgsl */`
struct CameraUniforms {
    viewProjection: mat4x4<f32>,
    view: mat4x4<f32>,
    projection: mat4x4<f32>,
    cameraPosition: vec3<f32>,
    _pad0: f32,
    invViewProjection: mat4x4<f32>,
};

struct SceneUniforms {
    envIntensity: f32,
    exposure: f32,
    toneMapping: u32,
    time: f32,
    resolution: vec2<f32>,
    _pad: vec2<f32>,
};

struct MaterialParams {
    baseColor: vec3<f32>,
    baseWeight: f32,
    baseMetalness: f32,
    baseDiffuseRoughness: f32,
    _pad0: vec2<f32>,
    specularColor: vec3<f32>,
    specularWeight: f32,
    specularRoughness: f32,
    specularIOR: f32,
    specularAnisotropy: f32,
    _pad1: f32,
    transmissionWeight: f32,
    transmissionDepth: f32,
    transmissionDispersion: f32,
    _pad2: f32,
    coatColor: vec3<f32>,
    coatWeight: f32,
    coatRoughness: f32,
    coatIOR: f32,
    _pad3: vec2<f32>,
    emissionColor: vec3<f32>,
    emissionLuminance: f32,
    sheenColor: vec3<f32>,
    sheenWeight: f32,
    baseColorTexIdx: i32,
    normalTexIdx: i32,
    roughnessTexIdx: i32,
    metalnessTexIdx: i32,
    emissiveTexIdx: i32,
    aoTexIdx: i32,
    _texPad: vec2<i32>,
};

struct InstanceData {
    modelMatrix: mat4x4<f32>,
    normalMatrix: mat4x4<f32>,
    materialIndex: u32,
    _pad: vec3<u32>,
};

@group(0) @binding(0) var<uniform> camera: CameraUniforms;
@group(0) @binding(1) var<uniform> scene: SceneUniforms;
@group(1) @binding(0) var<storage, read> materials: array<MaterialParams>;
@group(1) @binding(1) var<storage, read> instances: array<InstanceData>;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) tangent: vec4<f32>,
    @builtin(instance_index) instanceIndex: u32,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldPosition: vec3<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) @interpolate(flat) materialIndex: u32,
};

@vertex
fn vertexMain(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let instance = instances[input.instanceIndex];
    let worldPos = instance.modelMatrix * vec4<f32>(input.position, 1.0);
    output.position = camera.viewProjection * worldPos;
    output.worldPosition = worldPos.xyz;
    output.worldNormal = normalize((instance.normalMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.uv = input.uv;
    output.materialIndex = instance.materialIndex;
    return output;
}

const PI: f32 = 3.14159265359;

fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

@fragment
fn fragmentMain(input: VertexOutput) -> @location(0) vec4<f32> {
    let mat = materials[input.materialIndex];
    let baseColor = mat.baseColor;
    let roughness = mat.specularRoughness;
    let metalness = mat.baseMetalness;

    let N = normalize(input.worldNormal);
    let V = normalize(camera.cameraPosition - input.worldPosition);
    let NdotV = max(dot(N, V), 0.001);

    // Simple directional light
    let lightDir = normalize(vec3<f32>(1.0, 1.0, 1.0));
    let NdotL = max(dot(N, lightDir), 0.0);
    let H = normalize(V + lightDir);
    let NdotH = max(dot(N, H), 0.0);

    let F0 = mix(vec3<f32>(0.04), baseColor, metalness);
    let F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    // Diffuse
    let diffuse = baseColor * (1.0 - metalness) * NdotL * INV_PI;

    // Specular (simple)
    let specPower = (1.0 - roughness) * 64.0 + 2.0;
    let specular = F * pow(NdotH, specPower) * NdotL;

    // Ambient
    let ambient = baseColor * 0.03;

    // Emission
    let emission = mat.emissionColor * mat.emissionLuminance;

    var finalColor = ambient + diffuse + specular + emission;
    finalColor *= scene.exposure;

    // Gamma correction
    finalColor = pow(finalColor, vec3<f32>(1.0/2.2));

    return vec4<f32>(finalColor, 1.0);
}

const INV_PI: f32 = 0.31830988618;
`;

// Shader with texture and IBL support
const IBL_SHADER = /* wgsl */`
struct CameraUniforms {
    viewProjection: mat4x4<f32>,
    view: mat4x4<f32>,
    projection: mat4x4<f32>,
    cameraPosition: vec3<f32>,
    _pad0: f32,
    invViewProjection: mat4x4<f32>,
};

struct SceneUniforms {
    envIntensity: f32,
    exposure: f32,
    toneMapping: u32,
    time: f32,
    resolution: vec2<f32>,
    _pad: vec2<f32>,
};

struct MaterialParams {
    baseColor: vec3<f32>,
    baseWeight: f32,
    baseMetalness: f32,
    baseDiffuseRoughness: f32,
    _pad0: vec2<f32>,
    specularColor: vec3<f32>,
    specularWeight: f32,
    specularRoughness: f32,
    specularIOR: f32,
    specularAnisotropy: f32,
    _pad1: f32,
    transmissionWeight: f32,
    transmissionDepth: f32,
    transmissionDispersion: f32,
    _pad2: f32,
    coatColor: vec3<f32>,
    coatWeight: f32,
    coatRoughness: f32,
    coatIOR: f32,
    _pad3: vec2<f32>,
    emissionColor: vec3<f32>,
    emissionLuminance: f32,
    sheenColor: vec3<f32>,
    sheenWeight: f32,
    baseColorTexIdx: i32,
    normalTexIdx: i32,
    roughnessTexIdx: i32,
    metalnessTexIdx: i32,
    emissiveTexIdx: i32,
    aoTexIdx: i32,
    _texPad: vec2<i32>,
};

struct InstanceData {
    modelMatrix: mat4x4<f32>,
    normalMatrix: mat4x4<f32>,
    materialIndex: u32,
    _pad: vec3<u32>,
};

@group(0) @binding(0) var<uniform> camera: CameraUniforms;
@group(0) @binding(1) var<uniform> scene: SceneUniforms;
@group(1) @binding(0) var<storage, read> materials: array<MaterialParams>;
@group(1) @binding(1) var<storage, read> instances: array<InstanceData>;

// Textures
@group(2) @binding(0) var texSampler: sampler;
@group(2) @binding(1) var baseColorTex: texture_2d<f32>;
@group(2) @binding(2) var normalTex: texture_2d<f32>;
@group(2) @binding(3) var ormTex: texture_2d<f32>;
@group(2) @binding(4) var emissiveTex: texture_2d<f32>;

// IBL
@group(3) @binding(0) var envSampler: sampler;
@group(3) @binding(1) var irradianceMap: texture_cube<f32>;
@group(3) @binding(2) var prefilteredMap: texture_cube<f32>;
@group(3) @binding(3) var brdfLUT: texture_2d<f32>;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) tangent: vec4<f32>,
    @builtin(instance_index) instanceIndex: u32,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldPosition: vec3<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) @interpolate(flat) materialIndex: u32,
    @location(4) worldTangent: vec3<f32>,
    @location(5) worldBitangent: vec3<f32>,
};

@vertex
fn vertexMain(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    let instance = instances[input.instanceIndex];
    let worldPos = instance.modelMatrix * vec4<f32>(input.position, 1.0);
    output.position = camera.viewProjection * worldPos;
    output.worldPosition = worldPos.xyz;
    output.worldNormal = normalize((instance.normalMatrix * vec4<f32>(input.normal, 0.0)).xyz);
    output.uv = input.uv;
    output.materialIndex = instance.materialIndex;

    let worldTangent = normalize((instance.modelMatrix * vec4<f32>(input.tangent.xyz, 0.0)).xyz);
    output.worldTangent = worldTangent;
    output.worldBitangent = cross(output.worldNormal, worldTangent) * input.tangent.w;

    return output;
}

const PI: f32 = 3.14159265359;
const INV_PI: f32 = 0.31830988618;

fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

fn fresnelSchlickRoughness(cosTheta: f32, F0: vec3<f32>, roughness: f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

fn distributionGGX(NdotH: f32, roughness: f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

fn geometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
    let r = roughness + 1.0;
    let k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn geometrySmith(NdotV: f32, NdotL: f32, roughness: f32) -> f32 {
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

fn sRGBToLinear(color: vec3<f32>) -> vec3<f32> {
    return pow(color, vec3<f32>(2.2));
}

fn linearToSRGB(color: vec3<f32>) -> vec3<f32> {
    return pow(color, vec3<f32>(1.0 / 2.2));
}

fn acesToneMap(color: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

@fragment
fn fragmentMain(input: VertexOutput) -> @location(0) vec4<f32> {
    let mat = materials[input.materialIndex];

    // Sample ALL textures unconditionally (WGSL requires uniform control flow for textureSample)
    let baseColorSample = textureSample(baseColorTex, texSampler, input.uv);
    let ormSample = textureSample(ormTex, texSampler, input.uv);
    let normalSample = textureSample(normalTex, texSampler, input.uv);
    let emissiveSample = textureSample(emissiveTex, texSampler, input.uv);

    // Apply base color texture if available
    var baseColor = mat.baseColor;
    if (mat.baseColorTexIdx >= 0) {
        baseColor = sRGBToLinear(baseColorSample.rgb) * mat.baseColor;
    }

    // Apply ORM texture (Occlusion, Roughness, Metalness) if available
    var roughness = mat.specularRoughness;
    var metalness = mat.baseMetalness;
    var ao = 1.0;
    if (mat.roughnessTexIdx >= 0) {
        ao = ormSample.r;
        roughness = ormSample.g * mat.specularRoughness;
        metalness = ormSample.b * mat.baseMetalness;
    }

    // Normal mapping
    var N = normalize(input.worldNormal);
    if (mat.normalTexIdx >= 0) {
        let tangentNormal = normalSample.rgb * 2.0 - 1.0;
        let TBN = mat3x3<f32>(
            normalize(input.worldTangent),
            normalize(input.worldBitangent),
            N
        );
        N = normalize(TBN * tangentNormal);
    }

    // Emission
    var emission = mat.emissionColor * mat.emissionLuminance;
    if (mat.emissiveTexIdx >= 0) {
        emission = sRGBToLinear(emissiveSample.rgb) * mat.emissionLuminance;
    }

    let V = normalize(camera.cameraPosition - input.worldPosition);
    let R = reflect(-V, N);
    let NdotV = max(dot(N, V), 0.001);

    // Calculate F0
    let dielectricF0 = vec3<f32>(pow((mat.specularIOR - 1.0) / (mat.specularIOR + 1.0), 2.0));
    let F0 = mix(dielectricF0 * mat.specularColor, baseColor, metalness);

    // IBL - Diffuse
    let irradiance = textureSample(irradianceMap, envSampler, N).rgb * scene.envIntensity;
    let kS = fresnelSchlickRoughness(NdotV, F0, roughness);
    let kD = (1.0 - kS) * (1.0 - metalness);
    let diffuseIBL = kD * irradiance * baseColor;

    // IBL - Specular
    let maxMipLevel = 4.0;
    let mipLevel = roughness * maxMipLevel;
    let prefilteredColor = textureSampleLevel(prefilteredMap, envSampler, R, mipLevel).rgb * scene.envIntensity;
    let brdf = textureSample(brdfLUT, texSampler, vec2<f32>(NdotV, roughness)).rg;
    let specularIBL = prefilteredColor * (kS * brdf.x + brdf.y);

    // Simple directional light for additional lighting
    let lightDir = normalize(vec3<f32>(1.0, 1.0, 0.5));
    let lightColor = vec3<f32>(1.0, 0.98, 0.95);
    let NdotL = max(dot(N, lightDir), 0.0);
    let H = normalize(V + lightDir);
    let NdotH = max(dot(N, H), 0.0);

    let D = distributionGGX(NdotH, roughness);
    let G = geometrySmith(NdotV, NdotL, roughness);
    let F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    let numerator = D * G * F;
    let denominator = 4.0 * NdotV * NdotL + 0.0001;
    let specularDirect = numerator / denominator;

    let diffuseDirect = baseColor * (1.0 - metalness) * INV_PI;
    let directLight = (diffuseDirect + specularDirect) * lightColor * NdotL * 0.5;

    // Combine
    var finalColor = (diffuseIBL + specularIBL) * ao + directLight + emission;

    // Coat layer
    if (mat.coatWeight > 0.0) {
        let coatF0 = vec3<f32>(pow((mat.coatIOR - 1.0) / (mat.coatIOR + 1.0), 2.0));
        let coatF = fresnelSchlickRoughness(NdotV, coatF0, mat.coatRoughness);
        let coatMipLevel = mat.coatRoughness * maxMipLevel;
        let coatSpecular = textureSampleLevel(prefilteredMap, envSampler, R, coatMipLevel).rgb;
        finalColor += coatSpecular * coatF * mat.coatColor * mat.coatWeight * scene.envIntensity;
    }

    // Exposure
    finalColor *= scene.exposure;

    // Tone mapping (ACES)
    finalColor = acesToneMap(finalColor);

    // Gamma correction
    finalColor = linearToSRGB(finalColor);

    return vec4<f32>(finalColor, 1.0);
}
`;

// Float32 to Float16 conversion
function floatToHalf(value) {
    const floatView = new Float32Array(1);
    const int32View = new Int32Array(floatView.buffer);
    floatView[0] = value;
    const x = int32View[0];

    let bits = (x >> 16) & 0x8000; // sign
    let m = (x >> 12) & 0x07ff;    // mantissa
    let e = (x >> 23) & 0xff;      // exponent

    if (e < 103) {
        return bits;
    }
    if (e > 142) {
        bits |= 0x7c00;
        bits |= ((e === 255) ? 0 : 1) && (x & 0x007fffff);
        return bits;
    }
    if (e < 113) {
        m |= 0x0800;
        bits |= (m >> (114 - e)) + ((m >> (113 - e)) & 1);
        return bits;
    }

    bits |= ((e - 112) << 10) | (m >> 1);
    bits += m & 1;
    return bits;
}

// ============================================================================
// WebGPU Renderer Class
// ============================================================================

class WebGPURenderer {
    constructor(canvas) {
        this.canvas = canvas;
        this.device = null;
        this.context = null;
        this.format = null;

        // Pipelines
        this.pipeline = null;
        this.simplePipeline = null;

        // Buffers
        this.cameraBuffer = null;
        this.sceneBuffer = null;
        this.materialBuffer = null;
        this.instanceBuffer = null;

        // Bind groups
        this.cameraBindGroup = null;
        this.materialBindGroup = null;
        this.textureBindGroup = null;
        this.envBindGroup = null;

        // Texture arrays
        this.baseColorTextureArray = null;
        this.normalTextureArray = null;
        this.pbrTextureArray = null;
        this.textureArraySize = 0;

        // Environment maps
        this.envMap = null;
        this.irradianceMap = null;
        this.prefilteredMap = null;
        this.brdfLUT = null;
        this.envLoaded = false;

        // Depth buffer
        this.depthTexture = null;

        // Scene data
        this.meshes = [];
        this.materials = [];
        this.instances = [];

        // Camera
        this.camera = {
            position: [3, 2, 5],
            target: [0, 0, 0],
            up: [0, 1, 0],
            fov: 45,
            near: 0.1,
            far: 1000
        };

        // Scene settings
        this.settings = {
            envIntensity: 1.0,
            exposure: 1.0,
            toneMapping: 2, // ACES
            showWireframe: false,
            showNormals: false
        };

        // Stats
        this.stats = {
            fps: 0,
            gpuTime: 0,
            uboUpdates: 0,
            drawCalls: 0,
            triangles: 0
        };

        this.lastFrameTime = performance.now();
        this.frameCount = 0;
    }

    async init() {
        if (!navigator.gpu) {
            throw new Error('WebGPU not supported');
        }

        const adapter = await navigator.gpu.requestAdapter({
            powerPreference: 'high-performance'
        });

        if (!adapter) {
            throw new Error('No WebGPU adapter found');
        }

        this.device = await adapter.requestDevice({
            requiredFeatures: [],
            requiredLimits: {
                maxStorageBufferBindingSize: 256 * 1024 * 1024, // 256MB for large scenes
                maxBufferSize: 256 * 1024 * 1024
            }
        });

        this.context = this.canvas.getContext('webgpu');
        this.format = navigator.gpu.getPreferredCanvasFormat();

        this.context.configure({
            device: this.device,
            format: this.format,
            alphaMode: 'premultiplied'
        });

        await this.createBuffers();
        await this.createPipelines();
        this.createDepthBuffer();

        console.log('WebGPU initialized');
    }

    async createBuffers() {
        // Camera uniform buffer (288 bytes aligned to 16)
        // Layout: viewProjection(64) + view(64) + projection(64) + cameraPosition(16) + invViewProjection(64) = 272, rounded to 288
        this.cameraBuffer = this.device.createBuffer({
            size: 288,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
        });

        // Scene uniform buffer
        this.sceneBuffer = this.device.createBuffer({
            size: 32,
            usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
        });

        // Material storage buffer (supports up to 256 materials, 192 bytes each)
        this.materialBuffer = this.device.createBuffer({
            size: 256 * 192,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST
        });

        // Instance storage buffer (supports up to 65536 instances)
        this.instanceBuffer = this.device.createBuffer({
            size: 65536 * 144, // 144 bytes per instance
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST
        });

        // Create placeholder textures for bind groups
        await this.createPlaceholderTextures();
    }

    async createPlaceholderTextures() {
        // Create 1x1 white placeholder texture
        const whiteData = new Uint8Array([255, 255, 255, 255]);
        // Create 1x1 normal placeholder (pointing up: 128,128,255)
        const normalData = new Uint8Array([128, 128, 255, 255]);

        // Placeholder 2D textures (will be replaced when textures are loaded)
        this.textureCache = new Map(); // Cache loaded textures by URL/path
        this.loadedTextures = [];      // Array of loaded GPU textures
        this.textureIndexMap = new Map(); // Map texture path to index

        // Create single placeholder texture for when no textures are loaded
        this.placeholderTexture = this.device.createTexture({
            size: [1, 1],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
            { texture: this.placeholderTexture },
            whiteData,
            { bytesPerRow: 4 },
            { width: 1, height: 1 }
        );

        this.placeholderNormalTexture = this.device.createTexture({
            size: [1, 1],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST
        });
        this.device.queue.writeTexture(
            { texture: this.placeholderNormalTexture },
            normalData,
            { bytesPerRow: 4 },
            { width: 1, height: 1 }
        );

        // Environment map placeholders (will be replaced when env map is loaded)
        this.envCubeMap = this.createPlaceholderCubeMap();
        this.irradianceMap = this.createPlaceholderCubeMap();
        this.prefilteredMap = this.createPlaceholderCubeMap(true); // with mipmaps
        this.envMapLoaded = false;

        // Generate BRDF LUT
        await this.generateBRDFLUT();

        // Create samplers
        this.textureSampler = this.device.createSampler({
            magFilter: 'linear',
            minFilter: 'linear',
            mipmapFilter: 'linear',
            addressModeU: 'repeat',
            addressModeV: 'repeat',
            maxAnisotropy: 16
        });

        this.envSampler = this.device.createSampler({
            magFilter: 'linear',
            minFilter: 'linear',
            mipmapFilter: 'linear',
            addressModeU: 'clamp-to-edge',
            addressModeV: 'clamp-to-edge',
            addressModeW: 'clamp-to-edge'
        });
    }

    createPlaceholderCubeMap(withMipmaps = false) {
        const size = 4;
        const mipLevelCount = withMipmaps ? Math.floor(Math.log2(size)) + 1 : 1;
        return this.device.createTexture({
            size: [size, size, 6],
            format: 'rgba8unorm',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
            mipLevelCount
        });
    }

    async generateBRDFLUT() {
        const size = 256;
        this.brdfLUT = this.device.createTexture({
            size: [size, size],
            format: 'rgba16float',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.STORAGE_BINDING
        });

        // BRDF LUT compute shader
        const brdfComputeShader = /* wgsl */`
            @group(0) @binding(0) var outputTexture: texture_storage_2d<rgba16float, write>;

            const PI: f32 = 3.14159265359;

            fn radicalInverse_VdC(bits_in: u32) -> f32 {
                var bits = bits_in;
                bits = (bits << 16u) | (bits >> 16u);
                bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
                bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
                bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
                bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
                return f32(bits) * 2.3283064365386963e-10;
            }

            fn hammersley(i: u32, N: u32) -> vec2<f32> {
                return vec2<f32>(f32(i) / f32(N), radicalInverse_VdC(i));
            }

            fn importanceSampleGGX(Xi: vec2<f32>, N: vec3<f32>, roughness: f32) -> vec3<f32> {
                let a = roughness * roughness;
                let phi = 2.0 * PI * Xi.x;
                let cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
                let sinTheta = sqrt(1.0 - cosTheta * cosTheta);

                let H = vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

                let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(N.z) < 0.999);
                let tangent = normalize(cross(up, N));
                let bitangent = cross(N, tangent);

                return normalize(tangent * H.x + bitangent * H.y + N * H.z);
            }

            fn geometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
                let a = roughness;
                let k = (a * a) / 2.0;
                return NdotV / (NdotV * (1.0 - k) + k);
            }

            fn geometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32) -> f32 {
                let NdotV = max(dot(N, V), 0.0);
                let NdotL = max(dot(N, L), 0.0);
                return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
            }

            fn integrateBRDF(NdotV: f32, roughness: f32) -> vec2<f32> {
                let V = vec3<f32>(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
                var A = 0.0;
                var B = 0.0;
                let N = vec3<f32>(0.0, 0.0, 1.0);
                let SAMPLE_COUNT = 1024u;

                for (var i = 0u; i < SAMPLE_COUNT; i++) {
                    let Xi = hammersley(i, SAMPLE_COUNT);
                    let H = importanceSampleGGX(Xi, N, roughness);
                    let L = normalize(2.0 * dot(V, H) * H - V);

                    let NdotL = max(L.z, 0.0);
                    let NdotH = max(H.z, 0.0);
                    let VdotH = max(dot(V, H), 0.0);

                    if (NdotL > 0.0) {
                        let G = geometrySmith(N, V, L, roughness);
                        let G_Vis = (G * VdotH) / (NdotH * NdotV);
                        let Fc = pow(1.0 - VdotH, 5.0);

                        A += (1.0 - Fc) * G_Vis;
                        B += Fc * G_Vis;
                    }
                }

                return vec2<f32>(A, B) / f32(SAMPLE_COUNT);
            }

            @compute @workgroup_size(16, 16)
            fn main(@builtin(global_invocation_id) id: vec3<u32>) {
                let dimensions = textureDimensions(outputTexture);
                if (id.x >= dimensions.x || id.y >= dimensions.y) {
                    return;
                }

                let NdotV = (f32(id.x) + 0.5) / f32(dimensions.x);
                let roughness = (f32(id.y) + 0.5) / f32(dimensions.y);

                let brdf = integrateBRDF(max(NdotV, 0.001), max(roughness, 0.001));
                textureStore(outputTexture, vec2<i32>(id.xy), vec4<f32>(brdf, 0.0, 1.0));
            }
        `;

        const brdfModule = this.device.createShaderModule({ code: brdfComputeShader });
        const brdfPipeline = this.device.createComputePipeline({
            layout: 'auto',
            compute: { module: brdfModule, entryPoint: 'main' }
        });

        const brdfBindGroup = this.device.createBindGroup({
            layout: brdfPipeline.getBindGroupLayout(0),
            entries: [{ binding: 0, resource: this.brdfLUT.createView() }]
        });

        const commandEncoder = this.device.createCommandEncoder();
        const passEncoder = commandEncoder.beginComputePass();
        passEncoder.setPipeline(brdfPipeline);
        passEncoder.setBindGroup(0, brdfBindGroup);
        passEncoder.dispatchWorkgroups(Math.ceil(size / 16), Math.ceil(size / 16));
        passEncoder.end();
        this.device.queue.submit([commandEncoder.finish()]);

        console.log('BRDF LUT generated');
    }

    async loadTexture(imageData, width, height, format = 'rgba8unorm', sRGB = true) {
        const texture = this.device.createTexture({
            size: [width, height],
            format: format,
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
            mipLevelCount: Math.floor(Math.log2(Math.max(width, height))) + 1
        });

        this.device.queue.writeTexture(
            { texture },
            imageData,
            { bytesPerRow: width * (format==='rgba16float'?8:4) },
            { width, height }
        );

        // Generate mipmaps
        await this.generateMipmaps(texture, width, height);

        return texture;
    }

    async generateMipmaps(texture, width, height) {
        const mipLevelCount = Math.floor(Math.log2(Math.max(width, height))) + 1;
        if (mipLevelCount <= 1) return;

        // Simple blit-based mipmap generation
        const mipmapShader = /* wgsl */`
            @group(0) @binding(0) var srcTexture: texture_2d<f32>;
            @group(0) @binding(1) var srcSampler: sampler;

            struct VertexOutput {
                @builtin(position) position: vec4<f32>,
                @location(0) uv: vec2<f32>,
            }

            @vertex
            fn vertexMain(@builtin(vertex_index) vertexIndex: u32) -> VertexOutput {
                var pos = array<vec2<f32>, 3>(
                    vec2<f32>(-1.0, -1.0),
                    vec2<f32>(3.0, -1.0),
                    vec2<f32>(-1.0, 3.0)
                );
                var output: VertexOutput;
                output.position = vec4<f32>(pos[vertexIndex], 0.0, 1.0);
                output.uv = (pos[vertexIndex] + 1.0) * 0.5;
                output.uv.y = 1.0 - output.uv.y;
                return output;
            }

            @fragment
            fn fragmentMain(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
                return textureSample(srcTexture, srcSampler, uv);
            }
        `;

        const shaderModule = this.device.createShaderModule({ code: mipmapShader });
        const pipeline = this.device.createRenderPipeline({
            layout: 'auto',
            vertex: { module: shaderModule, entryPoint: 'vertexMain' },
            fragment: {
                module: shaderModule,
                entryPoint: 'fragmentMain',
                targets: [{ format: texture.format }]
            },
            primitive: { topology: 'triangle-list' }
        });

        const sampler = this.device.createSampler({
            magFilter: 'linear',
            minFilter: 'linear'
        });

        const commandEncoder = this.device.createCommandEncoder();

        let srcView = texture.createView({ baseMipLevel: 0, mipLevelCount: 1 });
        let mipWidth = width;
        let mipHeight = height;

        for (let level = 1; level < mipLevelCount; level++) {
            mipWidth = Math.max(1, Math.floor(mipWidth / 2));
            mipHeight = Math.max(1, Math.floor(mipHeight / 2));

            const dstView = texture.createView({ baseMipLevel: level, mipLevelCount: 1 });

            const bindGroup = this.device.createBindGroup({
                layout: pipeline.getBindGroupLayout(0),
                entries: [
                    { binding: 0, resource: srcView },
                    { binding: 1, resource: sampler }
                ]
            });

            const renderPass = commandEncoder.beginRenderPass({
                colorAttachments: [{
                    view: dstView,
                    loadOp: 'clear',
                    storeOp: 'store'
                }]
            });

            renderPass.setPipeline(pipeline);
            renderPass.setBindGroup(0, bindGroup);
            renderPass.draw(3);
            renderPass.end();

            srcView = dstView;
        }

        this.device.queue.submit([commandEncoder.finish()]);
    }

    async loadEnvironmentMap(url) {
        console.log('Loading environment map:', url);

        try {
            const response = await fetch(url);
            if (!response.ok) {
                throw new Error(`Failed to fetch ${url}: ${response.status} ${response.statusText}`);
            }

            // Check content type to detect if server returned HTML error page
            const contentType = response.headers.get('content-type') || '';
            if (contentType.includes('text/html')) {
                throw new Error(`Server returned HTML instead of HDR file for ${url}`);
            }

            const ext = url.split('.').pop().toLowerCase();
            let cubeMapData;

            if (ext === 'hdr') {
                const buffer = await response.arrayBuffer();
                cubeMapData = await this.parseHDRToCubemap(buffer);
            } else {
                // Assume it's a single equirectangular image
                const blob = await response.blob();
                const imageBitmap = await createImageBitmap(blob);
                cubeMapData = await this.equirectangularToCubemap(imageBitmap);
            }

            // Create environment cubemap
            const cubeSize = cubeMapData.size;
            const mipLevels = Math.floor(Math.log2(cubeSize)) + 1;

            this.envCubeMap = this.device.createTexture({
                size: [cubeSize, cubeSize, 6],
                format: 'rgba16float',
                usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT,
                mipLevelCount: mipLevels
            });

            // Copy faces to cubemap
            for (let face = 0; face < 6; face++) {
                this.device.queue.writeTexture(
                    { texture: this.envCubeMap, origin: [0, 0, face] },
                    cubeMapData.faces[face],
                    { bytesPerRow: cubeSize * 8 },  // 4 channels * 2 bytes (float16)
                    { width: cubeSize, height: cubeSize }
                );
            }

            // Generate irradiance map
            await this.generateIrradianceMap(cubeSize);

            // Generate prefiltered map
            await this.generatePrefilteredMap(cubeSize);

            // Update bind groups
            this.envMapLoaded = true;
            this.recreateBindGroups();

            console.log('Environment map loaded successfully');

        } catch (error) {
            console.error('Failed to load environment map:', error);
            throw error; // Re-throw so caller can handle it
        }
    }

    createFallbackEnvironment() {
        // Create simple gradient environment cubemap for fallback
        const size = 64;

        // Helper to create cubemap texture
        const createCube = (cubeSize, withMips) => {
            const mipLevels = withMips ? Math.floor(Math.log2(cubeSize)) + 1 : 1;
            return this.device.createTexture({
                size: [cubeSize, cubeSize, 6],
                format: 'rgba8unorm',
                usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
                mipLevelCount: mipLevels
            });
        };

        // Create cubemap textures with a simple gradient
        this.envCubeMap = createCube(size, false);
        this.irradianceMap = createCube(32, false);
        this.prefilteredMap = createCube(size, true);

        // Fill with a simple gradient color (sky blue to horizon white)
        const faceData = new Uint8Array(size * size * 4);
        for (let y = 0; y < size; y++) {
            for (let x = 0; x < size; x++) {
                const idx = (y * size + x) * 4;
                // Gradient from top (sky blue) to bottom (warm white)
                const t = y / size;
                faceData[idx] = Math.floor(200 + 55 * t);     // R
                faceData[idx + 1] = Math.floor(220 + 35 * t); // G
                faceData[idx + 2] = 255;                       // B
                faceData[idx + 3] = 255;                       // A
            }
        }

        // Write to all 6 faces
        for (let face = 0; face < 6; face++) {
            this.device.queue.writeTexture(
                { texture: this.envCubeMap, origin: [0, 0, face] },
                faceData,
                { bytesPerRow: size * 4 },
                { width: size, height: size }
            );
            this.device.queue.writeTexture(
                { texture: this.prefilteredMap, origin: [0, 0, face] },
                faceData,
                { bytesPerRow: size * 4 },
                { width: size, height: size }
            );
        }

        // Irradiance map (smaller, solid average color)
        const irrSize = 32;
        const irrData = new Uint8Array(irrSize * irrSize * 4);
        for (let i = 0; i < irrSize * irrSize; i++) {
            irrData[i * 4] = 230;     // R
            irrData[i * 4 + 1] = 235; // G
            irrData[i * 4 + 2] = 255; // B
            irrData[i * 4 + 3] = 255; // A
        }
        for (let face = 0; face < 6; face++) {
            this.device.queue.writeTexture(
                { texture: this.irradianceMap, origin: [0, 0, face] },
                irrData,
                { bytesPerRow: irrSize * 4 },
                { width: irrSize, height: irrSize }
            );
        }

        this.envMapLoaded = true;
        this.recreateBindGroups();
        console.log('Fallback environment created');
    }

    async parseHDRToCubemap(buffer) {
        // Robust HDR parser for Radiance RGBE format
        const data = new Uint8Array(buffer);
        let offset = 0;

        console.log('Parsing HDR, buffer size:', buffer.byteLength);

        // Read header as text
        const textDecoder = new TextDecoder('ascii');
        const headerEnd = Math.min(data.length, 4096); // Headers shouldn't be more than 4KB
        const headerText = textDecoder.decode(data.subarray(0, headerEnd));

        console.log('HDR header preview:', headerText.substring(0, 100));

        // Check for valid HDR signature
        if (!headerText.startsWith('#?RADIANCE') && !headerText.startsWith('#?RGBE')) {
            console.error('HDR header bytes:', Array.from(data.subarray(0, 20)).map(b => b.toString(16).padStart(2, '0')).join(' '));
            throw new Error('Invalid HDR format: missing RADIANCE/RGBE signature');
        }

        // Find the resolution line (starts with -Y or +Y after blank line)
        const resMatch = headerText.match(/\n\n(-?\+?Y\s+\d+\s+-?\+?X\s+\d+)\n/);
        if (!resMatch) {
            // Try alternative: single newline before resolution
            const altMatch = headerText.match(/\n(-Y\s+(\d+)\s+\+X\s+(\d+))\n/);
            if (!altMatch) throw new Error('Invalid HDR format: cannot find resolution');
            offset = headerText.indexOf(altMatch[1]) + altMatch[1].length + 1;
            var height = parseInt(altMatch[2]);
            var width = parseInt(altMatch[3]);
        } else {
            const resLine = resMatch[1];
            offset = headerText.indexOf(resMatch[0]) + resMatch[0].length;
            const dimMatch = resLine.match(/-?Y\s+(\d+)\s+[+-]?X\s+(\d+)/);
            if (!dimMatch) throw new Error('Invalid HDR format: cannot parse resolution');
            var height = parseInt(dimMatch[1]);
            var width = parseInt(dimMatch[2]);
        }

        console.log(`HDR: ${width}x${height}, data offset: ${offset}`);

        // Decode RGBE data
        const pixels = new Float32Array(width * height * 4);
        let pixelIndex = 0;

        for (let y = 0; y < height; y++) {
            // Check for new RLE format (adaptive RLE)
            if (data[offset] === 2 && data[offset + 1] === 2 &&
                ((data[offset + 2] << 8) | data[offset + 3]) === width) {
                const scanlineWidth = (data[offset + 2] << 8) | data[offset + 3];
                offset += 4;

                const scanline = new Uint8Array(scanlineWidth * 4);

                for (let c = 0; c < 4; c++) {
                    let x = 0;
                    while (x < scanlineWidth) {
                        const count = data[offset++];
                        if (count > 128) {
                            const runLength = count - 128;
                            const value = data[offset++];
                            for (let i = 0; i < runLength; i++) {
                                scanline[x * 4 + c] = value;
                                x++;
                            }
                        } else {
                            for (let i = 0; i < count; i++) {
                                scanline[x * 4 + c] = data[offset++];
                                x++;
                            }
                        }
                    }
                }

                for (let x = 0; x < scanlineWidth; x++) {
                    const r = scanline[x * 4];
                    const g = scanline[x * 4 + 1];
                    const b = scanline[x * 4 + 2];
                    const e = scanline[x * 4 + 3];

                    if (e === 0) {
                        pixels[pixelIndex++] = 0;
                        pixels[pixelIndex++] = 0;
                        pixels[pixelIndex++] = 0;
                        pixels[pixelIndex++] = 1;
                    } else {
                        const scale = Math.pow(2, e - 128 - 8);
                        pixels[pixelIndex++] = r * scale;
                        pixels[pixelIndex++] = g * scale;
                        pixels[pixelIndex++] = b * scale;
                        pixels[pixelIndex++] = 1;
                    }
                }
            } else {
                // Uncompressed or old RLE format - read raw RGBE pixels
                for (let x = 0; x < width; x++) {
                    const r = data[offset++];
                    const g = data[offset++];
                    const b = data[offset++];
                    const e = data[offset++];

                    if (e === 0) {
                        pixels[pixelIndex++] = 0;
                        pixels[pixelIndex++] = 0;
                        pixels[pixelIndex++] = 0;
                        pixels[pixelIndex++] = 1;
                    } else {
                        const scale = Math.pow(2, e - 128 - 8);
                        pixels[pixelIndex++] = r * scale;
                        pixels[pixelIndex++] = g * scale;
                        pixels[pixelIndex++] = b * scale;
                        pixels[pixelIndex++] = 1;
                    }
                }
            }
        }

        // Convert equirectangular to cubemap
        return this.equirectangularDataToCubemap(pixels, width, height);
    }

    equirectangularDataToCubemap(srcPixels, srcWidth, srcHeight) {
        const cubeSize = Math.min(512, Math.max(srcWidth / 4, srcHeight / 2));
        const faces = [];

        // Face directions
        const faceVectors = [
            { right: [0, 0, -1], up: [0, 1, 0], forward: [1, 0, 0] },   // +X
            { right: [0, 0, 1], up: [0, 1, 0], forward: [-1, 0, 0] },  // -X
            { right: [1, 0, 0], up: [0, 0, 1], forward: [0, 1, 0] },   // +Y
            { right: [1, 0, 0], up: [0, 0, -1], forward: [0, -1, 0] }, // -Y
            { right: [1, 0, 0], up: [0, 1, 0], forward: [0, 0, 1] },   // +Z
            { right: [-1, 0, 0], up: [0, 1, 0], forward: [0, 0, -1] }  // -Z
        ];

        for (let face = 0; face < 6; face++) {
            const faceData = new Uint16Array(cubeSize * cubeSize * 4); // RGBA16F
            const { right, up, forward } = faceVectors[face];

            for (let y = 0; y < cubeSize; y++) {
                for (let x = 0; x < cubeSize; x++) {
                    const u = (x + 0.5) / cubeSize * 2 - 1;
                    const v = (y + 0.5) / cubeSize * 2 - 1;

                    // Direction vector
                    const dir = [
                        forward[0] + right[0] * u + up[0] * (-v),
                        forward[1] + right[1] * u + up[1] * (-v),
                        forward[2] + right[2] * u + up[2] * (-v)
                    ];

                    // Normalize
                    const len = Math.sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                    dir[0] /= len; dir[1] /= len; dir[2] /= len;

                    // Convert to equirectangular UV
                    const theta = Math.atan2(dir[2], dir[0]);
                    const phi = Math.asin(Math.max(-1, Math.min(1, dir[1])));

                    const srcU = (theta / Math.PI + 1) * 0.5;
                    const srcV = 0.5 - phi / Math.PI;

                    // Sample source
                    const srcX = Math.floor(srcU * srcWidth) % srcWidth;
                    const srcY = Math.min(srcHeight - 1, Math.max(0, Math.floor(srcV * srcHeight)));
                    const srcIdx = (srcY * srcWidth + srcX) * 4;

                    const dstIdx = (y * cubeSize + x) * 4;

                    // Convert to float16
                    faceData[dstIdx] = floatToHalf(srcPixels[srcIdx]);
                    faceData[dstIdx + 1] = floatToHalf(srcPixels[srcIdx + 1]);
                    faceData[dstIdx + 2] = floatToHalf(srcPixels[srcIdx + 2]);
                    faceData[dstIdx + 3] = floatToHalf(1.0);
                }
            }

            faces.push(faceData);
        }

        return { size: cubeSize, faces };
    }

    async equirectangularToCubemap(imageBitmap) {
        const canvas = document.createElement('canvas');
        canvas.width = imageBitmap.width;
        canvas.height = imageBitmap.height;
        const ctx = canvas.getContext('2d');
        ctx.drawImage(imageBitmap, 0, 0);
        const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);

        // Convert to float
        const floatData = new Float32Array(imageData.data.length);
        for (let i = 0; i < imageData.data.length; i += 4) {
            // Assume sRGB, convert to linear
            floatData[i] = Math.pow(imageData.data[i] / 255, 2.2);
            floatData[i + 1] = Math.pow(imageData.data[i + 1] / 255, 2.2);
            floatData[i + 2] = Math.pow(imageData.data[i + 2] / 255, 2.2);
            floatData[i + 3] = 1.0;
        }

        return this.equirectangularDataToCubemap(floatData, canvas.width, canvas.height);
    }

    async generateIrradianceMap(sourceSize) {
        const size = 32; // Irradiance map can be small
        this.irradianceMap = this.device.createTexture({
            size: [size, size, 6],
            format: 'rgba16float',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.STORAGE_BINDING
        });

        const irradianceShader = /* wgsl */`
            @group(0) @binding(0) var envMap: texture_cube<f32>;
            @group(0) @binding(1) var envSampler: sampler;
            @group(0) @binding(2) var outputTex: texture_storage_2d_array<rgba16float, write>;

            const PI: f32 = 3.14159265359;

            fn getFaceDirection(face: u32, uv: vec2<f32>) -> vec3<f32> {
                let u = uv.x * 2.0 - 1.0;
                let v = uv.y * 2.0 - 1.0;

                switch(face) {
                    case 0u: { return normalize(vec3<f32>(1.0, -v, -u)); }   // +X
                    case 1u: { return normalize(vec3<f32>(-1.0, -v, u)); }   // -X
                    case 2u: { return normalize(vec3<f32>(u, 1.0, v)); }     // +Y
                    case 3u: { return normalize(vec3<f32>(u, -1.0, -v)); }   // -Y
                    case 4u: { return normalize(vec3<f32>(u, -v, 1.0)); }    // +Z
                    default: { return normalize(vec3<f32>(-u, -v, -1.0)); }  // -Z
                }
            }

            @compute @workgroup_size(8, 8, 1)
            fn main(@builtin(global_invocation_id) id: vec3<u32>) {
                let dims = textureDimensions(outputTex);
                if (id.x >= dims.x || id.y >= dims.y || id.z >= 6u) { return; }

                let uv = (vec2<f32>(id.xy) + 0.5) / vec2<f32>(dims.xy);
                let N = getFaceDirection(id.z, uv);

                var irradiance = vec3<f32>(0.0);
                var up = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0), abs(N.y) > 0.999);
                let right = normalize(cross(up, N));
                up = cross(N, right);

                let sampleDelta = 0.025;
                var nrSamples = 0.0;

                for (var phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
                    for (var theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
                        let tangentSample = vec3<f32>(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
                        let sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

                        irradiance += textureSampleLevel(envMap, envSampler, sampleVec, 0.0).rgb * cos(theta) * sin(theta);
                        nrSamples += 1.0;
                    }
                }

                irradiance = PI * irradiance / nrSamples;
                textureStore(outputTex, vec2<i32>(id.xy), i32(id.z), vec4<f32>(irradiance, 1.0));
            }
        `;

        const module = this.device.createShaderModule({ code: irradianceShader });
        const pipeline = this.device.createComputePipeline({
            layout: 'auto',
            compute: { module, entryPoint: 'main' }
        });

        const bindGroup = this.device.createBindGroup({
            layout: pipeline.getBindGroupLayout(0),
            entries: [
                { binding: 0, resource: this.envCubeMap.createView({ dimension: 'cube' }) },
                { binding: 1, resource: this.envSampler },
                { binding: 2, resource: this.irradianceMap.createView() }
            ]
        });

        const commandEncoder = this.device.createCommandEncoder();
        const pass = commandEncoder.beginComputePass();
        pass.setPipeline(pipeline);
        pass.setBindGroup(0, bindGroup);
        pass.dispatchWorkgroups(Math.ceil(size / 8), Math.ceil(size / 8), 6);
        pass.end();
        this.device.queue.submit([commandEncoder.finish()]);

        console.log('Irradiance map generated');
    }

    async generatePrefilteredMap(sourceSize) {
        const size = 128;
        const mipLevels = 5;

        this.prefilteredMap = this.device.createTexture({
            size: [size, size, 6],
            format: 'rgba16float',
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.STORAGE_BINDING,
            mipLevelCount: mipLevels
        });

        const prefilterShader = /* wgsl */`
            struct Params {
                roughness: f32,
                mipLevel: u32,
            }

            @group(0) @binding(0) var envMap: texture_cube<f32>;
            @group(0) @binding(1) var envSampler: sampler;
            @group(0) @binding(2) var outputTex: texture_storage_2d_array<rgba16float, write>;
            @group(0) @binding(3) var<uniform> params: Params;

            const PI: f32 = 3.14159265359;

            fn radicalInverse_VdC(bits_in: u32) -> f32 {
                var bits = bits_in;
                bits = (bits << 16u) | (bits >> 16u);
                bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
                bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
                bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
                bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
                return f32(bits) * 2.3283064365386963e-10;
            }

            fn hammersley(i: u32, N: u32) -> vec2<f32> {
                return vec2<f32>(f32(i) / f32(N), radicalInverse_VdC(i));
            }

            fn importanceSampleGGX(Xi: vec2<f32>, N: vec3<f32>, roughness: f32) -> vec3<f32> {
                let a = roughness * roughness;
                let phi = 2.0 * PI * Xi.x;
                let cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
                let sinTheta = sqrt(1.0 - cosTheta * cosTheta);

                let H = vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

                let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(N.z) < 0.999);
                let tangent = normalize(cross(up, N));
                let bitangent = cross(N, tangent);

                return normalize(tangent * H.x + bitangent * H.y + N * H.z);
            }

            fn getFaceDirection(face: u32, uv: vec2<f32>) -> vec3<f32> {
                let u = uv.x * 2.0 - 1.0;
                let v = uv.y * 2.0 - 1.0;

                switch(face) {
                    case 0u: { return normalize(vec3<f32>(1.0, -v, -u)); }
                    case 1u: { return normalize(vec3<f32>(-1.0, -v, u)); }
                    case 2u: { return normalize(vec3<f32>(u, 1.0, v)); }
                    case 3u: { return normalize(vec3<f32>(u, -1.0, -v)); }
                    case 4u: { return normalize(vec3<f32>(u, -v, 1.0)); }
                    default: { return normalize(vec3<f32>(-u, -v, -1.0)); }
                }
            }

            @compute @workgroup_size(8, 8, 1)
            fn main(@builtin(global_invocation_id) id: vec3<u32>) {
                let dims = textureDimensions(outputTex);
                if (id.x >= dims.x || id.y >= dims.y || id.z >= 6u) { return; }

                let uv = (vec2<f32>(id.xy) + 0.5) / vec2<f32>(dims.xy);
                let N = getFaceDirection(id.z, uv);
                let R = N;
                let V = R;

                let SAMPLE_COUNT = 1024u;
                var prefilteredColor = vec3<f32>(0.0);
                var totalWeight = 0.0;

                for (var i = 0u; i < SAMPLE_COUNT; i++) {
                    let Xi = hammersley(i, SAMPLE_COUNT);
                    let H = importanceSampleGGX(Xi, N, params.roughness);
                    let L = normalize(2.0 * dot(V, H) * H - V);

                    let NdotL = max(dot(N, L), 0.0);
                    if (NdotL > 0.0) {
                        prefilteredColor += textureSampleLevel(envMap, envSampler, L, 0.0).rgb * NdotL;
                        totalWeight += NdotL;
                    }
                }

                prefilteredColor = prefilteredColor / totalWeight;
                textureStore(outputTex, vec2<i32>(id.xy), i32(id.z), vec4<f32>(prefilteredColor, 1.0));
            }
        `;

        const module = this.device.createShaderModule({ code: prefilterShader });
        const pipeline = this.device.createComputePipeline({
            layout: 'auto',
            compute: { module, entryPoint: 'main' }
        });

        for (let mip = 0; mip < mipLevels; mip++) {
            const mipSize = size >> mip;
            const roughness = mip / (mipLevels - 1);

            const paramsBuffer = this.device.createBuffer({
                size: 8,
                usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
            });
            const paramsData = new ArrayBuffer(8);
            new Float32Array(paramsData, 0, 1)[0] = roughness;
            new Uint32Array(paramsData, 4, 1)[0] = mip;
            this.device.queue.writeBuffer(paramsBuffer, 0, paramsData);

            const mipView = this.prefilteredMap.createView({
                baseMipLevel: mip,
                mipLevelCount: 1
            });

            const bindGroup = this.device.createBindGroup({
                layout: pipeline.getBindGroupLayout(0),
                entries: [
                    { binding: 0, resource: this.envCubeMap.createView({ dimension: 'cube' }) },
                    { binding: 1, resource: this.envSampler },
                    { binding: 2, resource: mipView },
                    { binding: 3, resource: { buffer: paramsBuffer } }
                ]
            });

            const commandEncoder = this.device.createCommandEncoder();
            const pass = commandEncoder.beginComputePass();
            pass.setPipeline(pipeline);
            pass.setBindGroup(0, bindGroup);
            pass.dispatchWorkgroups(Math.ceil(mipSize / 8), Math.ceil(mipSize / 8), 6);
            pass.end();
            this.device.queue.submit([commandEncoder.finish()]);

            paramsBuffer.destroy();
        }

        console.log('Prefiltered environment map generated');
    }

    recreateBindGroups() {
        // Recreate bind groups with new textures/environment maps
        if (this.envMapLoaded) {
            this.createIBLBindGroup();
        }
    }

    createIBLBindGroup() {
        if (!this.iblBindGroupLayout) return;

        this.iblBindGroup = this.device.createBindGroup({
            layout: this.iblBindGroupLayout,
            entries: [
                { binding: 0, resource: this.envSampler },
                { binding: 1, resource: this.irradianceMap.createView({ dimension: 'cube' }) },
                { binding: 2, resource: this.prefilteredMap.createView({ dimension: 'cube' }) },
                { binding: 3, resource: this.brdfLUT.createView() }
            ]
        });
    }

    // Load a single texture from image data and return its index
    async loadTextureFromData(imageData, width, height, texturePath) {
        if (this.textureIndexMap.has(texturePath)) {
            return this.textureIndexMap.get(texturePath);
        }

        const texture = await this.loadTexture(imageData, width, height);
        const index = this.loadedTextures.length;
        this.loadedTextures.push(texture);
        this.textureIndexMap.set(texturePath, index);

        // Update texture bind group
        this.updateTextureBindGroup();

        return index;
    }

    updateTextureBindGroup() {
        if (this.loadedTextures.length === 0) return;

        // Recreate texture bind group with all loaded textures
        // For simplicity, we'll use individual textures instead of texture arrays
        // since WebGPU texture arrays require same dimensions
    }

    async createPipelines() {
        // Create bind group layouts
        this.cameraBindGroupLayout = this.device.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } }
            ]
        });

        this.materialBindGroupLayout = this.device.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
                { binding: 1, visibility: GPUShaderStage.VERTEX, buffer: { type: 'read-only-storage' } }
            ]
        });

        // Texture bind group layout
        this.textureBindGroupLayout = this.device.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d' } },
                { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d' } },
                { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d' } },
                { binding: 4, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d' } }
            ]
        });

        // IBL bind group layout
        this.iblBindGroupLayout = this.device.createBindGroupLayout({
            entries: [
                { binding: 0, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
                { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: 'cube' } },
                { binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: 'cube' } },
                { binding: 3, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float', viewDimension: '2d' } }
            ]
        });

        // Pipeline layout for simple shader (no textures)
        const simplePipelineLayout = this.device.createPipelineLayout({
            bindGroupLayouts: [this.cameraBindGroupLayout, this.materialBindGroupLayout]
        });

        // Pipeline layout for IBL shader (with textures and IBL)
        const iblPipelineLayout = this.device.createPipelineLayout({
            bindGroupLayouts: [this.cameraBindGroupLayout, this.materialBindGroupLayout, this.textureBindGroupLayout, this.iblBindGroupLayout]
        });

        const vertexBufferLayout = {
            arrayStride: 48, // position(12) + normal(12) + uv(8) + tangent(16)
            attributes: [
                { shaderLocation: 0, offset: 0, format: 'float32x3' },  // position
                { shaderLocation: 1, offset: 12, format: 'float32x3' }, // normal
                { shaderLocation: 2, offset: 24, format: 'float32x2' }, // uv
                { shaderLocation: 3, offset: 32, format: 'float32x4' }  // tangent
            ]
        };

        // Create simple pipeline (without IBL)
        const simpleShaderModule = this.device.createShaderModule({ code: SIMPLE_SHADER });
        this.simplePipeline = this.device.createRenderPipeline({
            layout: simplePipelineLayout,
            vertex: { module: simpleShaderModule, entryPoint: 'vertexMain', buffers: [vertexBufferLayout] },
            fragment: { module: simpleShaderModule, entryPoint: 'fragmentMain', targets: [{ format: this.format }] },
            primitive: { topology: 'triangle-list', cullMode: 'back' },
            depthStencil: { depthWriteEnabled: true, depthCompare: 'less', format: 'depth24plus' }
        });

        // Create IBL pipeline (with textures and IBL)
        const iblShaderModule = this.device.createShaderModule({ code: IBL_SHADER });
        this.iblPipeline = this.device.createRenderPipeline({
            layout: iblPipelineLayout,
            vertex: { module: iblShaderModule, entryPoint: 'vertexMain', buffers: [vertexBufferLayout] },
            fragment: { module: iblShaderModule, entryPoint: 'fragmentMain', targets: [{ format: this.format }] },
            primitive: { topology: 'triangle-list', cullMode: 'back' },
            depthStencil: { depthWriteEnabled: true, depthCompare: 'less', format: 'depth24plus' }
        });

        // Create bind groups
        this.cameraBindGroup = this.device.createBindGroup({
            layout: this.cameraBindGroupLayout,
            entries: [
                { binding: 0, resource: { buffer: this.cameraBuffer } },
                { binding: 1, resource: { buffer: this.sceneBuffer } }
            ]
        });

        this.materialBindGroup = this.device.createBindGroup({
            layout: this.materialBindGroupLayout,
            entries: [
                { binding: 0, resource: { buffer: this.materialBuffer } },
                { binding: 1, resource: { buffer: this.instanceBuffer } }
            ]
        });

        // Create default texture bind group with placeholders
        this.textureBindGroup = this.device.createBindGroup({
            layout: this.textureBindGroupLayout,
            entries: [
                { binding: 0, resource: this.textureSampler },
                { binding: 1, resource: this.placeholderTexture.createView() },
                { binding: 2, resource: this.placeholderNormalTexture.createView() },
                { binding: 3, resource: this.placeholderTexture.createView() },
                { binding: 4, resource: this.placeholderTexture.createView() }
            ]
        });

        // Create default IBL bind group with placeholders
        this.iblBindGroup = this.device.createBindGroup({
            layout: this.iblBindGroupLayout,
            entries: [
                { binding: 0, resource: this.envSampler },
                { binding: 1, resource: this.irradianceMap.createView({ dimension: 'cube' }) },
                { binding: 2, resource: this.prefilteredMap.createView({ dimension: 'cube' }) },
                { binding: 3, resource: this.brdfLUT.createView() }
            ]
        });
    }

    createDepthBuffer() {
        if (this.depthTexture) {
            this.depthTexture.destroy();
        }

        this.depthTexture = this.device.createTexture({
            size: [this.canvas.width, this.canvas.height],
            format: 'depth24plus',
            usage: GPUTextureUsage.RENDER_ATTACHMENT
        });
    }

    resize(width, height) {
        this.canvas.width = width;
        this.canvas.height = height;
        this.createDepthBuffer();
    }

    updateCameraUniforms() {
        const aspect = this.canvas.width / this.canvas.height;
        const fovRad = this.camera.fov * Math.PI / 180;

        // Create matrices
        const view = mat4LookAt(this.camera.position, this.camera.target, this.camera.up);
        const projection = mat4Perspective(fovRad, aspect, this.camera.near, this.camera.far);
        const viewProjection = mat4Multiply(projection, view);
        const invViewProjection = mat4Inverse(viewProjection);

        // Pack into buffer (288 bytes)
        const data = new Float32Array(72);
        data.set(viewProjection, 0);      // viewProjection: 64 bytes (offset 0)
        data.set(view, 16);               // view: 64 bytes (offset 64)
        data.set(projection, 32);         // projection: 64 bytes (offset 128)
        data.set(this.camera.position, 48); // cameraPosition: 12 bytes + 4 padding (offset 192)
        data.set(invViewProjection, 52);  // invViewProjection: 64 bytes (offset 208)

        this.device.queue.writeBuffer(this.cameraBuffer, 0, data);
    }

    updateSceneUniforms() {
        const data = new Float32Array(8);
        data[0] = this.settings.envIntensity;
        data[1] = this.settings.exposure;
        data[2] = this.settings.toneMapping;
        data[3] = performance.now() / 1000;
        data[4] = this.canvas.width;
        data[5] = this.canvas.height;

        this.device.queue.writeBuffer(this.sceneBuffer, 0, data);
        this.stats.uboUpdates++;
    }

    updateMaterials(materials) {
        this.materials = materials;

        // Each material is 192 bytes (aligned)
        const data = new Float32Array(materials.length * 48);

        for (let i = 0; i < materials.length; i++) {
            const mat = materials[i];
            const offset = i * 48;

            // Base layer
            data[offset + 0] = mat.baseColor?.[0] ?? 0.8;
            data[offset + 1] = mat.baseColor?.[1] ?? 0.8;
            data[offset + 2] = mat.baseColor?.[2] ?? 0.8;
            data[offset + 3] = mat.baseWeight ?? 1.0;
            data[offset + 4] = mat.baseMetalness ?? 0.0;
            data[offset + 5] = mat.baseDiffuseRoughness ?? 0.0;

            // Specular layer
            data[offset + 8] = mat.specularColor?.[0] ?? 1.0;
            data[offset + 9] = mat.specularColor?.[1] ?? 1.0;
            data[offset + 10] = mat.specularColor?.[2] ?? 1.0;
            data[offset + 11] = mat.specularWeight ?? 1.0;
            data[offset + 12] = mat.specularRoughness ?? 0.5;
            data[offset + 13] = mat.specularIOR ?? 1.5;
            data[offset + 14] = mat.specularAnisotropy ?? 0.0;

            // Transmission layer
            data[offset + 16] = mat.transmissionWeight ?? 0.0;
            data[offset + 17] = mat.transmissionDepth ?? 0.0;
            data[offset + 18] = mat.transmissionDispersion ?? 0.0;

            // Coat layer
            data[offset + 20] = mat.coatColor?.[0] ?? 1.0;
            data[offset + 21] = mat.coatColor?.[1] ?? 1.0;
            data[offset + 22] = mat.coatColor?.[2] ?? 1.0;
            data[offset + 23] = mat.coatWeight ?? 0.0;
            data[offset + 24] = mat.coatRoughness ?? 0.1;
            data[offset + 25] = mat.coatIOR ?? 1.5;

            // Emission layer
            data[offset + 28] = mat.emissionColor?.[0] ?? 0.0;
            data[offset + 29] = mat.emissionColor?.[1] ?? 0.0;
            data[offset + 30] = mat.emissionColor?.[2] ?? 0.0;
            data[offset + 31] = mat.emissionLuminance ?? 0.0;

            // Sheen layer
            data[offset + 32] = mat.sheenColor?.[0] ?? 1.0;
            data[offset + 33] = mat.sheenColor?.[1] ?? 1.0;
            data[offset + 34] = mat.sheenColor?.[2] ?? 1.0;
            data[offset + 35] = mat.sheenWeight ?? 0.0;

            // Texture indices (as int32, reinterpret as float32 for buffer)
            const intView = new Int32Array(data.buffer, (offset + 36) * 4, 8);
            intView[0] = mat.baseColorTexIdx ?? -1;
            intView[1] = mat.normalTexIdx ?? -1;
            intView[2] = mat.roughnessTexIdx ?? -1;
            intView[3] = mat.metalnessTexIdx ?? -1;
            intView[4] = mat.emissiveTexIdx ?? -1;
            intView[5] = mat.aoTexIdx ?? -1;
        }

        this.device.queue.writeBuffer(this.materialBuffer, 0, data);
        this.stats.uboUpdates++;
    }

    updateInstances(instances) {
        this.instances = instances;

        // Each instance is 144 bytes (modelMatrix: 64, normalMatrix: 64, materialIndex: 4, padding: 12)
        const data = new Float32Array(instances.length * 36);
        const intView = new Uint32Array(data.buffer);

        for (let i = 0; i < instances.length; i++) {
            const inst = instances[i];
            const offset = i * 36;

            // Model matrix
            data.set(inst.modelMatrix || mat4Identity(), offset);

            // Normal matrix (inverse transpose of model matrix)
            const normalMatrix = inst.normalMatrix || mat4Identity();
            data.set(normalMatrix, offset + 16);

            // Material index
            intView[offset + 32] = inst.materialIndex ?? 0;
        }

        this.device.queue.writeBuffer(this.instanceBuffer, 0, data);
        this.stats.uboUpdates++;
    }

    createMeshBuffers(meshData) {
        const { positions, normals, uvs, tangents, indices } = meshData;

        // Interleave vertex data: position(3) + normal(3) + uv(2) + tangent(4) = 12 floats = 48 bytes
        const vertexCount = positions.length / 3;
        const vertexData = new Float32Array(vertexCount * 12);

        for (let i = 0; i < vertexCount; i++) {
            const vOffset = i * 12;
            const pOffset = i * 3;
            const uvOffset = i * 2;
            const tOffset = i * 4;

            // Position
            vertexData[vOffset + 0] = positions[pOffset + 0];
            vertexData[vOffset + 1] = positions[pOffset + 1];
            vertexData[vOffset + 2] = positions[pOffset + 2];

            // Normal
            vertexData[vOffset + 3] = normals?.[pOffset + 0] ?? 0;
            vertexData[vOffset + 4] = normals?.[pOffset + 1] ?? 1;
            vertexData[vOffset + 5] = normals?.[pOffset + 2] ?? 0;

            // UV
            vertexData[vOffset + 6] = uvs?.[uvOffset + 0] ?? 0;
            vertexData[vOffset + 7] = uvs?.[uvOffset + 1] ?? 0;

            // Tangent
            vertexData[vOffset + 8] = tangents?.[tOffset + 0] ?? 1;
            vertexData[vOffset + 9] = tangents?.[tOffset + 1] ?? 0;
            vertexData[vOffset + 10] = tangents?.[tOffset + 2] ?? 0;
            vertexData[vOffset + 11] = tangents?.[tOffset + 3] ?? 1;
        }

        const vertexBuffer = this.device.createBuffer({
            size: vertexData.byteLength,
            usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
            mappedAtCreation: true
        });
        new Float32Array(vertexBuffer.getMappedRange()).set(vertexData);
        vertexBuffer.unmap();

        let indexBuffer = null;
        let indexCount = 0;

        if (indices && indices.length > 0) {
            indexCount = indices.length;
            indexBuffer = this.device.createBuffer({
                size: indices.byteLength,
                usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST,
                mappedAtCreation: true
            });
            new Uint32Array(indexBuffer.getMappedRange()).set(indices);
            indexBuffer.unmap();
        }

        return {
            vertexBuffer,
            indexBuffer,
            vertexCount,
            indexCount
        };
    }

    render() {
        const now = performance.now();
        this.frameCount++;

        if (now - this.lastFrameTime >= 1000) {
            this.stats.fps = Math.round(this.frameCount * 1000 / (now - this.lastFrameTime));
            this.frameCount = 0;
            this.lastFrameTime = now;
        }

        this.stats.uboUpdates = 0;
        this.stats.drawCalls = 0;
        this.stats.triangles = 0;

        this.updateCameraUniforms();
        this.updateSceneUniforms();

        const commandEncoder = this.device.createCommandEncoder();

        const renderPass = commandEncoder.beginRenderPass({
            colorAttachments: [{
                view: this.context.getCurrentTexture().createView(),
                clearValue: { r: 0.1, g: 0.1, b: 0.1, a: 1 },
                loadOp: 'clear',
                storeOp: 'store'
            }],
            depthStencilAttachment: {
                view: this.depthTexture.createView(),
                depthClearValue: 1.0,
                depthLoadOp: 'clear',
                depthStoreOp: 'store'
            }
        });

        // Use IBL pipeline if we have textures or env map, otherwise use simple pipeline
        const useIBL = this.iblPipeline && this.textureBindGroup && this.iblBindGroup;

        if (useIBL) {
            renderPass.setPipeline(this.iblPipeline);
            renderPass.setBindGroup(0, this.cameraBindGroup);
            renderPass.setBindGroup(1, this.materialBindGroup);
            renderPass.setBindGroup(2, this.textureBindGroup);
            renderPass.setBindGroup(3, this.iblBindGroup);
        } else {
            renderPass.setPipeline(this.simplePipeline);
            renderPass.setBindGroup(0, this.cameraBindGroup);
            renderPass.setBindGroup(1, this.materialBindGroup);
        }

        // Draw all meshes
        for (const mesh of this.meshes) {
            renderPass.setVertexBuffer(0, mesh.vertexBuffer);

            if (mesh.indexBuffer) {
                renderPass.setIndexBuffer(mesh.indexBuffer, 'uint32');
                renderPass.drawIndexed(mesh.indexCount, mesh.instanceCount || 1, 0, 0, mesh.firstInstance || 0);
                this.stats.triangles += mesh.indexCount / 3;
            } else {
                renderPass.draw(mesh.vertexCount, mesh.instanceCount || 1, 0, mesh.firstInstance || 0);
                this.stats.triangles += mesh.vertexCount / 3;
            }
            this.stats.drawCalls++;
        }

        renderPass.end();
        this.device.queue.submit([commandEncoder.finish()]);
    }

    // Update texture bind group with loaded textures
    setTextures(baseColorTex, normalTex, ormTex, emissiveTex) {
        this.textureBindGroup = this.device.createBindGroup({
            layout: this.textureBindGroupLayout,
            entries: [
                { binding: 0, resource: this.textureSampler },
                { binding: 1, resource: (baseColorTex || this.placeholderTexture).createView() },
                { binding: 2, resource: (normalTex || this.placeholderNormalTexture).createView() },
                { binding: 3, resource: (ormTex || this.placeholderTexture).createView() },
                { binding: 4, resource: (emissiveTex || this.placeholderTexture).createView() }
            ]
        });
    }

    // Update IBL bind group after environment map is loaded
    updateIBLBindGroup() {
        this.iblBindGroup = this.device.createBindGroup({
            layout: this.iblBindGroupLayout,
            entries: [
                { binding: 0, resource: this.envSampler },
                { binding: 1, resource: this.irradianceMap.createView({ dimension: 'cube' }) },
                { binding: 2, resource: this.prefilteredMap.createView({ dimension: 'cube' }) },
                { binding: 3, resource: this.brdfLUT.createView() }
            ]
        });
    }

    // Clear mesh buffers only (used when loading a new scene)
    clearMeshes() {
        for (const mesh of this.meshes) {
            mesh.vertexBuffer?.destroy();
            mesh.indexBuffer?.destroy();
        }
        this.meshes = [];
    }

    // Full destroy (used when shutting down the renderer)
    destroy() {
        this.clearMeshes();

        this.cameraBuffer?.destroy();
        this.sceneBuffer?.destroy();
        this.materialBuffer?.destroy();
        this.instanceBuffer?.destroy();
        this.depthTexture?.destroy();

        this.cameraBuffer = null;
        this.sceneBuffer = null;
        this.materialBuffer = null;
        this.instanceBuffer = null;
        this.depthTexture = null;
    }
}

// ============================================================================
// Matrix Utility Functions
// ============================================================================

function mat4Identity() {
    return new Float32Array([
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    ]);
}

function mat4Perspective(fov, aspect, near, far) {
    const f = 1.0 / Math.tan(fov / 2);
    const nf = 1 / (near - far);

    return new Float32Array([
        f / aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (far + near) * nf, -1,
        0, 0, 2 * far * near * nf, 0
    ]);
}

function mat4LookAt(eye, target, up) {
    const zAxis = normalize3(subtract3(eye, target));
    const xAxis = normalize3(cross3(up, zAxis));
    const yAxis = cross3(zAxis, xAxis);

    return new Float32Array([
        xAxis[0], yAxis[0], zAxis[0], 0,
        xAxis[1], yAxis[1], zAxis[1], 0,
        xAxis[2], yAxis[2], zAxis[2], 0,
        -dot3(xAxis, eye), -dot3(yAxis, eye), -dot3(zAxis, eye), 1
    ]);
}

function mat4Multiply(a, b) {
    const result = new Float32Array(16);
    for (let i = 0; i < 4; i++) {
        for (let j = 0; j < 4; j++) {
            result[j * 4 + i] =
                a[i] * b[j * 4] +
                a[i + 4] * b[j * 4 + 1] +
                a[i + 8] * b[j * 4 + 2] +
                a[i + 12] * b[j * 4 + 3];
        }
    }
    return result;
}

function mat4Inverse(m) {
    const result = new Float32Array(16);
    const inv = new Float32Array(16);

    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    let det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (Math.abs(det) < 1e-10) {
        return mat4Identity();
    }

    det = 1.0 / det;
    for (let i = 0; i < 16; i++) {
        result[i] = inv[i] * det;
    }

    return result;
}

function mat4Translation(x, y, z) {
    return new Float32Array([
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        x, y, z, 1
    ]);
}

function mat4Scale(x, y, z) {
    return new Float32Array([
        x, 0, 0, 0,
        0, y, 0, 0,
        0, 0, z, 0,
        0, 0, 0, 1
    ]);
}

function mat4RotationY(angle) {
    const c = Math.cos(angle);
    const s = Math.sin(angle);
    return new Float32Array([
        c, 0, -s, 0,
        0, 1, 0, 0,
        s, 0, c, 0,
        0, 0, 0, 1
    ]);
}

function normalize3(v) {
    const len = Math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len < 1e-10) return [0, 0, 0];
    return [v[0]/len, v[1]/len, v[2]/len];
}

function subtract3(a, b) {
    return [a[0]-b[0], a[1]-b[1], a[2]-b[2]];
}

function cross3(a, b) {
    return [
        a[1]*b[2] - a[2]*b[1],
        a[2]*b[0] - a[0]*b[2],
        a[0]*b[1] - a[1]*b[0]
    ];
}

function dot3(a, b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

function length3(v) {
    return Math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

// ============================================================================
// Orbit Camera Controls
// ============================================================================

class OrbitControls {
    constructor(camera, canvas) {
        this.camera = camera;
        this.canvas = canvas;

        this.spherical = { radius: 5, phi: Math.PI / 4, theta: Math.PI / 4 };
        this.target = [0, 0, 0];

        this.rotateSpeed = 0.005;
        this.zoomSpeed = 0.001;
        this.panSpeed = 0.005;

        this.isDragging = false;
        this.isPanning = false;
        this.lastMouse = { x: 0, y: 0 };

        this.setupEventListeners();
        this.update();
    }

    setupEventListeners() {
        this.canvas.addEventListener('mousedown', this.onMouseDown.bind(this));
        this.canvas.addEventListener('mousemove', this.onMouseMove.bind(this));
        this.canvas.addEventListener('mouseup', this.onMouseUp.bind(this));
        this.canvas.addEventListener('wheel', this.onWheel.bind(this));
        this.canvas.addEventListener('contextmenu', e => e.preventDefault());
    }

    onMouseDown(e) {
        if (e.button === 0) {
            this.isDragging = true;
        } else if (e.button === 2) {
            this.isPanning = true;
        }
        this.lastMouse = { x: e.clientX, y: e.clientY };
    }

    onMouseMove(e) {
        const dx = e.clientX - this.lastMouse.x;
        const dy = e.clientY - this.lastMouse.y;
        this.lastMouse = { x: e.clientX, y: e.clientY };

        if (this.isDragging) {
            this.spherical.theta -= dx * this.rotateSpeed;
            this.spherical.phi -= dy * this.rotateSpeed;
            this.spherical.phi = Math.max(0.01, Math.min(Math.PI - 0.01, this.spherical.phi));
            this.update();
        } else if (this.isPanning) {
            const right = normalize3(cross3(subtract3(this.camera.position, this.target), this.camera.up));
            const up = this.camera.up;

            const panX = dx * this.panSpeed * this.spherical.radius;
            const panY = dy * this.panSpeed * this.spherical.radius;

            this.target[0] -= right[0] * panX - up[0] * panY;
            this.target[1] -= right[1] * panX - up[1] * panY;
            this.target[2] -= right[2] * panX - up[2] * panY;
            this.update();
        }
    }

    onMouseUp() {
        this.isDragging = false;
        this.isPanning = false;
    }

    onWheel(e) {
        e.preventDefault();
        this.spherical.radius *= 1 + e.deltaY * this.zoomSpeed;
        this.spherical.radius = Math.max(0.1, Math.min(1000, this.spherical.radius));
        this.update();
    }

    update() {
        const sinPhi = Math.sin(this.spherical.phi);
        const cosPhi = Math.cos(this.spherical.phi);
        const sinTheta = Math.sin(this.spherical.theta);
        const cosTheta = Math.cos(this.spherical.theta);

        this.camera.position = [
            this.target[0] + this.spherical.radius * sinPhi * cosTheta,
            this.target[1] + this.spherical.radius * cosPhi,
            this.target[2] + this.spherical.radius * sinPhi * sinTheta
        ];

        this.camera.target = this.target;
    }

    fitToBox(min, max) {
        const center = [
            (min[0] + max[0]) / 2,
            (min[1] + max[1]) / 2,
            (min[2] + max[2]) / 2
        ];

        const size = [
            max[0] - min[0],
            max[1] - min[1],
            max[2] - min[2]
        ];

        const maxDim = Math.max(size[0], size[1], size[2]);
        this.target = center;
        this.spherical.radius = maxDim * 2;
        this.update();
    }
}

// ============================================================================
// Application
// ============================================================================

class Application {
    constructor() {
        this.canvas = document.getElementById('webgpu-canvas');
        this.renderer = null;
        this.controls = null;
        this.loader = null;
        this.nativeLoader = null;
        this.animationId = null;

        this.settings = {
            envMapPreset: 'goegap_1k',
            envIntensity: 1.0,
            exposure: 1.0,
            toneMapping: 'aces',
            showWireframe: false,
            showNormals: false
        };
    }

    async init() {
        try {
            // Check WebGPU support
            if (!navigator.gpu) {
                document.getElementById('error-message').style.display = 'block';
                throw new Error('WebGPU not supported');
            }

            this.updateStatus('Initializing WebGPU...');

            // Initialize renderer
            this.renderer = new WebGPURenderer(this.canvas);
            await this.renderer.init();

            // Setup controls
            this.controls = new OrbitControls(this.renderer.camera, this.canvas);

            // Initialize LightUSD loader
            this.updateStatus('Initializing LightUSD WASM...');
            this.loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512 });
            await this.loader.init({ useMemory64: false });

            // Setup UI
            this.setupUI();
            this.setupEventListeners();

            // Handle resize
            this.resize();
            window.addEventListener('resize', () => this.resize());

            // Load default environment map
            this.updateStatus('Loading environment map...');
            await this.loadEnvironmentMap('studio');

            // Load default scene
            await this.loadDefaultScene();

            // Start render loop
            this.animate();

            this.updateStatus('Ready');

        } catch (error) {
            console.error('Initialization failed:', error);
            this.updateStatus('Error: ' + error.message);
        }
    }

    async loadEnvironmentMap(preset) {
        // Environment map presets - using actual HDR files in the assets folder
        const envMaps = {
            'studio': new URL('./assets/textures/goegap_1k.hdr',import.meta.url).href,
            'outdoor': new URL('./assets/textures/goegap_1k.hdr',import.meta.url).href,
            'sunset': new URL('./assets/textures/env_sunsky_sunset.hdr',import.meta.url).href,
            'night': new URL('./assets/textures/goegap_1k.hdr',import.meta.url).href
        };

        const url = envMaps[preset] || envMaps['studio'];

        try {
            await this.renderer.loadEnvironmentMap(url);
            console.log(`Environment map '${preset}' loaded`);
        } catch (error) {
            console.warn(`Failed to load environment map '${preset}':`, error);
            // Create fallback environment - solid color cubemap
            this.renderer.createFallbackEnvironment();
        }
    }

    setupUI() {
        // Environment select
        const envSelect = document.getElementById('env-select');
        if (envSelect) {
            envSelect.addEventListener('change', async (e) => {
                this.settings.envMapPreset = e.target.value;
                await this.loadEnvironmentMap(e.target.value);
            });
        }

        // Env intensity
        const envIntensitySlider = document.getElementById('env-intensity');
        const envIntensityValue = document.getElementById('env-intensity-value');
        envIntensitySlider.addEventListener('input', (e) => {
            this.settings.envIntensity = parseFloat(e.target.value);
            this.renderer.settings.envIntensity = this.settings.envIntensity;
            envIntensityValue.textContent = this.settings.envIntensity.toFixed(1);
        });

        // Exposure
        const exposureSlider = document.getElementById('exposure');
        const exposureValue = document.getElementById('exposure-value');
        exposureSlider.addEventListener('input', (e) => {
            this.settings.exposure = parseFloat(e.target.value);
            this.renderer.settings.exposure = this.settings.exposure;
            exposureValue.textContent = this.settings.exposure.toFixed(1);
        });

        // Tone mapping
        const tonemapSelect = document.getElementById('tonemap-select');
        tonemapSelect.addEventListener('change', (e) => {
            const mapping = { linear: 0, reinhard: 1, aces: 2, agx: 3 };
            this.settings.toneMapping = e.target.value;
            this.renderer.settings.toneMapping = mapping[e.target.value] ?? 2;
        });

        // Wireframe
        document.getElementById('show-wireframe').addEventListener('change', (e) => {
            this.settings.showWireframe = e.target.checked;
            this.renderer.settings.showWireframe = e.target.checked;
        });

        // Normals
        document.getElementById('show-normals').addEventListener('change', (e) => {
            this.settings.showNormals = e.target.checked;
            this.renderer.settings.showNormals = e.target.checked;
        });
    }

    setupEventListeners() {
        // File input
        const fileInput = document.getElementById('file-input');
        fileInput.addEventListener('change', (e) => {
            if (e.target.files.length > 0) {
                this.loadUSDFile(e.target.files[0]);
            }
            e.target.value = '';
        });

        // Drag and drop
        const container = document.getElementById('canvas-container');
        container.addEventListener('dragover', (e) => {
            e.preventDefault();
            e.dataTransfer.dropEffect = 'copy';
            container.classList.add('drag-over');
        });

        container.addEventListener('dragleave', (e) => {
            e.preventDefault();
            container.classList.remove('drag-over');
        });

        container.addEventListener('drop', (e) => {
            e.preventDefault();
            container.classList.remove('drag-over');

            const files = e.dataTransfer.files;
            if (files.length > 0) {
                const file = files[0];
                const ext = file.name.toLowerCase().split('.').pop();
                if (['usd', 'usda', 'usdc', 'usdz'].includes(ext)) {
                    this.loadUSDFile(file);
                } else {
                    this.updateStatus('Please drop a USD file (.usd, .usda, .usdc, .usdz)');
                }
            }
        });
    }

    resize() {
        const width = window.innerWidth;
        const height = window.innerHeight;
        this.canvas.width = width * window.devicePixelRatio;
        this.canvas.height = height * window.devicePixelRatio;
        this.canvas.style.width = width + 'px';
        this.canvas.style.height = height + 'px';
        this.renderer.resize(this.canvas.width, this.canvas.height);
    }

    async loadDefaultScene() {
        this.updateStatus('Loading default teapot...');

        try {
            const response = await fetch(new URL('./assets/fancy-teapot-mtlx.usdz',import.meta.url));
            if (!response.ok) throw new Error('Failed to fetch default scene');

            const arrayBuffer = await response.arrayBuffer();
            const data = new Uint8Array(arrayBuffer);
            await this.loadUSDFromData(data, 'fancy-teapot-mtlx.usdz');
        } catch (error) {
            console.error('Failed to load default scene:', error);
            this.updateStatus('Failed to load default scene, creating fallback...');
            this.createFallbackScene();
        }
    }

    async loadUSDFile(file) {
        this.updateStatus(`Loading: ${file.name}...`);

        try {
            const arrayBuffer = await file.arrayBuffer();
            const data = new Uint8Array(arrayBuffer);
            await this.loadUSDFromData(data, file.name);
        } catch (error) {
            console.error('Failed to load USD file:', error);
            this.updateStatus('Error: ' + error.message);
        }
    }

    async loadUSDFromData(data, filename) {
        // Clear previous scene (only mesh buffers, keep uniform buffers)
        this.renderer.clearMeshes();
        for(const texture of this.loadedUSDTextures||[])texture.destroy();
        this.loadedUSDTextures=[];

        this.nativeLoader?.delete();
        this.nativeLoader=null;
        this.nativeLoader=await createCoreRenderInspector(this.loader.native_,data,filename);

        const numMeshes = this.nativeLoader.numMeshes();
        const numMaterials = this.nativeLoader.numMaterials();
        const numTextures = this.nativeLoader.numTextures ? this.nativeLoader.numTextures() : 0;
        const safeCount = (getter) => {
            if (typeof getter !== 'function') return 0;
            try {
                const value = getter.call(this.nativeLoader);
                return Number.isFinite(value) ? value : 0;
            } catch {
                return 0;
            }
        };
        const sceneCounts = {
            lights: safeCount(this.nativeLoader.numLights),
            cameras: safeCount(this.nativeLoader.numCameras),
            nodes: safeCount(this.nativeLoader.numNodes),
            animations: safeCount(this.nativeLoader.numAnimations),
            pointInstancers: safeCount(this.nativeLoader.numPointInstancers),
            pointInstanceDraws: safeCount(this.nativeLoader.numPointInstanceDraws),
            skeletons: safeCount(this.nativeLoader.numSkeletons),
            unsupportedRenderables: safeCount(this.nativeLoader.numUnsupportedRenderables)
        };
        if (!sceneCounts.unsupportedRenderables && typeof this.nativeLoader.getUnsupportedRenderables === 'function') {
            try {
                const list = this.nativeLoader.getUnsupportedRenderables();
                if (Array.isArray(list)) {
                    sceneCounts.unsupportedRenderables = list.length;
                }
            } catch {
                // ignore
            }
        }

        this.updateStatus(`Processing: ${numMeshes} meshes, ${numMaterials} materials, ${numTextures} textures...`);

        // Load textures from USD
        const loadedTextures = new Map();
        let baseColorTex = null;
        let normalTex = null;
        let ormTex = null;
        let emissiveTex = null;

        if (numTextures > 0 && this.nativeLoader.getTexture) {
            for (let i = 0; i < numTextures; i++) {
                try {
                    const meta=this.nativeLoader.getTextureRecord(i);
                    const image=this.nativeLoader.getImageCopy(meta.imageId);
                    const texData=image.decoded?{...coreWebGPUImage(image),name:meta.name||meta.uri}:null;
                    if (texData && texData.data && texData.width > 0 && texData.height > 0) {
                        const texture = await this.renderer.loadTexture(
                            texData.data,
                            texData.width,
                            texData.height,
                            texData.format
                        );
                        loadedTextures.set(i, texture);
                        this.loadedUSDTextures.push(texture);

                        // Assign textures based on usage (simplified - first texture is base color, etc.)
                        const name = (texData.name || '').toLowerCase();
                        if (name.includes('color') || name.includes('diffuse') || name.includes('albedo') || i === 0) {
                            if (!baseColorTex) baseColorTex = texture;
                        } else if (name.includes('normal')) {
                            if (!normalTex) normalTex = texture;
                        } else if (name.includes('rough') || name.includes('metal') || name.includes('orm')) {
                            if (!ormTex) ormTex = texture;
                        } else if (name.includes('emissive') || name.includes('emission')) {
                            if (!emissiveTex) emissiveTex = texture;
                        }

                        console.log(`Loaded texture ${i}: ${texData.name || 'unnamed'} (${texData.width}x${texData.height})`);
                    }
                } catch (e) {
                    console.warn(`Failed to load texture ${i}:`, e);
                }
            }
        }

        // Update renderer texture bind group
        this.renderer.setTextures(baseColorTex, normalTex, ormTex, emissiveTex);

        // Load materials
        const materials = [];
        for (let i = 0; i < numMaterials; i++) {
            try {
                const result = this.nativeLoader.getMaterialWithFormat(i, 'json');
                if (!result.error) {
                    const matData = JSON.parse(result.data);
                    const material = this.convertMaterialToWebGPU(matData, i);

                    // Set texture indices if textures were loaded
                    if (baseColorTex) material.baseColorTexIdx = 0;
                    if (normalTex) material.normalTexIdx = 0;
                    if (ormTex) material.roughnessTexIdx = 0;
                    if (emissiveTex) material.emissiveTexIdx = 0;

                    materials.push(material);
                } else {
                    materials.push(this.createDefaultMaterial());
                }
            } catch (e) {
                console.warn(`Failed to get material ${i}:`, e);
                materials.push(this.createDefaultMaterial());
            }
        }

        // Ensure at least one default material
        if (materials.length === 0) {
            materials.push(this.createDefaultMaterial());
        }

        this.renderer.updateMaterials(materials);

        // Load meshes and create instances
        const instances = [];
        let boundingBox = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };

        for (let i = 0; i < numMeshes; i++) {
            const meshData = getCoreInspectorMesh(this.nativeLoader,i);
            if (!meshData) continue;

            // Convert mesh to WebGPU buffers
            const gpuMesh = this.renderer.createMeshBuffers({
                positions: meshData.points,
                normals: meshData.normals,
                uvs: meshData.texcoords,
                tangents: meshData.tangents,
                indices: meshData.faceVertexIndices
            });

            // Determine material index
            let materialIndex = 0;
            if (meshData.materialId !== undefined && meshData.materialId >= 0 && meshData.materialId < materials.length) {
                materialIndex = meshData.materialId;
            }

            // Create instance
            const world=new Matrix4().fromArray(meshData.worldMatrix||mat4Identity());
            instances.push({
                modelMatrix: world.toArray(),
                normalMatrix: world.clone().invert().transpose().toArray(),
                materialIndex: materialIndex
            });

            gpuMesh.instanceCount = 1;
            gpuMesh.firstInstance = instances.length - 1;
            this.renderer.meshes.push(gpuMesh);

            // Bounds and camera framing use the same authored world transform.
            const point=new Vector3();
            for(let j=0;j<meshData.points.length;j+=3) {
                point.fromArray(meshData.points,j).applyMatrix4(world);
                for(let c=0;c<3;c++) {
                    boundingBox.min[c]=Math.min(boundingBox.min[c],point.getComponent(c));
                    boundingBox.max[c]=Math.max(boundingBox.max[c],point.getComponent(c));
                }
            }
        }

        this.renderer.updateInstances(instances);

        // Fit camera to scene
        if (boundingBox.min[0] !== Infinity) {
            this.controls.fitToBox(boundingBox.min, boundingBox.max);
        }

        // Update UI
        document.getElementById('model-info').style.display = 'block';
        document.getElementById('mesh-count').textContent = numMeshes;
        document.getElementById('material-count').textContent = numMaterials;
        document.getElementById('light-count').textContent = sceneCounts.lights;
        document.getElementById('camera-count').textContent = sceneCounts.cameras;
        document.getElementById('node-count').textContent = sceneCounts.nodes;
        document.getElementById('animation-count').textContent = sceneCounts.animations;
        document.getElementById('point-instancer-count').textContent = sceneCounts.pointInstancers;
        document.getElementById('point-instance-draw-count').textContent = sceneCounts.pointInstanceDraws;
        document.getElementById('skeleton-count').textContent = sceneCounts.skeletons;
        document.getElementById('unsupported-renderable-count').textContent = sceneCounts.unsupportedRenderables;
        document.getElementById('draw-call-count').textContent = this.renderer.meshes.length;
        document.getElementById('triangle-count').textContent = this.renderer.stats.triangles;

        this.updateStatus(`Loaded: ${numMeshes} meshes, ${numMaterials} materials`);
    }

    convertMaterialToWebGPU(matData, index) {
        const material = this.createDefaultMaterial();

        // Check for OpenPBR data
        if (matData.hasOpenPBR && matData.openPBRShader) {
            const pbr = matData.openPBRShader;

            // Base layer
            if (pbr.base_color) {
                material.baseColor = pbr.base_color;
            }
            material.baseWeight = pbr.base_weight ?? 1.0;
            material.baseMetalness = pbr.base_metalness ?? 0.0;
            material.baseDiffuseRoughness = pbr.base_diffuse_roughness ?? 0.0;

            // Specular layer
            if (pbr.specular_color) {
                material.specularColor = pbr.specular_color;
            }
            material.specularWeight = pbr.specular_weight ?? 1.0;
            material.specularRoughness = pbr.specular_roughness ?? 0.5;
            material.specularIOR = pbr.specular_ior ?? 1.5;
            material.specularAnisotropy = pbr.specular_anisotropy ?? 0.0;

            // Transmission layer
            material.transmissionWeight = pbr.transmission_weight ?? 0.0;
            material.transmissionDepth = pbr.transmission_depth ?? 0.0;
            material.transmissionDispersion = pbr.transmission_dispersion ?? 0.0;

            // Coat layer
            if (pbr.coat_color) {
                material.coatColor = pbr.coat_color;
            }
            material.coatWeight = pbr.coat_weight ?? 0.0;
            material.coatRoughness = pbr.coat_roughness ?? 0.1;
            material.coatIOR = pbr.coat_ior ?? 1.5;

            // Emission layer
            if (pbr.emission_color) {
                material.emissionColor = pbr.emission_color;
            }
            material.emissionLuminance = pbr.emission_luminance ?? 0.0;

            // Fuzz/Sheen layer
            if (pbr.fuzz_color) {
                material.sheenColor = pbr.fuzz_color;
            }
            material.sheenWeight = pbr.fuzz_weight ?? 0.0;

            console.log(`Material ${index}: OpenPBR`, material);
        }
        // Fallback to UsdPreviewSurface
        else if (matData.hasUsdPreviewSurface) {
            if (matData.diffuseColor) {
                material.baseColor = matData.diffuseColor;
            }
            material.baseMetalness = matData.metallic ?? 0.0;
            material.specularRoughness = matData.roughness ?? 0.5;
            material.specularIOR = matData.ior ?? 1.5;
            material.coatWeight = matData.clearcoat ?? 0.0;
            material.coatRoughness = matData.clearcoatRoughness ?? 0.1;

            if (matData.emissiveColor) {
                material.emissionColor = matData.emissiveColor;
                material.emissionLuminance = 1.0;
            }

            console.log(`Material ${index}: UsdPreviewSurface`, material);
        }

        return material;
    }

    createDefaultMaterial() {
        return {
            baseColor: [0.8, 0.8, 0.8],
            baseWeight: 1.0,
            baseMetalness: 0.0,
            baseDiffuseRoughness: 0.0,
            specularColor: [1.0, 1.0, 1.0],
            specularWeight: 1.0,
            specularRoughness: 0.5,
            specularIOR: 1.5,
            specularAnisotropy: 0.0,
            transmissionWeight: 0.0,
            transmissionDepth: 0.0,
            transmissionDispersion: 0.0,
            coatColor: [1.0, 1.0, 1.0],
            coatWeight: 0.0,
            coatRoughness: 0.1,
            coatIOR: 1.5,
            emissionColor: [0.0, 0.0, 0.0],
            emissionLuminance: 0.0,
            sheenColor: [1.0, 1.0, 1.0],
            sheenWeight: 0.0,
            baseColorTexIdx: -1,
            normalTexIdx: -1,
            roughnessTexIdx: -1,
            metalnessTexIdx: -1,
            emissiveTexIdx: -1,
            aoTexIdx: -1
        };
    }

    createFallbackScene() {
        // Create a simple sphere
        const segments = 32;
        const rings = 16;
        const positions = [];
        const normals = [];
        const uvs = [];
        const indices = [];

        for (let y = 0; y <= rings; y++) {
            for (let x = 0; x <= segments; x++) {
                const u = x / segments;
                const v = y / rings;
                const theta = u * Math.PI * 2;
                const phi = v * Math.PI;

                const px = Math.sin(phi) * Math.cos(theta);
                const py = Math.cos(phi);
                const pz = Math.sin(phi) * Math.sin(theta);

                positions.push(px, py, pz);
                normals.push(px, py, pz);
                uvs.push(u, 1 - v);
            }
        }

        for (let y = 0; y < rings; y++) {
            for (let x = 0; x < segments; x++) {
                const a = y * (segments + 1) + x;
                const b = a + segments + 1;
                indices.push(a, b, a + 1);
                indices.push(b, b + 1, a + 1);
            }
        }

        const gpuMesh = this.renderer.createMeshBuffers({
            positions: new Float32Array(positions),
            normals: new Float32Array(normals),
            uvs: new Float32Array(uvs),
            tangents: null,
            indices: new Uint32Array(indices)
        });

        // Create default material
        const material = this.createDefaultMaterial();
        material.baseColor = [0.9, 0.7, 0.3]; // Gold
        material.baseMetalness = 0.8;
        material.specularRoughness = 0.3;

        this.renderer.updateMaterials([material]);

        // Create instance
        this.renderer.updateInstances([{
            modelMatrix: mat4Identity(),
            normalMatrix: mat4Identity(),
            materialIndex: 0
        }]);

        gpuMesh.instanceCount = 1;
        gpuMesh.firstInstance = 0;
        this.renderer.meshes.push(gpuMesh);

        // Fit camera
        this.controls.fitToBox([-1, -1, -1], [1, 1, 1]);

        document.getElementById('model-info').style.display = 'block';
        document.getElementById('mesh-count').textContent = '1';
        document.getElementById('material-count').textContent = '1';
        document.getElementById('light-count').textContent = '0';
        document.getElementById('camera-count').textContent = '0';
        document.getElementById('node-count').textContent = '0';
        document.getElementById('animation-count').textContent = '0';
        document.getElementById('point-instancer-count').textContent = '0';
        document.getElementById('point-instance-draw-count').textContent = '0';
        document.getElementById('skeleton-count').textContent = '0';
        document.getElementById('unsupported-renderable-count').textContent = '0';

        this.updateStatus('Fallback scene loaded');
    }

    animate() {
        this.animationId = requestAnimationFrame(() => this.animate());

        this.renderer.render();

        // Update stats display
        document.getElementById('fps').textContent = this.renderer.stats.fps;
        document.getElementById('gpu-time').textContent = this.renderer.stats.gpuTime.toFixed(2) + 'ms';
        document.getElementById('ubo-updates').textContent = this.renderer.stats.uboUpdates;
        document.getElementById('draw-call-count').textContent = this.renderer.stats.drawCalls;
        document.getElementById('triangle-count').textContent = this.renderer.stats.triangles;
    }

    updateStatus(message) {
        const statusEl = document.getElementById('status');
        if (statusEl) {
            statusEl.textContent = message;
        }
        console.log(message);
    }
}

// ============================================================================
// Global Functions
// ============================================================================

window.loadFile = () => document.getElementById('file-input').click();

// ============================================================================
// Start Application
// ============================================================================

const app = new Application();
window._debugOpenPBRWebGPU=app;
app.init().catch(err => {
    console.error('Failed to initialize:', err);
    document.getElementById('status').textContent = 'Error: ' + err.message;
});
