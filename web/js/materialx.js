// LightUSD MaterialX/OpenPBR Simple Demo with Three.js
// Simple viewer for USD files with MaterialX/OpenPBR and UsdPreviewSurface material support

import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { EXRLoader } from 'three/examples/jsm/loaders/EXRLoader.js';
import GUI from 'three/examples/jsm/libs/lil-gui.module.min.js';
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils, TextureLoadingManager } from 'lightusd/LightUSDLoaderUtils.js';
import {
    buildNextThreeNode,
    isNextScene,
    nextCountsFromScene,
    readNextSceneMeta
} from 'lightusd/NextRenderSceneUtils.js';
import {
    getAssetUriFromURL
} from 'lightusd/LoaderConfigUtils.js';
import { setLightUSD as setMaterialXLightUSD } from 'lightusd/LightUSDMaterialX.js';
import {installTextureColorTransform} from 'lightusd/ColorSpaceUtils.js';
import { OpenPBRMaterial } from 'lightusd/LightUSDOpenPBRSimple.js';
import { OpenPBRValidator, OpenPBRGroundTruth } from './tests/OpenPBRValidation.js';

// ============================================================================
// Constants
// ============================================================================

const DEFAULT_BACKGROUND_COLOR = 0x1a1a1a;
const CAMERA_PADDING = 1.2;
const MAX_RENDER_PIXEL_RATIO = 2.0;

const ENV_PRESETS = {
    'usd_dome': 'usd',
    'goegap_1k': 'assets/textures/goegap_1k.hdr',
    'env_sunsky_sunset': 'assets/textures/env_sunsky_sunset.hdr',
    'studio': null,
    'constant_color': 'constant'
};

const TONE_MAPPINGS = {
    'none': THREE.NoToneMapping,
    'linear': THREE.LinearToneMapping,
    'reinhard': THREE.ReinhardToneMapping,
    'cineon': THREE.CineonToneMapping,
    'aces_1.3': THREE.ACESFilmicToneMapping,
    'aces_2.0': 'custom_aces2',  // Custom implementation
    'agx': THREE.AgXToneMapping,
    'neutral': THREE.NeutralToneMapping
};

const TRACE_LOAD = new URLSearchParams(window.location.search).get('traceLoad') === 'true';
const TRACE_LOAD_T0 = performance.now();
const LOADER_BACKEND = 'next';

function traceLoadPhase(name, detail = '') {
    if (!TRACE_LOAD) {
        return;
    }
    const elapsed = (performance.now() - TRACE_LOAD_T0).toFixed(1);
    console.log(`[materialx-load] ${elapsed} ms ${name}${detail ? ` ${detail}` : ''}`);
}

function getStartupUSDModelURI(params = new URLSearchParams(window.location.search)) {
    return getAssetUriFromURL(params, ['usd']);
}

function getDisplayNameFromURI(uri) {
    try {
        const parsed = new URL(uri, window.location.href);
        const pathname = parsed.pathname || uri;
        const basename = pathname.split('/').filter(Boolean).pop();
        return basename || uri;
    } catch {
        return uri.split('/').pop() || uri;
    }
}

function readBooleanParam(params, names, currentValue, { falseOnly = false } = {}) {
    for (const name of names) {
        if (!params.has(name)) continue;
        const value = params.get(name);
        if (falseOnly) {
            return value !== 'false' && value !== '0';
        }
        return value === 'true' || value === '1';
    }
    return currentValue;
}

function nativeOptimizationOptions() {
    return {
        materialDedup: settings.nativeMaterialDedup,
        mergeMeshes: settings.nativeMeshMerge,
        mergeMeshesBakeTransform: settings.nativeMeshMergeBakeTransform,
        flattenRenderTree: settings.nativeFlattenRenderTree
    };
}

function describeNativeOptimizations() {
    const enabled = [];
    if (settings.nativeMaterialDedup) enabled.push('material-dedup');
    if (settings.nativeMeshMerge) {
        enabled.push(settings.nativeMeshMergeBakeTransform ? 'merge-meshes(bake)' : 'merge-meshes');
    }
    if (settings.nativeFlattenRenderTree) enabled.push('flatten-tree');
    return enabled.length ? enabled.join(', ') : 'no native optimizations';
}

function describeNextNativeStats(stats) {
    if (!stats) return '';
    const parts = [];
    if (Number.isFinite(stats.sourceMeshes) && Number.isFinite(stats.optimizedMeshes)) {
        parts.push(`${stats.sourceMeshes}->${stats.optimizedMeshes} meshes`);
    }
    if (Number.isFinite(stats.sourceMaterials) && Number.isFinite(stats.optimizedMaterials)) {
        parts.push(`${stats.sourceMaterials}->${stats.optimizedMaterials} mats`);
    }
    if (Number.isFinite(stats.sourceTextures) && Number.isFinite(stats.optimizedTextures)) {
        parts.push(`${stats.sourceTextures}->${stats.optimizedTextures} tex`);
    }
    return parts.length ? ` · native ${parts.join(', ')}` : '';
}

function formatDurationMs(ms) {
    if (!Number.isFinite(ms)) return 'n/a';
    if (ms < 1000) return `${ms.toFixed(0)} ms`;
    return `${(ms / 1000).toFixed(2)} s`;
}

function formatBytes(bytes) {
    if (!Number.isFinite(bytes)) return 'n/a';
    const sign = bytes < 0 ? '-' : '';
    const units = ['B', 'KB', 'MB', 'GB'];
    let value = Math.abs(bytes);
    let unitIndex = 0;
    while (value >= 1024 && unitIndex < units.length - 1) {
        value /= 1024;
        unitIndex++;
    }
    return `${sign}${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

function captureMemorySnapshot() {
    const jsHeap = performance.memory?.usedJSHeapSize;
    const wasmHeap = loaderState.loader?.native_?.HEAPU8?.buffer?.byteLength;
    return {
        jsHeap: Number.isFinite(jsHeap) ? jsHeap : null,
        wasmHeap: Number.isFinite(wasmHeap) ? wasmHeap : null
    };
}

let activeLoadStats = null;

function formatMemoryUse(before, after, key) {
    const current = after?.[key];
    if (!Number.isFinite(current)) return 'n/a';

    const previous = before?.[key];
    if (!Number.isFinite(previous)) {
        return formatBytes(current);
    }

    const delta = current - previous;
    const sign = delta > 0 ? '+' : '';
    return `${formatBytes(current)} (${sign}${formatBytes(delta)})`;
}

function formatTextureStats(stats) {
    if (!stats || !Number.isFinite(stats.textureTotal) || stats.textureTotal <= 0) {
        if (settings.skipTextures) return 'disabled';
        return stats?.textureMode === 'sync' ? 'synchronous' : 'queued 0';
    }
    const countText = `${stats.textureLoaded}/${stats.textureTotal}` +
        (stats.textureFailed ? ` (${stats.textureFailed} failed)` : '');
    if (stats.textureComplete && Number.isFinite(stats.textureMs)) {
        return `${countText}, ${formatDurationMs(stats.textureMs)}`;
    }
    return `${countText}, loading`;
}

function updateLoadStatsPanel(stats) {
    const container = document.getElementById('load-stats');
    const details = document.getElementById('load-stats-details');
    if (!container || !details) return;

    container.style.display = 'block';

    if (stats.status === 'loading') {
        details.textContent = 'Loading...';
        return;
    }

    if (stats.status === 'failed') {
        details.textContent = 'Failed';
        return;
    }

    details.textContent = [
        `File: ${formatBytes(stats.fileSize)}`,
        `Fetch/read: ${stats.fetchMs === null ? 'n/a' : formatDurationMs(stats.fetchMs)}`,
        `Parse/load: ${formatDurationMs(stats.parseMs)}`,
        `Process/build: ${formatDurationMs(stats.processMs)}`,
        `Textures: ${formatTextureStats(stats)}`,
        `Total: ${formatDurationMs(stats.totalMs)}`,
        `JS heap: ${formatMemoryUse(stats.memoryBefore, stats.memoryAfter, 'jsHeap')}`,
        `WASM heap: ${formatMemoryUse(stats.memoryBefore, stats.memoryAfter, 'wasmHeap')}`
    ].join('\n');
}

function beginLoadStats(fileSize = null) {
    const stats = {
        status: 'loading',
        startTime: performance.now(),
        fileSize,
        fetchMs: null,
        parseMs: null,
        processMs: null,
        textureMs: null,
        textureLoaded: 0,
        textureFailed: 0,
        textureTotal: 0,
        textureComplete: false,
        textureMode: settings.deferTextures ? 'deferred' : 'sync',
        totalMs: null,
        memoryBefore: captureMemorySnapshot(),
        memoryAfter: null
    };
    activeLoadStats = stats;
    updateLoadStatsPanel(stats);
    return stats;
}

function failLoadStats(stats) {
    if (!stats || stats.status === 'done') return;
    stats.status = 'failed';
    stats.totalMs = performance.now() - stats.startTime;
    stats.memoryAfter = captureMemorySnapshot();
    updateLoadStatsPanel(stats);
}

function updateTextureStats(stats, info = {}) {
    if (!stats) return;
    stats.textureLoaded = Number.isFinite(info.loaded) ? info.loaded : stats.textureLoaded;
    stats.textureFailed = Number.isFinite(info.failed) ? info.failed : stats.textureFailed;
    stats.textureTotal = Number.isFinite(info.total) ? info.total : stats.textureTotal;
    if (info.complete && Number.isFinite(info.ms)) {
        stats.textureMs = info.ms;
        stats.textureComplete = true;
        stats.memoryAfter = captureMemorySnapshot();
    }
    updateLoadStatsPanel(stats);
}

function startTrackedTextureLoading(manager, stats, traceName) {
    if (!manager || !Number.isFinite(manager.total) || manager.total <= 0 || settings.skipTextures) {
        updateTextureStats(stats, { loaded: 0, failed: 0, total: 0, complete: true, ms: 0 });
        return null;
    }
    sceneState.textureLoadingManager = manager;
    const textureStart = performance.now();
    updateTextureStats(stats, {
        loaded: manager.loaded || 0,
        failed: manager.failed || 0,
        total: manager.total || 0
    });
    traceLoadPhase(`${traceName}:start`, `${manager.total} textures`);
    return manager.startLoading({
        concurrency: settings.textureConcurrency,
        onTextureLoaded: (material) => {
            material.needsUpdate = true;
        },
        onProgress: (info) => {
            updateTextureStats(stats, info);
            if (TRACE_LOAD && (info.loaded + info.failed === info.total || ((info.loaded + info.failed) % 100) === 0)) {
                traceLoadPhase(`${traceName}:progress`, `${info.loaded}/${info.total} failed=${info.failed || 0}`);
            }
        }
    }).then((status) => {
        const elapsed = performance.now() - textureStart;
        const loaded = status.loaded || 0;
        const failed = status.failed || 0;
        const total = status.total || 0;
        if (sceneState.textureLoadingManager === manager) {
            sceneState.textureLoadingManager = null;
        }
        if (typeof manager.reset === 'function') {
            manager.reset();
        }
        updateTextureStats(stats, {
            loaded,
            failed,
            total,
            complete: true,
            ms: elapsed
        });
        traceLoadPhase(`${traceName}:done`, `${loaded}/${total} failed=${failed} ${formatDurationMs(elapsed)}`);
        return status;
    }).catch((err) => {
        console.warn(`${traceName} failed:`, err);
        const elapsed = performance.now() - textureStart;
        const status = manager.getStatus ? manager.getStatus() : null;
        const loaded = status?.loaded || manager.loaded || 0;
        const failed = status?.failed || manager.failed || 0;
        const total = status?.total || manager.total || 0;
        if (sceneState.textureLoadingManager === manager) {
            sceneState.textureLoadingManager = null;
        }
        if (typeof manager.reset === 'function') {
            manager.reset();
        }
        updateTextureStats(stats, {
            loaded,
            failed,
            total,
            complete: true,
            ms: elapsed
        });
        return status;
    });
}

// ACES 2.0 Tonemapping Shader
// Simplified real-time approximation based on ACES 2.0 Output Transform
// Key features: better hue preservation, highlight desaturation, improved S-curve
// Store original shader chunk for restoration
let originalTonemappingParsFragment = null;

const ACES2_TONEMAP_SHADER = `
// Attempt a simplified ACES 2.0 approximation for real-time rendering
// Based on the key principles: luminance-based tonemapping with hue preservation

// sRGB luminance coefficients
const vec3 ACES2_LUMA = vec3(0.2126, 0.7152, 0.0722);

// Attempt a simplified tone curve inspired by Daniele Evo curve
// This attempt uses a modified Reinhard-style curve with better highlight rolloff
float aces2_tonecurve(float x) {
    // attempt parameters tuned for SDR output
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    // attempt ACES-style filmic curve
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// attempt per-channel with luminance preservation for hue stability
vec3 ACES2ToneMapping(vec3 color) {
    // attempt clamp negative values
    color = max(color, vec3(0.0));

    // attempt exposure adjustment (attempt ACES-like pre-scaling)
    color *= 0.6;

    // attempt compute luminance before tonemapping
    float Lin = dot(color, ACES2_LUMA);

    // attempt apply tone curve to luminance
    float Lout = aces2_tonecurve(Lin);

    // attempt per-channel tonemapping
    vec3 Cout = vec3(
        aces2_tonecurve(color.r),
        aces2_tonecurve(color.g),
        aces2_tonecurve(color.b)
    );

    // attempt blend between per-channel and luminance-based for hue preservation
    // attempt more luminance-based in highlights to preserve hue
    float t = smoothstep(0.0, 1.0, Lout);

    // attempt luminance-based result (perfect hue preservation)
    vec3 LumBased = Lin > 0.0 ? color * (Lout / Lin) : vec3(0.0);
    LumBased = clamp(LumBased, 0.0, 1.0);

    // attempt blend: use more luminance-based in highlights
    vec3 result = mix(Cout, LumBased, t * 0.5);

    // attempt highlight desaturation (attempt ACES 2.0 style)
    float saturation = 1.0 - smoothstep(0.5, 1.0, Lout) * 0.3;
    float finalLum = dot(result, ACES2_LUMA);
    result = mix(vec3(finalLum), result, saturation);

    return clamp(result, 0.0, 1.0);
}
`;

// Helper function to apply ACES 2.0 custom tonemapping
function applyACES2Tonemapping() {
    // Save original shader chunk if not already saved
    if (originalTonemappingParsFragment === null) {
        originalTonemappingParsFragment = THREE.ShaderChunk.tonemapping_pars_fragment;
    }

    // Remove existing CustomToneMapping function from original chunk to avoid duplicate definition
    const originalWithoutCustom = originalTonemappingParsFragment.replace(
        /vec3 CustomToneMapping\s*\(\s*vec3 color\s*\)\s*\{[^}]*\}/g,
        '// CustomToneMapping replaced by ACES 2.0'
    );

    // Create custom tonemapping shader with ACES2ToneMapping as CustomToneMapping
    const customTonemappingShader = `
${ACES2_TONEMAP_SHADER}

// Map CustomToneMapping to our ACES 2.0 implementation
vec3 CustomToneMapping( vec3 color ) {
    return ACES2ToneMapping( color );
}
${originalWithoutCustom}
`;

    // Patch Three.js shader chunk
    THREE.ShaderChunk.tonemapping_pars_fragment = customTonemappingShader;

    // Set renderer to use custom tonemapping
    if (threeState.renderer) {
        threeState.renderer.toneMapping = THREE.CustomToneMapping;

        // Force all materials to recompile
        if (sceneState.root) {
            sceneState.root.traverse((object) => {
                if (object.isMesh && object.material) {
                    object.material.needsUpdate = true;
                }
            });
        }
    }
}

// Helper function to restore standard tonemapping
function restoreStandardTonemapping(tonemappingType) {
    // Restore original shader chunk if we modified it
    if (originalTonemappingParsFragment !== null) {
        THREE.ShaderChunk.tonemapping_pars_fragment = originalTonemappingParsFragment;
    }

    // Set the requested tonemapping
    if (threeState.renderer) {
        threeState.renderer.toneMapping = tonemappingType;

        // Force all materials to recompile
        if (sceneState.root) {
            sceneState.root.traverse((object) => {
                if (object.isMesh && object.material) {
                    object.material.needsUpdate = true;
                }
            });
        }
    }
}

// Set tonemapping mode (handles both standard and custom modes)
function setTonemapping(mode) {
    const mapping = TONE_MAPPINGS[mode];

    if (mapping === 'custom_aces2') {
        applyACES2Tonemapping();
        console.log('Applied ACES 2.0 tonemapping (Hellwig 2022 CAM-based)');
    } else {
        restoreStandardTonemapping(mapping || THREE.ACESFilmicToneMapping);
    }
}

const TEXTURE_MAPS = [
    { prop: 'map', name: 'Base Color' },
    { prop: 'normalMap', name: 'Normal' },
    { prop: 'roughnessMap', name: 'Roughness' },
    { prop: 'metalnessMap', name: 'Metalness' },
    { prop: 'emissiveMap', name: 'Emissive' },
    { prop: 'aoMap', name: 'AO' },
    { prop: 'alphaMap', name: 'Alpha' },
    { prop: 'clearcoatMap', name: 'Clearcoat' },
    { prop: 'clearcoatRoughnessMap', name: 'Clearcoat Roughness' },
    { prop: 'sheenColorMap', name: 'Fuzz Color' },
    { prop: 'iridescenceMap', name: 'Iridescence' }
];

const DEFAULT_USDA_SCENE = `#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Sphere "Sphere"
    {
        double radius = 1.0
        rel material:binding = </World/_materials/DefaultMaterial>
    }

    def Scope "_materials"
    {
        def Material "DefaultMaterial"
        {
            token outputs:surface.connect = </World/_materials/DefaultMaterial/OpenPBRSurface.outputs:surface>

            def Shader "OpenPBRSurface"
            {
                uniform token info:id = "OpenPBRSurface"

                # Base layer - metallic gold
                color3f inputs:base_color = (0.9, 0.7, 0.3)
                float inputs:base_metalness = 0.8
                float inputs:base_weight = 1.0

                # Specular layer
                float inputs:specular_roughness = 0.3
                float inputs:specular_ior = 1.5
                float inputs:specular_weight = 1.0

                token outputs:surface
            }
        }
    }
}
`;

// ============================================================================
// Global State
// ============================================================================

function createTimer() {
    if (typeof THREE.Timer === 'function') {
        return new THREE.Timer();
    }
    let lastTime = performance.now();
    return {
        reset() {
            lastTime = performance.now();
        },
        getDelta() {
            const now = performance.now();
            const delta = (now - lastTime) / 1000;
            lastTime = now;
            return delta;
        }
    };
}

// Three.js core objects
const threeState = {
    scene: null,
    camera: null,
    renderer: null,
    controls: null,
    pmremGenerator: null,
    envMap: null,
    timer: createTimer(),
    gridHelper: null,
    axesHelper: null
};

const frameState = {
    lastFpsUpdateMs: performance.now(),
    frameCount: 0
};

// USD loader state
const loaderState = {
    loader: null,
    nativeLoader: null,
    currentFileName: ''
};

// Scene state
const sceneState = {
    root: null,
    nextNodeIndexMap: null,
    materials: [],
    materialData: [],
    textureCache: new Map(),
    upAxis: 'Y',
    metadata: null,
    showingNormals: false,
    originalMaterialsMap: new Map(),
    domeLightData: null,
    textureLoadingManager: null
};

// Picking state
const pickState = {
    raycaster: new THREE.Raycaster(),
    mouse: new THREE.Vector2(),
    selectedObject: null,
    selectionHelper: null
};

// Animation state
const animationState = {
    mixer: null,
    clips: [],
    action: null,
    params: {
        isPlaying: false,  // Do not auto-play by default
        time: 0,
        beginTime: 0,
        endTime: 10,
        speed: 24.0
    }
};

// GUI state
const guiState = {
    gui: null,
    materialFolder: null,
    textureFolder: null,
    animationFolder: null,
    timeController: null,
    envPresetController: null,
    envColorController: null,
    envColorspaceController: null,
    envIntensityController: null,
    hidden: false
};

// User settings
const settings = {
    materialType: 'auto',
    materialImplementation: 'physical', // 'physical' | 'openpbr' | 'auto'
    fastMaterials: false,
    fastMaterialMode: 'full',
    deferTextures: true,
    textureConcurrency: 16,
    buildYieldMode: 'raf',
    buildYieldIntervalMs: 250,
    useMeshPtr: true,
    nativeMaterialDedup: false,
    nativeMeshMerge: false,
    nativeMeshMergeBakeTransform: true,
    nativeFlattenRenderTree: false,
    meshAggregation: 'off',
    meshAggregationMinMeshes: 2,
    computeMissingTangents: false,
    skipTextures: false,
    skipEnvironment: false,
    skipDomeLight: false,
    skipPMREM: false,
    antialias: true,
    suppressNativeInfoLogs: true,
    envMapPreset: 'goegap_1k',
    envMapIntensity: 1.0,
    envConstantColor: '#ffffff',
    envColorspace: 'sRGB',
    showBackground: true,
    exposure: 1.0,
    toneMapping: 'aces_1.3',
    applyUpAxisConversion: false,
    showNormals: false,
    normalDisplayMode: 'mapped', // 'geometry': mesh normals only, 'mapped': with normal maps applied
    normalAbsMode: false, // true: map [-1,1] to [0,1], false: standard MeshNormalMaterial
    showGrid: false,
    showAxes: false,
    gridSize: 10,
    gridDivisions: 10
};

// Normal vector visualization state (per-selected object)
const normalVectorState = {
    enabled: false,
    type: 'vertex', // 'vertex' | 'face'
    length: 0.1,
    helper: null
};

// Validator instance (created after renderer init)
let validator = null;

// ============================================================================
// Colorspace Utilities
// ============================================================================

/**
 * Convert sRGB component to linear
 */
function sRGBComponentToLinear(c) {
    return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}

/**
 * Convert linear component to sRGB
 */
function linearComponentToSRGB(c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1.0 / 2.4) - 0.055;
}

/**
 * Parse hex color and convert to RGB [0, 1] with optional linear conversion
 */
function parseHexColor(hexColor, toLinear = false) {
    const hex = hexColor.replace('#', '');
    const r = parseInt(hex.substring(0, 2), 16) / 255;
    const g = parseInt(hex.substring(2, 4), 16) / 255;
    const b = parseInt(hex.substring(4, 6), 16) / 255;

    if (toLinear) {
        return {
            r: sRGBComponentToLinear(r),
            g: sRGBComponentToLinear(g),
            b: sRGBComponentToLinear(b)
        };
    }
    return { r, g, b };
}

/**
 * Convert RGB [0, 1] to hex color string
 */
function rgbToHex(r, g, b) {
    const toHex = (c) => {
        const clamped = Math.max(0, Math.min(1, c));
        return Math.round(clamped * 255).toString(16).padStart(2, '0');
    };
    return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
}

/**
 * Convert sRGB THREE.Color to linear
 */
function sRGBToLinear(color) {
    return new THREE.Color(
        sRGBComponentToLinear(color.r),
        sRGBComponentToLinear(color.g),
        sRGBComponentToLinear(color.b)
    );
}

/**
 * Convert linear THREE.Color to sRGB
 */
function linearToSRGB(color) {
    return new THREE.Color(
        linearComponentToSRGB(color.r),
        linearComponentToSRGB(color.g),
        linearComponentToSRGB(color.b)
    );
}

// ============================================================================
// Initialization
// ============================================================================

async function init() {
    traceLoadPhase('init:start');
    // URL parameter support for batch rendering (check early for autoRender mode)
    const urlParams = new URLSearchParams(window.location.search);
    const usdPath = getStartupUSDModelURI(urlParams);
    const autoRender = urlParams.get('autoRender') === 'true';
    settings.fastMaterials = urlParams.get('fastMaterials') === 'true';
    if (urlParams.has('fastMaterialMode')) {
        settings.fastMaterialMode = urlParams.get('fastMaterialMode') || settings.fastMaterialMode;
    }
    settings.deferTextures = readBooleanParam(urlParams, ['deferTextures'], settings.deferTextures) || settings.fastMaterials;
    if (urlParams.has('textureConcurrency')) {
        const n = Number.parseInt(urlParams.get('textureConcurrency'), 10);
        if (Number.isFinite(n) && n > 0) {
            settings.textureConcurrency = n;
        }
    }
    if (urlParams.has('buildYieldMode')) {
        const mode = urlParams.get('buildYieldMode');
        if (['raf', 'timeout', 'none'].includes(mode)) {
            settings.buildYieldMode = mode;
        }
    }
    if (urlParams.has('buildYieldIntervalMs')) {
        const n = Number.parseFloat(urlParams.get('buildYieldIntervalMs'));
        if (Number.isFinite(n) && n >= 0) {
            settings.buildYieldIntervalMs = n;
        }
    }
    settings.useMeshPtr = readBooleanParam(urlParams, ['useMeshPtr'], settings.useMeshPtr, { falseOnly: true });
    settings.nativeMaterialDedup = readBooleanParam(
        urlParams, ['nativeMaterialDedup', 'dedup'], settings.nativeMaterialDedup);
    settings.nativeMeshMerge = readBooleanParam(
        urlParams, ['nativeMeshMerge', 'merge'], settings.nativeMeshMerge);
    settings.nativeMeshMergeBakeTransform = readBooleanParam(
        urlParams, ['nativeMeshMergeBakeTransform', 'bake'], settings.nativeMeshMergeBakeTransform,
        { falseOnly: true });
    settings.nativeFlattenRenderTree = readBooleanParam(
        urlParams, ['nativeFlattenRenderTree', 'flatten'], settings.nativeFlattenRenderTree);
    if (urlParams.has('meshAggregation')) {
        const mode = urlParams.get('meshAggregation') || 'off';
        settings.meshAggregation = mode === 'true' ? 'material' : mode;
    }
    if (urlParams.has('meshAggregationMinMeshes')) {
        const n = Number.parseInt(urlParams.get('meshAggregationMinMeshes'), 10);
        if (Number.isFinite(n) && n > 1) {
            settings.meshAggregationMinMeshes = n;
        }
    }
    if (urlParams.has('computeMissingTangents')) {
        settings.computeMissingTangents = urlParams.get('computeMissingTangents') === 'true';
    }
    settings.skipTextures = urlParams.get('skipTextures') === 'true' || urlParams.get('skipTextures') === '1';
    settings.skipEnvironment = urlParams.get('skipEnvironment') === 'true' || urlParams.get('skipEnvironment') === '1';
    settings.skipDomeLight = urlParams.get('skipDomeLight') === 'true' || urlParams.get('skipDomeLight') === '1';
    settings.skipPMREM = urlParams.get('skipPMREM') === 'true' || urlParams.get('skipPMREM') === '1' ||
        (settings.skipEnvironment && settings.skipDomeLight);
    if (urlParams.has('antialias')) {
        settings.antialias = urlParams.get('antialias') !== 'false' && urlParams.get('antialias') !== '0';
    }
    if (urlParams.has('suppressNativeInfoLogs')) {
        settings.suppressNativeInfoLogs = urlParams.get('suppressNativeInfoLogs') !== 'false';
    }
    const renderTimeout = parseInt(urlParams.get('renderTimeout') || '60000', 10);

    try {
        initThreeJS();
        traceLoadPhase('init:three-ready');
    } catch (error) {
        console.error('Initialization failed:', error);
        if (autoRender) {
            window.renderInitFailed = true;
            window.renderError = error.message;
            window.renderComplete = true; // Signal completion to stop waiting
        }
        return;
    }

    initControls();
    traceLoadPhase('init:controls-ready');
    await initLoader();
    traceLoadPhase('init:loader-ready');
    setupGUI();
    traceLoadPhase('init:gui-ready');
    setupEventListeners();
    if (settings.skipEnvironment) {
        traceLoadPhase('init:environment-skipped');
    } else {
        await loadEnvironment(settings.envMapPreset);
        traceLoadPhase('init:environment-ready');
    }

    if (usdPath) {
        // Load USD file from URL parameter
        await loadUSDFromURI(usdPath, autoRender);
    } else {
        await loadDefaultUSDFile();
    }

    animate();
    traceLoadPhase('init:animate-started');

    if (autoRender) {
        // Signal completion after render stabilization (default 15 seconds)
        setTimeout(() => {
            window.renderComplete = true;
            console.log(`Render complete signaled after ${renderTimeout}ms`);
        }, renderTimeout);
    }
}

async function loadUSDFromURI(uri, autoRender = false) {
    const displayName = getDisplayNameFromURI(uri);
    const stats = beginLoadStats();
    updateStatus(`Loading ${displayName}...`);
    try {
        const fetchStart = performance.now();
        const response = await fetch(uri);
        if (!response.ok) {
            throw new Error(`Failed to fetch ${uri}: ${response.statusText}`);
        }
        const arrayBuffer = await response.arrayBuffer();
        stats.fetchMs = performance.now() - fetchStart;
        stats.fileSize = arrayBuffer.byteLength;
        traceLoadPhase('usd:fetch-done', `${arrayBuffer.byteLength} bytes`);
        const data = new Uint8Array(arrayBuffer);
        await loadUSDFromData(data, displayName, stats);
        window.renderComplete = true;
    } catch (error) {
        failLoadStats(stats);
        console.error(`Failed to load USD file (${uri}):`, error, error?.stack || '');
        updateStatus(`Error: ${error.message}`);
        if (autoRender) {
            window.renderComplete = true;
            window.renderError = error.message;
        }
    }
}

function initThreeJS() {
    traceLoadPhase('initThreeJS:start');
    threeState.scene = new THREE.Scene();
    threeState.scene.background = new THREE.Color(DEFAULT_BACKGROUND_COLOR);
    traceLoadPhase('initThreeJS:scene');

    threeState.camera = new THREE.PerspectiveCamera(45, window.innerWidth / window.innerHeight, 0.1, 1000);
    threeState.camera.position.set(3, 2, 5);
    traceLoadPhase('initThreeJS:camera');

    try {
        traceLoadPhase('initThreeJS:renderer-create:start');
        threeState.renderer = new THREE.WebGLRenderer({ antialias: settings.antialias });
        traceLoadPhase('initThreeJS:renderer-create:done');
        // Check if WebGL context was created successfully
        if (!threeState.renderer.getContext()) {
            throw new Error('WebGL context is null');
        }
        traceLoadPhase('initThreeJS:renderer-context');
    } catch (error) {
        console.error('Failed to create WebGL renderer:', error);
        window.renderInitFailed = true;
        window.renderError = `WebGL initialization failed: ${error.message}`;
        throw error;
    }

    traceLoadPhase('initThreeJS:renderer-config:start');
    threeState.renderer.setSize(window.innerWidth, window.innerHeight);
    threeState.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, MAX_RENDER_PIXEL_RATIO));
    // Initialize tonemapping from settings (default: aces_1.3)
    setTonemapping(settings.toneMapping);
    threeState.renderer.toneMappingExposure = settings.exposure;
    threeState.renderer.outputColorSpace = THREE.SRGBColorSpace;
    traceLoadPhase('initThreeJS:renderer-config:done');
    traceLoadPhase('initThreeJS:dom-append:start');
    document.getElementById('canvas-container').appendChild(threeState.renderer.domElement);
    traceLoadPhase('initThreeJS:dom-append:done');

    if (!settings.skipPMREM) {
        traceLoadPhase('initThreeJS:pmrem:start');
        threeState.pmremGenerator = new THREE.PMREMGenerator(threeState.renderer);
        threeState.pmremGenerator.compileEquirectangularShader();
        traceLoadPhase('initThreeJS:pmrem:done');
    }
}

function initControls() {
    threeState.controls = new OrbitControls(threeState.camera, threeState.renderer.domElement);
    threeState.controls.enableDamping = true;
    threeState.controls.dampingFactor = 0.05;
    threeState.controls.screenSpacePanning = true;
    threeState.controls.minDistance = 0.1;
    threeState.controls.maxDistance = Infinity;
    threeState.controls.zoomSpeed = 0.8;
    threeState.controls.panSpeed = 1.0;
    threeState.controls.mouseButtons = {
        LEFT: THREE.MOUSE.ROTATE,
        MIDDLE: THREE.MOUSE.PAN,
        RIGHT: THREE.MOUSE.DOLLY
    };
    threeState.controls.keys = {
        LEFT: 'ArrowLeft',
        UP: 'ArrowUp',
        RIGHT: 'ArrowRight',
        BOTTOM: 'ArrowDown'
    };
    threeState.controls.enableKeys = true;
    threeState.controls.keyPanSpeed = 20.0;
}

async function initLoader() {
    updateStatus('Initializing LightUSD WASM...');
    traceLoadPhase('wasm:init:start');
    loaderState.loader = new LightUSDLoader(null, {
        maxMemoryLimitMB: 512,
        suppressNativeInfoLogs: settings.suppressNativeInfoLogs
    });
    await loaderState.loader.init({ useMemory64: false });
    traceLoadPhase('wasm:init:done');

    // Set LightUSD WASM module reference for HDR/EXR texture decoding fallback
    const lightusdModule = loaderState.loader.native_;
    LightUSDLoaderUtils.setLightUSD(lightusdModule);
    setMaterialXLightUSD(lightusdModule);

    updateStatus('LightUSD initialized');
}

function setupGUI() {
    guiState.gui = new GUI({ title: 'MaterialX Demo' });
    guiState.gui.domElement.style.position = 'absolute';
    guiState.gui.domElement.style.top = '10px';
    guiState.gui.domElement.style.right = '10px';
    guiState.gui.domElement.style.maxHeight = 'calc(100vh - 20px)';
    guiState.gui.domElement.style.overflowY = 'auto';

    setupSceneFolder();
    setupMaterialTypeFolder();

    guiState.materialFolder = guiState.gui.addFolder('Material Parameters');
    guiState.materialFolder.open();

    guiState.textureFolder = guiState.gui.addFolder('Textures');
    guiState.textureFolder.close();

    // Animation disabled for now - may revisit later
    // setupAnimationFolder();

    // OpenPBR validation folder
    setupValidationFolder(guiState.gui);
}

function setupSceneFolder() {
    const sceneFolder = guiState.gui.addFolder('Scene');

    // Action buttons
    const actions = {
        fitToScene: fitCameraToScene,
        clearSelection: clearSelection
    };
    sceneFolder.add(actions, 'fitToScene').name('Fit to Scene');
    sceneFolder.add(actions, 'clearSelection').name('Clear Selection');

    guiState.envPresetController = sceneFolder.add(settings, 'envMapPreset', Object.keys(ENV_PRESETS))
        .name('Environment')
        .onChange((value) => {
            loadEnvironment(value);
            updateEnvColorControlsState();
        });

    guiState.envColorController = sceneFolder.addColor(settings, 'envConstantColor')
        .name('Env Color')
        .onChange(updateConstantColorEnvironment);

    guiState.envColorspaceController = sceneFolder.add(settings, 'envColorspace', ['sRGB', 'linear'])
        .name('Env Colorspace')
        .onChange(updateConstantColorEnvironment);

    // Initialize the enabled state of env color controls
    updateEnvColorControlsState();

    guiState.envIntensityController = sceneFolder.add(settings, 'envMapIntensity', 0, 100, 0.1)
        .name('Env Intensity')
        .onChange(updateEnvIntensity);

    sceneFolder.add(settings, 'showBackground')
        .name('Show Background')
        .onChange(updateBackground);

    sceneFolder.add(settings, 'exposure', 0, 100, 0.1)
        .name('Exposure')
        .onChange(v => { threeState.renderer.toneMappingExposure = v; });

    sceneFolder.add(settings, 'toneMapping', Object.keys(TONE_MAPPINGS))
        .name('Tone Mapping')
        .onChange(v => { setTonemapping(v); });

    sceneFolder.add(settings, 'applyUpAxisConversion')
        .name('Z-up to Y-up Fix')
        .onChange(applyUpAxisConversion);

    sceneFolder.add(settings, 'showNormals')
        .name('Show Normals')
        .onChange(toggleNormalDisplay);

    sceneFolder.add(settings, 'normalDisplayMode', ['geometry', 'mapped'])
        .name('Normal Mode')
        .onChange(() => {
            if (settings.showNormals) {
                toggleNormalDisplay(); // Re-apply with new mode
            }
        });

    sceneFolder.add(settings, 'normalAbsMode')
        .name('Normal Abs Color')
        .onChange(() => {
            if (settings.showNormals) {
                toggleNormalDisplay(); // Re-apply with new mode
            }
        });

    sceneFolder.add(settings, 'showGrid')
        .name('Show Grid')
        .onChange(toggleGrid);

    sceneFolder.add(settings, 'showAxes')
        .name('Show Axes')
        .onChange(toggleAxes);
}

/*
 * Animation UI - disabled for now, may revisit later
 *
function setupAnimationFolder() {
    guiState.animationFolder = guiState.gui.addFolder('Animation');

    // Play/Pause button
    const playPauseBtn = {
        toggle: toggleAnimationPlayback
    };
    guiState.animationFolder.add(playPauseBtn, 'toggle').name('Play / Pause');

    // Reset button
    const resetBtn = {
        reset: resetAnimation
    };
    guiState.animationFolder.add(resetBtn, 'reset').name('Reset');

    // Playing state indicator (read-only checkbox)
    guiState.animationFolder.add(animationState.params, 'isPlaying')
        .name('Playing')
        .listen()
        .onChange((value) => {
            // Sync the playing state with actions
            if (animationState.mixer) {
                animationState.clips.forEach((clip) => {
                    const action = animationState.mixer.clipAction(clip);
                    action.paused = !value;
                });
                // Reset timer to avoid jump
                if (value) {
                    threeState.timer.reset();
                }
            }
        });

    guiState.timeController = guiState.animationFolder.add(animationState.params, 'time', 0, 10, 0.01)
        .name('Time')
        .listen()
        .onChange(scrubToTime);

    guiState.animationFolder.add(animationState.params, 'speed', 0.1, 120, 0.1)
        .name('Speed (FPS)')
        .listen();

    guiState.animationFolder.add(animationState.params, 'beginTime')
        .name('Begin Time')
        .listen()
        .disable();

    guiState.animationFolder.add(animationState.params, 'endTime')
        .name('End Time')
        .listen()
        .disable();

    guiState.animationFolder.close();
}
*/

function setupMaterialTypeFolder() {
    const materialTypeFolder = guiState.gui.addFolder('Material Type');
    materialTypeFolder.add(settings, 'materialType', ['auto', 'openpbr', 'usdpreviewsurface'])
        .name('Preferred Type')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'materialImplementation', ['physical', 'openpbr', 'auto'])
        .name('Implementation')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'fastMaterials')
        .name('Fast Materials')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'fastMaterialMode', ['full', 'preview'])
        .name('Fast Mode')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'deferTextures')
        .name('Defer Textures')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'buildYieldMode', ['raf', 'timeout', 'none'])
        .name('Build Yield')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'buildYieldIntervalMs', 0, 1000, 1)
        .name('Yield Interval')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'useMeshPtr')
        .name('Mesh Ptr')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'nativeMaterialDedup')
        .name('Native Mat Dedup')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'nativeMeshMerge')
        .name('Native Mesh Merge')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'nativeMeshMergeBakeTransform')
        .name('Native Bake Xform')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'nativeFlattenRenderTree')
        .name('Native Flat Tree')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'meshAggregation', ['off', 'material'])
        .name('Mesh Aggregate')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'meshAggregationMinMeshes', 2, 64, 1)
        .name('Aggregate Min')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'computeMissingTangents')
        .name('Tangents')
        .onChange(reloadMaterials);

    // Show discrepancy report
    materialTypeFolder.add({
        showDiscrepancies: showMaterialDiscrepancyReport
    }, 'showDiscrepancies').name('Show Limitations');

    materialTypeFolder.open();
}

/**
 * Show MeshPhysicalMaterial vs OpenPBRMaterial discrepancy report
 */
function showMaterialDiscrepancyReport() {
    const report = `
=== MeshPhysicalMaterial vs OpenPBRMaterial ===

Features MISSING in MeshPhysicalMaterial:
- base_diffuse_roughness (Oren-Nayar diffuse)
- coat_color (always white)
- coat_ior (fixed at 1.5)
- specular_weight (different from specularIntensity)
- base_weight

Features with DIFFERENT models:
- Sheen: Three.js uses Charlie, OpenPBR uses different formulation
- Thin Film: Different parameterization

OUT OF SCOPE (require raytracing):
- Subsurface scattering (SSS)
- Transmission scatter
- Dispersion

Use 'openpbr' implementation to get accurate OpenPBR rendering.
`.trim();

    console.log(report);
    alert(report);
}

function setupEventListeners() {
    window.addEventListener('resize', onWindowResize);
    window.addEventListener('keydown', onKeyDown);

    const fileInput = document.getElementById('file-input');
    fileInput.addEventListener('change', onFileSelect);

    const container = document.getElementById('canvas-container');
    container.addEventListener('dragover', onDragOver);
    container.addEventListener('drop', onFileDrop);
    container.addEventListener('dragleave', onDragLeave);

    // Object picking - use click event
    container.addEventListener('click', onCanvasClick);

    const visibilityToggle = document.getElementById('gui-visibility-toggle');
    if (visibilityToggle) {
        visibilityToggle.addEventListener('click', () => setGuiHidden(!guiState.hidden));
        setGuiHidden(false);
    }
}

/*
 * ============================================================================
 * Animation Control - disabled for now, may revisit later
 * ============================================================================

function scrubToTime(time) {
    if (!animationState.mixer || !animationState.action) return;

    const wasPaused = !animationState.params.isPlaying;

    animationState.mixer.timeScale = 1.0;
    animationState.mixer.time = 0;

    animationState.clips.forEach((clip) => {
        const action = animationState.mixer.clipAction(clip);
        action.paused = false;
        action.enabled = true;
        action.time = time;
        action.weight = 1.0;
    });

    animationState.mixer.update(0.0001);

    animationState.clips.forEach((clip) => {
        const action = animationState.mixer.clipAction(clip);
        action.time = time;
    });

    if (wasPaused) {
        animationState.clips.forEach((clip) => {
            const action = animationState.mixer.clipAction(clip);
            action.paused = true;
        });
    }
}

// ============================================================================
// USD Animation Extraction
// ============================================================================

function convertUSDAnimationsToThreeJS(usdLoader, root) {
    const clips = [];
    const numAnimations = usdLoader.numAnimations();

    if (numAnimations === 0) return clips;

    // Next animation target_node is a RenderScene node-table index; use the
    // table map from buildNextThreeNode when present, not a DFS walk (the
    // next tree inserts wrapper groups, so DFS indices drift).
    const nodeIndexMap = sceneState.nextNodeIndexMap || buildNodeIndexMap(root);

    for (let i = 0; i < numAnimations; i++) {
        const usdAnimation = usdLoader.getAnimation(i);
        const clip = convertSingleAnimation(usdAnimation, i, root, nodeIndexMap);
        if (clip) clips.push(clip);
    }

    return clips;
}

function buildNodeIndexMap(root) {
    const map = new Map();
    let index = 0;
    root.traverse((obj) => {
        map.set(index++, obj);
    });
    return map;
}

function convertSingleAnimation(usdAnimation, index, root, nodeIndexMap) {
    // Handle track-based animation (legacy format)
    if (usdAnimation.tracks && usdAnimation.tracks.length > 0) {
        return convertTrackBasedAnimation(usdAnimation, index, root);
    }

    // Handle channel-based animation
    if (usdAnimation.channels && usdAnimation.samplers) {
        return convertChannelBasedAnimation(usdAnimation, index, nodeIndexMap);
    }

    return null;
}

function convertTrackBasedAnimation(usdAnimation, index, root) {
    const keyframeTracks = [];
    const targetObject = findAnimationTarget(usdAnimation.name, root);
    const targetUUID = targetObject.uuid;

    for (const track of usdAnimation.tracks) {
        if (!track.times || !track.values) continue;

        const times = Array.isArray(track.times) ? track.times : Array.from(track.times);
        const values = Array.isArray(track.values) ? track.values : Array.from(track.values);
        const interpolation = getUSDInterpolationMode(track.interpolation);

        const keyframeTrack = createKeyframeTrack(track.path, targetUUID, times, values, interpolation);
        if (keyframeTrack) keyframeTracks.push(keyframeTrack);
    }

    if (keyframeTracks.length === 0) return null;

    return new THREE.AnimationClip(
        usdAnimation.name || `Animation_${index}`,
        usdAnimation.duration || -1,
        keyframeTracks
    );
}

function convertChannelBasedAnimation(usdAnimation, index, nodeIndexMap) {
    const nodeChannels = usdAnimation.channels.filter(channel => {
        const targetType = channel.target_type || 'SceneNode';
        return targetType === 'SceneNode';
    });

    if (nodeChannels.length === 0) return null;

    const keyframeTracks = [];

    for (const channel of nodeChannels) {
        const sampler = usdAnimation.samplers[channel.sampler];
        if (!sampler || !sampler.times || !sampler.values) continue;

        const targetObject = nodeIndexMap.get(channel.target_node);
        if (!targetObject) continue;

        const times = Array.isArray(sampler.times) ? sampler.times : Array.from(sampler.times);
        const values = Array.isArray(sampler.values) ? sampler.values : Array.from(sampler.values);
        const interpolation = getUSDInterpolationMode(sampler.interpolation);

        const keyframeTrack = createKeyframeTrack(channel.path, targetObject.uuid, times, values, interpolation);
        if (keyframeTrack) keyframeTracks.push(keyframeTrack);
    }

    if (keyframeTracks.length === 0) return null;

    return new THREE.AnimationClip(
        usdAnimation.name || `Animation_${index}`,
        usdAnimation.duration || -1,
        keyframeTracks
    );
}

function findAnimationTarget(animationName, root) {
    let targetObject = root;

    if (animationName) {
        let searchName = animationName.replace(/_xform$/, '').replace(/_anim$/, '');

        // Try exact match
        root.traverse((obj) => {
            if (obj.name === searchName) targetObject = obj;
        });

        // Try prefix match
        if (targetObject === root) {
            root.traverse((obj) => {
                if (obj.name && obj.name.startsWith(searchName)) targetObject = obj;
            });
        }
    }

    // Fallback to first mesh or group
    if (targetObject === root) {
        root.traverse((obj) => {
            if ((obj.isMesh || obj.isGroup) && obj !== root) {
                targetObject = obj;
                return;
            }
        });
    }

    return targetObject;
}

function createKeyframeTrack(path, targetUUID, times, values, interpolation) {
    const normalizedPath = path.toLowerCase();

    if (normalizedPath === 'translation') {
        return new THREE.VectorKeyframeTrack(`${targetUUID}.position`, times, values, interpolation);
    }
    if (normalizedPath === 'rotation') {
        return new THREE.QuaternionKeyframeTrack(`${targetUUID}.quaternion`, times, values, interpolation);
    }
    if (normalizedPath === 'scale') {
        return new THREE.VectorKeyframeTrack(`${targetUUID}.scale`, times, values, interpolation);
    }
    if (normalizedPath === 'weights') {
        return new THREE.NumberKeyframeTrack(`.uuid[${targetUUID}].morphTargetInfluences`, times, values, interpolation);
    }

    return null;
}

function getUSDInterpolationMode(interpolation) {
    const mode = (interpolation || '').toUpperCase();
    if (mode === 'STEP') return THREE.InterpolateDiscrete;
    if (mode === 'CUBICSPLINE') return THREE.InterpolateSmooth;
    return THREE.InterpolateLinear;
}

End of Animation Control block
*/

// ============================================================================
// Environment Loading
// ============================================================================

async function loadEnvironment(preset) {
    settings.envMapPreset = preset;
    const path = ENV_PRESETS[preset];

    if (!path) {
        threeState.envMap = createStudioEnvironment();
        applyEnvironment();
        return;
    }

    if (path === 'usd') {
        loadUSDDomeEnvironment();
        return;
    }

    if (path === 'constant') {
        threeState.envMap = createConstantColorEnvironment(settings.envConstantColor, settings.envColorspace);
        applyEnvironment();
        return;
    }

    updateStatus(`Loading environment: ${preset}...`);
    try {
        const hdrLoader = new HDRLoader();
        const texture = await hdrLoader.loadAsync(path);
        threeState.envMap = threeState.pmremGenerator.fromEquirectangular(texture).texture;
        texture.dispose();
        applyEnvironment();
        updateStatus('Environment loaded');
    } catch (error) {
        console.error('Failed to load environment:', error);
        updateStatus('Failed to load environment');
        threeState.envMap = createStudioEnvironment();
        applyEnvironment();
    }
}

function loadUSDDomeEnvironment() {
    if (sceneState.domeLightData && sceneState.domeLightData.envMap) {
        threeState.envMap = sceneState.domeLightData.envMap;
        settings.envMapIntensity = sceneState.domeLightData.intensity || 1.0;
        applyEnvironment();
        // Update GUI controller to reflect DomeLight intensity
        if (guiState.envIntensityController) {
            guiState.envIntensityController.updateDisplay();
        }
        updateStatus('Using USD DomeLight environment');
    } else {
        updateStatus('No USD DomeLight available - using studio lighting');
        threeState.envMap = createStudioEnvironment();
        applyEnvironment();
    }
}

function createStudioEnvironment() {
    const canvas = document.createElement('canvas');
    canvas.width = 256;
    canvas.height = 256;
    const ctx = canvas.getContext('2d');

    const gradient = ctx.createLinearGradient(0, 0, 0, 256);
    gradient.addColorStop(0, '#ffffff');
    gradient.addColorStop(0.5, '#cccccc');
    gradient.addColorStop(1, '#666666');
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, 256, 256);

    const texture = new THREE.CanvasTexture(canvas);
    texture.mapping = THREE.EquirectangularReflectionMapping;
    return threeState.pmremGenerator.fromEquirectangular(texture).texture;
}

function createConstantColorEnvironment(color, colorspace = 'sRGB') {
    const canvas = document.createElement('canvas');
    canvas.width = 256;
    canvas.height = 256;
    const ctx = canvas.getContext('2d');

    let fillColor = color;
    if (colorspace === 'sRGB') {
        const rgb = parseHexColor(color, true);
        fillColor = rgbToHex(rgb.r, rgb.g, rgb.b);
    }

    ctx.fillStyle = fillColor;
    ctx.fillRect(0, 0, 256, 256);

    const texture = new THREE.CanvasTexture(canvas);
    texture.mapping = THREE.EquirectangularReflectionMapping;
    texture.colorSpace = THREE.LinearSRGBColorSpace;

    return threeState.pmremGenerator.fromEquirectangular(texture).texture;
}

function applyEnvironment() {
    threeState.scene.environment = threeState.envMap;
    updateBackground();
    updateEnvIntensity();

    sceneState.materials.forEach((mat) => {
        mat.envMap = threeState.envMap;
        mat.needsUpdate = true;
    });
}

function updateBackground() {
    threeState.scene.background = (settings.showBackground && threeState.envMap)
        ? threeState.envMap
        : new THREE.Color(DEFAULT_BACKGROUND_COLOR);
}

function updateConstantColorEnvironment() {
    if (settings.envMapPreset === 'constant_color') {
        threeState.envMap = createConstantColorEnvironment(settings.envConstantColor, settings.envColorspace);
        applyEnvironment();
    }
}

/**
 * Update the enabled state of Env Color and Env Colorspace controls
 * These should only be editable when Environment is set to 'constant_color'
 */
function updateEnvColorControlsState() {
    const isConstantColor = settings.envMapPreset === 'constant_color';

    if (guiState.envColorController) {
        if (isConstantColor) {
            guiState.envColorController.enable();
        } else {
            guiState.envColorController.disable();
        }
    }

    if (guiState.envColorspaceController) {
        if (isConstantColor) {
            guiState.envColorspaceController.enable();
        } else {
            guiState.envColorspaceController.disable();
        }
    }
}

function updateEnvIntensity() {
    sceneState.materials.forEach(mat => {
        if (mat.envMapIntensity !== undefined) {
            mat.envMapIntensity = settings.envMapIntensity;
        }
    });
}

// ============================================================================
// DomeLight Loading (uses LightUSDLoaderUtils)
// ============================================================================

/**
 * Load DomeLight from USD and apply to scene
 * Uses LightUSDLoaderUtils.loadDomeLightFromUSD for the heavy lifting
 */
async function loadDomeLightFromUSD(usdLoader) {
    const result = await LightUSDLoaderUtils.loadDomeLightFromUSD(usdLoader, threeState.pmremGenerator);

    if (result) {
        // Apply result to app state
        threeState.envMap = result.texture;
        settings.envMapIntensity = result.intensity;
        settings.envMapPreset = 'usd_dome';

        if (result.colorHex) {
            settings.envConstantColor = result.colorHex;
        }

        applyEnvironment();

        // Update GUI controllers to reflect DomeLight settings
        if (guiState.envIntensityController) {
            guiState.envIntensityController.updateDisplay();
        }

        // Store DomeLight data for reference
        sceneState.domeLightData = {
            name: result.name,
            textureFile: result.textureFile,
            envmapTextureId: result.envmapTextureId,
            intensity: result.intensity,
            color: result.color,
            exposure: result.exposure,
            envMap: threeState.envMap
        };

        return sceneState.domeLightData;
    }

    return null;
}

// ============================================================================
// UpAxis Conversion
// ============================================================================

/**
 * Initialize upAxis conversion setting based on USD file's upAxis
 * Called once when a new file is loaded
 */
function initUpAxisConversion() {
    // Automatically enable Z-up to Y-up conversion when USD file has upAxis = 'Z'
    if (sceneState.upAxis === 'Z') {
        settings.applyUpAxisConversion = true;
    } else {
        settings.applyUpAxisConversion = false;
    }

    // Update GUI checkbox if available
    if (guiState.gui) {
        guiState.gui.controllersRecursive().forEach(controller => {
            if (controller.property === 'applyUpAxisConversion') {
                controller.updateDisplay();
            }
        });
    }
}

function applyUpAxisConversion() {
    if (!sceneState.root) return;

    if (settings.applyUpAxisConversion && sceneState.upAxis === 'Z') {
        sceneState.root.rotation.x = -Math.PI / 2;
    } else {
        sceneState.root.rotation.x = 0;
    }
}

// ============================================================================
// Normal Display Mode
// ============================================================================

/**
 * Create a shader material that displays normals as absolute colors
 * Maps normal components from [-1, 1] to [0, 1]
 * This shows world-space normal direction regardless of view angle
 * If normalMap is provided, it will show perturbed normals
 */
function createAbsNormalMaterial(normalMap = null, normalScale = new THREE.Vector2(1, 1)) {
    const material = new THREE.ShaderMaterial({
        uniforms: {
            normalMap: { value: normalMap },
            normalScale: { value: normalScale },
            useNormalMap: { value: normalMap !== null }
        },
        vertexShader: `
            varying vec3 vNormal;
            varying vec3 vWorldNormal;
            varying vec2 vUv;
            varying vec3 vTangent;
            varying vec3 vBitangent;

            #ifdef USE_TANGENT
                attribute vec4 tangent;
            #endif

            void main() {
                // Transform normal to world space using inverse transpose of model matrix
                // For uniform scale, mat3(modelMatrix) works; for non-uniform, need proper normal matrix
                mat3 worldNormalMatrix = mat3(modelMatrix);
                vWorldNormal = normalize(worldNormalMatrix * normal);
                vNormal = normalize(normalMatrix * normal);
                vUv = uv;

                #ifdef USE_TANGENT
                    vTangent = normalize(worldNormalMatrix * tangent.xyz);
                    vBitangent = normalize(cross(vWorldNormal, vTangent) * tangent.w);
                #else
                    // Compute tangent - align with world X axis for horizontal planes
                    vTangent = normalize(worldNormalMatrix * vec3(1.0, 0.0, 0.0));
                    vBitangent = normalize(cross(vWorldNormal, vTangent));
                #endif

                gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
            }
        `,
        fragmentShader: `
            uniform sampler2D normalMap;
            uniform vec2 normalScale;
            uniform bool useNormalMap;

            varying vec3 vNormal;
            varying vec3 vWorldNormal;
            varying vec2 vUv;
            varying vec3 vTangent;
            varying vec3 vBitangent;

            void main() {
                // Use world-space normal directly - don't flip based on facing
                // We want to show the actual geometric normal direction
                vec3 n = normalize(vWorldNormal);

                if (useNormalMap) {
                    // Sample normal map and transform to world space
                    vec3 mapN = texture2D(normalMap, vUv).xyz * 2.0 - 1.0;
                    mapN.xy *= normalScale;
                    mapN = normalize(mapN);

                    // Build TBN matrix - use geometric normal for the N component
                    vec3 T = normalize(vTangent);
                    vec3 B = normalize(vBitangent);
                    vec3 N = n;
                    mat3 TBN = mat3(T, B, N);
                    n = normalize(TBN * mapN);
                }

                // Map from [-1, 1] to [0, 1]
                vec3 color = n * 0.5 + 0.5;
                gl_FragColor = vec4(color, 1.0);
            }
        `,
        side: THREE.DoubleSide
    });

    return material;
}

/**
 * Create a shader material that displays normals like MeshNormalMaterial but with normal map support
 * This shows view-space normals with normal map perturbation applied
 */
function createViewNormalMaterial(normalMap = null, normalScale = new THREE.Vector2(1, 1)) {
    const material = new THREE.ShaderMaterial({
        uniforms: {
            normalMap: { value: normalMap },
            normalScale: { value: normalScale },
            useNormalMap: { value: normalMap !== null }
        },
        vertexShader: `
            varying vec3 vNormal;
            varying vec3 vViewNormal;
            varying vec2 vUv;
            varying vec3 vTangent;
            varying vec3 vBitangent;

            #ifdef USE_TANGENT
                attribute vec4 tangent;
            #endif

            void main() {
                vNormal = normalize(normalMatrix * normal);
                vViewNormal = vNormal;
                vUv = uv;

                // Compute tangent and bitangent in view space
                #ifdef USE_TANGENT
                    vTangent = normalize(normalMatrix * tangent.xyz);
                    vBitangent = normalize(cross(vNormal, vTangent) * tangent.w);
                #else
                    // Compute tangent from normal - works for most cases
                    vec3 up = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
                    vTangent = normalize(normalMatrix * normalize(cross(up, normal)));
                    vBitangent = normalize(cross(vNormal, vTangent));
                #endif

                gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
            }
        `,
        fragmentShader: `
            uniform sampler2D normalMap;
            uniform vec2 normalScale;
            uniform bool useNormalMap;

            varying vec3 vNormal;
            varying vec3 vViewNormal;
            varying vec2 vUv;
            varying vec3 vTangent;
            varying vec3 vBitangent;

            void main() {
                vec3 normal = normalize(vViewNormal);

                if (useNormalMap) {
                    // Sample normal map and transform to view space
                    vec3 mapN = texture2D(normalMap, vUv).xyz * 2.0 - 1.0;
                    mapN.xy *= normalScale;
                    mapN = normalize(mapN);

                    // Build TBN matrix in view space
                    vec3 T = normalize(vTangent);
                    vec3 B = normalize(vBitangent);
                    vec3 N = normalize(vNormal);
                    mat3 TBN = mat3(T, B, N);
                    normal = normalize(TBN * mapN);
                }

                // MeshNormalMaterial-style coloring: map view-space normal to RGB
                // normal.xyz is in [-1, 1], map to [0, 1]
                gl_FragColor = vec4(normal * 0.5 + 0.5, 1.0);
            }
        `,
        side: THREE.DoubleSide
    });

    return material;
}

/**
 * Extract normal map from material, checking various sources
 * Works with both OpenPBR/MaterialX and UsdPreviewSurface materials
 */
function extractNormalMapFromMaterial(material) {
    if (!material) return { normalMap: null, normalScale: new THREE.Vector2(1, 1) };

    let normalMap = null;
    let normalScale = new THREE.Vector2(1, 1);

    // 1. Check direct normalMap property (MeshPhysicalMaterial, MeshStandardMaterial)
    if (material.normalMap) {
        normalMap = material.normalMap;
        if (material.normalScale) {
            normalScale = material.normalScale.clone();
        }
    }

    // 2. Check userData for materials where normalMap was loaded asynchronously
    if (!normalMap && material.userData) {
        // Check for textures stored in userData (common pattern for async-loaded textures)
        if (material.userData.textures && material.userData.textures.normalMap) {
            normalMap = material.userData.textures.normalMap;
        }
        // Check for raw material data (UsdPreviewSurface)
        if (!normalMap && material.userData.rawData) {
            const rawData = material.userData.rawData;
            // UsdPreviewSurface stores normalTextureId in surfaceShader
            if (rawData.surfaceShader && rawData.surfaceShader.normalTextureId !== undefined) {
                // Normal map should have been loaded - check material.normalMap again
                // This handles the case where texture was loaded after material creation
                if (material.normalMap) {
                    normalMap = material.normalMap;
                }
            }
        }
    }

    // 3. Handle array of materials
    if (Array.isArray(material)) {
        // Try to find a material with normalMap in the array
        for (const mat of material) {
            const result = extractNormalMapFromMaterial(mat);
            if (result.normalMap) {
                return result;
            }
        }
    }

    return { normalMap, normalScale };
}

function toggleNormalDisplay() {
    if (!sceneState.root) return;

    if (settings.showNormals) {
        sceneState.showingNormals = true;
        sceneState.root.traverse(obj => {
            if (obj.isMesh && obj.material) {
                // Store original material if not already stored
                if (!sceneState.originalMaterialsMap.has(obj)) {
                    sceneState.originalMaterialsMap.set(obj, obj.material);
                }

                const origMat = sceneState.originalMaterialsMap.get(obj);

                // Extract normal map based on display mode
                let normalMap = null;
                let normalScale = new THREE.Vector2(1, 1);

                if (settings.normalDisplayMode === 'mapped') {
                    // Show normal-mapped normals - extract from material
                    const extracted = extractNormalMapFromMaterial(origMat);
                    normalMap = extracted.normalMap;
                    normalScale = extracted.normalScale;
                }
                // If mode is 'geometry', normalMap stays null to show mesh normals only

                if (settings.normalAbsMode) {
                    // Absolute color mode: [-1,1] -> [0,1]
                    // Pass normal map based on display mode
                    obj.material = createAbsNormalMaterial(normalMap, normalScale);
                } else {
                    // Standard MeshNormalMaterial (view-dependent coloring)
                    // Note: MeshNormalMaterial doesn't support normal maps, so we use
                    // our custom shader if we need to show normal-mapped normals
                    if (normalMap && settings.normalDisplayMode === 'mapped') {
                        // Use custom shader to show normal-mapped normals in standard view mode
                        obj.material = createViewNormalMaterial(normalMap, normalScale);
                    } else {
                        obj.material = new THREE.MeshNormalMaterial({
                            flatShading: false,
                            side: THREE.DoubleSide
                        });
                    }
                }
            }
        });
    } else {
        sceneState.showingNormals = false;
        sceneState.root.traverse(obj => {
            if (obj.isMesh && sceneState.originalMaterialsMap.has(obj)) {
                obj.material = sceneState.originalMaterialsMap.get(obj);
            }
        });
        sceneState.originalMaterialsMap.clear();
    }
}

// ============================================================================
// Grid and Axes Helpers
// ============================================================================

function toggleGrid() {
    if (settings.showGrid) {
        if (!threeState.gridHelper) {
            createGridHelper();
        }
        threeState.gridHelper.visible = true;
    } else {
        if (threeState.gridHelper) {
            threeState.gridHelper.visible = false;
        }
    }
}

function toggleAxes() {
    if (settings.showAxes) {
        if (!threeState.axesHelper) {
            createAxesHelper();
        }
        threeState.axesHelper.visible = true;
    } else {
        if (threeState.axesHelper) {
            threeState.axesHelper.visible = false;
        }
    }
}

function createGridHelper() {
    const size = settings.gridSize;
    const divisions = settings.gridDivisions;

    threeState.gridHelper = new THREE.GridHelper(size, divisions, 0x444444, 0x222222);
    threeState.gridHelper.visible = settings.showGrid;
    threeState.scene.add(threeState.gridHelper);
}

function createAxesHelper() {
    const size = settings.gridSize / 2;

    threeState.axesHelper = new THREE.AxesHelper(size);
    threeState.axesHelper.visible = settings.showAxes;
    threeState.scene.add(threeState.axesHelper);
}

function updateHelpersSize() {
    // Update grid size based on scene bounds
    if (sceneState.root) {
        const box = new THREE.Box3().setFromObject(sceneState.root);
        const size = box.getSize(new THREE.Vector3());
        const maxDim = Math.max(size.x, size.y, size.z);

        // Set grid size to be larger than the scene
        settings.gridSize = Math.ceil(maxDim * 2);
        settings.gridDivisions = Math.min(20, Math.max(10, Math.ceil(settings.gridSize)));

        // Recreate helpers with new size
        if (threeState.gridHelper) {
            threeState.scene.remove(threeState.gridHelper);
            threeState.gridHelper.dispose();
            threeState.gridHelper = null;
            if (settings.showGrid) {
                createGridHelper();
            }
        }

        if (threeState.axesHelper) {
            threeState.scene.remove(threeState.axesHelper);
            threeState.axesHelper.dispose();
            threeState.axesHelper = null;
            if (settings.showAxes) {
                createAxesHelper();
            }
        }
    }
}

// ============================================================================
// USD Loading
// ============================================================================

async function loadDefaultScene() {
    updateStatus('Loading default scene...');
    const encoder = new TextEncoder();
    const data = encoder.encode(DEFAULT_USDA_SCENE);
    const stats = beginLoadStats(data.byteLength);
    await loadUSDFromData(data, 'default.usda', stats);
}

async function loadDefaultUSDFile() {
    const defaultFile = './assets/fancy-teapot-mtlx.usdz';
    const stats = beginLoadStats();
    updateStatus(`Loading ${defaultFile}...`);
    try {
        const fetchStart = performance.now();
        const response = await fetch(defaultFile);
        if (!response.ok) {
            throw new Error(`Failed to fetch ${defaultFile}: ${response.statusText}`);
        }
        const arrayBuffer = await response.arrayBuffer();
        stats.fetchMs = performance.now() - fetchStart;
        stats.fileSize = arrayBuffer.byteLength;
        const data = new Uint8Array(arrayBuffer);
        await loadUSDFromData(data, defaultFile, stats);
    } catch (error) {
        failLoadStats(stats);
        console.error(`Failed to load default USD file (${defaultFile}):`, error);
        updateStatus(`Failed to load ${defaultFile}, loading fallback scene...`);
        await loadDefaultScene();
    }
}

async function loadUSDFromFile(file) {
    const stats = beginLoadStats();
    updateStatus(`Loading: ${file.name}...`);
    try {
        const readStart = performance.now();
        const arrayBuffer = await file.arrayBuffer();
        stats.fetchMs = performance.now() - readStart;
        stats.fileSize = arrayBuffer.byteLength;
        const data = new Uint8Array(arrayBuffer);
        await loadUSDFromData(data, file.name, stats);
    } catch (error) {
        failLoadStats(stats);
        console.error('Failed to load USD file:', error);
        updateStatus(`Error: ${error.message}`);
    }
}

async function loadUSDFromData(data, filename, stats = null) {
    stats = stats || beginLoadStats(data.byteLength);
    if (!Number.isFinite(stats.fileSize)) {
        stats.fileSize = data.byteLength;
    }
    traceLoadPhase('loadUSDFromData:start', `${filename} ${data.byteLength} bytes`);
    loaderState.currentFileName = filename || '';
    clearScene();
    traceLoadPhase('loadUSDFromData:clearScene-done');

    if (LOADER_BACKEND === 'next' || LOADER_BACKEND === 'auto') {
        const parseStart = performance.now();
        const usdScene = await new Promise((resolve, reject) => {
            loaderState.loader.parse(data, filename, resolve, reject, {
                backend: LOADER_BACKEND,
                ...nativeOptimizationOptions()
            });
        });
        stats.parseMs = performance.now() - parseStart;
        if (isNextScene(usdScene)) {
            const processStart = performance.now();
            const meta = readNextSceneMeta(usdScene);
            sceneState.upAxis = meta.upAxis || 'Y';
            sceneState.metadata = {
                upAxis: sceneState.upAxis,
                metersPerUnit: meta.metersPerUnit || 1.0,
                framesPerSecond: 24.0,
                timeCodesPerSecond: 24.0
            };
            const built = buildNextThreeNode(usdScene, {
                skipTextures: settings.skipTextures,
                lazyTextures: !settings.skipTextures
            });
            sceneState.root = built.node;
            sceneState.nextNodeIndexMap = built.nodeIndexMap || null;
            threeState.scene.add(sceneState.root);
            if (built.textureManager && !settings.skipTextures) {
                startTrackedTextureLoading(built.textureManager, stats, 'nextTextureQueue');
            }
            collectMaterialsFromScene();
            updateMaterialUI();
            initUpAxisConversion();
            applyUpAxisConversion();
            fitCameraToScene();
            updateHelpersSize();
            const counts = nextCountsFromScene(usdScene);
            const meshCount = counts.meshes;
            const materialCount = sceneState.materials.length || counts.materials;
            document.getElementById('model-info').style.display = 'block';
            document.getElementById('current-file').textContent = loaderState.currentFileName || '-';
            document.getElementById('mesh-count').textContent = meshCount;
            document.getElementById('material-count').textContent = materialCount;
            setSceneEntityCountsFromScene(counts);
            updateStatus(`Loaded: ${meshCount} meshes, ${materialCount} materials ` +
                `(backend: next · MaterialX/OpenPBR material data · ${describeNativeOptimizations()})${describeNextNativeStats(counts.stats)}`);
            if (typeof usdScene.releaseBuildData === 'function') {
                usdScene.releaseBuildData();
            }
            stats.processMs = performance.now() - processStart;
            stats.totalMs = performance.now() - stats.startTime;
            stats.memoryAfter = captureMemorySnapshot();
            stats.status = 'done';
            updateLoadStatsPanel(stats);
            window.renderComplete = true;
            return;
        }
    }
    throw new Error('Core scene adapter was not produced');
}

function isNativeInfoLog(args) {
    if (!settings.suppressNativeInfoLogs) {
        return false;
    }
    if (!args.length || typeof args[0] !== 'string') {
        return false;
    }
    return args[0].startsWith('[INFO] ');
}

function withNativeInfoLogFilter(fn) {
    if (!settings.suppressNativeInfoLogs) {
        return fn();
    }
    const originalLog = console.log;
    const originalInfo = console.info;
    console.log = (...args) => {
        if (!isNativeInfoLog(args)) {
            originalLog.apply(console, args);
        }
    };
    console.info = (...args) => {
        if (!isNativeInfoLog(args)) {
            originalInfo.apply(console, args);
        }
    };
    try {
        return fn();
    } finally {
        console.log = originalLog;
        console.info = originalInfo;
    }
}

function loadSceneMetadata() {
    const metadata = loaderState.nativeLoader.getSceneMetadata ? loaderState.nativeLoader.getSceneMetadata() : {};
    sceneState.upAxis = metadata.upAxis || 'Y';
    sceneState.metadata = {
        upAxis: sceneState.upAxis,
        metersPerUnit: metadata.metersPerUnit || 1.0,
        framesPerSecond: metadata.framesPerSecond || 24.0,
        timeCodesPerSecond: metadata.timeCodesPerSecond || 24.0,
        startTimeCode: metadata.startTimeCode,
        endTimeCode: metadata.endTimeCode
    };
}

/**
 * Build scene graph using LightUSDLoaderUtils.buildThreeNode
 * This properly reflects USD Prim xformOps hierarchy
 */
async function buildSceneGraph() {
    traceLoadPhase('buildSceneGraph:reset-state');
    // Clear state
    sceneState.materialData = [];
    sceneState.materials = [];
    sceneState.textureCache.clear();

    // Get the USD root node for hierarchy traversal
    const usdRootNode = loaderState.nativeLoader.getDefaultRootNode();
    traceLoadPhase('buildSceneGraph:root-node', usdRootNode ? 'found' : 'missing');
    if (!usdRootNode) {
        console.warn('No default root node found, falling back to flat mesh loading');
        await buildMeshesFallback();
        return;
    }

    // Create default material with environment map
    const defaultMtl = new THREE.MeshPhysicalMaterial({
        color: 0x888888,
        roughness: 0.5,
        metalness: 0.0,
        envMap: threeState.envMap,
        envMapIntensity: settings.envMapIntensity
    });

    // Build options for material conversion
    const options = {
        overrideMaterial: false,
        envMap: threeState.envMap,
        envMapIntensity: settings.envMapIntensity,
        preferredMaterialType: settings.materialType,
        textureCache: sceneState.textureCache,
        materialCache: new Map(),
        materialSignatureCache: settings.fastMaterials ? new Map() : null,
        materialSignatureTextureCache: settings.fastMaterials ? new Map() : null,
        fastMaterials: settings.fastMaterials,
        fastMaterialMode: settings.fastMaterialMode,
        textureLoadingManager: settings.deferTextures ? new TextureLoadingManager() : null,
        useMeshPtr: settings.useMeshPtr,
        meshAggregation: settings.meshAggregation,
        meshAggregationMinMeshes: settings.meshAggregationMinMeshes,
        computeMissingTangents: settings.computeMissingTangents,
        sourceFileName: loaderState.currentFileName,
        skipTextures: settings.skipTextures,
        yieldMode: settings.buildYieldMode,
        yieldIntervalMs: settings.buildYieldIntervalMs,
        debugLogEveryMeshes: 250,
        debugLog: TRACE_LOAD ? (info) => {
            traceLoadPhase(`loader:${info.stage}`, info.detail || '');
        } : null
    };

    // Build Three.js scene graph from USD hierarchy
    traceLoadPhase('buildSceneGraph:buildThreeNode:start');
    sceneState.nextNodeIndexMap = null;
    sceneState.root = await LightUSDLoaderUtils.buildThreeNode(
        usdRootNode,
        defaultMtl,
        loaderState.nativeLoader,
        options
    );
    traceLoadPhase('buildSceneGraph:buildThreeNode:done');
    if (options.textureLoadingManager) {
        startTrackedTextureLoading(options.textureLoadingManager, activeLoadStats, 'textureQueue');
    }

    // Add to scene
    threeState.scene.add(sceneState.root);

    // Collect materials and material data from the scene graph
    collectMaterialsFromScene();
    traceLoadPhase('buildSceneGraph:collectMaterials:done', `${sceneState.materials.length} materials`);

    // Store USD scene reference on meshes for material reloading
    sceneState.root.traverse((child) => {
        if (child.isMesh) {
            child.userData.usdScene = loaderState.nativeLoader;
        }
    });
    traceLoadPhase('buildSceneGraph:mesh-userdata:done');

    const numMeshes = loaderState.nativeLoader.numMeshes();
    const renderMeshes = countRenderMeshesFromScene();
    const numMaterials = sceneState.materials.length;
    const meshLabel = renderMeshes === numMeshes ?
        `${numMeshes} meshes` :
        `${renderMeshes} render meshes (${numMeshes} native)`;
    updateStatus(`Loaded: ${meshLabel}, ${numMaterials} materials ` +
        `(upAxis: ${sceneState.upAxis} · ${describeNativeOptimizations()})`);
}

/**
 * Collect materials from the scene graph after buildThreeNode
 * This extracts materials from meshes for UI and management
 */
function collectMaterialsFromScene() {
    const materialSet = new Set();

    sceneState.root.traverse((obj) => {
        if (obj.isMesh && obj.material) {
            if (Array.isArray(obj.material)) {
                obj.material.forEach(mat => materialSet.add(mat));
            } else {
                materialSet.add(obj.material);
            }
        }
    });

    sceneState.materials = Array.from(materialSet);

    // Extract material data from userData if available
    sceneState.materialData = sceneState.materials.map(mat => {
        const data = mat.userData?.rawData || null;
        if (data) {
            mat.userData.typeInfo = LightUSDLoaderUtils.getMaterialType(data);
            mat.userData.typeString = LightUSDLoaderUtils.getMaterialTypeString(data);
        }
        return data;
    });
}

function countRenderMeshesFromScene() {
    let count = 0;
    if (!sceneState.root) {
        return count;
    }
    sceneState.root.traverse((obj) => {
        if (obj.isMesh) {
            count++;
        }
    });
    return count;
}

/**
 * Fallback to flat mesh loading if getDefaultRootNode is not available
 */
async function buildMeshesFallback() {
    const numMeshes = loaderState.nativeLoader.numMeshes();
    const numMaterials = loaderState.nativeLoader.numMaterials();

    // Pre-load materials
    for (let i = 0; i < numMaterials; i++) {
        try {
            const result = loaderState.nativeLoader.getMaterialWithFormat(i, 'json');
            if (!result.error) {
                sceneState.materialData.push(JSON.parse(result.data));
            }
        } catch (e) {
            console.warn(`Failed to get material ${i}:`, e);
        }
    }

    for (let i = 0; i < sceneState.materialData.length; i++) {
        const mat = await convertMaterial(sceneState.materialData[i], i);
        sceneState.materials.push(mat);
    }

    sceneState.root = new THREE.Group();
    threeState.scene.add(sceneState.root);

    for (let i = 0; i < numMeshes; i++) {
        const meshData = loaderState.nativeLoader.getMeshCopy(i);
        if (!meshData) continue;

        const geometry = LightUSDLoaderUtils.convertUsdMeshToThreeMesh(meshData);
        if (!geometry) continue;

        const mesh = createMeshWithMaterialsFallback(geometry, meshData, i);
        mesh.name = meshData.name || `Mesh_${i}`;
        sceneState.root.add(mesh);
    }

    updateStatus(`Loaded: ${numMeshes} meshes, ${numMaterials} materials (upAxis: ${sceneState.upAxis}) [fallback mode]`);
}

/**
 * Create mesh with materials for fallback mode
 */
function createMeshWithMaterialsFallback(geometry, meshData, index) {
    const submeshes = geometry.userData['submeshes'];

    if (submeshes && submeshes.length > 0) {
        const materials = [];
        const materialIdToIndex = new Map();

        for (const submesh of submeshes) {
            const matId = submesh.materialId;
            if (!materialIdToIndex.has(matId)) {
                const material = (matId >= 0 && matId < sceneState.materials.length)
                    ? sceneState.materials[matId]
                    : new THREE.MeshPhysicalMaterial({ color: 0x888888 });
                materialIdToIndex.set(matId, materials.length);
                materials.push(material);
            }
        }

        for (const submesh of submeshes) {
            const matIndex = materialIdToIndex.get(submesh.materialId);
            geometry.addGroup(submesh.start, submesh.count, matIndex);
        }

        return new THREE.Mesh(geometry, materials);
    }

    const material = (meshData.materialId !== undefined && meshData.materialId >= 0 && meshData.materialId < sceneState.materials.length)
        ? sceneState.materials[meshData.materialId]
        : new THREE.MeshPhysicalMaterial({ color: 0x888888 });

    return new THREE.Mesh(geometry, material);
}

async function loadDomeLight() {
    if (!threeState.pmremGenerator) {
        traceLoadPhase('loadDomeLight:skipped-no-pmrem');
        return;
    }
    try {
        const domeLightData = await loadDomeLightFromUSD(loaderState.nativeLoader);
        if (domeLightData && guiState.envPresetController) {
            guiState.envPresetController.updateDisplay();
        }
    } catch (error) {
        console.warn('Error checking for DomeLight:', error);
    }
}

function loadAnimations() {
    try {
        const numAnimations = loaderState.nativeLoader.numAnimations();
        console.log(`USD file contains ${numAnimations} animations`);

        if (numAnimations === 0) {
            console.log('No animations in USD file');
            return;
        }

        animationState.clips = convertUSDAnimationsToThreeJS(loaderState.nativeLoader, sceneState.root);
        console.log(`Converted ${animationState.clips.length} animation clips`);

        if (animationState.clips.length > 0) {
            // Create mixer on the scene root
            animationState.mixer = new THREE.AnimationMixer(sceneState.root);
            console.log('Created AnimationMixer on sceneState.root:', sceneState.root.name, 'uuid:', sceneState.root.uuid);

            // Debug: List objects in the scene that could be animation targets
            console.log('=== Scene objects available for animation ===');
            sceneState.root.traverse((obj) => {
                console.log(`  "${obj.name}" (${obj.type}) uuid: ${obj.uuid.slice(0, 8)}`);
            });
            console.log('============================================');

            updateAnimationParams();
            prepareAnimationClips();  // Prepare but don't auto-play

            if (guiState.timeController) {
                guiState.timeController.max(animationState.params.endTime);
                guiState.timeController.updateDisplay();
            }

            if (guiState.animationFolder) {
                guiState.animationFolder.open();
            }

            const numMeshes = loaderState.nativeLoader.numMeshes();
            const numMaterials = loaderState.nativeLoader.numMaterials();
            updateStatus(`Loaded: ${numMeshes} meshes, ${numMaterials} materials, ${animationState.clips.length} animations (paused)`);
        }
    } catch (error) {
        console.error('Error loading animations:', error);
    }
}

function updateAnimationParams() {
    const metadata = sceneState.metadata;

    if (metadata.startTimeCode !== undefined && metadata.endTimeCode !== undefined) {
        animationState.params.beginTime = metadata.startTimeCode;
        animationState.params.endTime = metadata.endTimeCode;
    } else if (animationState.clips.length > 0) {
        animationState.params.beginTime = 0;
        animationState.params.endTime = animationState.clips[0].duration;
    }

    animationState.params.speed = metadata.framesPerSecond || metadata.timeCodesPerSecond || 24.0;
    animationState.params.time = metadata.startTimeCode !== undefined ? metadata.startTimeCode : 0;
}

/**
 * Prepare animation clips for playback (but don't auto-play)
 * Sets up all clips with proper loop settings and initial time
 */
function prepareAnimationClips() {
    const startTime = sceneState.metadata?.startTimeCode ?? 0;

    animationState.clips.forEach((clip, clipIndex) => {
        const action = animationState.mixer.clipAction(clip);
        action.loop = THREE.LoopRepeat;
        action.clampWhenFinished = false;
        action.enabled = true;
        action.setEffectiveWeight(1.0);
        action.time = startTime;
        action.paused = true;  // Start paused
        action.play();  // Register the action (but it's paused)

        console.log(`Prepared animation clip ${clipIndex}: "${clip.name}", ${clip.tracks.length} tracks, duration: ${clip.duration}s`);

        // Debug: log track targets
        clip.tracks.forEach(track => {
            console.log(`  Track: ${track.name}`);
        });
    });

    if (animationState.clips.length > 0) {
        animationState.action = animationState.mixer.clipAction(animationState.clips[0]);
    }

    // Force initial pose evaluation
    // This is critical - without this, the paused animation won't show initial state
    animationState.mixer.update(0);

    // Reset the timer so delta starts fresh when playing begins
    threeState.timer.reset();

    console.log(`Animation prepared: ${animationState.clips.length} clips, starting paused at time ${startTime}`);
}

/**
 * Toggle animation playback
 */
function toggleAnimationPlayback() {
    if (!animationState.mixer) {
        console.warn('No animation mixer - cannot toggle playback');
        return;
    }

    animationState.params.isPlaying = !animationState.params.isPlaying;
    console.log(`Animation playback: ${animationState.params.isPlaying ? 'PLAYING' : 'PAUSED'}`);

    animationState.clips.forEach((clip) => {
        const action = animationState.mixer.clipAction(clip);
        action.paused = !animationState.params.isPlaying;
        console.log(`  Action "${clip.name}": paused=${action.paused}, time=${action.time.toFixed(3)}`);
    });

    // Reset timer to avoid large jump when resuming
    if (animationState.params.isPlaying) {
        threeState.timer.reset();
    }
}

/**
 * Reset animation to beginning
 */
function resetAnimation() {
    if (!animationState.mixer) return;

    animationState.params.time = animationState.params.beginTime;

    animationState.clips.forEach((clip) => {
        const action = animationState.mixer.clipAction(clip);
        action.time = animationState.params.beginTime;
    });

    // Force update to show the reset position
    animationState.mixer.update(0.0001);

    // Reset to exact time after evaluation
    animationState.clips.forEach((clip) => {
        const action = animationState.mixer.clipAction(clip);
        action.time = animationState.params.beginTime;
    });
}

function readNumberFromMethod(context, method) {
    if (typeof method !== 'function') return null;
    try {
        const value = method.call(context);
        return Number.isFinite(value) ? value : null;
    } catch {
        return null;
    }
}

function readLegacySceneEntityCounts(usd) {
    if (!usd) return {};

    let unsupportedRenderables = readNumberFromMethod(usd, usd.numUnsupportedRenderables);
    if (unsupportedRenderables === null && typeof usd.getUnsupportedRenderables === 'function') {
        try {
            const list = usd.getUnsupportedRenderables();
            if (Array.isArray(list)) {
                unsupportedRenderables = list.length;
            }
        } catch {
            unsupportedRenderables = null;
        }
    }

    return {
        lights: readNumberFromMethod(usd, usd.numLights),
        cameras: readNumberFromMethod(usd, usd.numCameras),
        nodes: readNumberFromMethod(usd, usd.numNodes),
        pointInstancers: readNumberFromMethod(usd, usd.numPointInstancers),
        pointInstanceDraws: readNumberFromMethod(usd, usd.numPointInstanceDraws),
        skeletons: readNumberFromMethod(usd, usd.numSkeletons),
        animations: readNumberFromMethod(usd, usd.numAnimations),
        unsupportedRenderables: Number.isFinite(unsupportedRenderables) ? unsupportedRenderables : 0
    };
}

function setSceneEntityCountsFromScene(counts) {
    const safe = (value) => Number.isFinite(value) ? value : 0;
    document.getElementById('light-count').textContent = safe(counts?.lights);
    document.getElementById('camera-count').textContent = safe(counts?.cameras);
    document.getElementById('node-count').textContent = safe(counts?.nodes);
    document.getElementById('point-instancer-count').textContent = safe(counts?.pointInstancers);
    document.getElementById('point-instance-draw-count').textContent = safe(counts?.pointInstanceDraws);
    document.getElementById('animation-count').textContent = safe(counts?.animations);
    document.getElementById('skeleton-count').textContent = safe(counts?.skeletons);
    document.getElementById('unsupported-renderable-count').textContent = safe(counts?.unsupportedRenderables);
}

function updateModelInfo() {
    const numMeshes = loaderState.nativeLoader.numMeshes();
    const numMaterials = loaderState.nativeLoader.numMaterials();
    const renderMeshes = countRenderMeshesFromScene();

    document.getElementById('model-info').style.display = 'block';
    document.getElementById('current-file').textContent = loaderState.currentFileName || '-';
    document.getElementById('mesh-count').textContent = renderMeshes && renderMeshes !== numMeshes ?
        `${renderMeshes} (${numMeshes} native)` :
        numMeshes;
    document.getElementById('material-count').textContent = numMaterials;

    const entityCounts = readLegacySceneEntityCounts(loaderState.nativeLoader);
    setSceneEntityCountsFromScene(entityCounts);
}

async function convertMaterial(matData, index) {
    const previousMaterial = sceneState.materials[index] || null;
    const typeInfo = LightUSDLoaderUtils.getMaterialType(matData);
    const typeString = LightUSDLoaderUtils.getMaterialTypeString(matData);

    try {
        // Check if we should use OpenPBRMaterial
        const useOpenPBRMaterial = shouldUseOpenPBRMaterial(typeInfo, settings.materialImplementation);

        let material;

        if (useOpenPBRMaterial) {
            console.log("use OpenPBRMaterial");
            // Create OpenPBRMaterial directly (Loaded version waits for textures)
            material = await convertToOpenPBRMaterialLoaded(matData, loaderState.nativeLoader);
            material.envMap = threeState.envMap;
            material.envMapIntensity = settings.envMapIntensity;
        } else {
            // Use LightUSDLoaderUtils for MeshPhysicalMaterial
            material = await LightUSDLoaderUtils.convertMaterial(
                matData,
                loaderState.nativeLoader,
                {
                    preferredMaterialType: settings.materialType,
                    envMap: threeState.envMap,
                    envMapIntensity: settings.envMapIntensity,
                    textureCache: sceneState.textureCache
                }
            );

            // Load normal map from OpenPBR geometry.normal if not already loaded
            // LightUSDLoaderUtils expects normalTextureId at top level, but MaterialX has it nested
            if (!material.normalMap) {
                const openPBR = matData.openPBR || matData.openPBRShader || {};
                const geometrySection = openPBR.geometry || {};
                const normalParam = geometrySection.normal || openPBR.normal || openPBR.geometry_normal;
                const normalTextureId = extractTextureId(normalParam);

                if (normalTextureId >= 0 && loaderState.nativeLoader) {
                    try {
                        // Find embedded texture (bufferId >= 0) for the same filename
                        let actualTextureId = normalTextureId;
                        const tex = loaderState.nativeLoader.getTexture(normalTextureId);
                        const texImage = loaderState.nativeLoader.getImageCopy(tex.textureImageId);

                        if (texImage.bufferId === -1 && texImage.uri) {
                            const filename = texImage.uri.replace(/^\.\//, '');
                            const numImages = loaderState.nativeLoader.numImages();
                            for (let i = 0; i < numImages; i++) {
                                const altImage = loaderState.nativeLoader.getImageCopy(i);
                                if (altImage.bufferId >= 0 && altImage.uri === filename) {
                                    const numTextures = loaderState.nativeLoader.numTextures();
                                    for (let t = 0; t < numTextures; t++) {
                                        const altTex = loaderState.nativeLoader.getTexture(t);
                                        if (altTex.textureImageId === i) {
                                            actualTextureId = t;
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                        }

                        const texture = await LightUSDLoaderUtils.getTextureFromUSD(loaderState.nativeLoader, actualTextureId);
                        if (texture) {
                            material.normalMap = texture;
                            // Extract normal map scale from OpenPBR
                            const normalMapScale = geometrySection.normal_map_scale !== undefined
                                ? (typeof geometrySection.normal_map_scale === 'object' ? geometrySection.normal_map_scale.value : geometrySection.normal_map_scale)
                                : 1.0;
                            material.normalScale = new THREE.Vector2(normalMapScale, normalMapScale);
                            material.needsUpdate = true;
                        }
                    } catch (err) {
                        console.warn('Failed to load normal map texture for MeshPhysicalMaterial:', err);
                    }
                }
            }
        }

        material.userData.typeInfo = typeInfo;
        material.userData.typeString = typeString + (useOpenPBRMaterial ? ' [OpenPBRMaterial]' : ' [MeshPhysicalMaterial]');
        material.userData.rawData = matData;
        if (matData?.__nextMaterial && previousMaterial) {
            copyCoreMaterialTextures(previousMaterial, material);
        }
        return material;
    } catch (e) {
        console.warn(`Failed to convert material ${index}:`, e);
        return new THREE.MeshPhysicalMaterial({
            color: 0x888888,
            roughness: 0.5,
            metalness: 0.0
        });
    }
}

/**
 * Determine if we should use OpenPBRMaterial based on settings and material type
 */
function shouldUseOpenPBRMaterial(typeInfo, implementation) {
    if (implementation === 'physical') {
        return false;
    }
    if (implementation === 'openpbr') {
        return typeInfo.hasOpenPBR;
    }
    // 'auto' - use OpenPBRMaterial for OpenPBR materials
    return typeInfo.hasOpenPBR;
}

/**
 * Extract texture ID from a property value
 * Handles formats like "texture_id[N]" or objects with texture_id
 */
function extractTextureId(param) {
    if (param === undefined || param === null) return -1;

    // Handle string format "texture_id[N]"
    if (typeof param === 'string') {
        const match = param.match(/texture_id\[(\d+)\]/);
        if (match) return parseInt(match[1], 10);
    }

    // Handle object with texture_id property
    if (typeof param === 'object') {
        if (param.texture_id !== undefined) return param.texture_id;
        if (param.textureId !== undefined) return param.textureId;
    }

    return -1;
}

// ============================================================================
// OpenPBRMaterial Conversion Helpers
// ============================================================================

/**
 * Extract value from OpenPBR parameter
 */
function extractOpenPBRValue(param, defaultVal) {
    return materialScalar(param) ?? defaultVal;
}

/**
 * Extract color from OpenPBR parameter
 */
function extractOpenPBRColor(param, defaultVal) {
    if (!param) return new THREE.Color(...defaultVal);
    if (param.r !== undefined) return new THREE.Color(param.r, param.g, param.b);
    if (Array.isArray(param)) return new THREE.Color(...param);
    if (typeof param === 'object' && param.value) {
        if (Array.isArray(param.value)) return new THREE.Color(...param.value);
        if (param.value.r !== undefined) return new THREE.Color(param.value.r, param.value.g, param.value.b);
    }
    return new THREE.Color(...defaultVal);
}

/**
 * Check if OpenPBR parameter has a texture
 */
function hasOpenPBRTexture(param) {
    return param && typeof param === 'object' && param.textureId !== undefined && param.textureId >= 0;
}

/**
 * Get texture ID from OpenPBR parameter
 */
function getOpenPBRTextureId(param) {
    if (!param || typeof param !== 'object') return -1;
    return param.textureId !== undefined ? param.textureId : -1;
}

/**
 * Resolve actual texture ID (handles duplicate images in USDZ)
 */
function resolveTextureId(nativeLoader, textureId) {
    if (textureId < 0) return textureId;
    try {
        const tex = nativeLoader.getTexture(textureId);
        const texImage = nativeLoader.getImageCopy(tex.textureImageId);

        if (texImage.bufferId === -1 && texImage.uri) {
            const filename = texImage.uri.replace(/^\.\//, '');
            const numImages = nativeLoader.numImages();
            for (let i = 0; i < numImages; i++) {
                const altImage = nativeLoader.getImageCopy(i);
                if (altImage.bufferId >= 0 && altImage.uri === filename) {
                    const numTextures = nativeLoader.numTextures();
                    for (let t = 0; t < numTextures; t++) {
                        const altTex = nativeLoader.getTexture(t);
                        if (altTex.textureImageId === i) {
                            return t;
                        }
                    }
                    break;
                }
            }
        }
    } catch (e) {
        // Ignore errors, return original ID
    }
    return textureId;
}

/**
 * Create base OpenPBRMaterial with scalar/color values (no textures)
 */
function createBaseOpenPBRMaterial(openPBR) {
    const material = new OpenPBRMaterial({
        // Base layer
        base_weight: extractOpenPBRValue(openPBR.base_weight, 1.0),
        base_color: extractOpenPBRColor(openPBR.base_color, [0.8, 0.8, 0.8]),
        base_metalness: extractOpenPBRValue(openPBR.base_metalness, 0.0),
        base_diffuse_roughness: extractOpenPBRValue(openPBR.base_diffuse_roughness, 0.0),

        // Specular layer
        specular_weight: extractOpenPBRValue(openPBR.specular_weight, 1.0),
        specular_color: extractOpenPBRColor(openPBR.specular_color, [1.0, 1.0, 1.0]),
        specular_roughness: extractOpenPBRValue(openPBR.specular_roughness, 0.3),
        specular_ior: extractOpenPBRValue(openPBR.specular_ior, 1.5),
        specular_anisotropy: extractOpenPBRValue(openPBR.specular_anisotropy, 0.0),

        // Coat layer
        coat_weight: extractOpenPBRValue(openPBR.coat_weight, 0.0),
        coat_color: extractOpenPBRColor(openPBR.coat_color, [1.0, 1.0, 1.0]),
        coat_roughness: extractOpenPBRValue(openPBR.coat_roughness, 0.0),
        coat_ior: extractOpenPBRValue(openPBR.coat_ior, 1.5),

        // Fuzz layer
        fuzz_weight: extractOpenPBRValue(openPBR.fuzz_weight || openPBR.sheen_weight, 0.0),
        fuzz_color: extractOpenPBRColor(openPBR.fuzz_color || openPBR.sheen_color, [1.0, 1.0, 1.0]),
        fuzz_roughness: extractOpenPBRValue(openPBR.fuzz_roughness || openPBR.sheen_roughness, 0.5),

        // Thin film
        thin_film_weight: extractOpenPBRValue(openPBR.thin_film_weight, 0.0),
        thin_film_thickness: extractOpenPBRValue(openPBR.thin_film_thickness, 500.0),
        thin_film_ior: extractOpenPBRValue(openPBR.thin_film_ior, 1.5),

        // Emission
        emission_luminance: extractOpenPBRValue(openPBR.emission_luminance, 0.0),
        emission_color: extractOpenPBRColor(openPBR.emission_color, [1.0, 1.0, 1.0]),

        // Geometry
        geometry_opacity: extractOpenPBRValue(openPBR.geometry_opacity || openPBR.opacity, 1.0)
    });

    // Store texture references for later management
    material.userData.textures = {};
    material.userData.materialType = 'OpenPBRMaterial';

    return material;
}

/**
 * Convert material data to OpenPBRMaterial
 * Waits for all textures to load before returning.
 * @param {Object} matData - Material data from USD
 * @param {Object} nativeLoader - LightUSD native loader for texture access
 * @returns {Promise<OpenPBRMaterial>} The created material
 */
async function convertToOpenPBRMaterialLoaded(matData, nativeLoader = null) {
    const openPBR = matData.openPBR || matData.openPBRShader || matData;
    const material = createBaseOpenPBRMaterial(openPBR);
    const geometrySection = openPBR.geometry || {};

    if (!nativeLoader) return material;

    // Load base color texture (map)
    if (hasOpenPBRTexture(openPBR.base_color)) {
        try {
            const texId = resolveTextureId(nativeLoader, getOpenPBRTextureId(openPBR.base_color));
            const texture = await LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId);
            if (texture) {
                material.map = texture;
                material.userData.textures.map = { textureId: texId, texture };
            }
        } catch (err) {
            console.warn('Failed to load base color texture:', err);
        }
    }

    // Load roughness texture
    if (hasOpenPBRTexture(openPBR.specular_roughness)) {
        try {
            const texId = resolveTextureId(nativeLoader, getOpenPBRTextureId(openPBR.specular_roughness));
            const texture = await LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId);
            if (texture) {
                material.roughnessMap = texture;
                material.userData.textures.roughnessMap = { textureId: texId, texture };
            }
        } catch (err) {
            console.warn('Failed to load roughness texture:', err);
        }
    }

    // Load metalness texture
    if (hasOpenPBRTexture(openPBR.base_metalness)) {
        try {
            const texId = resolveTextureId(nativeLoader, getOpenPBRTextureId(openPBR.base_metalness));
            const texture = await LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId);
            if (texture) {
                material.metalnessMap = texture;
                material.userData.textures.metalnessMap = { textureId: texId, texture };
            }
        } catch (err) {
            console.warn('Failed to load metalness texture:', err);
        }
    }

    // Load emissive texture
    if (hasOpenPBRTexture(openPBR.emission_color)) {
        try {
            const texId = resolveTextureId(nativeLoader, getOpenPBRTextureId(openPBR.emission_color));
            const texture = await LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId);
            if (texture) {
                material.emissiveMap = texture;
                material.userData.textures.emissiveMap = { textureId: texId, texture };
            }
        } catch (err) {
            console.warn('Failed to load emissive texture:', err);
        }
    }

    // Load normal map
    const normalParam = geometrySection.normal || openPBR.normal || openPBR.geometry_normal;
    if (hasOpenPBRTexture(normalParam)) {
        try {
            const texId = resolveTextureId(nativeLoader, getOpenPBRTextureId(normalParam));
            const texture = await LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId);
            if (texture) {
                material.normalMap = texture;
                material.userData.textures.normalMap = { textureId: texId, texture };
                const normalMapScale = extractOpenPBRValue(geometrySection.normal_map_scale || openPBR.normal_map_scale, 1.0);
                if (material.uniforms.normalScale) {
                    material.uniforms.normalScale.value.set(normalMapScale, normalMapScale);
                }
            }
        } catch (err) {
            console.warn('Failed to load normal map texture:', err);
        }
    }

    // Load AO map (if available in geometry section)
    const aoParam = geometrySection.ambient_occlusion || openPBR.ambient_occlusion;
    if (hasOpenPBRTexture(aoParam)) {
        try {
            const texId = resolveTextureId(nativeLoader, getOpenPBRTextureId(aoParam));
            const texture = await LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId);
            if (texture) {
                material.aoMap = texture;
                material.userData.textures.aoMap = { textureId: texId, texture };
            }
        } catch (err) {
            console.warn('Failed to load AO texture:', err);
        }
    }

    // Store geometry info in userData
    material.userData.geometry_normal = {
        hasTexture: hasOpenPBRTexture(normalParam),
        textureId: getOpenPBRTextureId(normalParam),
        scale: extractOpenPBRValue(geometrySection.normal_map_scale || openPBR.normal_map_scale, 1.0)
    };


    return material;
}

/**
 * Convert material data to OpenPBRMaterial (legacy pattern)
 * Returns material immediately, textures load asynchronously in background.
 * @param {Object} matData - Material data from USD
 * @param {Object} nativeLoader - LightUSD native loader for texture access
 * @returns {OpenPBRMaterial} The created material (textures load asynchronously)
 */
function convertToOpenPBRMaterial(matData, nativeLoader = null) {
    const openPBR = matData.openPBR || matData.openPBRShader || matData;
    const material = createBaseOpenPBRMaterial(openPBR);
    const geometrySection = openPBR.geometry || {};

    if (!nativeLoader) return material;

    // Helper for fire-and-forget texture loading
    const loadTextureAsync = (param, mapName, onLoad = null) => {
        if (!hasOpenPBRTexture(param)) return;
        const texId = resolveTextureId(nativeLoader, getOpenPBRTextureId(param));
        LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId).then((texture) => {
            if (texture) {
                material[mapName] = texture;
                material.userData.textures[mapName] = { textureId: texId, texture };
                material.needsUpdate = true;
                if (onLoad) onLoad(texture);
            }
        }).catch((err) => {
            console.warn(`Failed to load ${mapName} texture:`, err);
        });
    };

    // Queue texture loading (fire-and-forget)
    loadTextureAsync(openPBR.base_color, 'map');
    loadTextureAsync(openPBR.specular_roughness, 'roughnessMap');
    loadTextureAsync(openPBR.base_metalness, 'metalnessMap');
    loadTextureAsync(openPBR.emission_color, 'emissiveMap');

    // Normal map with scale
    const normalParam = geometrySection.normal || openPBR.normal || openPBR.geometry_normal;
    loadTextureAsync(normalParam, 'normalMap', () => {
        const normalMapScale = extractOpenPBRValue(geometrySection.normal_map_scale || openPBR.normal_map_scale, 1.0);
        if (material.uniforms.normalScale) {
            material.uniforms.normalScale.value.set(normalMapScale, normalMapScale);
        }
    });

    // AO map
    const aoParam = geometrySection.ambient_occlusion || openPBR.ambient_occlusion;
    loadTextureAsync(aoParam, 'aoMap');

    // Store geometry info in userData
    material.userData.geometry_normal = {
        hasTexture: hasOpenPBRTexture(normalParam),
        textureId: getOpenPBRTextureId(normalParam),
        scale: extractOpenPBRValue(geometrySection.normal_map_scale || openPBR.normal_map_scale, 1.0)
    };

    return material;
}

function copyCoreMaterialTextures(previousMaterial, material) {
            material.userData.nextTexturePaths = {...previousMaterial.userData.nextTexturePaths};
            material.userData.nextTextureMetadata = {...previousMaterial.userData.nextTextureMetadata};
            // Reuse decoded core archive textures across implementation changes.
            // Material disposal does not own these shared texture objects.
            for (const key of ['map','normalMap','roughnessMap','metalnessMap','aoMap',
                'emissiveMap','alphaMap','bumpMap','displacementMap','clearcoatMap',
                'clearcoatRoughnessMap','clearcoatNormalMap','sheenColorMap',
                'sheenRoughnessMap','transmissionMap','thicknessMap','specularIntensityMap',
                'specularColorMap']) {
                if (previousMaterial[key]) material[key] = previousMaterial[key];
            }
            if (previousMaterial.normalScale && material.normalScale)
                material.normalScale.copy(previousMaterial.normalScale);
            material.side = previousMaterial.side;
            material.vertexColors = previousMaterial.vertexColors;
            material.alphaTest = previousMaterial.alphaTest;
            material.depthWrite = previousMaterial.depthWrite;
            if (material.alphaMap) material.transparent = true;
            material.needsUpdate = true;
    for (const [key, transform] of Object.entries(
        previousMaterial.userData.lightusdTextureColorTransforms || {})) {
        installTextureColorTransform(material, key, transform);
    }
}

async function reloadMaterials() {
    if (!sceneState.root) return;

    updateStatus('Reloading materials...');

    // Build a map from old material to new material for replacement
    const oldToNewMaterialMap = new Map();
    const newMaterialSet = new Set();

    // Traverse scene and reload each unique material
    for (const mat of sceneState.materials) {
        if (oldToNewMaterialMap.has(mat)) continue;

        // Get raw material data from userData (stored by buildThreeNode)
        const rawData = mat.userData?.rawData;
        if (rawData) {
            const newMat = await convertMaterial(rawData, sceneState.materials.indexOf(mat));
            oldToNewMaterialMap.set(mat, newMat);
            newMaterialSet.add(newMat);
        } else {
            // No raw data available, keep original material
            oldToNewMaterialMap.set(mat, mat);
            newMaterialSet.add(mat);
        }
    }

    // A texture may finish while an asynchronous material conversion yields.
    // Refresh completed maps, then redirect all still-pending bindings.
    for (const [oldMat, newMat] of oldToNewMaterialMap) {
        if (oldMat !== newMat && oldMat.userData?.rawData?.__nextMaterial)
            copyCoreMaterialTextures(oldMat, newMat);
    }
    sceneState.textureLoadingManager?.replaceMaterials(oldToNewMaterialMap);

    // Replace materials on meshes
    sceneState.root.traverse(obj => {
        if (obj.isMesh && obj.material) {
            if (Array.isArray(obj.material)) {
                obj.material = obj.material.map(mat => oldToNewMaterialMap.get(mat) || mat);
            } else {
                const newMat = oldToNewMaterialMap.get(obj.material);
                if (newMat && newMat !== obj.material) {
                    obj.material = newMat;
                }
            }
        }
    });

    // Dispose old materials that were replaced
    for (const [oldMat, newMat] of oldToNewMaterialMap) {
        if (oldMat !== newMat && oldMat.dispose) {
            oldMat.dispose();
        }
    }

    // Update state
    sceneState.materials = Array.from(newMaterialSet);
    sceneState.materialData = sceneState.materials.map(mat => mat.userData?.rawData || null);

    updateMaterialUI();
    updateStatus('Materials reloaded');
}

function clearScene() {
    if (sceneState.textureLoadingManager) {
        try {
            sceneState.textureLoadingManager.abort();
            if (typeof sceneState.textureLoadingManager.reset === 'function') {
                sceneState.textureLoadingManager.reset();
            }
        } catch (_) {
            // Ignore stale background texture queue cleanup errors.
        }
        sceneState.textureLoadingManager = null;
    }

    // Dispose normal visualization materials
    if (sceneState.showingNormals && sceneState.root) {
        sceneState.root.traverse(obj => {
            if (obj.isMesh && obj.material && obj.material.dispose) {
                obj.material.dispose();
            }
        });
    }
    sceneState.originalMaterialsMap.clear();
    sceneState.showingNormals = false;

    // Reset GUI checkbox
    if (settings.showNormals) {
        settings.showNormals = false;
        if (guiState.gui) {
            guiState.gui.controllersRecursive().forEach(controller => {
                if (controller.property === 'showNormals') {
                    controller.updateDisplay();
                }
            });
        }
    }

    // Clear scene objects
    if (sceneState.root) {
        sceneState.root.traverse(obj => {
            if (obj.isMesh) {
                obj.geometry?.dispose();
                if (obj.material) {
                    if (Array.isArray(obj.material)) {
                        obj.material.forEach(m => m.dispose());
                    } else {
                        obj.material.dispose();
                    }
                }
            }
        });
        threeState.scene.remove(sceneState.root);
        sceneState.root = null;
    }

    sceneState.materials = [];
    sceneState.materialData = [];
    sceneState.nextNodeIndexMap = null;

    // Dispose texture cache
    sceneState.textureCache.forEach(texture => {
        if (texture && texture.dispose) texture.dispose();
    });
    sceneState.textureCache.clear();

    // Clear animation state
    if (animationState.mixer) {
        animationState.mixer.stopAllAction();
        animationState.mixer = null;
    }
    animationState.clips = [];
    animationState.action = null;
    animationState.params.isPlaying = false;  // Reset to not playing
    animationState.params.time = 0;
    animationState.params.beginTime = 0;
    animationState.params.endTime = 10;
    threeState.timer.reset();

    // Clear pick state
    clearSelectionHighlight();
    pickState.selectedObject = null;

    // Reset scene state
    sceneState.upAxis = 'Y';
    sceneState.metadata = null;
    sceneState.domeLightData = null;

    // Clear WASM memory
    if (loaderState.nativeLoader) {
        try {
            loaderState.nativeLoader.reset();
        } catch (e) {
            try {
                loaderState.nativeLoader.clearAssets();
            } catch (e2) {
                // Ignore
            }
        }
        loaderState.nativeLoader = null;
    }
}

function fitCameraToScene() {
    if (!sceneState.root) return;

    sceneState.root.updateWorldMatrix(true, false);
    const cachedBox = sceneState.root.userData?.localBoundsBox;
    const box = cachedBox?.min && cachedBox?.max ?
        cachedBox.clone().applyMatrix4(sceneState.root.matrixWorld) :
        new THREE.Box3().setFromObject(sceneState.root);
    const size = box.getSize(new THREE.Vector3());
    const center = box.getCenter(new THREE.Vector3());

    const boundingSphereRadius = Math.max(size.length() * 0.5, 0.001);
    const fov = threeState.camera.fov * (Math.PI / 180);
    const aspectRatio = threeState.camera.aspect;

    const verticalFOV = fov;
    const horizontalFOV = 2 * Math.atan(Math.tan(verticalFOV / 2) * aspectRatio);
    const effectiveFOV = Math.min(verticalFOV, horizontalFOV);

    let cameraDistance = boundingSphereRadius / Math.sin(effectiveFOV / 2);
    cameraDistance *= CAMERA_PADDING;

    const cameraOffset = new THREE.Vector3(
        cameraDistance * 0.5,
        cameraDistance * 0.5,
        cameraDistance * 0.866
    );

    threeState.camera.position.copy(center.clone().add(cameraOffset));
    threeState.camera.lookAt(center);
    threeState.controls.target.copy(center);

    const minDistance = Math.max(boundingSphereRadius * 0.0005, 0.001);
    const maxDistance = Math.max(cameraDistance * 50, boundingSphereRadius * 100, 1000);

    threeState.camera.near = Math.max(boundingSphereRadius / 100000, 0.001);
    threeState.camera.far = Math.max(maxDistance * 2, cameraDistance * 10, 1000);
    threeState.camera.updateProjectionMatrix();

    threeState.controls.minDistance = minDistance;
    threeState.controls.maxDistance = maxDistance;
    threeState.controls.panSpeed = Math.max(boundingSphereRadius / 200, 1.0);
    threeState.controls.keyPanSpeed = Math.max(boundingSphereRadius / 50, 20.0);
    threeState.controls.zoomSpeed = Math.max(0.8, Math.min(1.5, boundingSphereRadius / 500));
    threeState.controls.update();
}

// ============================================================================
// Material UI
// ============================================================================

function updateMaterialUI() {
    // Clear existing controllers and folders
    while (guiState.materialFolder.controllers.length > 0) {
        guiState.materialFolder.controllers[0].destroy();
    }
    while (guiState.materialFolder.folders.length > 0) {
        guiState.materialFolder.folders[0].destroy();
    }
    while (guiState.textureFolder.controllers.length > 0) {
        guiState.textureFolder.controllers[0].destroy();
    }
    while (guiState.textureFolder.folders.length > 0) {
        guiState.textureFolder.folders[0].destroy();
    }

    // Determine which materials to show
    let materialsToShow = [];
    let headerText = '';
    const meshCount = countMeshes();

    if (pickState.selectedObject) {
        // Show only materials from selected object
        materialsToShow = getMaterialsFromObject(pickState.selectedObject);
        const objName = pickState.selectedObject.name || 'Unnamed';
        headerText = `Selected: ${objName}`;
    } else if (meshCount === 1) {
        // Single mesh in scene - auto-select its materials
        sceneState.root?.traverse(obj => {
            if (obj.isMesh && materialsToShow.length === 0) {
                materialsToShow = getMaterialsFromObject(obj);
            }
        });
        headerText = 'Single Mesh';
    } else {
        // Multiple objects - show nothing initially, require selection
        materialsToShow = [];
        headerText = 'Click object to select';
    }

    // Add header info
    if (headerText) {
        guiState.materialFolder.add({ info: headerText }, 'info').name('Showing').disable();
    }

    // Add material controls
    materialsToShow.forEach((mat, index) => {
        const globalIndex = sceneState.materials.indexOf(mat);
        const matName = globalIndex >= 0 ? `Material ${globalIndex}` : `Material ${index}`;
        const matFolder = guiState.materialFolder.addFolder(matName);
        addMaterialControls(matFolder, mat);
        addTextureUI(mat, globalIndex >= 0 ? globalIndex : index);
    });

    // If no materials, show message
    if (materialsToShow.length === 0) {
        guiState.materialFolder.add({ info: 'No materials' }, 'info').name('Status').disable();
    }

    // Add normal vector visualization controls (only when an object is selected)
    if (pickState.selectedObject && pickState.selectedObject.isMesh) {
        const normalVectorFolder = guiState.materialFolder.addFolder('Normal Vectors');

        normalVectorFolder.add(normalVectorState, 'enabled')
            .name('Show Vectors')
            .onChange(updateNormalVectorVisualization);

        normalVectorFolder.add(normalVectorState, 'type', ['vertex', 'face'])
            .name('Type')
            .onChange(updateNormalVectorVisualization);

        normalVectorFolder.add(normalVectorState, 'length', 0.01, 1.0, 0.01)
            .name('Length')
            .onChange(updateNormalVectorVisualization);

        normalVectorFolder.open();
    }
}

function addMaterialControls(folder, mat) {
    // Get material name from raw data
    const rawData = mat.userData?.rawData;
    const materialName = rawData?.name || rawData?.materialName || mat.name || 'Unnamed';
    folder.add({ name: materialName }, 'name').name('Name').disable();

    const typeString = mat.userData.typeString || 'Unknown';
    folder.add({ type: typeString }, 'type').name('Type').disable();

    // Double-sided toggle
    if (mat.side !== undefined) {
        const sideControl = { doubleSided: mat.side === THREE.DoubleSide };
        folder.add(sideControl, 'doubleSided').name('Double Sided').onChange(v => {
            mat.side = v ? THREE.DoubleSide : THREE.FrontSide;
            mat.needsUpdate = true;
        });
    }

    if (mat.color) {
        const displayColor = linearToSRGB(mat.color.clone());
        const colorObj = { color: '#' + displayColor.getHexString() };
        folder.addColor(colorObj, 'color').name('Base Color (sRGB)').onChange(v => {
            mat.color.copy(sRGBToLinear(new THREE.Color(v)));
        });
    }

    // --- Base Layer ---
    if (mat.metalness !== undefined) {
        folder.add(mat, 'metalness', 0, 1, 0.01).name('Metalness');
    }

    addDiffuseRoughnessControl(folder, mat);

    // --- Specular Layer ---
    if (mat.roughness !== undefined) {
        folder.add(mat, 'roughness', 0, 1, 0.01).name('Specular Roughness');
    }

    if (mat.ior !== undefined) {
        folder.add(mat, 'ior', 1, 3, 0.01).name('Specular IOR');
    }

    addSpecularWeightControl(folder, mat);
    addSpecularColorControl(folder, mat);

    // --- Coat Layer ---
    if (mat.clearcoat !== undefined) {
        folder.add(mat, 'clearcoat', 0, 1, 0.01).name('Coat Weight');
    }

    if (mat.clearcoatRoughness !== undefined) {
        folder.add(mat, 'clearcoatRoughness', 0, 1, 0.01).name('Coat Roughness');
    }

    addCoatIORControl(folder, mat);
    addCoatColorControl(folder, mat);

    // --- Fuzz/Sheen Layer ---
    if (mat.sheen !== undefined) {
        folder.add(mat, 'sheen', 0, 1, 0.01).name('Fuzz Weight');
    }

    if (mat.sheenRoughness !== undefined) {
        folder.add(mat, 'sheenRoughness', 0, 1, 0.01).name('Fuzz Roughness');
    }

    addFuzzColorControl(folder, mat);

    // --- Other Layers ---
    if (mat.transmission !== undefined) {
        folder.add(mat, 'transmission', 0, 1, 0.01).name('Transmission');
    }

    if (mat.emissive) {
        const displayEmissive = linearToSRGB(mat.emissive.clone());
        const emissiveObj = { emissive: '#' + displayEmissive.getHexString() };
        folder.addColor(emissiveObj, 'emissive').name('Emissive (sRGB)').onChange(v => {
            mat.emissive.copy(sRGBToLinear(new THREE.Color(v)));
        });
    }

    if (mat.emissiveIntensity !== undefined) {
        folder.add(mat, 'emissiveIntensity', 0, 10, 0.1).name('Emissive Intensity');
    }

    // --- Geometry Layer (Normal Map) ---
    addGeometryNormalControl(folder, mat);
}

// Core material parameters retain value/texture metadata together. Inspector
// controls read the value while edits preserve the remaining parameter fields.
function materialParamValue(value) {
    return value && typeof value === 'object' && !Array.isArray(value) ? value.value : value;
}
function materialScalar(value) {
    value = materialParamValue(value);
    if (Array.isArray(value)) value = value[0];
    return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}
function materialColor(value) {
    value = materialParamValue(value);
    return Array.isArray(value) && value.length >= 3 && value.slice(0,3).every(Number.isFinite)
        ? value.slice(0,3) : undefined;
}
function setMaterialParam(container,key,value) {
    if (!container) return;
    const parameter = container[key];
    if (parameter && typeof parameter === 'object' && !Array.isArray(parameter) && 'value' in parameter) {
        const values = Array.isArray(value) ? value : [value];
        parameter.value = Array.isArray(parameter.value)
            ? [...values,...parameter.value.slice(values.length)] : value;
    } else container[key] = value;
}

function addDiffuseRoughnessControl(folder, mat) {
    let diffuseRoughnessValue;
    const rawData = mat.userData.rawData;

    if (rawData?.base_diffuse_roughness !== undefined) {
        diffuseRoughnessValue = rawData.base_diffuse_roughness;
    } else if (rawData?.openPBR?.base_diffuse_roughness !== undefined) {
        diffuseRoughnessValue = rawData.openPBR.base_diffuse_roughness;
    } else if (rawData?.openPBRShader?.base_diffuse_roughness !== undefined) {
        diffuseRoughnessValue = rawData.openPBRShader.base_diffuse_roughness;
    }

    diffuseRoughnessValue = materialScalar(diffuseRoughnessValue);

    if (diffuseRoughnessValue === undefined) return;

    if (!mat.userData.customParams) {
        mat.userData.customParams = {};
    }
    mat.userData.customParams.baseDiffuseRoughness = diffuseRoughnessValue;

    folder.add(mat.userData.customParams, 'baseDiffuseRoughness', 0, 1, 0.01)
        .name('Diffuse Roughness')
        .onChange(v => {
            if (rawData?.base_diffuse_roughness !== undefined) {
                setMaterialParam(rawData, 'base_diffuse_roughness', v);
            }
            if (rawData?.openPBR?.base_diffuse_roughness !== undefined) {
                setMaterialParam(rawData.openPBR, 'base_diffuse_roughness', v);
            }
            if (rawData?.openPBRShader?.base_diffuse_roughness !== undefined) {
                setMaterialParam(rawData.openPBRShader, 'base_diffuse_roughness', v);
            }
            if ('baseDiffuseRoughness' in mat) mat.baseDiffuseRoughness = v;
            mat.needsUpdate = true;
        });
}

function addSpecularWeightControl(folder, mat) {
    let specularWeightValue;
    const rawData = mat.userData?.rawData;

    // Next-backend OpenPBR params are objects ({value, texture, ...}) or
    // vectors; unwrap to the scalar lil-gui needs.
    const unwrapScalar = (v) => {
        if (v && typeof v === 'object' && !Array.isArray(v)) v = v.value;
        if (Array.isArray(v)) v = v[0];
        return typeof v === 'number' && Number.isFinite(v) ? v : undefined;
    };

    if (unwrapScalar(rawData?.specular_weight) !== undefined) {
        specularWeightValue = unwrapScalar(rawData.specular_weight);
    } else if (unwrapScalar(rawData?.openPBR?.specular_weight) !== undefined) {
        specularWeightValue = unwrapScalar(rawData.openPBR.specular_weight);
    } else if (unwrapScalar(rawData?.openPBRShader?.specular_weight) !== undefined) {
        specularWeightValue = unwrapScalar(rawData.openPBRShader.specular_weight);
    }

    // Default to 1.0 if OpenPBR material but no specular_weight specified
    if (specularWeightValue === undefined) {
        if (rawData?.openPBR || rawData?.openPBRShader) {
            specularWeightValue = 1.0;
        } else {
            return; // Not an OpenPBR material
        }
    }

    if (!mat.userData.customParams) {
        mat.userData.customParams = {};
    }
    mat.userData.customParams.specularWeight = specularWeightValue;

    folder.add(mat.userData.customParams, 'specularWeight', 0, 1, 0.01)
        .name('Specular Weight')
        .onChange(v => {
            if (rawData?.specular_weight !== undefined) {
                setMaterialParam(rawData, 'specular_weight', v);
            }
            if (rawData?.openPBR) {
                setMaterialParam(rawData.openPBR, 'specular_weight', v);
            }
            if (rawData?.openPBRShader) {
                setMaterialParam(rawData.openPBRShader, 'specular_weight', v);
            }
            if ('specularWeight' in mat) mat.specularWeight = v;
            // Update Three.js specularIntensity if available
            if (mat.specularIntensity !== undefined) {
                mat.specularIntensity = v;
            }
            mat.needsUpdate = true;
        });
}

function addSpecularColorControl(folder, mat) {
    let specularColorValue;
    const rawData = mat.userData?.rawData;

    if (rawData?.specular_color !== undefined) {
        specularColorValue = rawData.specular_color;
    } else if (rawData?.openPBR?.specular_color !== undefined) {
        specularColorValue = rawData.openPBR.specular_color;
    } else if (rawData?.openPBRShader?.specular_color !== undefined) {
        specularColorValue = rawData.openPBRShader.specular_color;
    }

    specularColorValue = materialColor(specularColorValue);

    // Default to white if OpenPBR material but no specular_color specified
    if (specularColorValue === undefined) {
        if (rawData?.openPBR || rawData?.openPBRShader) {
            specularColorValue = [1.0, 1.0, 1.0];
        } else {
            return; // Not an OpenPBR material
        }
    }

    if (!mat.userData.customParams) {
        mat.userData.customParams = {};
    }

    // Convert to THREE.Color for display
    const color = Array.isArray(specularColorValue)
        ? new THREE.Color(specularColorValue[0], specularColorValue[1], specularColorValue[2])
        : new THREE.Color(specularColorValue);

    const displayColor = linearToSRGB(color.clone());
    mat.userData.customParams.specularColorHex = '#' + displayColor.getHexString();

    folder.addColor(mat.userData.customParams, 'specularColorHex')
        .name('Specular Color')
        .onChange(v => {
            const newColor = sRGBToLinear(new THREE.Color(v));
            const colorArray = [newColor.r, newColor.g, newColor.b];

            if (rawData?.specular_color !== undefined) {
                setMaterialParam(rawData, 'specular_color', colorArray);
            }
            if (rawData?.openPBR) {
                setMaterialParam(rawData.openPBR, 'specular_color', colorArray);
            }
            if (rawData?.openPBRShader) {
                setMaterialParam(rawData.openPBRShader, 'specular_color', colorArray);
            }
            // Update Three.js specularColor if available
            if (mat.specularColor) {
                mat.specularColor.copy(newColor);
            }
            mat.needsUpdate = true;
        });
}

function addCoatIORControl(folder, mat) {
    let coatIORValue;
    const rawData = mat.userData?.rawData;

    if (rawData?.coat_ior !== undefined) {
        coatIORValue = rawData.coat_ior;
    } else if (rawData?.openPBR?.coat_ior !== undefined) {
        coatIORValue = rawData.openPBR.coat_ior;
    } else if (rawData?.openPBRShader?.coat_ior !== undefined) {
        coatIORValue = rawData.openPBRShader.coat_ior;
    }

    coatIORValue = materialScalar(coatIORValue);

    // Default to 1.5 if OpenPBR material (show regardless of coat weight)
    if (coatIORValue === undefined) {
        if (rawData?.openPBR || rawData?.openPBRShader) {
            coatIORValue = 1.5;
        } else {
            return; // Not an OpenPBR material
        }
    }

    if (!mat.userData.customParams) {
        mat.userData.customParams = {};
    }
    mat.userData.customParams.coatIOR = coatIORValue;

    folder.add(mat.userData.customParams, 'coatIOR', 1, 3, 0.01)
        .name('Coat IOR')
        .onChange(v => {
            if (rawData?.coat_ior !== undefined) {
                setMaterialParam(rawData, 'coat_ior', v);
            }
            if (rawData?.openPBR) {
                setMaterialParam(rawData.openPBR, 'coat_ior', v);
            }
            if (rawData?.openPBRShader) {
                setMaterialParam(rawData.openPBRShader, 'coat_ior', v);
            }
            if ('coatIor' in mat) mat.coatIor = v;
            mat.needsUpdate = true;
        });
}

function addCoatColorControl(folder, mat) {
    let coatColorValue;
    const rawData = mat.userData?.rawData;

    if (rawData?.coat_color !== undefined) {
        coatColorValue = rawData.coat_color;
    } else if (rawData?.openPBR?.coat_color !== undefined) {
        coatColorValue = rawData.openPBR.coat_color;
    } else if (rawData?.openPBRShader?.coat_color !== undefined) {
        coatColorValue = rawData.openPBRShader.coat_color;
    }

    coatColorValue = materialColor(coatColorValue);

    // Default to white if OpenPBR material (show regardless of coat weight)
    if (coatColorValue === undefined) {
        if (rawData?.openPBR || rawData?.openPBRShader) {
            coatColorValue = [1.0, 1.0, 1.0];
        } else {
            return; // Not an OpenPBR material
        }
    }

    if (!mat.userData.customParams) {
        mat.userData.customParams = {};
    }

    const color = Array.isArray(coatColorValue)
        ? new THREE.Color(coatColorValue[0], coatColorValue[1], coatColorValue[2])
        : new THREE.Color(coatColorValue);

    const displayColor = linearToSRGB(color.clone());
    mat.userData.customParams.coatColorHex = '#' + displayColor.getHexString();

    folder.addColor(mat.userData.customParams, 'coatColorHex')
        .name('Coat Color')
        .onChange(v => {
            const newColor = sRGBToLinear(new THREE.Color(v));
            const colorArray = [newColor.r, newColor.g, newColor.b];

            if (rawData?.coat_color !== undefined) {
                setMaterialParam(rawData, 'coat_color', colorArray);
            }
            if (rawData?.openPBR) {
                setMaterialParam(rawData.openPBR, 'coat_color', colorArray);
            }
            if (rawData?.openPBRShader) {
                setMaterialParam(rawData.openPBRShader, 'coat_color', colorArray);
            }
            if (mat.coatColor) mat.coatColor.copy(newColor);
            mat.needsUpdate = true;
        });
}

function addFuzzColorControl(folder, mat) {
    let fuzzColorValue;
    const rawData = mat.userData?.rawData;

    if (rawData?.fuzz_color !== undefined) {
        fuzzColorValue = rawData.fuzz_color;
    } else if (rawData?.openPBR?.fuzz_color !== undefined) {
        fuzzColorValue = rawData.openPBR.fuzz_color;
    } else if (rawData?.openPBRShader?.fuzz_color !== undefined) {
        fuzzColorValue = rawData.openPBRShader.fuzz_color;
    }

    // Use Three.js sheenColor if available and no OpenPBR value
    if (fuzzColorValue === undefined && mat.sheenColor) {
        fuzzColorValue = [mat.sheenColor.r, mat.sheenColor.g, mat.sheenColor.b];
    }

    fuzzColorValue = materialColor(fuzzColorValue);

    // Default to white if has sheen/fuzz but no color specified
    if (fuzzColorValue === undefined) {
        const hasFuzz = mat.sheen !== undefined && mat.sheen > 0;
        if (hasFuzz || rawData?.openPBR || rawData?.openPBRShader) {
            fuzzColorValue = [1.0, 1.0, 1.0];
        } else {
            return; // Not applicable
        }
    }

    if (!mat.userData.customParams) {
        mat.userData.customParams = {};
    }

    const color = Array.isArray(fuzzColorValue)
        ? new THREE.Color(fuzzColorValue[0], fuzzColorValue[1], fuzzColorValue[2])
        : new THREE.Color(fuzzColorValue);

    const displayColor = linearToSRGB(color.clone());
    mat.userData.customParams.fuzzColorHex = '#' + displayColor.getHexString();

    folder.addColor(mat.userData.customParams, 'fuzzColorHex')
        .name('Fuzz Color')
        .onChange(v => {
            const newColor = sRGBToLinear(new THREE.Color(v));
            const colorArray = [newColor.r, newColor.g, newColor.b];

            if (rawData?.fuzz_color !== undefined) {
                setMaterialParam(rawData, 'fuzz_color', colorArray);
            }
            if (rawData?.openPBR) {
                setMaterialParam(rawData.openPBR, 'fuzz_color', colorArray);
            }
            if (rawData?.openPBRShader) {
                setMaterialParam(rawData.openPBRShader, 'fuzz_color', colorArray);
            }
            // Update Three.js sheenColor if available
            if (mat.sheenColor) {
                mat.sheenColor.copy(newColor);
            }
            mat.needsUpdate = true;
        });
}

/**
 * Add geometry_normal (normal map) control to the material UI
 * Shows normal map status, texture info, and scale control
 */
function addGeometryNormalControl(folder, mat) {
    const rawData = mat.userData?.rawData;
    const openPBR = rawData?.openPBR || rawData?.openPBRShader;
    const geometryNormal = mat.userData?.geometry_normal;

    // Check if normal map is available
    const hasNormalMap = mat.normalMap !== null && mat.normalMap !== undefined;

    // Get normal map scale
    let normalScale = 1.0;
    if (openPBR?.normal_map_scale !== undefined) {
        normalScale = materialScalar(openPBR.normal_map_scale) ?? 1;
    } else if (mat.uniforms?.normalScale?.value) {
        normalScale = mat.uniforms.normalScale.value.x;
    } else if (mat.normalScale) {
        normalScale = mat.normalScale.x;
    }

    // Only show control if this is an OpenPBR material or has normal map
    if (!openPBR && !hasNormalMap) return;

    if (!mat.userData.customParams) {
        mat.userData.customParams = {};
    }

    // Create a subfolder for geometry/normal map settings
    const geoFolder = folder.addFolder('Geometry Normal');

    // Show normal map status
    const normalStatus = hasNormalMap ? 'Enabled' : 'None';
    geoFolder.add({ status: normalStatus }, 'status').name('Normal Map').disable();

    // Show texture ID if available
    if (geometryNormal?.textureId >= 0) {
        geoFolder.add({ textureId: `texture_id[${geometryNormal.textureId}]` }, 'textureId')
            .name('Texture ID').disable();
    }

    // Normal map scale control (only if normal map exists)
    if (hasNormalMap) {
        mat.userData.customParams.normalScale = normalScale;

        geoFolder.add(mat.userData.customParams, 'normalScale', 0, 3, 0.01)
            .name('Normal Scale')
            .onChange(v => {
                // Update material's normalScale uniform
                if (mat.uniforms?.normalScale?.value) {
                    mat.uniforms.normalScale.value.set(v, v);
                } else if (mat.normalScale) {
                    mat.normalScale.set(v, v);
                }

                // Update raw data
                if (openPBR) {
                    openPBR.normal_map_scale = v;
                }

                mat.needsUpdate = true;
            });

        // Toggle normal map
        mat.userData.customParams.normalMapEnabled = true;
        const cachedNormalMap = mat.normalMap;

        geoFolder.add(mat.userData.customParams, 'normalMapEnabled')
            .name('Enabled')
            .onChange(v => {
                mat.normalMap = v ? cachedNormalMap : null;
                mat.needsUpdate = true;
            });

        // Preview button
        geoFolder.add({
            preview: () => previewTexture(mat.normalMap, 'Normal Map')
        }, 'preview').name('Preview');
    }

    geoFolder.close();
}

function addTextureUI(material, index) {
    const texFolder = guiState.textureFolder.addFolder(`Material ${index} Textures`);
    let hasTextures = false;

    TEXTURE_MAPS.forEach(({ prop, name }) => {
        const tex = material[prop];
        if (tex) {
            hasTextures = true;
            const texInfo = {
                enabled: true,
                preview: () => previewTexture(tex, name)
            };
            const folder = texFolder.addFolder(name);
            folder.add(texInfo, 'enabled').name('Enabled').onChange(v => {
                material[prop] = v ? tex : null;
                material.needsUpdate = true;
            });
            folder.add(texInfo, 'preview').name('Preview');
            folder.close();
        }
    });

    if (!hasTextures) {
        texFolder.add({ info: 'No textures' }, 'info').name('Status').disable();
    }
}

function previewTexture(texture, name) {
    const existing = document.getElementById('texture-preview-modal');
    if (existing) existing.remove();

    const modal = document.createElement('div');
    modal.id = 'texture-preview-modal';
    modal.style.cssText = `
        position: fixed; top: 0; left: 0; width: 100%; height: 100%;
        background: rgba(0, 0, 0, 0.8); display: flex;
        justify-content: center; align-items: center; z-index: 10000; cursor: pointer;
    `;
    modal.onclick = () => modal.remove();

    const container = document.createElement('div');
    container.style.cssText = `
        background: #2a2a2a; padding: 20px; border-radius: 10px;
        max-width: 80%; max-height: 80%;
    `;

    const title = document.createElement('h3');
    title.textContent = name;
    title.style.cssText = 'color: white; margin: 0 0 10px 0;';
    container.appendChild(title);

    const canvas = document.createElement('canvas');
    canvas.width = 512;
    canvas.height = 512;
    canvas.style.cssText = 'max-width: 100%; border-radius: 5px;';

    const tempScene = new THREE.Scene();
    const tempCamera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
    const geometry = new THREE.PlaneGeometry(2, 2);
    const material = new THREE.MeshBasicMaterial({ map: texture });
    tempScene.add(new THREE.Mesh(geometry, material));

    const tempRenderer = new THREE.WebGLRenderer({ canvas });
    tempRenderer.setSize(512, 512);
    tempRenderer.render(tempScene, tempCamera);
    tempRenderer.dispose();
    geometry.dispose();
    material.dispose();

    container.appendChild(canvas);
    modal.appendChild(container);
    document.body.appendChild(modal);
}

// ============================================================================
// Object Picking
// ============================================================================

/**
 * Handle canvas click for object picking
 */
function onCanvasClick(event) {
    // Ignore if clicking on GUI
    if (event.target !== threeState.renderer.domElement) return;

    // Calculate normalized device coordinates
    const rect = threeState.renderer.domElement.getBoundingClientRect();
    pickState.mouse.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
    pickState.mouse.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;

    // Perform raycasting
    pickState.raycaster.setFromCamera(pickState.mouse, threeState.camera);

    if (!sceneState.root) return;

    const intersects = pickState.raycaster.intersectObjects(sceneState.root.children, true);

    if (intersects.length > 0) {
        // Find the first mesh
        const hit = intersects.find(i => i.object.isMesh);
        if (hit) {
            selectObject(hit.object);
        } else {
            // Clicked on non-mesh object, clear selection
            clearSelection();
        }
    } else {
        // Clicked on background, clear selection
        clearSelection();
    }
}

/**
 * Select an object and update UI
 */
function selectObject(object) {
    // Clear previous selection
    clearSelectionHighlight();

    pickState.selectedObject = object;

    // Create selection highlight (wireframe box)
    const box = new THREE.Box3().setFromObject(object);
    const helper = new THREE.Box3Helper(box, 0x00ff00);
    helper.name = '__selectionHelper__';
    threeState.scene.add(helper);
    pickState.selectionHelper = helper;

    // Update material UI for selected object
    updateMaterialUI();

    // Update status
    const objName = object.name || 'Unnamed';
    const absPath = object.userData['primMeta.absPath'] || '';
    updateStatus(`Selected: ${objName}${absPath ? ' (' + absPath + ')' : ''}`);
}

/**
 * Clear normal vector helper
 */
function clearNormalVectorHelper() {
    if (normalVectorState.helper) {
        threeState.scene.remove(normalVectorState.helper);
        if (normalVectorState.helper.geometry) {
            normalVectorState.helper.geometry.dispose();
        }
        if (normalVectorState.helper.material) {
            normalVectorState.helper.material.dispose();
        }
        normalVectorState.helper = null;
    }
}

/**
 * Create vertex normal vectors for a mesh
 */
function createVertexNormalHelper(mesh, length) {
    const geometry = mesh.geometry;
    if (!geometry) return null;

    const posAttr = geometry.attributes.position;
    const normalAttr = geometry.attributes.normal;
    if (!posAttr || !normalAttr) return null;

    const positions = [];
    const colors = [];
    const tempPos = new THREE.Vector3();
    const tempNormal = new THREE.Vector3();

    // Get world matrix
    mesh.updateMatrixWorld(true);
    const normalMatrix = new THREE.Matrix3().getNormalMatrix(mesh.matrixWorld);

    for (let i = 0; i < posAttr.count; i++) {
        tempPos.fromBufferAttribute(posAttr, i);
        tempNormal.fromBufferAttribute(normalAttr, i);

        // Transform to world space
        tempPos.applyMatrix4(mesh.matrixWorld);
        tempNormal.applyMatrix3(normalMatrix).normalize();

        // Start point
        positions.push(tempPos.x, tempPos.y, tempPos.z);
        // End point
        positions.push(
            tempPos.x + tempNormal.x * length,
            tempPos.y + tempNormal.y * length,
            tempPos.z + tempNormal.z * length
        );

        // Color based on normal direction (start: cyan, end: yellow)
        colors.push(0, 1, 1); // Cyan at base
        colors.push(1, 1, 0); // Yellow at tip
    }

    const lineGeom = new THREE.BufferGeometry();
    lineGeom.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    lineGeom.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3));

    const lineMat = new THREE.LineBasicMaterial({
        vertexColors: true,
        depthTest: true,
        depthWrite: false,
        transparent: true,
        opacity: 0.8
    });

    return new THREE.LineSegments(lineGeom, lineMat);
}

/**
 * Create face normal vectors for a mesh
 */
function createFaceNormalHelper(mesh, length) {
    const geometry = mesh.geometry;
    if (!geometry) return null;

    const posAttr = geometry.attributes.position;
    const normalAttr = geometry.attributes.normal;
    const indexAttr = geometry.index;
    if (!posAttr || !normalAttr) return null;

    const positions = [];
    const colors = [];
    const tempPos = new THREE.Vector3();
    const tempNormal = new THREE.Vector3();
    const v0 = new THREE.Vector3();
    const v1 = new THREE.Vector3();
    const v2 = new THREE.Vector3();
    const n0 = new THREE.Vector3();
    const n1 = new THREE.Vector3();
    const n2 = new THREE.Vector3();
    const center = new THREE.Vector3();
    const avgNormal = new THREE.Vector3();

    // Get world matrix
    mesh.updateMatrixWorld(true);
    const normalMatrix = new THREE.Matrix3().getNormalMatrix(mesh.matrixWorld);

    const processedFaces = new Set();

    const addFace = (i0, i1, i2) => {
        const faceKey = [i0, i1, i2].sort().join(',');
        if (processedFaces.has(faceKey)) return;
        processedFaces.add(faceKey);

        v0.fromBufferAttribute(posAttr, i0);
        v1.fromBufferAttribute(posAttr, i1);
        v2.fromBufferAttribute(posAttr, i2);

        n0.fromBufferAttribute(normalAttr, i0);
        n1.fromBufferAttribute(normalAttr, i1);
        n2.fromBufferAttribute(normalAttr, i2);

        // Calculate face center
        center.copy(v0).add(v1).add(v2).divideScalar(3);

        // Average face normal
        avgNormal.copy(n0).add(n1).add(n2).normalize();

        // Transform to world space
        center.applyMatrix4(mesh.matrixWorld);
        avgNormal.applyMatrix3(normalMatrix).normalize();

        // Start point (center of face)
        positions.push(center.x, center.y, center.z);
        // End point
        positions.push(
            center.x + avgNormal.x * length,
            center.y + avgNormal.y * length,
            center.z + avgNormal.z * length
        );

        // Color: magenta at base, green at tip
        colors.push(1, 0, 1); // Magenta at base
        colors.push(0, 1, 0); // Green at tip
    };

    if (indexAttr) {
        for (let i = 0; i < indexAttr.count; i += 3) {
            addFace(indexAttr.getX(i), indexAttr.getX(i + 1), indexAttr.getX(i + 2));
        }
    } else {
        for (let i = 0; i < posAttr.count; i += 3) {
            addFace(i, i + 1, i + 2);
        }
    }

    const lineGeom = new THREE.BufferGeometry();
    lineGeom.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
    lineGeom.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3));

    const lineMat = new THREE.LineBasicMaterial({
        vertexColors: true,
        depthTest: true,
        depthWrite: false,
        transparent: true,
        opacity: 0.8
    });

    return new THREE.LineSegments(lineGeom, lineMat);
}

/**
 * Update normal vector visualization for selected object
 */
function updateNormalVectorVisualization() {
    clearNormalVectorHelper();

    if (!normalVectorState.enabled || !pickState.selectedObject) {
        return;
    }

    const mesh = pickState.selectedObject;
    if (!mesh.isMesh) return;

    if (normalVectorState.type === 'vertex') {
        normalVectorState.helper = createVertexNormalHelper(mesh, normalVectorState.length);
    } else {
        normalVectorState.helper = createFaceNormalHelper(mesh, normalVectorState.length);
    }

    if (normalVectorState.helper) {
        normalVectorState.helper.name = '__normalVectorHelper__';
        threeState.scene.add(normalVectorState.helper);
    }
}

/**
 * Clear selection highlight
 */
function clearSelectionHighlight() {
    if (pickState.selectionHelper) {
        threeState.scene.remove(pickState.selectionHelper);
        pickState.selectionHelper.dispose();
        pickState.selectionHelper = null;
    }
    // Also clear normal vector helper when selection is cleared
    clearNormalVectorHelper();
    normalVectorState.enabled = false;
}

/**
 * Clear selection and show all materials
 */
function clearSelection() {
    clearSelectionHighlight();
    pickState.selectedObject = null;
    updateMaterialUI();
    updateStatus('Selection cleared - showing all materials');
}

/**
 * Get materials from a specific object
 */
function getMaterialsFromObject(object) {
    const materials = [];
    if (!object) return materials;

    if (object.material) {
        if (Array.isArray(object.material)) {
            object.material.forEach(mat => {
                if (!materials.includes(mat)) materials.push(mat);
            });
        } else {
            if (!materials.includes(object.material)) {
                materials.push(object.material);
            }
        }
    }

    return materials;
}

/**
 * Count meshes in the scene
 */
function countMeshes() {
    let count = 0;
    if (sceneState.root) {
        sceneState.root.traverse(obj => {
            if (obj.isMesh) count++;
        });
    }
    return count;
}

// ============================================================================
// Event Handlers
// ============================================================================

function onWindowResize() {
    threeState.camera.aspect = window.innerWidth / window.innerHeight;
    threeState.camera.updateProjectionMatrix();
    threeState.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, MAX_RENDER_PIXEL_RATIO));
    threeState.renderer.setSize(window.innerWidth, window.innerHeight);
}

function onFileSelect(event) {
    const files = event.target.files;
    if (files.length > 0) {
        loadUSDFromFile(files[0]);
    }
    event.target.value = '';
}

function onDragOver(event) {
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
    document.getElementById('canvas-container').classList.add('drag-over');
}

function onDragLeave(event) {
    event.preventDefault();
    document.getElementById('canvas-container').classList.remove('drag-over');
}

function onFileDrop(event) {
    event.preventDefault();
    document.getElementById('canvas-container').classList.remove('drag-over');

    const files = event.dataTransfer.files;
    if (files.length > 0) {
        const file = files[0];
        const ext = file.name.toLowerCase().split('.').pop();
        if (['usd', 'usda', 'usdc', 'usdz'].includes(ext)) {
            loadUSDFromFile(file);
        } else {
            updateStatus('Please drop a USD file (.usd, .usda, .usdc, .usdz)');
        }
    }
}

function setGuiHidden(hidden) {
    guiState.hidden = hidden;
    document.body.classList.toggle('gui-hidden', hidden);
    const toggle = document.getElementById('gui-visibility-toggle');
    if (toggle) {
        toggle.title = hidden ? 'Show UI (H)' : 'Hide UI (H)';
        toggle.setAttribute('aria-label', hidden ? 'Show UI' : 'Hide UI');
    }
}

function isEditableElement(element) {
    if (!element) return false;
    const tag = element.tagName;
    return element.isContentEditable ||
        tag === 'INPUT' ||
        tag === 'TEXTAREA' ||
        tag === 'SELECT';
}

function onKeyDown(event) {
    if (event.key !== 'h' && event.key !== 'H') return;
    if (event.ctrlKey || event.metaKey || event.altKey || isEditableElement(event.target)) return;
    event.preventDefault();
    setGuiHidden(!guiState.hidden);
}

// ============================================================================
// UI Helpers
// ============================================================================

function updateStatus(message) {
    const statusEl = document.getElementById('status');
    if (statusEl) {
        statusEl.textContent = message;
    }
}

window.loadFile = () => document.getElementById('file-input').click();

// ============================================================================
// Render Loop
// ============================================================================

function animate() {
    requestAnimationFrame(animate);
    frameState.frameCount++;
    const now = performance.now();
    if (now - frameState.lastFpsUpdateMs >= 500) {
        const fps = frameState.frameCount * 1000 / (now - frameState.lastFpsUpdateMs);
        const fpsEl = document.getElementById('fps-value');
        if (fpsEl) {
            fpsEl.textContent = fps.toFixed(1);
        }
        frameState.frameCount = 0;
        frameState.lastFpsUpdateMs = now;
    }
    threeState.controls.update();

    // Animation disabled for now - may revisit later
    // if (animationState.mixer && animationState.params.isPlaying) {
    //     const delta = threeState.timer.getDelta();
    //     const scaledDelta = delta * (animationState.params.speed / 24.0);
    //     animationState.mixer.update(scaledDelta);
    //
    //     if (animationState.action) {
    //         animationState.params.time = animationState.action.time;
    //     }
    // }

    threeState.renderer.render(threeState.scene, threeState.camera);
}

// ============================================================================
// OpenPBR Validation Framework
// ============================================================================

/**
 * OpenPBR Ground Truth BRDF Implementation
 * Based on https://academysoftwarefoundation.github.io/OpenPBR/
 */
const OpenPBRValidation = {
    // Current material parameters for validation
    params: {
        base_color: [0.9, 0.7, 0.3],
        base_metalness: 0.8,
        base_weight: 1.0,
        specular_roughness: 0.3,
        specular_ior: 1.5,
        specular_weight: 1.0
    },

    // Validation results
    results: {
        sphereCenter: null,
        sphereEdge: null,
        sphereMidpoint: null,
        groundTruth: null,
        differences: null
    },

    /**
     * Fresnel Schlick approximation
     * F(μ) = F₀ + (1 - F₀)(1 - μ)⁵
     * @param {number} cosTheta - cos of angle between view and half-vector
     * @param {number[]} f0 - Fresnel reflectance at normal incidence
     * @returns {number[]} Fresnel term RGB
     */
    fresnelSchlick(cosTheta, f0) {
        const t = Math.pow(1.0 - Math.max(0, cosTheta), 5);
        return f0.map(f => f + (1.0 - f) * t);
    },

    /**
     * Calculate F0 from IOR using Schlick's approximation
     * F₀ = ((n₁ - n₂) / (n₁ + n₂))²
     * For air (n=1) to material (n=ior)
     * @param {number} ior - Index of refraction
     * @returns {number} F0 value
     */
    iorToF0(ior) {
        const r = (ior - 1.0) / (ior + 1.0);
        return r * r;
    },

    /**
     * GGX Normal Distribution Function
     * D(m) = α² / (π * (cos²θ * (α² - 1) + 1)²)
     * @param {number} NdotH - cos of angle between normal and half-vector
     * @param {number} roughness - Surface roughness [0, 1]
     * @returns {number} NDF value
     */
    ggxNDF(NdotH, roughness) {
        const a = roughness * roughness;
        const a2 = a * a;
        const NdotH2 = NdotH * NdotH;
        const denom = NdotH2 * (a2 - 1.0) + 1.0;
        return a2 / (Math.PI * denom * denom);
    },

    /**
     * GGX Smith Geometry Function (single direction)
     * G₁(v) = 2 * NdotV / (NdotV + sqrt(α² + (1 - α²) * NdotV²))
     * @param {number} NdotV - cos of angle between normal and direction
     * @param {number} roughness - Surface roughness
     * @returns {number} Geometry term
     */
    ggxGeometry1(NdotV, roughness) {
        const a = roughness * roughness;
        const a2 = a * a;
        const NdotV2 = NdotV * NdotV;
        return 2.0 * NdotV / (NdotV + Math.sqrt(a2 + (1.0 - a2) * NdotV2));
    },

    /**
     * GGX Smith Geometry Function (full)
     * G(l, v) = G₁(l) * G₁(v)
     * @param {number} NdotV - cos of angle between normal and view
     * @param {number} NdotL - cos of angle between normal and light
     * @param {number} roughness - Surface roughness
     * @returns {number} Geometry term
     */
    ggxGeometry(NdotV, NdotL, roughness) {
        return this.ggxGeometry1(NdotV, roughness) * this.ggxGeometry1(NdotL, roughness);
    },

    /**
     * Cook-Torrance specular BRDF
     * f_spec = D * F * G / (4 * NdotL * NdotV)
     * @param {number} NdotL - cos of angle between normal and light
     * @param {number} NdotV - cos of angle between normal and view
     * @param {number} NdotH - cos of angle between normal and half-vector
     * @param {number} VdotH - cos of angle between view and half-vector
     * @param {number[]} f0 - Fresnel F0
     * @param {number} roughness - Surface roughness
     * @returns {number[]} Specular BRDF RGB
     */
    specularBRDF(NdotL, NdotV, NdotH, VdotH, f0, roughness) {
        if (NdotL <= 0 || NdotV <= 0) return [0, 0, 0];

        const D = this.ggxNDF(NdotH, roughness);
        const F = this.fresnelSchlick(VdotH, f0);
        const G = this.ggxGeometry(NdotV, NdotL, roughness);

        const denom = 4.0 * NdotL * NdotV;
        return F.map(f => (D * f * G) / Math.max(denom, 0.001));
    },

    /**
     * Lambertian diffuse BRDF
     * f_diff = base_color / π
     * @param {number[]} baseColor - Base color RGB
     * @returns {number[]} Diffuse BRDF RGB
     */
    diffuseBRDF(baseColor) {
        return baseColor.map(c => c / Math.PI);
    },

    /**
     * Full OpenPBR BRDF evaluation
     * Combines metal and dielectric responses based on metalness
     * @param {Object} params - Material parameters
     * @param {number} NdotL - cos of angle between normal and light
     * @param {number} NdotV - cos of angle between normal and view
     * @param {number} NdotH - cos of angle between normal and half-vector
     * @param {number} VdotH - cos of angle between view and half-vector
     * @returns {number[]} BRDF RGB
     */
    evaluateBRDF(params, NdotL, NdotV, NdotH, VdotH) {
        const metalness = params.base_metalness;
        const roughness = params.specular_roughness;
        const baseColor = params.base_color;
        const ior = params.specular_ior;

        // Dielectric F0 from IOR
        const dielectricF0 = this.iorToF0(ior);

        // Metal F0 is the base color (conductor Fresnel)
        const metalF0 = baseColor;

        // Blend F0 between dielectric and metal
        const f0 = baseColor.map((c, i) => {
            const dielectric = dielectricF0;
            const metal = metalF0[i];
            return dielectric * (1.0 - metalness) + metal * metalness;
        });

        // Specular contribution
        const specular = this.specularBRDF(NdotL, NdotV, NdotH, VdotH, f0, roughness);

        // Diffuse contribution (only for dielectrics)
        const fresnel = this.fresnelSchlick(VdotH, f0);
        const diffuse = this.diffuseBRDF(baseColor);

        // Metals have no diffuse, dielectrics have diffuse weighted by (1 - F)
        const result = specular.map((s, i) => {
            const diffuseContrib = diffuse[i] * (1.0 - fresnel[i]) * (1.0 - metalness);
            return s + diffuseContrib;
        });

        return result;
    },

    /**
     * Calculate expected radiance for a sphere under furnace test
     * Furnace test uses constant environment illumination
     * @param {Object} params - Material parameters
     * @param {number[]} envColor - Environment color (linear RGB)
     * @param {number} theta - Angle from sphere center (0 = center, PI/2 = edge)
     * @returns {number[]} Expected radiance RGB
     */
    furnaceTestRadiance(params, envColor, theta) {
        // View direction is always towards camera (0, 0, 1) in view space
        // Normal at theta from center: N = (sin(theta), 0, cos(theta))
        const sinTheta = Math.sin(theta);
        const cosTheta = Math.cos(theta);

        const N = [sinTheta, 0, cosTheta];
        const V = [0, 0, 1]; // View direction

        // NdotV
        const NdotV = Math.max(0, N[2]); // N.z since V = (0,0,1)

        // For furnace test, we integrate over hemisphere
        // With constant illumination, this simplifies to:
        // L_out = f_r * L_in * cos_weighted_hemisphere_integral

        // For a perfect mirror: reflect all light back
        // For diffuse: L_out = albedo * L_in (due to 1/π in BRDF and π from hemisphere integral)

        // Simplified furnace test: at each point, compute BRDF * cosine * 2π
        // (hemisphere integral of constant lighting)

        // Use multiple sample directions for more accurate integration
        const numSamples = 64;
        let accum = [0, 0, 0];

        for (let i = 0; i < numSamples; i++) {
            for (let j = 0; j < numSamples; j++) {
                // Uniform hemisphere sampling
                const u1 = (i + 0.5) / numSamples;
                const u2 = (j + 0.5) / numSamples;

                // Cosine-weighted hemisphere sampling
                const phi = 2 * Math.PI * u1;
                const r = Math.sqrt(u2);
                const x = r * Math.cos(phi);
                const y = r * Math.sin(phi);
                const z = Math.sqrt(1 - u2);

                // Transform to world space aligned with N
                // Create tangent frame
                const up = Math.abs(N[1]) < 0.999 ? [0, 1, 0] : [1, 0, 0];
                const T = this.normalize(this.cross(up, N));
                const B = this.cross(N, T);

                // Light direction in world space
                const L = [
                    T[0] * x + B[0] * y + N[0] * z,
                    T[1] * x + B[1] * y + N[1] * z,
                    T[2] * x + B[2] * y + N[2] * z
                ];

                // Half vector
                const H = this.normalize([V[0] + L[0], V[1] + L[1], V[2] + L[2]]);

                // Dot products
                const NdotL = Math.max(0, N[0] * L[0] + N[1] * L[1] + N[2] * L[2]);
                const NdotH = Math.max(0, N[0] * H[0] + N[1] * H[1] + N[2] * H[2]);
                const VdotH = Math.max(0, V[0] * H[0] + V[1] * H[1] + V[2] * H[2]);

                if (NdotL > 0) {
                    const brdf = this.evaluateBRDF(params, NdotL, NdotV, NdotH, VdotH);

                    // For cosine-weighted sampling, PDF = cos(theta) / π
                    // Contribution = BRDF * L_in * NdotL / PDF = BRDF * L_in * π
                    // But we already have cosine in sampling, so just BRDF * L_in
                    accum[0] += brdf[0] * envColor[0] * Math.PI;
                    accum[1] += brdf[1] * envColor[1] * Math.PI;
                    accum[2] += brdf[2] * envColor[2] * Math.PI;
                }
            }
        }

        const numTotalSamples = numSamples * numSamples;
        return accum.map(v => v / numTotalSamples);
    },

    // Vector math helpers
    normalize(v) {
        const len = Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return len > 0 ? [v[0] / len, v[1] / len, v[2] / len] : [0, 0, 0];
    },

    cross(a, b) {
        return [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        ];
    },

    dot(a, b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    },

    /**
     * Capture pixel from renderer at screen coordinates
     * @param {THREE.WebGLRenderer} renderer
     * @param {number} x - Screen X coordinate
     * @param {number} y - Screen Y coordinate
     * @returns {number[]} RGB values [0, 1]
     */
    capturePixel(renderer, x, y) {
        const renderTarget = renderer.getRenderTarget();
        const pixelBuffer = new Float32Array(4);

        // Create a small render target to read from
        const readTarget = new THREE.WebGLRenderTarget(1, 1, {
            format: THREE.RGBAFormat,
            type: THREE.FloatType
        });

        // We need to read from the main framebuffer
        // Three.js doesn't directly support reading pixels, so we use WebGL
        const gl = renderer.getContext();
        const pixels = new Uint8Array(4);
        gl.readPixels(Math.floor(x), Math.floor(renderer.domElement.height - y), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixels);

        readTarget.dispose();

        // Convert from 0-255 sRGB to 0-1 linear
        return [
            sRGBComponentToLinear(pixels[0] / 255),
            sRGBComponentToLinear(pixels[1] / 255),
            sRGBComponentToLinear(pixels[2] / 255)
        ];
    },

    /**
     * Get sphere screen positions for validation points
     * @param {THREE.Camera} camera
     * @param {THREE.Mesh} sphereMesh
     * @param {THREE.WebGLRenderer} renderer
     * @returns {Object} Positions for center, edge, and midpoint
     */
    getSphereScreenPositions(camera, sphereMesh, renderer) {
        const sphere = new THREE.Sphere();
        sphereMesh.geometry.computeBoundingSphere();
        sphere.copy(sphereMesh.geometry.boundingSphere);
        sphere.applyMatrix4(sphereMesh.matrixWorld);

        const center = sphere.center.clone();
        const radius = sphere.radius;

        // Get camera right vector
        const camRight = new THREE.Vector3();
        camera.getWorldDirection(camRight);
        const camUp = new THREE.Vector3(0, 1, 0);
        camRight.crossVectors(camUp, camRight).normalize();

        // Center position
        const centerScreen = this.worldToScreen(center, camera, renderer);

        // Edge position (right edge of sphere)
        const edgeWorld = center.clone().add(camRight.clone().multiplyScalar(radius * 0.95));
        const edgeScreen = this.worldToScreen(edgeWorld, camera, renderer);

        // Midpoint position (halfway between center and edge)
        const midWorld = center.clone().add(camRight.clone().multiplyScalar(radius * 0.5));
        const midScreen = this.worldToScreen(midWorld, camera, renderer);

        return {
            center: centerScreen,
            edge: edgeScreen,
            midpoint: midScreen,
            // Angular positions for ground truth calculation
            centerTheta: 0,
            edgeTheta: Math.asin(0.95), // ~72 degrees
            midpointTheta: Math.asin(0.5) // 30 degrees
        };
    },

    /**
     * Convert world position to screen coordinates
     */
    worldToScreen(worldPos, camera, renderer) {
        const pos = worldPos.clone().project(camera);
        return {
            x: (pos.x + 1) / 2 * renderer.domElement.width,
            y: (1 - pos.y) / 2 * renderer.domElement.height
        };
    },

    /**
     * Run furnace test validation
     * @param {THREE.WebGLRenderer} renderer
     * @param {THREE.Scene} scene
     * @param {THREE.Camera} camera
     * @param {THREE.Mesh} sphereMesh
     * @param {Object} materialParams - OpenPBR material parameters
     * @param {number[]} envColor - Environment color (linear RGB)
     */
    runFurnaceTest(renderer, scene, camera, sphereMesh, materialParams, envColor) {
        console.log('=== OpenPBR Furnace Test Validation ===');
        console.log('Material parameters:', materialParams);
        console.log('Environment color (linear):', envColor);

        // Update internal params
        this.params = { ...this.params, ...materialParams };

        // Get screen positions
        const positions = this.getSphereScreenPositions(camera, sphereMesh, renderer);
        console.log('Screen positions:', positions);

        // Render the scene
        renderer.render(scene, camera);

        // Capture pixels
        this.results.sphereCenter = this.capturePixel(renderer, positions.center.x, positions.center.y);
        this.results.sphereEdge = this.capturePixel(renderer, positions.edge.x, positions.edge.y);
        this.results.sphereMidpoint = this.capturePixel(renderer, positions.midpoint.x, positions.midpoint.y);

        console.log('Captured pixels (linear RGB):');
        console.log('  Center:', this.results.sphereCenter);
        console.log('  Edge:', this.results.sphereEdge);
        console.log('  Midpoint:', this.results.sphereMidpoint);

        // Calculate ground truth
        this.results.groundTruth = {
            center: this.furnaceTestRadiance(this.params, envColor, positions.centerTheta),
            edge: this.furnaceTestRadiance(this.params, envColor, positions.edgeTheta),
            midpoint: this.furnaceTestRadiance(this.params, envColor, positions.midpointTheta)
        };

        console.log('Ground truth (linear RGB):');
        console.log('  Center:', this.results.groundTruth.center);
        console.log('  Edge:', this.results.groundTruth.edge);
        console.log('  Midpoint:', this.results.groundTruth.midpoint);

        // Calculate differences
        this.results.differences = {
            center: this.calculateDifference(this.results.sphereCenter, this.results.groundTruth.center),
            edge: this.calculateDifference(this.results.sphereEdge, this.results.groundTruth.edge),
            midpoint: this.calculateDifference(this.results.sphereMidpoint, this.results.groundTruth.midpoint)
        };

        console.log('Differences (absolute):');
        console.log('  Center:', this.results.differences.center);
        console.log('  Edge:', this.results.differences.edge);
        console.log('  Midpoint:', this.results.differences.midpoint);

        // Summary
        const avgDiff = (
            this.results.differences.center.magnitude +
            this.results.differences.edge.magnitude +
            this.results.differences.midpoint.magnitude
        ) / 3;

        console.log(`Average difference magnitude: ${avgDiff.toFixed(4)}`);
        console.log('=== End Furnace Test ===');

        return this.results;
    },

    /**
     * Calculate difference between captured and expected values
     */
    calculateDifference(captured, expected) {
        const diff = captured.map((c, i) => c - expected[i]);
        const magnitude = Math.sqrt(diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2]);
        return { rgb: diff, magnitude };
    },

    /**
     * Create a simple validation report
     */
    generateReport() {
        if (!this.results.differences) {
            return 'No validation results available. Run furnace test first.';
        }

        const lines = [
            '=== OpenPBR Validation Report ===',
            '',
            'Material Parameters:',
            `  Base Color: [${this.params.base_color.map(v => v.toFixed(3)).join(', ')}]`,
            `  Metalness: ${this.params.base_metalness.toFixed(3)}`,
            `  Roughness: ${this.params.specular_roughness.toFixed(3)}`,
            `  IOR: ${this.params.specular_ior.toFixed(3)}`,
            '',
            'Results:',
            '',
            'Sphere Center:',
            `  Captured: [${this.results.sphereCenter.map(v => v.toFixed(4)).join(', ')}]`,
            `  Expected: [${this.results.groundTruth.center.map(v => v.toFixed(4)).join(', ')}]`,
            `  Diff Mag: ${this.results.differences.center.magnitude.toFixed(4)}`,
            '',
            'Sphere Edge:',
            `  Captured: [${this.results.sphereEdge.map(v => v.toFixed(4)).join(', ')}]`,
            `  Expected: [${this.results.groundTruth.edge.map(v => v.toFixed(4)).join(', ')}]`,
            `  Diff Mag: ${this.results.differences.edge.magnitude.toFixed(4)}`,
            '',
            'Sphere Midpoint:',
            `  Captured: [${this.results.sphereMidpoint.map(v => v.toFixed(4)).join(', ')}]`,
            `  Expected: [${this.results.groundTruth.midpoint.map(v => v.toFixed(4)).join(', ')}]`,
            `  Diff Mag: ${this.results.differences.midpoint.magnitude.toFixed(4)}`,
            '',
            '=== End Report ==='
        ];

        return lines.join('\n');
    }
};

/**
 * Validation state and UI
 */
const validationState = {
    enabled: false,
    useConstantEnv: true,
    envColor: '#ffffff'
};

/**
 * Setup validation UI in the GUI
 */
function setupValidationFolder(gui) {
    const validationFolder = gui.addFolder('OpenPBR Validation');

    validationFolder.add(validationState, 'enabled')
        .name('Enable Validation')
        .onChange(toggleValidationMode);

    validationFolder.add({
        runTest: () => runValidationTest()
    }, 'runTest').name('Run Furnace Test');

    validationFolder.add({
        runBRDFTests: () => runBRDFValidationTests()
    }, 'runBRDFTests').name('Run BRDF Tests');

    validationFolder.add({
        runMultiRes: () => runMultiResolutionBRDFTests()
    }, 'runMultiRes').name('Multi-Res Tests (256-1024)');

    validationFolder.add({
        runLayerTests: () => runLayerValidationTests()
    }, 'runLayerTests').name('Run Layer Tests');

    validationFolder.add({
        showReport: () => {
            const report = OpenPBRValidation.generateReport();
            console.log(report);
            alert(report);
        }
    }, 'showReport').name('Show Report');

    validationFolder.close();
}

/**
 * Run BRDF validation tests using OpenPBRValidator
 */
function runBRDFValidationTests(resolution = 256) {
    // Create validator with specified resolution
    const testValidator = new OpenPBRValidator(threeState.renderer, resolution);

    // Get material params from current sphere if available
    let testOptions = {};
    if (sceneState.root) {
        let sphereMesh = null;
        sceneState.root.traverse(obj => {
            if (obj.isMesh && !sphereMesh) {
                sphereMesh = obj;
            }
        });

        if (sphereMesh && sphereMesh.material) {
            const mat = sphereMesh.material;
            testOptions = {
                baseColor: mat.color ? [mat.color.r, mat.color.g, mat.color.b] : [0.8, 0.8, 0.8],
                metalness: mat.metalness !== undefined ? mat.metalness : 0.0,
                ior: mat.ior || 1.5
            };
        }
    }

    console.log(`\n>>> Running BRDF tests at ${resolution}x${resolution} resolution <<<\n`);
    const results = testValidator.runAllTests(testOptions);

    // Cleanup
    testValidator.dispose();

    // Show summary using the allPassed field from results
    updateStatus(`BRDF Validation (${resolution}x${resolution}): ${results.allPassed ? 'ALL PASSED' : 'SOME FAILED'}`);

    return results;
}

/**
 * Run BRDF validation at multiple resolutions to compare accuracy
 */
function runMultiResolutionBRDFTests() {
    const resolutions = [256, 512, 1024];
    const allResults = {};

    console.log('========================================');
    console.log('Multi-Resolution BRDF Validation');
    console.log('========================================');

    for (const res of resolutions) {
        allResults[res] = runBRDFValidationTests(res);
    }

    // Print comparison table
    console.log('\n========================================');
    console.log('Resolution Comparison Summary');
    console.log('========================================');
    console.log('Resolution | Fresnel Avg | GGX Avg    | Smith G Avg | Full BRDF Avg');
    console.log('-----------|-------------|------------|-------------|---------------');

    for (const res of resolutions) {
        const r = allResults[res];
        console.log(`${res.toString().padStart(10)} | ${r.fresnel.avgError.toFixed(6).padStart(11)} | ${r.ggxNDF.avgError.toFixed(6).padStart(10)} | ${r.smithG.avgError.toFixed(6).padStart(11)} | ${r.brdfFull.avgError.toFixed(6)}`);
    }
    console.log('========================================');

    updateStatus('Multi-resolution validation complete - see console');
}

/**
 * Run layer mixing and energy conservation validation tests
 */
function runLayerValidationTests(resolution = 256) {
    // Create validator with specified resolution
    const testValidator = new OpenPBRValidator(threeState.renderer, resolution);

    console.log('\n========================================');
    console.log(`Layer Validation Tests (${resolution}x${resolution})`);
    console.log('========================================\n');

    // Run layer tests
    const results = testValidator.runLayerTests();

    // Cleanup
    testValidator.dispose();

    // Show summary - results has { allPassed, configResults, energyConservation }
    updateStatus(`Layer Validation: ${results.allPassed ? 'ALL PASSED' : 'SOME FAILED'}`);

    return results;
}

/**
 * Toggle validation mode
 */
function toggleValidationMode(enabled) {
    if (enabled) {
        // Switch to constant color environment for furnace test
        settings.envMapPreset = 'constant_color';
        settings.envConstantColor = '#ffffff';
        loadEnvironment('constant_color');
        updateStatus('Validation mode: Furnace test environment enabled');
    }
}

/**
 * Run the validation test
 */
function runValidationTest() {
    if (!sceneState.root) {
        updateStatus('No scene loaded for validation');
        return;
    }

    // Find the sphere mesh
    let sphereMesh = null;
    sceneState.root.traverse(obj => {
        if (obj.isMesh && !sphereMesh) {
            sphereMesh = obj;
        }
    });

    if (!sphereMesh) {
        updateStatus('No mesh found for validation');
        return;
    }

    // Get material parameters from the sphere
    const mat = sphereMesh.material;
    const rawData = mat.userData?.rawData;

    let materialParams = { ...OpenPBRValidation.params };

    if (rawData?.openPBR || rawData?.openPBRShader) {
        const openPBR = rawData.openPBR || rawData.openPBRShader;
        if (openPBR.base_color) {
            materialParams.base_color = Array.isArray(openPBR.base_color)
                ? openPBR.base_color
                : [openPBR.base_color.r || 0.8, openPBR.base_color.g || 0.8, openPBR.base_color.b || 0.8];
        }
        if (openPBR.base_metalness !== undefined) {
            materialParams.base_metalness = openPBR.base_metalness;
        }
        if (openPBR.specular_roughness !== undefined) {
            materialParams.specular_roughness = openPBR.specular_roughness;
        }
        if (openPBR.specular_ior !== undefined) {
            materialParams.specular_ior = openPBR.specular_ior;
        }
    } else if (mat.color && mat.metalness !== undefined && mat.roughness !== undefined) {
        // Use Three.js material properties
        materialParams.base_color = [mat.color.r, mat.color.g, mat.color.b];
        materialParams.base_metalness = mat.metalness;
        materialParams.specular_roughness = mat.roughness;
        materialParams.specular_ior = mat.ior || 1.5;
    }

    // Get environment color (convert from hex to linear RGB)
    const envColor = parseHexColor(settings.envConstantColor, true);
    const envColorArray = [envColor.r, envColor.g, envColor.b];

    // Run the furnace test
    const results = OpenPBRValidation.runFurnaceTest(
        threeState.renderer,
        threeState.scene,
        threeState.camera,
        sphereMesh,
        materialParams,
        envColorArray
    );

    // Update status with summary
    const avgDiff = (
        results.differences.center.magnitude +
        results.differences.edge.magnitude +
        results.differences.midpoint.magnitude
    ) / 3;

    updateStatus(`Validation complete. Avg difference: ${avgDiff.toFixed(4)}`);
}

// Export for external access
window.OpenPBRValidation = OpenPBRValidation;
window.runValidationTest = runValidationTest;
// Debug exports
window._debugMaterialX = {
    get sceneState() { return sceneState; },
    get threeState() { return threeState; },
    get loaderState() { return loaderState; },
    get settings() { return settings; },
    selectMesh(index) {
        const meshes = [];
        if (sceneState.root) {
            sceneState.root.traverse(obj => {
                if (obj.isMesh) meshes.push(obj);
            });
        }
        if (index >= 0 && index < meshes.length) {
            selectObject(meshes[index]);
            return `Selected mesh ${index}: ${meshes[index].name || 'Unnamed'}`;
        }
        return `Invalid index. Available: 0-${meshes.length - 1}`;
    },
    listMeshes() {
        const meshes = [];
        if (sceneState.root) {
            sceneState.root.traverse(obj => {
                if (obj.isMesh) meshes.push(obj.name || 'Unnamed');
            });
        }
        return meshes;
    },
    get normalVectorState() { return normalVectorState; },
    updateNormalVectors() { updateNormalVectorVisualization(); }
};

// ============================================================================
// Start
// ============================================================================

init().catch(err => {
    console.error('Initialization failed:', err);
    updateStatus('Initialization failed: ' + err.message);
});
