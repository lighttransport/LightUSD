// LightUSD MaterialX/OpenPBR Demo with Three.js WebGPU + NodeMaterial (TSL)
// Simple viewer for USD files with MaterialX/OpenPBR and UsdPreviewSurface material support
// Uses WebGPU renderer and Three Shading Language (TSL) for material nodes

// Import Three.js WebGPU build (includes WebGPURenderer and NodeMaterial classes)
import * as THREE from 'three/webgpu';

// Import custom OpenPBR TSL material
import { createOpenPBRMaterial, MtlxNodes, MtlxNodeGraphProcessor } from 'lightusd/LightUSDOpenPBR_TSL.js';
import {
    createColor,
    extractValue,
    hasTexture,
    getTextureId,
    applyOpenPBRNormalMapFromGetter,
    loadTextureFromUSD
} from 'lightusd/LightUSDMaterialX.js';

import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { EXRLoader } from 'three/examples/jsm/loaders/EXRLoader.js';
import GUI from 'three/examples/jsm/libs/lil-gui.module.min.js';
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';

// ============================================================================
// Constants
// ============================================================================

const DEFAULT_BACKGROUND_COLOR = 0x1a1a1a;
const CAMERA_PADDING = 1.2;

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
    'aces': THREE.ACESFilmicToneMapping,
    'agx': THREE.AgXToneMapping,
    'neutral': THREE.NeutralToneMapping
};

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

// Three.js core objects
const threeState = {
    scene: null,
    camera: null,
    renderer: null,
    controls: null,
    pmremGenerator: null,
    envMap: null,
    gridHelper: null,
    axesHelper: null
};

// USD loader state
const loaderState = {
    loader: null,
    nativeLoader: null
};

// Scene state
const sceneState = {
    root: null,
    materials: [],
    materialData: [],
    textureCache: new Map(),
    upAxis: 'Y',
    metadata: null,
    showingNormals: false,
    originalMaterialsMap: new Map(),
    domeLightData: null
};

// Picking state
const pickState = {
    raycaster: new THREE.Raycaster(),
    mouse: new THREE.Vector2(),
    selectedObject: null,
    selectionHelper: null
};

// GUI state
const guiState = {
    gui: null,
    materialFolder: null,
    textureFolder: null,
    envPresetController: null
};

// User settings
const settings = {
    materialType: 'auto',
    shaderMode: 'openpbr-tsl',  // 'physical' = MeshPhysicalMaterial, 'openpbr-tsl' = custom OpenPBR
    envMapPreset: 'goegap_1k',
    envMapIntensity: 1.0,
    envConstantColor: '#ffffff',
    envColorspace: 'sRGB',
    showBackground: true,
    exposure: 1.0,
    toneMapping: 'aces',
    applyUpAxisConversion: false,
    showNormals: false,
    useBasicMaterial: false,
    showGrid: false,
    showAxes: false,
    gridSize: 10,
    gridDivisions: 10
};

// ============================================================================
// Colorspace Utilities
// ============================================================================

function sRGBComponentToLinear(c) {
    return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}

function linearComponentToSRGB(c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1.0 / 2.4) - 0.055;
}

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

function rgbToHex(r, g, b) {
    const toHex = (c) => {
        const clamped = Math.max(0, Math.min(1, c));
        return Math.round(clamped * 255).toString(16).padStart(2, '0');
    };
    return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
}

function sRGBToLinear(color) {
    return new THREE.Color(
        sRGBComponentToLinear(color.r),
        sRGBComponentToLinear(color.g),
        sRGBComponentToLinear(color.b)
    );
}

function linearToSRGB(color) {
    return new THREE.Color(
        linearComponentToSRGB(color.r),
        linearComponentToSRGB(color.g),
        linearComponentToSRGB(color.b)
    );
}

// ============================================================================
// WebGPU Check
// ============================================================================

async function checkWebGPUSupport() {
    if (!navigator.gpu) {
        return false;
    }
    try {
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) {
            return false;
        }
        return true;
    } catch (e) {
        return false;
    }
}

// ============================================================================
// Initialization
// ============================================================================

async function init() {
    // Check WebGPU support
    const webgpuSupported = await checkWebGPUSupport();
    if (!webgpuSupported) {
        updateStatus('WebGPU not supported in this browser. Please use Chrome 113+ or similar.');
        document.getElementById('model-info').style.display = 'none';
        return;
    }

    await initThreeJS();
    initControls();
    await initLoader();
    setupGUI();
    setupEventListeners();
    await loadEnvironment(settings.envMapPreset);
    await loadDefaultUSDFile();
    animate();
}

async function initThreeJS() {
    threeState.scene = new THREE.Scene();
    threeState.scene.background = new THREE.Color(DEFAULT_BACKGROUND_COLOR);

    threeState.camera = new THREE.PerspectiveCamera(45, window.innerWidth / window.innerHeight, 0.1, 1000);
    threeState.camera.position.set(3, 2, 5);

    // Use WebGPU Renderer
    threeState.renderer = new THREE.WebGPURenderer({
        antialias: true,
        powerPreference: 'high-performance'
    });
    threeState.renderer.setSize(window.innerWidth, window.innerHeight);
    threeState.renderer.setPixelRatio(window.devicePixelRatio);
    threeState.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    threeState.renderer.toneMappingExposure = settings.exposure;
    threeState.renderer.outputColorSpace = THREE.SRGBColorSpace;

    // Initialize WebGPU
    await threeState.renderer.init();

    document.getElementById('canvas-container').appendChild(threeState.renderer.domElement);

    threeState.pmremGenerator = new THREE.PMREMGenerator(threeState.renderer);
    threeState.pmremGenerator.compileEquirectangularShader();

    updateStatus('WebGPU initialized successfully');
}

function initControls() {
    threeState.controls = new OrbitControls(threeState.camera, threeState.renderer.domElement);
    threeState.controls.enableDamping = true;
    threeState.controls.dampingFactor = 0.05;
    threeState.controls.screenSpacePanning = true;
    threeState.controls.minDistance = 0.1;
    threeState.controls.maxDistance = 500;
    threeState.controls.mouseButtons = {
        LEFT: THREE.MOUSE.ROTATE,
        MIDDLE: THREE.MOUSE.PAN,
        RIGHT: THREE.MOUSE.DOLLY
    };
}

async function initLoader() {
    updateStatus('Initializing LightUSD WASM...');
    // Still on the legacy product: this demo drives LightUSDLoaderNative and
    // converts legacy material JSON for WebGPU. Pinned while next is default.
    loaderState.loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512, backend: 'legacy' });
    await loaderState.loader.init({ useMemory64: false });
    updateStatus('LightUSD initialized');
}

function setupGUI() {
    guiState.gui = new GUI({ title: 'MaterialX WebGPU Demo' });
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
}

function setupSceneFolder() {
    const sceneFolder = guiState.gui.addFolder('Scene');

    const actions = {
        fitToScene: fitCameraToScene,
        clearSelection: clearSelection
    };
    sceneFolder.add(actions, 'fitToScene').name('Fit to Scene');
    sceneFolder.add(actions, 'clearSelection').name('Clear Selection');

    guiState.envPresetController = sceneFolder.add(settings, 'envMapPreset', Object.keys(ENV_PRESETS))
        .name('Environment')
        .onChange(loadEnvironment);

    sceneFolder.addColor(settings, 'envConstantColor')
        .name('Env Color')
        .onChange(updateConstantColorEnvironment);

    sceneFolder.add(settings, 'envColorspace', ['sRGB', 'linear'])
        .name('Env Colorspace')
        .onChange(updateConstantColorEnvironment);

    sceneFolder.add(settings, 'envMapIntensity', 0, 100, 0.1)
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
        .onChange(v => { threeState.renderer.toneMapping = TONE_MAPPINGS[v] || THREE.ACESFilmicToneMapping; });

    sceneFolder.add(settings, 'applyUpAxisConversion')
        .name('Z-up to Y-up Fix')
        .onChange(applyUpAxisConversion);

    sceneFolder.add(settings, 'showNormals')
        .name('Show Normals')
        .onChange(toggleNormalDisplay);

    sceneFolder.add(settings, 'useBasicMaterial')
        .name('Basic Material (Perf Test)')
        .onChange(toggleBasicMaterial);

    sceneFolder.add(settings, 'showGrid')
        .name('Show Grid')
        .onChange(toggleGrid);

    sceneFolder.add(settings, 'showAxes')
        .name('Show Axes')
        .onChange(toggleAxes);
}

function setupMaterialTypeFolder() {
    const materialTypeFolder = guiState.gui.addFolder('Material Type');
    materialTypeFolder.add(settings, 'materialType', ['auto', 'openpbr', 'usdpreviewsurface'])
        .name('Preferred Type')
        .onChange(reloadMaterials);

    materialTypeFolder.add(settings, 'shaderMode', ['physical', 'openpbr-tsl'])
        .name('Shader Mode')
        .onChange(reloadMaterials);

    // Add info about shader modes
    const shaderInfo = {
        info: 'physical: Three.js built-in | openpbr-tsl: Custom TSL shader'
    };
    materialTypeFolder.add(shaderInfo, 'info').name('Info').disable();

    materialTypeFolder.open();
}

function setupEventListeners() {
    window.addEventListener('resize', onWindowResize);

    const fileInput = document.getElementById('file-input');
    fileInput.addEventListener('change', onFileSelect);

    const container = document.getElementById('canvas-container');
    container.addEventListener('dragover', onDragOver);
    container.addEventListener('drop', onFileDrop);
    container.addEventListener('dragleave', onDragLeave);
    container.addEventListener('click', onCanvasClick);
}

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

function updateEnvIntensity() {
    sceneState.materials.forEach(mat => {
        if (mat.envMapIntensity !== undefined) {
            mat.envMapIntensity = settings.envMapIntensity;
        }
    });
}

// ============================================================================
// OpenPBR to NodeMaterial (TSL) Conversion
// ============================================================================

/**
 * Convert OpenPBR material data to custom OpenPBRNodeMaterial (TSL)
 *
 * This creates a custom NodeMaterial that implements the OpenPBR shading model
 * using TSL (Three Shading Language) for WebGPU rendering.
 */
async function convertOpenPBRToTSLMaterial(materialData, usdScene = null, options = {}) {
    // Create the material using factory function
    const material = createOpenPBRMaterial();

    material.userData.textures = {};
    material.userData.materialType = 'OpenPBR-TSL';
    material.userData.openPBRData = materialData;

    // Get OpenPBR data - support multiple formats
    let pbr = null;

    if (materialData.openPBR) {
        pbr = materialData.openPBR;
        material.userData.format = 'grouped';
    } else if (materialData.base_color !== undefined ||
               materialData.base_metalness !== undefined ||
               materialData.specular_roughness !== undefined) {
        pbr = { flat: materialData };
        material.userData.format = 'flat';
    } else if (materialData.openPBRShader) {
        pbr = { flat: materialData.openPBRShader };
        material.userData.format = 'flat';
    }

    if (!pbr) {
        console.warn('No OpenPBR data found in material');
        return material;
    }

    const textureCache = options.textureCache || new Map();

    // Helper to get flat or grouped parameters
    const getParam = (name, group = null) => {
        if (pbr.flat) {
            return pbr.flat[name];
        } else if (group && pbr[group]) {
            return pbr[group][name];
        }
        return undefined;
    };

    // ========== Base Layer ==========
    const baseColorValue = extractValue(getParam('base_color', 'base')) || [0.8, 0.8, 0.8];
    const baseWeight = extractValue(getParam('base_weight', 'base')) ?? 1.0;
    const baseMetalness = extractValue(getParam('base_metalness', 'base')) ?? 0.0;
    const baseDiffuseRoughness = extractValue(getParam('base_diffuse_roughness', 'base')) ?? 0.0;

    // Set MeshPhysicalMaterial properties
    if (Array.isArray(baseColorValue)) {
        material.color.setRGB(baseColorValue[0], baseColorValue[1], baseColorValue[2]);
    }
    material._openPBR.base_weight = baseWeight;
    material.metalness = baseMetalness;
    material._openPBR.base_diffuse_roughness = baseDiffuseRoughness;

    // Load base color texture if present
    const baseColorParam = getParam('base_color', 'base');
    if (usdScene && hasTexture(baseColorParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(baseColorParam), textureCache);
        if (texture) {
            texture.colorSpace = THREE.SRGBColorSpace;
            material.map = texture;
            material.userData.textures.map = { textureId: getTextureId(baseColorParam), texture };
        }
    }

    // Load metalness texture if present
    const metalnessParam = getParam('base_metalness', 'base');
    if (usdScene && hasTexture(metalnessParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(metalnessParam), textureCache);
        if (texture) {
            material.metalnessMap = texture;
            material.userData.textures.metalnessMap = { textureId: getTextureId(metalnessParam), texture };
        }
    }

    // ========== Specular Layer ==========
    const specularRoughness = extractValue(getParam('specular_roughness', 'specular')) ?? 0.5;
    const specularIOR = extractValue(getParam('specular_ior', 'specular')) ?? 1.5;
    const specularWeight = extractValue(getParam('specular_weight', 'specular')) ?? 1.0;

    material.roughness = specularRoughness;
    material.ior = specularIOR;
    material._openPBR.specular_weight = specularWeight;

    // Load roughness texture if present
    const roughnessParam = getParam('specular_roughness', 'specular');
    if (usdScene && hasTexture(roughnessParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(roughnessParam), textureCache);
        if (texture) {
            material.roughnessMap = texture;
            material.userData.textures.roughnessMap = { textureId: getTextureId(roughnessParam), texture };
        }
    }

    // ========== Transmission Layer ==========
    const transmissionWeight = extractValue(getParam('transmission_weight', 'transmission')) ?? 0.0;
    material.transmission = transmissionWeight;

    // ========== Coat (Clearcoat) Layer ==========
    const coatWeight = extractValue(getParam('coat_weight', 'coat')) ?? 0.0;
    const coatRoughness = extractValue(getParam('coat_roughness', 'coat')) ?? 0.0;

    material.clearcoat = coatWeight;
    material.clearcoatRoughness = coatRoughness;

    // ========== Sheen/Fuzz Layer ==========
    let sheenWeight = extractValue(getParam('sheen_weight', 'sheen')) ?? 0.0;
    let sheenColorValue = extractValue(getParam('sheen_color', 'sheen'));

    // Fallback to fuzz parameters
    if (sheenWeight === 0) {
        sheenWeight = extractValue(getParam('fuzz_weight', 'fuzz')) ?? 0.0;
        sheenColorValue = extractValue(getParam('fuzz_color', 'fuzz'));
    }

    material.sheen = sheenWeight;
    if (sheenColorValue && Array.isArray(sheenColorValue)) {
        material.sheenColor.setRGB(sheenColorValue[0], sheenColorValue[1], sheenColorValue[2]);
    }

    // ========== Thin Film (Iridescence) Layer ==========
    const thinFilmWeight = extractValue(getParam('thin_film_weight', 'thin_film')) ?? 0.0;
    material.iridescence = thinFilmWeight;

    // ========== Emission Layer ==========
    const emissionColorValue = extractValue(getParam('emission_color', 'emission'));
    const emissionLuminance = extractValue(getParam('emission_luminance', 'emission')) ?? 0.0;

    if (emissionColorValue && Array.isArray(emissionColorValue)) {
        material.emissive.setRGB(emissionColorValue[0], emissionColorValue[1], emissionColorValue[2]);
    }
    material.emissiveIntensity = emissionLuminance;

    // Load emission texture if present
    const emissionColorParam = getParam('emission_color', 'emission');
    if (usdScene && hasTexture(emissionColorParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(emissionColorParam), textureCache);
        if (texture) {
            texture.colorSpace = THREE.SRGBColorSpace;
            material.emissiveMap = texture;
            material.userData.textures.emissiveMap = { textureId: getTextureId(emissionColorParam), texture };
        }
    }

    // ========== Geometry Layer ==========
    const opacityParam = getParam('opacity', 'geometry') ?? getParam('geometry_opacity', 'geometry');
    if (opacityParam !== undefined) {
        const opacityValue = extractValue(opacityParam);
        if (typeof opacityValue === 'number') {
            material.opacity = opacityValue;
            material.transparent = opacityValue < 1.0;
        }
    }

    await applyOpenPBRNormalMapFromGetter(
        material, getParam, usdScene, textureCache, loadTextureFromUSD);

    // Set material name
    if (materialData.name) {
        material.name = materialData.name;
    }

    // Store raw data for UI
    material.userData.rawData = materialData;
    material.userData.typeInfo = getMaterialType(materialData);
    material.userData.typeString = getMaterialTypeString(materialData) + ' (TSL)';

    return material;
}

/**
 * Convert OpenPBR material data to THREE.MeshPhysicalMaterial or OpenPBRNodeMaterial
 *
 * This uses Three.js NodeMaterial system with TSL for WebGPU rendering.
 * When shaderMode is 'openpbr-tsl', uses custom OpenPBR TSL shader.
 * Otherwise uses THREE.MeshPhysicalMaterial.
 */
async function convertOpenPBRToNodeMaterial(materialData, usdScene = null, options = {}) {
    const shaderMode = options.shaderMode || settings.shaderMode || 'physical';

    // Use custom OpenPBR TSL shader
    if (shaderMode === 'openpbr-tsl') {
        return await convertOpenPBRToTSLMaterial(materialData, usdScene, options);
    }

    // Use THREE.MeshPhysicalMaterial for full PBR support with TSL
    const material = new THREE.MeshPhysicalMaterial();

    material.userData.textures = {};
    material.userData.materialType = 'OpenPBR';
    material.userData.openPBRData = materialData;

    // Get OpenPBR data - support multiple formats
    let pbr = null;

    if (materialData.openPBR) {
        pbr = materialData.openPBR;
        material.userData.format = 'grouped';
    } else if (materialData.base_color !== undefined ||
               materialData.base_metalness !== undefined ||
               materialData.specular_roughness !== undefined) {
        pbr = { flat: materialData };
        material.userData.format = 'flat';
    } else if (materialData.openPBRShader) {
        pbr = { flat: materialData.openPBRShader };
        material.userData.format = 'flat';
    }

    if (!pbr) {
        console.warn('No OpenPBR data found in material');
        return material;
    }

    const textureCache = options.textureCache || new Map();

    // Helper to get flat or grouped parameters
    const getParam = (name, group = null) => {
        if (pbr.flat) {
            return pbr.flat[name];
        } else if (group && pbr[group]) {
            return pbr[group][name];
        }
        return undefined;
    };

    // ========== Base Layer ==========
    const baseColorValue = extractValue(getParam('base_color', 'base')) || [0.8, 0.8, 0.8];
    const baseWeight = extractValue(getParam('base_weight', 'base')) ?? 1.0;
    const baseMetalness = extractValue(getParam('base_metalness', 'base')) ?? 0.0;
    const baseDiffuseRoughness = extractValue(getParam('base_diffuse_roughness', 'base')) ?? 0.0;

    // Set base color
    material.color = createColor(baseColorValue);
    material.metalness = baseMetalness;

    // Load base color texture if present
    const baseColorParam = getParam('base_color', 'base');
    if (usdScene && hasTexture(baseColorParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(baseColorParam), textureCache);
        if (texture) {
            texture.colorSpace = THREE.SRGBColorSpace;
            material.map = texture;
            material.userData.textures.map = { textureId: getTextureId(baseColorParam), texture };
        }
    }

    // Load metalness texture if present
    const metalnessParam = getParam('base_metalness', 'base');
    if (usdScene && hasTexture(metalnessParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(metalnessParam), textureCache);
        if (texture) {
            material.metalnessMap = texture;
            material.userData.textures.metalnessMap = { textureId: getTextureId(metalnessParam), texture };
        }
    }

    // ========== Specular Layer ==========
    const specularRoughness = extractValue(getParam('specular_roughness', 'specular')) ?? 0.5;
    const specularIOR = extractValue(getParam('specular_ior', 'specular')) ?? 1.5;
    const specularColorValue = extractValue(getParam('specular_color', 'specular'));
    const specularWeight = extractValue(getParam('specular_weight', 'specular')) ?? 1.0;
    const specularAnisotropy = extractValue(getParam('specular_anisotropy', 'specular')) ?? 0.0;

    material.roughness = specularRoughness;
    material.ior = specularIOR;

    if (specularColorValue && Array.isArray(specularColorValue)) {
        material.specularColor = createColor(specularColorValue);
    }

    material.specularIntensity = specularWeight;
    material.anisotropy = specularAnisotropy;

    // Load roughness texture if present
    const roughnessParam = getParam('specular_roughness', 'specular');
    if (usdScene && hasTexture(roughnessParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(roughnessParam), textureCache);
        if (texture) {
            material.roughnessMap = texture;
            material.userData.textures.roughnessMap = { textureId: getTextureId(roughnessParam), texture };
        }
    }

    // ========== Transmission Layer ==========
    const transmissionWeight = extractValue(getParam('transmission_weight', 'transmission')) ?? 0.0;
    const transmissionColorValue = extractValue(getParam('transmission_color', 'transmission'));
    const transmissionDepth = extractValue(getParam('transmission_depth', 'transmission')) ?? 0.0;

    if (transmissionWeight > 0) {
        material.transmission = transmissionWeight;
        material.thickness = transmissionDepth;
        if (transmissionColorValue && Array.isArray(transmissionColorValue)) {
            material.attenuationColor = createColor(transmissionColorValue);
        }
        material.attenuationDistance = transmissionDepth > 0 ? transmissionDepth : Infinity;
    }

    // ========== Coat (Clearcoat) Layer ==========
    const coatWeight = extractValue(getParam('coat_weight', 'coat')) ?? 0.0;
    const coatRoughness = extractValue(getParam('coat_roughness', 'coat')) ?? 0.0;
    const coatIOR = extractValue(getParam('coat_ior', 'coat')) ?? 1.5;

    if (coatWeight > 0) {
        material.clearcoat = coatWeight;
        material.clearcoatRoughness = coatRoughness;
    }

    // Load coat/clearcoat roughness texture if present
    const coatRoughnessParam = getParam('coat_roughness', 'coat');
    if (usdScene && hasTexture(coatRoughnessParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(coatRoughnessParam), textureCache);
        if (texture) {
            material.clearcoatRoughnessMap = texture;
            material.userData.textures.clearcoatRoughnessMap = { textureId: getTextureId(coatRoughnessParam), texture };
        }
    }

    // ========== Sheen/Fuzz Layer ==========
    let sheenWeight = extractValue(getParam('sheen_weight', 'sheen')) ?? 0.0;
    let sheenColorValue = extractValue(getParam('sheen_color', 'sheen'));
    let sheenRoughness = extractValue(getParam('sheen_roughness', 'sheen')) ?? 0.5;

    // Fallback to fuzz parameters
    if (sheenWeight === 0) {
        sheenWeight = extractValue(getParam('fuzz_weight', 'fuzz')) ?? 0.0;
        sheenColorValue = extractValue(getParam('fuzz_color', 'fuzz'));
        sheenRoughness = extractValue(getParam('fuzz_roughness', 'fuzz')) ?? 0.5;
    }

    if (sheenWeight > 0) {
        material.sheen = sheenWeight;
        material.sheenRoughness = sheenRoughness;
        if (sheenColorValue && Array.isArray(sheenColorValue)) {
            material.sheenColor = createColor(sheenColorValue);
        }
    }

    // ========== Thin Film (Iridescence) Layer ==========
    const thinFilmWeight = extractValue(getParam('thin_film_weight', 'thin_film')) ?? 0.0;
    const thinFilmThickness = extractValue(getParam('thin_film_thickness', 'thin_film')) ?? 500;
    const thinFilmIOR = extractValue(getParam('thin_film_ior', 'thin_film')) ?? 1.5;

    if (thinFilmWeight > 0) {
        material.iridescence = thinFilmWeight;
        material.iridescenceIOR = thinFilmIOR;
        material.iridescenceThicknessRange = [100, thinFilmThickness];
    }

    // ========== Emission Layer ==========
    const emissionColorValue = extractValue(getParam('emission_color', 'emission'));
    const emissionLuminance = extractValue(getParam('emission_luminance', 'emission')) ?? 0.0;

    if (emissionColorValue && Array.isArray(emissionColorValue)) {
        material.emissive = createColor(emissionColorValue);
        material.emissiveIntensity = emissionLuminance;
    }

    // Load emission texture if present
    const emissionColorParam = getParam('emission_color', 'emission');
    if (usdScene && hasTexture(emissionColorParam)) {
        const texture = await loadTextureFromUSD(usdScene, getTextureId(emissionColorParam), textureCache);
        if (texture) {
            texture.colorSpace = THREE.SRGBColorSpace;
            material.emissiveMap = texture;
            material.userData.textures.emissiveMap = { textureId: getTextureId(emissionColorParam), texture };
        }
    }

    // ========== Geometry Layer ==========
    const opacityParam = getParam('opacity', 'geometry') ?? getParam('geometry_opacity', 'geometry');
    if (opacityParam !== undefined) {
        const opacityValue = extractValue(opacityParam);
        if (typeof opacityValue === 'number') {
            material.opacity = opacityValue;
            material.transparent = opacityValue < 1.0;
        }
        if (usdScene && hasTexture(opacityParam)) {
            const texture = await loadTextureFromUSD(usdScene, getTextureId(opacityParam), textureCache);
            if (texture) {
                material.alphaMap = texture;
                material.transparent = true;
                material.userData.textures.alphaMap = { textureId: getTextureId(opacityParam), texture };
            }
        }
    }

    await applyOpenPBRNormalMapFromGetter(
        material, getParam, usdScene, textureCache, loadTextureFromUSD);

    // ========== Environment Map ==========
    if (options.envMap) {
        material.envMap = options.envMap;
        material.envMapIntensity = options.envMapIntensity || 1.0;
    }

    // Set material name
    if (materialData.name) {
        material.name = materialData.name;
    }

    return material;
}

/**
 * Convert UsdPreviewSurface to THREE.MeshPhysicalMaterial
 */
function convertUsdMaterialToNodeMaterial(usdMaterial, usdScene) {
    const material = new THREE.MeshPhysicalMaterial();

    // Diffuse color
    material.color = new THREE.Color(0.18, 0.18, 0.18);
    if (Object.prototype.hasOwnProperty.call(usdMaterial, 'diffuseColor')) {
        const color = usdMaterial.diffuseColor;
        material.color = new THREE.Color(color[0], color[1], color[2]);
    }

    if (Object.prototype.hasOwnProperty.call(usdMaterial, 'diffuseColorTextureId')) {
        loadTextureFromUSD(usdScene, usdMaterial.diffuseColorTextureId, sceneState.textureCache).then((texture) => {
            if (texture) {
                texture.colorSpace = THREE.SRGBColorSpace;
                material.map = texture;
                material.needsUpdate = true;
            }
        }).catch(err => console.warn('Failed to load diffuse texture:', err));
    }

    // IOR
    material.ior = usdMaterial.ior ?? 1.5;

    // Clearcoat
    material.clearcoat = usdMaterial.clearcoat ?? 0.0;
    material.clearcoatRoughness = usdMaterial.clearcoatRoughness ?? 0.0;

    // Metalness/Roughness
    material.metalness = usdMaterial.metallic ?? 0.0;
    material.roughness = usdMaterial.roughness ?? 0.5;

    if (Object.prototype.hasOwnProperty.call(usdMaterial, 'metallicTextureId')) {
        loadTextureFromUSD(usdScene, usdMaterial.metallicTextureId, sceneState.textureCache).then((texture) => {
            if (texture) {
                material.metalnessMap = texture;
                material.needsUpdate = true;
            }
        }).catch(err => console.warn('Failed to load metallic texture:', err));
    }

    if (Object.prototype.hasOwnProperty.call(usdMaterial, 'roughnessTextureId')) {
        loadTextureFromUSD(usdScene, usdMaterial.roughnessTextureId, sceneState.textureCache).then((texture) => {
            if (texture) {
                material.roughnessMap = texture;
                material.needsUpdate = true;
            }
        }).catch(err => console.warn('Failed to load roughness texture:', err));
    }

    // Emissive
    if (Object.prototype.hasOwnProperty.call(usdMaterial, 'emissiveColor')) {
        const color = usdMaterial.emissiveColor;
        material.emissive = new THREE.Color(color[0], color[1], color[2]);
    }

    if (Object.prototype.hasOwnProperty.call(usdMaterial, 'emissiveColorTextureId')) {
        loadTextureFromUSD(usdScene, usdMaterial.emissiveColorTextureId, sceneState.textureCache).then((texture) => {
            if (texture) {
                texture.colorSpace = THREE.SRGBColorSpace;
                material.emissiveMap = texture;
                material.needsUpdate = true;
            }
        }).catch(err => console.warn('Failed to load emissive texture:', err));
    }

    // Opacity
    material.opacity = usdMaterial.opacity ?? 1.0;
    material.transparent = material.opacity < 1.0;

    // Normal map
    if (Object.prototype.hasOwnProperty.call(usdMaterial, 'normalTextureId')) {
        loadTextureFromUSD(usdScene, usdMaterial.normalTextureId, sceneState.textureCache).then((texture) => {
            if (texture) {
                material.normalMap = texture;
                material.needsUpdate = true;
            }
        }).catch(err => console.warn('Failed to load normal texture:', err));
    }

    return material;
}

/**
 * Create default NodeMaterial
 */
function createDefaultNodeMaterial() {
    return new THREE.MeshPhysicalMaterial({
        color: new THREE.Color(0.18, 0.18, 0.18),
        emissive: new THREE.Color(0x000000),
        metalness: 0.0,
        roughness: 0.5,
        transparent: false,
        depthTest: true,
        side: THREE.FrontSide
    });
}

/**
 * Get material type info
 */
function getMaterialType(materialData) {
    let parsedMaterial = materialData;
    if (typeof materialData === 'string') {
        try {
            parsedMaterial = JSON.parse(materialData);
        } catch (e) {
            return { hasOpenPBR: false, hasUsdPreviewSurface: false, hasBoth: false, hasNone: true, recommended: 'none' };
        }
    }

    if (!parsedMaterial) {
        return { hasOpenPBR: false, hasUsdPreviewSurface: false, hasBoth: false, hasNone: true, recommended: 'none' };
    }

    const hasOpenPBR = !!parsedMaterial.hasOpenPBR;
    const hasUsdPreviewSurface = !!parsedMaterial.hasUsdPreviewSurface;
    const hasBoth = hasOpenPBR && hasUsdPreviewSurface;
    const hasNone = !hasOpenPBR && !hasUsdPreviewSurface;

    let recommended = 'none';
    if (hasOpenPBR) recommended = 'openpbr';
    else if (hasUsdPreviewSurface) recommended = 'usdpreviewsurface';

    return { hasOpenPBR, hasUsdPreviewSurface, hasBoth, hasNone, recommended };
}

/**
 * Get material type string
 */
function getMaterialTypeString(materialData) {
    const typeInfo = getMaterialType(materialData);
    if (typeInfo.hasBoth) return 'Both';
    if (typeInfo.hasOpenPBR) return 'OpenPBR';
    if (typeInfo.hasUsdPreviewSurface) return 'UsdPreviewSurface';
    return 'None';
}

/**
 * Smart material conversion: automatically selects OpenPBR or UsdPreviewSurface
 */
async function convertMaterial(materialData, usdScene, options = {}) {
    const typeInfo = getMaterialType(materialData);

    if (typeInfo.hasNone) {
        return createDefaultNodeMaterial();
    }

    let parsedMaterial = materialData;
    if (typeof materialData === 'string') {
        try {
            parsedMaterial = JSON.parse(materialData);
        } catch (e) {
            return createDefaultNodeMaterial();
        }
    }

    const preferredType = options.preferredMaterialType || 'auto';
    let useOpenPBR = false;
    let useUsdPreviewSurface = false;

    switch (preferredType) {
        case 'auto':
            if (typeInfo.hasOpenPBR) useOpenPBR = true;
            else if (typeInfo.hasUsdPreviewSurface) useUsdPreviewSurface = true;
            break;
        case 'openpbr':
            if (typeInfo.hasOpenPBR) useOpenPBR = true;
            else if (typeInfo.hasUsdPreviewSurface) useUsdPreviewSurface = true;
            break;
        case 'usdpreviewsurface':
            if (typeInfo.hasUsdPreviewSurface) useUsdPreviewSurface = true;
            else if (typeInfo.hasOpenPBR) useOpenPBR = true;
            break;
        default:
            if (typeInfo.hasOpenPBR) useOpenPBR = true;
            else if (typeInfo.hasUsdPreviewSurface) useUsdPreviewSurface = true;
    }

    if (useOpenPBR) {
        return convertOpenPBRToNodeMaterial(parsedMaterial, usdScene, {
            ...options,
            shaderMode: options.shaderMode || settings.shaderMode
        });
    } else if (useUsdPreviewSurface) {
        return convertUsdMaterialToNodeMaterial(parsedMaterial, usdScene);
    }

    return createDefaultNodeMaterial();
}

// ============================================================================
// UpAxis Conversion
// ============================================================================

function initUpAxisConversion() {
    if (sceneState.upAxis === 'Z') {
        settings.applyUpAxisConversion = true;
    } else {
        settings.applyUpAxisConversion = false;
    }

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

function toggleNormalDisplay() {
    if (!sceneState.root) return;

    // Disable basic material mode if enabling normals
    if (settings.showNormals && settings.useBasicMaterial) {
        settings.useBasicMaterial = false;
        updateGUIController('useBasicMaterial');
    }

    if (settings.showNormals) {
        sceneState.showingNormals = true;
        sceneState.root.traverse(obj => {
            if (obj.isMesh && obj.material) {
                sceneState.originalMaterialsMap.set(obj, obj.material);
                obj.material = new THREE.MeshNormalMaterial({ flatShading: false });
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

/**
 * Toggle basic material mode for performance testing
 * Uses MeshBasicMaterial (no lighting calculations) to isolate
 * whether performance issues are from shading or geometry
 */
function toggleBasicMaterial() {
    if (!sceneState.root) return;

    // Disable normal display if enabling basic material
    if (settings.useBasicMaterial && settings.showNormals) {
        settings.showNormals = false;
        sceneState.showingNormals = false;
        updateGUIController('showNormals');
    }

    if (settings.useBasicMaterial) {
        // Store original materials and replace with basic material
        const basicMaterial = new THREE.MeshBasicMaterial({
            color: 0x888888,
            wireframe: false
        });

        sceneState.root.traverse(obj => {
            if (obj.isMesh && obj.material) {
                if (!sceneState.originalMaterialsMap.has(obj)) {
                    sceneState.originalMaterialsMap.set(obj, obj.material);
                }
                obj.material = basicMaterial;
            }
        });

        updateStatus('Basic material mode - shading disabled for performance test');
    } else {
        // Restore original materials
        sceneState.root.traverse(obj => {
            if (obj.isMesh && sceneState.originalMaterialsMap.has(obj)) {
                obj.material = sceneState.originalMaterialsMap.get(obj);
            }
        });
        sceneState.originalMaterialsMap.clear();

        updateStatus('PBR materials restored');
    }
}

/**
 * Helper to update GUI controller display
 */
function updateGUIController(propertyName) {
    if (guiState.gui) {
        guiState.gui.controllersRecursive().forEach(controller => {
            if (controller.property === propertyName) {
                controller.updateDisplay();
            }
        });
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
    threeState.gridHelper = new THREE.GridHelper(settings.gridSize, settings.gridDivisions, 0x444444, 0x222222);
    threeState.gridHelper.visible = settings.showGrid;
    threeState.scene.add(threeState.gridHelper);
}

function createAxesHelper() {
    threeState.axesHelper = new THREE.AxesHelper(settings.gridSize / 2);
    threeState.axesHelper.visible = settings.showAxes;
    threeState.scene.add(threeState.axesHelper);
}

function updateHelpersSize() {
    if (sceneState.root) {
        const box = new THREE.Box3().setFromObject(sceneState.root);
        const size = box.getSize(new THREE.Vector3());
        const maxDim = Math.max(size.x, size.y, size.z);

        settings.gridSize = Math.ceil(maxDim * 2);
        settings.gridDivisions = Math.min(20, Math.max(10, Math.ceil(settings.gridSize)));

        if (threeState.gridHelper) {
            threeState.scene.remove(threeState.gridHelper);
            threeState.gridHelper.dispose();
            threeState.gridHelper = null;
            if (settings.showGrid) createGridHelper();
        }

        if (threeState.axesHelper) {
            threeState.scene.remove(threeState.axesHelper);
            threeState.axesHelper.dispose();
            threeState.axesHelper = null;
            if (settings.showAxes) createAxesHelper();
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
    await loadUSDFromData(data, 'default.usda');
}

async function loadDefaultUSDFile() {
    const defaultFile = './assets/fancy-teapot-mtlx.usdz';
    updateStatus(`Loading ${defaultFile}...`);
    try {
        const response = await fetch(defaultFile);
        if (!response.ok) {
            throw new Error(`Failed to fetch ${defaultFile}: ${response.statusText}`);
        }
        const arrayBuffer = await response.arrayBuffer();
        const data = new Uint8Array(arrayBuffer);
        await loadUSDFromData(data, defaultFile);
    } catch (error) {
        console.error(`Failed to load default USD file (${defaultFile}):`, error);
        updateStatus(`Failed to load ${defaultFile}, loading fallback scene...`);
        await loadDefaultScene();
    }
}

async function loadUSDFromFile(file) {
    updateStatus(`Loading: ${file.name}...`);
    try {
        const arrayBuffer = await file.arrayBuffer();
        const data = new Uint8Array(arrayBuffer);
        await loadUSDFromData(data, file.name);
    } catch (error) {
        console.error('Failed to load USD file:', error);
        updateStatus(`Error: ${error.message}`);
    }
}

async function loadUSDFromData(data, filename) {
    clearScene();

    loaderState.nativeLoader = new loaderState.loader.native_.LightUSDLoaderNative();

    const success = loaderState.nativeLoader.loadFromBinary(data, filename);
    if (!success) {
        updateStatus(`Failed to parse USD file: ${filename}`);
        return;
    }

    loadSceneMetadata();
    await buildSceneGraph();
    initUpAxisConversion();
    applyUpAxisConversion();
    fitCameraToScene();
    updateHelpersSize();
    updateMaterialUI();
    updateModelInfo();
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
 * Build scene graph from USD hierarchy
 */
async function buildSceneGraph() {
    sceneState.materialData = [];
    sceneState.materials = [];
    sceneState.textureCache.clear();

    const usdRootNode = loaderState.nativeLoader.getDefaultRootNode();
    if (!usdRootNode) {
        console.warn('No default root node found, falling back to flat mesh loading');
        await buildMeshesFallback();
        return;
    }

    const defaultMtl = new THREE.MeshPhysicalMaterial({
        color: 0x888888,
        roughness: 0.5,
        metalness: 0.0,
        envMap: threeState.envMap,
        envMapIntensity: settings.envMapIntensity
    });

    const options = {
        overrideMaterial: false,
        envMap: threeState.envMap,
        envMapIntensity: settings.envMapIntensity,
        preferredMaterialType: settings.materialType,
        textureCache: sceneState.textureCache
    };

    sceneState.root = await buildThreeNode(usdRootNode, defaultMtl, loaderState.nativeLoader, options);
    threeState.scene.add(sceneState.root);

    collectMaterialsFromScene();

    sceneState.root.traverse((child) => {
        if (child.isMesh) {
            child.userData.usdScene = loaderState.nativeLoader;
        }
    });

    const numMeshes = loaderState.nativeLoader.numMeshes();
    const numMaterials = sceneState.materials.length;
    const shaderModeLabel = settings.shaderMode === 'openpbr-tsl' ? 'OpenPBR TSL' : 'Physical';
    updateStatus(`Loaded: ${numMeshes} meshes, ${numMaterials} materials (${shaderModeLabel}) [WebGPU]`);
}

/**
 * Convert row-major matrix array to THREE.Matrix4
 */
function toMatrix4(a) {
    const m = new THREE.Matrix4();
    m.set(a[0], a[4], a[8], a[12],
          a[1], a[5], a[9], a[13],
          a[2], a[6], a[10], a[14],
          a[3], a[7], a[11], a[15]);
    return m;
}

/**
 * Convert USD mesh data to Three.js BufferGeometry
 */
function convertUsdMeshToThreeMesh(mesh) {
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(mesh.points, 3));

    if (mesh.faceVertexIndices && mesh.faceVertexIndices.length > 0) {
        geometry.setIndex(new THREE.BufferAttribute(mesh.faceVertexIndices, 1));
    }

    if (mesh.texcoords) {
        geometry.setAttribute('uv', new THREE.BufferAttribute(mesh.texcoords, 2));
    }

    if (mesh.normals) {
        if (mesh.normalsFormat === 'snorm8') {
            geometry.setAttribute('normal',
                new THREE.BufferAttribute(new Int8Array(mesh.normals), 3, true));
        } else if (mesh.normalsFormat === 'snorm16') {
            geometry.setAttribute('normal',
                new THREE.BufferAttribute(new Int16Array(mesh.normals), 3, true));
        } else {
            geometry.setAttribute('normal',
                new THREE.BufferAttribute(new Float32Array(mesh.normals), 3));
        }
    } else {
        geometry.computeVertexNormals();
    }

    if (mesh.vertexColors) {
        geometry.setAttribute('color', new THREE.BufferAttribute(mesh.vertexColors, 3));
    }

    if (mesh.tangents) {
        geometry.setAttribute('tangent', new THREE.BufferAttribute(mesh.tangents, 4));
    } else if (mesh.texcoords && (mesh.normals || geometry.attributes.normal)) {
        geometry.computeTangents();
    }

    if (mesh.doubleSided !== undefined) {
        geometry.userData['doubleSided'] = mesh.doubleSided;
    }

    if (mesh.submeshes && mesh.submeshes.length > 0) {
        geometry.userData['submeshes'] = mesh.submeshes;
    }

    return geometry;
}

/**
 * Setup mesh with materials
 */
async function setupMesh(mesh, defaultMtl, usdScene, options) {
    const geometry = convertUsdMeshToThreeMesh(mesh);

    if (options.overrideMaterial) {
        return new THREE.Mesh(geometry, defaultMtl);
    }

    const hasMaterial = mesh.materialId !== undefined && mesh.materialId >= 0;
    let usdMaterialData = null;

    if (hasMaterial) {
        if (typeof usdScene.getMaterialWithFormat === 'function') {
            const result = usdScene.getMaterialWithFormat(mesh.materialId, 'json');
            if (!result.error) {
                usdMaterialData = JSON.parse(result.data);
            }
        } else {
            usdMaterialData = usdScene.getMaterial(mesh.materialId);
        }
    }

    let pbrMaterial;
    if (usdMaterialData) {
        pbrMaterial = await convertMaterial(usdMaterialData, usdScene, {
            preferredMaterialType: options.preferredMaterialType || 'auto',
            envMap: options.envMap || null,
            envMapIntensity: options.envMapIntensity || 1.0,
            textureCache: options.textureCache || new Map(),
            doubleSided: geometry.userData['doubleSided']
        });

        pbrMaterial.userData.rawData = usdMaterialData;
        pbrMaterial.userData.typeInfo = getMaterialType(usdMaterialData);
        pbrMaterial.userData.typeString = getMaterialTypeString(usdMaterialData);
    } else {
        pbrMaterial = defaultMtl || new THREE.MeshPhysicalMaterial({
            color: 0x888888,
            roughness: 0.5,
            metalness: 0.0
        });
    }

    pbrMaterial.envMap = options.envMap || null;
    pbrMaterial.envMapIntensity = options.envMapIntensity || 1.0;

    if (geometry.userData.doubleSided !== undefined) {
        pbrMaterial.side = geometry.userData.doubleSided ? THREE.DoubleSide : THREE.FrontSide;
    } else {
        pbrMaterial.side = THREE.FrontSide;
    }

    // Handle GeomSubsets (multi-material)
    if (geometry.userData['submeshes'] && geometry.userData['submeshes'].length > 0) {
        const submeshes = geometry.userData['submeshes'];
        const materials = [];
        const materialIdToIndex = new Map();

        for (const submesh of submeshes) {
            if (!materialIdToIndex.has(submesh.materialId)) {
                materialIdToIndex.set(submesh.materialId, materials.length);
                materials.push(null);
            }
        }

        for (const [matId, matIndex] of materialIdToIndex.entries()) {
            if (matId >= 0) {
                const materialData = usdScene.getMaterialWithFormat ?
                    JSON.parse(usdScene.getMaterialWithFormat(matId, 'json').data) :
                    usdScene.getMaterial(matId);

                const material = await convertMaterial(materialData, usdScene, {
                    preferredMaterialType: options.preferredMaterialType || 'auto',
                    envMap: options.envMap || null,
                    envMapIntensity: options.envMapIntensity || 1.0,
                    textureCache: options.textureCache || new Map()
                });

                material.envMap = options.envMap || null;
                material.envMapIntensity = options.envMapIntensity || 1.0;
                material.side = geometry.userData['doubleSided'] ? THREE.DoubleSide : THREE.FrontSide;
                material.userData.rawData = materialData;
                material.userData.typeInfo = getMaterialType(materialData);
                material.userData.typeString = getMaterialTypeString(materialData);

                materials[matIndex] = material;
            } else {
                materials[matIndex] = pbrMaterial;
            }
        }

        for (const submesh of submeshes) {
            const matIndex = materialIdToIndex.get(submesh.materialId);
            geometry.addGroup(submesh.start, submesh.count, matIndex);
        }

        return new THREE.Mesh(geometry, materials);
    }

    return new THREE.Mesh(geometry, pbrMaterial);
}

/**
 * Build Three.js node from USD node hierarchy
 */
async function buildThreeNode(usdNode, defaultMtl, usdScene, options) {
    let node = new THREE.Group();

    if (usdNode.nodeType === 'xform') {
        const matrix = toMatrix4(usdNode.localMatrix);
        node.applyMatrix4(matrix);
    } else if (usdNode.nodeType === 'mesh') {
        const mesh = usdScene.getMeshCopy(usdNode.contentId);
        const threeMesh = await setupMesh(mesh, defaultMtl, usdScene, options);
        node = threeMesh;

        if (usdNode.localMatrix) {
            const matrix = toMatrix4(usdNode.localMatrix);
            node.applyMatrix4(matrix);
        }
    } else {
        if (usdNode.localMatrix) {
            const matrix = toMatrix4(usdNode.localMatrix);
            node.applyMatrix4(matrix);
        }
    }

    node.name = usdNode.primName;
    node.userData['primMeta.displayName'] = usdNode.displayName;
    node.userData['primMeta.absPath'] = usdNode.absPath;

    if (usdNode.children) {
        for (const child of usdNode.children) {
            const childNode = await buildThreeNode(child, defaultMtl, usdScene, options);
            node.add(childNode);
        }
    }

    return node;
}

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
    sceneState.materialData = sceneState.materials.map(mat => mat.userData?.rawData || null);
}

async function buildMeshesFallback() {
    const numMeshes = loaderState.nativeLoader.numMeshes();
    const numMaterials = loaderState.nativeLoader.numMaterials();

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
        const mat = await convertMaterial(sceneState.materialData[i], loaderState.nativeLoader, {
            preferredMaterialType: settings.materialType,
            envMap: threeState.envMap,
            envMapIntensity: settings.envMapIntensity,
            textureCache: sceneState.textureCache
        });
        sceneState.materials.push(mat);
    }

    sceneState.root = new THREE.Group();
    threeState.scene.add(sceneState.root);

    for (let i = 0; i < numMeshes; i++) {
        const meshData = loaderState.nativeLoader.getMeshCopy(i);
        if (!meshData) continue;

        const geometry = convertUsdMeshToThreeMesh(meshData);
        if (!geometry) continue;

        const material = (meshData.materialId !== undefined && meshData.materialId >= 0 && meshData.materialId < sceneState.materials.length)
            ? sceneState.materials[meshData.materialId]
            : new THREE.MeshPhysicalMaterial({ color: 0x888888 });

        const mesh = new THREE.Mesh(geometry, material);
        mesh.name = meshData.name || `Mesh_${i}`;
        sceneState.root.add(mesh);
    }

    const shaderModeLabel = settings.shaderMode === 'openpbr-tsl' ? 'OpenPBR TSL' : 'Physical';
    updateStatus(`Loaded: ${numMeshes} meshes, ${numMaterials} materials (${shaderModeLabel}) [WebGPU fallback]`);
}

async function reloadMaterials() {
    if (!sceneState.root) return;

    updateStatus('Reloading materials...');

    const oldToNewMaterialMap = new Map();
    const newMaterialSet = new Set();

    for (const mat of sceneState.materials) {
        if (oldToNewMaterialMap.has(mat)) continue;

        const rawData = mat.userData?.rawData;
        if (rawData) {
            const newMat = await convertMaterial(rawData, loaderState.nativeLoader, {
                preferredMaterialType: settings.materialType,
                envMap: threeState.envMap,
                envMapIntensity: settings.envMapIntensity,
                textureCache: sceneState.textureCache
            });
            oldToNewMaterialMap.set(mat, newMat);
            newMaterialSet.add(newMat);
        } else {
            oldToNewMaterialMap.set(mat, mat);
            newMaterialSet.add(mat);
        }
    }

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

    for (const [oldMat, newMat] of oldToNewMaterialMap) {
        if (oldMat !== newMat && oldMat.dispose) {
            oldMat.dispose();
        }
    }

    sceneState.materials = Array.from(newMaterialSet);
    sceneState.materialData = sceneState.materials.map(mat => mat.userData?.rawData || null);

    updateMaterialUI();
    updateStatus('Materials reloaded');
}

function clearScene() {
    if (sceneState.showingNormals && sceneState.root) {
        sceneState.root.traverse(obj => {
            if (obj.isMesh && obj.material && obj.material.dispose) {
                obj.material.dispose();
            }
        });
    }
    sceneState.originalMaterialsMap.clear();
    sceneState.showingNormals = false;

    if (settings.showNormals) {
        settings.showNormals = false;
        updateGUIController('showNormals');
    }

    if (settings.useBasicMaterial) {
        settings.useBasicMaterial = false;
        updateGUIController('useBasicMaterial');
    }

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

    sceneState.textureCache.forEach(texture => {
        if (texture && texture.dispose) texture.dispose();
    });
    sceneState.textureCache.clear();

    clearSelectionHighlight();
    pickState.selectedObject = null;

    sceneState.upAxis = 'Y';
    sceneState.metadata = null;
    sceneState.domeLightData = null;

    if (loaderState.nativeLoader) {
        try {
            loaderState.nativeLoader.reset();
        } catch (e) {
            try {
                loaderState.nativeLoader.clearAssets();
            } catch (e2) {}
        }
        loaderState.nativeLoader = null;
    }
}

function fitCameraToScene() {
    if (!sceneState.root) return;

    const box = new THREE.Box3().setFromObject(sceneState.root);
    const size = box.getSize(new THREE.Vector3());
    const center = box.getCenter(new THREE.Vector3());

    const boundingSphereRadius = size.length() * 0.5;
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

    threeState.camera.near = Math.max(0.1, cameraDistance / 100);
    threeState.camera.far = Math.max(1000, cameraDistance * 10);
    threeState.camera.updateProjectionMatrix();

    threeState.controls.update();
}

// ============================================================================
// Material UI
// ============================================================================

function updateMaterialUI() {
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

    let materialsToShow = [];
    let headerText = '';
    const meshCount = countMeshes();

    if (pickState.selectedObject) {
        materialsToShow = getMaterialsFromObject(pickState.selectedObject);
        const objName = pickState.selectedObject.name || 'Unnamed';
        headerText = `Selected: ${objName}`;
    } else if (meshCount === 1) {
        sceneState.root?.traverse(obj => {
            if (obj.isMesh && materialsToShow.length === 0) {
                materialsToShow = getMaterialsFromObject(obj);
            }
        });
        headerText = 'Single Mesh';
    } else {
        materialsToShow = [];
        headerText = 'Click object to select';
    }

    if (headerText) {
        guiState.materialFolder.add({ info: headerText }, 'info').name('Showing').disable();
    }

    materialsToShow.forEach((mat, index) => {
        const globalIndex = sceneState.materials.indexOf(mat);
        const matName = globalIndex >= 0 ? `Material ${globalIndex}` : `Material ${index}`;
        const matFolder = guiState.materialFolder.addFolder(matName);
        addMaterialControls(matFolder, mat);
    });

    if (materialsToShow.length === 0) {
        guiState.materialFolder.add({ info: 'No materials' }, 'info').name('Status').disable();
    }
}

function addMaterialControls(folder, mat) {
    const rawData = mat.userData?.rawData;
    const materialName = rawData?.name || rawData?.materialName || mat.name || 'Unnamed';
    folder.add({ name: materialName }, 'name').name('Name').disable();

    const typeString = mat.userData.typeString || 'Unknown';
    folder.add({ type: typeString }, 'type').name('Type').disable();

    // Check if this is an OpenPBRNodeMaterial (TSL)
    const isOpenPBRTSL = mat.isOpenPBRNodeMaterial === true;

    if (isOpenPBRTSL) {
        // OpenPBR TSL Material controls
        addOpenPBRTSLControls(folder, mat);
    } else {
        // Standard MeshPhysicalMaterial controls
        addPhysicalMaterialControls(folder, mat);
    }
}

/**
 * Add controls for OpenPBRNodeMaterial (TSL)
 * Uses OpenPBR spec parameter names (e.g. base_color, specular_roughness)
 */
function addOpenPBRTSLControls(folder, mat) {
    // Base layer
    if (mat.color) {
        const displayColor = linearToSRGB(mat.color.clone());
        const colorObj = { color: '#' + displayColor.getHexString() };
        folder.addColor(colorObj, 'color').name('base_color').onChange(v => {
            mat.color.copy(sRGBToLinear(new THREE.Color(v)));
            mat.needsUpdate = true;
        });
    }

    if (mat._openPBR) {
        const proxy = { value: mat._openPBR.base_weight };
        folder.add(proxy, 'value', 0, 1, 0.01).name('base_weight').onChange(v => {
            mat._openPBR.base_weight = v;
        });
    }

    if (mat.metalness !== undefined) {
        folder.add(mat, 'metalness', 0, 1, 0.01).name('base_metalness');
    }

    if (mat._openPBR) {
        const proxy = { value: mat._openPBR.base_diffuse_roughness };
        folder.add(proxy, 'value', 0, 1, 0.01).name('base_diffuse_roughness').onChange(v => {
            mat._openPBR.base_diffuse_roughness = v;
        });
    }

    // Specular layer
    if (mat._openPBR) {
        const proxy = { value: mat._openPBR.specular_weight };
        folder.add(proxy, 'value', 0, 1, 0.01).name('specular_weight').onChange(v => {
            mat._openPBR.specular_weight = v;
        });
    }

    if (mat.roughness !== undefined) {
        folder.add(mat, 'roughness', 0, 1, 0.01).name('specular_roughness');
    }

    if (mat.ior !== undefined) {
        folder.add(mat, 'ior', 1, 3, 0.01).name('specular_ior');
    }

    // Coat layer
    if (mat.clearcoat !== undefined) {
        folder.add(mat, 'clearcoat', 0, 1, 0.01).name('coat_weight');
    }

    if (mat.clearcoatRoughness !== undefined) {
        folder.add(mat, 'clearcoatRoughness', 0, 1, 0.01).name('coat_roughness');
    }

    // Fuzz layer (OpenPBR uses fuzz, mapped to Three.js sheen)
    if (mat.sheen !== undefined) {
        folder.add(mat, 'sheen', 0, 1, 0.01).name('fuzz_weight');
    }

    // Thin film (OpenPBR thin_film, mapped to Three.js iridescence)
    if (mat.iridescence !== undefined) {
        folder.add(mat, 'iridescence', 0, 1, 0.01).name('thin_film_weight');
    }

    // Transmission
    if (mat.transmission !== undefined) {
        folder.add(mat, 'transmission', 0, 1, 0.01).name('transmission_weight');
    }

    // Emission
    if (mat.emissive) {
        const displayEmissive = linearToSRGB(mat.emissive.clone());
        const emissiveObj = { emissive: '#' + displayEmissive.getHexString() };
        folder.addColor(emissiveObj, 'emissive').name('emission_color').onChange(v => {
            mat.emissive.copy(sRGBToLinear(new THREE.Color(v)));
            mat.needsUpdate = true;
        });
    }

    if (mat.emissiveIntensity !== undefined) {
        folder.add(mat, 'emissiveIntensity', 0, 100, 0.1).name('emission_luminance');
    }

    // Geometry - opacity
    if (mat.opacity !== undefined) {
        folder.add(mat, 'opacity', 0, 1, 0.01).name('geometry_opacity').onChange(v => {
            mat.transparent = v < 1.0;
        });
    }
}

/**
 * Add controls for standard MeshPhysicalMaterial
 */
function addPhysicalMaterialControls(folder, mat) {
    if (mat.color) {
        const displayColor = linearToSRGB(mat.color.clone());
        const colorObj = { color: '#' + displayColor.getHexString() };
        folder.addColor(colorObj, 'color').name('Base Color (sRGB)').onChange(v => {
            mat.color.copy(sRGBToLinear(new THREE.Color(v)));
        });
    }

    if (mat.metalness !== undefined) {
        folder.add(mat, 'metalness', 0, 1, 0.01).name('Metalness');
    }

    if (mat.roughness !== undefined) {
        folder.add(mat, 'roughness', 0, 1, 0.01).name('Roughness');
    }

    if (mat.ior !== undefined) {
        folder.add(mat, 'ior', 1, 3, 0.01).name('IOR');
    }

    if (mat.clearcoat !== undefined) {
        folder.add(mat, 'clearcoat', 0, 1, 0.01).name('Clearcoat');
    }

    if (mat.clearcoatRoughness !== undefined) {
        folder.add(mat, 'clearcoatRoughness', 0, 1, 0.01).name('Clearcoat Roughness');
    }

    if (mat.transmission !== undefined) {
        folder.add(mat, 'transmission', 0, 1, 0.01).name('Transmission');
    }

    if (mat.sheen !== undefined) {
        folder.add(mat, 'sheen', 0, 1, 0.01).name('Sheen');
    }

    if (mat.iridescence !== undefined) {
        folder.add(mat, 'iridescence', 0, 1, 0.01).name('Iridescence');
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
}

function updateModelInfo() {
    const numMeshes = loaderState.nativeLoader.numMeshes();
    const numMaterials = loaderState.nativeLoader.numMaterials();

    document.getElementById('model-info').style.display = 'block';
    document.getElementById('mesh-count').textContent = numMeshes;
    document.getElementById('material-count').textContent = numMaterials;
}

// ============================================================================
// Object Picking
// ============================================================================

function onCanvasClick(event) {
    if (event.target !== threeState.renderer.domElement) return;

    const rect = threeState.renderer.domElement.getBoundingClientRect();
    pickState.mouse.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
    pickState.mouse.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;

    pickState.raycaster.setFromCamera(pickState.mouse, threeState.camera);

    if (!sceneState.root) return;

    const intersects = pickState.raycaster.intersectObjects(sceneState.root.children, true);

    if (intersects.length > 0) {
        const hit = intersects.find(i => i.object.isMesh);
        if (hit) {
            selectObject(hit.object);
        } else {
            clearSelection();
        }
    } else {
        clearSelection();
    }
}

function selectObject(object) {
    clearSelectionHighlight();

    pickState.selectedObject = object;

    const box = new THREE.Box3().setFromObject(object);
    const helper = new THREE.Box3Helper(box, 0x00ff00);
    helper.name = '__selectionHelper__';
    threeState.scene.add(helper);
    pickState.selectionHelper = helper;

    updateMaterialUI();

    const objName = object.name || 'Unnamed';
    const absPath = object.userData['primMeta.absPath'] || '';
    updateStatus(`Selected: ${objName}${absPath ? ' (' + absPath + ')' : ''}`);
}

function clearSelectionHighlight() {
    if (pickState.selectionHelper) {
        threeState.scene.remove(pickState.selectionHelper);
        pickState.selectionHelper.dispose();
        pickState.selectionHelper = null;
    }
}

function clearSelection() {
    clearSelectionHighlight();
    pickState.selectedObject = null;
    updateMaterialUI();
    updateStatus('Selection cleared');
}

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
    threeState.controls.update();
    threeState.renderer.render(threeState.scene, threeState.camera);
}

// ============================================================================
// Start
// ============================================================================

init().catch(err => {
    console.error('Initialization failed:', err);
    updateStatus('Initialization failed: ' + err.message);
});
