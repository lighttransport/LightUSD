// offscreengl.worker.js — Dedicated Web Worker that owns the WebGL context.
//
// CRITICAL: The document polyfill MUST be at the top of this file.
// LightUSDLoaderUtils calls document.createElement('canvas') in texture helpers.
// We stub it with OffscreenCanvas so those calls succeed inside the worker.
//
// NOTE: With Vite bundling, this code executes before any module side-effects that
// use `document`, because module functions only call document lazily (not at import
// evaluation time). The polyfill is therefore in place before we call any such function.

if (typeof document === 'undefined') {
    globalThis.document = {
        createElement(tag) {
            if (tag === 'canvas') return new OffscreenCanvas(1, 1);
            return {
                style: {},
                appendChild() {},
                setAttribute() {},
                addEventListener() {},
                removeEventListener() {},
            };
        },
        // THREE.ImageLoader (and others) may call createElementNS for <img>.
        // We return a minimal stub; the actual ImageLoader.load is patched below
        // to use fetch+createImageBitmap instead of HTMLImageElement.
        createElementNS(_ns, tag) {
            return this.createElement(tag);
        },
    };
}

import * as THREE from 'three';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { LightUSDLoader, NextRenderSceneAdapter } from './src/lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from './src/lightusd/LightUSDLoaderUtils.js';
import { setLightUSD as setMaterialXLightUSD } from './src/lightusd/LightUSDMaterialX.js';
import { buildNextThreeNode } from './src/lightusd/NextRenderSceneUtils.js';

// ─── Worker-compatible image loading patch ─────────────────────────────────
// THREE.ImageLoader normally calls document.createElementNS('...', 'img')
// to create an HTMLImageElement — unavailable in Web Workers.
// We replace the load method with fetch + createImageBitmap, both of which
// ARE available in dedicated workers. THREE.WebGLTextures already handles
// ImageBitmap objects on the GPU-upload path (since r118).

THREE.ImageLoader.prototype.load = function (url, onLoad, _onProgress, onError) {
    fetch(url)
        .then(r => {
            if (!r.ok) throw new Error(`HTTP ${r.status} loading ${url}`);
            return r.blob();
        })
        .then(blob => createImageBitmap(blob, {
            // Chrome's WebGL ignores UNPACK_FLIP_Y_WEBGL for ImageBitmap uploads.
            // Pre-flip here so the bitmap is already in bottom-to-top GPU order,
            // matching what HTMLImageElement + UNPACK_FLIP_Y_WEBGL=true produces on
            // the main thread (Three.js default: flipY=true for all Texture objects).
            imageOrientation: 'flipY',
            colorSpaceConversion: 'none',
        }))
        .then(bitmap => { if (onLoad) onLoad(bitmap); })
        .catch(err => {
            console.warn('[Worker] ImageLoader error:', err);
            if (onError) onError(err);
        });
};

// ─── Constants ─────────────────────────────────────────────────────────────

// Use 32-bit WASM (false) for widest browser compatibility.
// Set to true only if you need >2 GB heap and are on a memory64-capable browser.
const USE_MEMORY64    = false;

const CAMERA_FOV      = 45;
const CAMERA_NEAR     = 0.1;
const CAMERA_FAR      = 1000;
const CAMERA_PADDING  = 1.2;
const BG_COLOR        = 0x1a1a1a;

// ─── State ─────────────────────────────────────────────────────────────────

// Three.js objects
const three = {
    renderer: null,
    scene: null,
    camera: null,
    pmremGenerator: null,
    envMap: null,
};

// Loader state
const loaderState = {
    loader: null,
    nativeLoader: null,
};

// Scene state
const sceneState = {
    root: null,
    materials: [],
    textureCache: new Map(),
    upAxis: 'Y',
};

// Manual spherical-coordinate orbit camera state
// theta: azimuth (horizontal), phi: polar (vertical), radius: distance
const orbitState = {
    theta: 0.3,
    phi: 1.2,
    radius: 5,
    target: new THREE.Vector3(0, 0, 0),
    isDragging: false,
    isPanning: false,
    lastX: 0,
    lastY: 0,
};

// ─── Main message dispatch ─────────────────────────────────────────────────

self.addEventListener('message', async (e) => {
    const msg = e.data;
    switch (msg.type) {
        case 'init':
            await handleInit(msg);
            break;
        case 'resize':
            handleResize(msg);
            break;
        case 'pointerdown':
            handlePointerDown(msg);
            break;
        case 'pointermove':
            handlePointerMove(msg);
            break;
        case 'pointerup':
            handlePointerUp(msg);
            break;
        case 'wheel':
            handleWheel(msg);
            break;
        case 'loadFile':
            await handleLoadFile(msg);
            break;
        default:
            console.warn('[Worker] Unknown message type:', msg.type);
    }
});

// ─── Init ──────────────────────────────────────────────────────────────────

async function handleInit({ canvas, width, height, pixelRatio, testMode = false }) {
    sendStatus('Initializing renderer…');
    try {
        initThreeJS(canvas, width, height, pixelRatio);
    } catch (err) {
        sendError(`WebGL init failed: ${err.message}`);
        return;
    }

    sendStatus('Initializing LightUSD WASM…');
    try {
        await initLoader();
    } catch (err) {
        sendError(`WASM init failed: ${err.message}`);
        return;
    }

    sendStatus('Loading environment…');
    await loadEnvironment();

    if (testMode) {
        sendStatus('Ready for test input');
    } else {
        sendStatus('Loading default scene…');
        await loadDefaultUSDFile();
    }

    animate();
}

// ─── Three.js setup ────────────────────────────────────────────────────────

function initThreeJS(offscreenCanvas, width, height, pixelRatio) {
    three.scene = new THREE.Scene();
    three.scene.background = new THREE.Color(BG_COLOR);

    three.camera = new THREE.PerspectiveCamera(CAMERA_FOV, width / height, CAMERA_NEAR, CAMERA_FAR);
    three.camera.position.set(3, 2, 5);

    three.renderer = new THREE.WebGLRenderer({
        canvas: offscreenCanvas,
        antialias: true,
    });

    // false = do NOT touch canvas.style (OffscreenCanvas has no style property)
    three.renderer.setSize(width, height, false);
    three.renderer.setPixelRatio(pixelRatio);
    three.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    three.renderer.toneMappingExposure = 1.0;
    three.renderer.outputColorSpace = THREE.SRGBColorSpace;

    three.pmremGenerator = new THREE.PMREMGenerator(three.renderer);
    three.pmremGenerator.compileEquirectangularShader();
}

// ─── LightUSD WASM loader setup ────────────────────────────────────────────

async function initLoader() {
    // The worker renders through the next backend: the next-only module and
    // its RenderStream, presented to the scene builders by
    // NextRenderSceneAdapter (the legacy loader interface).
    loaderState.loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512, backend: 'next' });
    await loaderState.loader.init({ useMemory64: USE_MEMORY64 });

    const wasmModule = loaderState.loader.native_;
    LightUSDLoaderUtils.setLightUSD(wasmModule);
    setMaterialXLightUSD(wasmModule);
}

// ─── Environment (HDR) ─────────────────────────────────────────────────────

async function loadEnvironment() {
    try {
        const hdrLoader = new HDRLoader();
        const texture = await hdrLoader.loadAsync('./assets/textures/goegap_1k.hdr');
        three.envMap = three.pmremGenerator.fromEquirectangular(texture).texture;
        texture.dispose();
    } catch {
        // Fallback: gradient OffscreenCanvas as equirectangular env
        three.envMap = createGradientEnv();
    }
    applyEnv();
}

function createGradientEnv() {
    const oc = new OffscreenCanvas(256, 256);
    const ctx = oc.getContext('2d');
    const grad = ctx.createLinearGradient(0, 0, 0, 256);
    grad.addColorStop(0, '#ffffff');
    grad.addColorStop(0.5, '#cccccc');
    grad.addColorStop(1, '#666666');
    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, 256, 256);

    // Wrap OffscreenCanvas as ImageBitmap for THREE.CanvasTexture
    const texture = new THREE.CanvasTexture(oc);
    texture.mapping = THREE.EquirectangularReflectionMapping;
    return three.pmremGenerator.fromEquirectangular(texture).texture;
}

function applyEnv() {
    three.scene.environment = three.envMap;
    three.scene.background = three.envMap;
    sceneState.materials.forEach(mat => {
        mat.envMap = three.envMap;
        mat.needsUpdate = true;
    });
}

// ─── Default USD file loading ──────────────────────────────────────────────

const DEFAULT_USDZ_PATH = './assets/fancy-teapot-mtlx.usdz';

// Inline fallback USDA — metallic gold sphere with OpenPBR material
const FALLBACK_USDA = `#usda 1.0
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
                color3f inputs:base_color = (0.9, 0.7, 0.3)
                float inputs:base_metalness = 0.8
                float inputs:base_weight = 1.0
                float inputs:specular_roughness = 0.3
                float inputs:specular_ior = 1.5
                float inputs:specular_weight = 1.0
                token outputs:surface
            }
        }
    }
}
`;

async function loadDefaultUSDFile() {
    try {
        const response = await fetch(DEFAULT_USDZ_PATH);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const arrayBuffer = await response.arrayBuffer();
        await loadUSDFromData(new Uint8Array(arrayBuffer), DEFAULT_USDZ_PATH);
    } catch (err) {
        console.warn('[Worker] Failed to load default file, using fallback:', err);
        const encoder = new TextEncoder();
        await loadUSDFromData(encoder.encode(FALLBACK_USDA), 'fallback.usda');
    }
}

// ─── Generic USD data loading ──────────────────────────────────────────────

async function loadUSDFromData(data, filename) {
    clearScene();

    sendStatus(`Parsing: ${filename}…`);

    try {
        loaderState.nativeLoader = await NextRenderSceneAdapter.create(
            loaderState.loader.native_, data, filename);
    } catch (err) {
        sendError(`Failed to parse USD file: ${filename}: ${err?.message || err}`);
        return;
    }

    // Read up-axis from metadata
    const metadata = loaderState.nativeLoader.getSceneMetadata
        ? loaderState.nativeLoader.getSceneMetadata()
        : {};
    sceneState.upAxis = metadata.upAxis || 'Y';

    sendStatus('Building scene graph…');
    await buildSceneGraph();

    // Try to load DomeLight environment
    try {
        const result = await LightUSDLoaderUtils.loadDomeLightFromUSD(
            loaderState.nativeLoader,
            three.pmremGenerator
        );
        if (result) {
            three.envMap = result.texture;
            applyEnv();
        }
    } catch (err) {
        console.warn('[Worker] DomeLight load error (non-fatal):', err);
    }

    // Apply Z-up → Y-up rotation if needed
    if (sceneState.root && sceneState.upAxis === 'Z') {
        sceneState.root.rotation.x = -Math.PI / 2;
    }

    fitCameraToScene();

    const numMeshes = loaderState.nativeLoader.numMeshes();
    const numMats = sceneState.materials.length;
    const sceneEntityCounts = readSceneEntityCounts(loaderState.nativeLoader);

    sendLoaded(numMeshes, numMats, sceneState.upAxis, sceneEntityCounts);
}

// ─── Scene graph construction ──────────────────────────────────────────────

async function buildSceneGraph() {
    sceneState.materials = [];
    sceneState.textureCache.clear();

    // The next adapter carries next render records (materials, renderables
    // attached by prim path), so build with the next scene builder rather
    // than the legacy node-tree converter.
    const built = buildNextThreeNode(loaderState.nativeLoader, {
        skipTextures: false,
        lazyTextures: true,
    });
    sceneState.root = built.node;
    three.scene.add(sceneState.root);

    // Collect materials from scene graph
    const matSet = new Set();
    sceneState.root.traverse(obj => {
        if (obj.isMesh && obj.material) {
            const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
            mats.forEach(m => matSet.add(m));
        }
    });
    sceneState.materials = Array.from(matSet);
    sceneState.materials.forEach(mat => {
        mat.envMap = three.envMap;
        mat.needsUpdate = true;
    });

    if (built.textureManager && built.textureManager.total > 0) {
        await built.textureManager.startLoading({
            onTextureLoaded: (material) => { material.needsUpdate = true; },
            concurrency: 16,
            yieldInterval: 16,
        });
    }
}

// ─── Scene cleanup / dispose ───────────────────────────────────────────────

function clearScene() {
    if (sceneState.root) {
        sceneState.root.traverse(obj => {
            if (obj.isMesh) {
                obj.geometry?.dispose();
                const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
                mats.forEach(m => m?.dispose());
            }
        });
        three.scene.remove(sceneState.root);
        sceneState.root = null;
    }

    sceneState.materials = [];
    sceneState.textureCache.forEach(t => t?.dispose());
    sceneState.textureCache.clear();

    if (loaderState.nativeLoader) {
        try { loaderState.nativeLoader.delete(); } catch (_) { /* already released */ }
        loaderState.nativeLoader = null;
    }
}

// ─── Camera: fit to scene bounding sphere ─────────────────────────────────

function fitCameraToScene() {
    if (!sceneState.root) return;

    // Box3.setFromObject reads child world matrices, which are stale until
    // the first render; refresh them so scaled prims are measured correctly.
    sceneState.root.updateMatrixWorld(true);
    const box = new THREE.Box3().setFromObject(sceneState.root);
    if (box.isEmpty()) return;

    const size   = box.getSize(new THREE.Vector3());
    const center = box.getCenter(new THREE.Vector3());

    const sphereR = size.length() * 0.5;
    const fovRad  = CAMERA_FOV * (Math.PI / 180);
    const aspect  = three.camera.aspect;
    const hFov    = 2 * Math.atan(Math.tan(fovRad / 2) * aspect);
    const effFov  = Math.min(fovRad, hFov);
    const dist    = (sphereR / Math.sin(effFov / 2)) * CAMERA_PADDING;

    // Update orbit state (camera position derived from orbit in updateCamera)
    orbitState.target.copy(center);
    orbitState.radius = dist;
    orbitState.theta  = 0.3;
    orbitState.phi    = 1.2;

    three.camera.near = Math.max(0.01, dist / 100);
    three.camera.far  = Math.max(1000, dist * 10);
    three.camera.updateProjectionMatrix();

    updateCamera();
}

// ─── Manual orbit camera update ────────────────────────────────────────────

function updateCamera() {
    const { theta, phi, radius, target } = orbitState;
    const sinPhi = Math.sin(phi);
    const cosPhi = Math.cos(phi);
    const sinTheta = Math.sin(theta);
    const cosTheta = Math.cos(theta);

    three.camera.position.set(
        target.x + radius * sinPhi * sinTheta,
        target.y + radius * cosPhi,
        target.z + radius * sinPhi * cosTheta,
    );
    three.camera.lookAt(target);
}

// ─── Pointer / orbit / pan handlers ───────────────────────────────────────

function handlePointerDown({ x, y, button }) {
    orbitState.isDragging = button === 0;
    orbitState.isPanning  = button === 2;
    orbitState.lastX = x;
    orbitState.lastY = y;
}

function handlePointerMove({ x, y, buttons }) {
    if (buttons === 0) {
        orbitState.isDragging = false;
        orbitState.isPanning  = false;
        return;
    }

    const dx = x - orbitState.lastX;
    const dy = y - orbitState.lastY;
    orbitState.lastX = x;
    orbitState.lastY = y;

    if (orbitState.isDragging && buttons & 1) {
        // Left-drag: orbit
        orbitState.theta -= dx * 0.01;
        orbitState.phi    = Math.max(0.01, Math.min(Math.PI - 0.01, orbitState.phi + dy * 0.01));
    } else if (orbitState.isPanning && buttons & 2) {
        // Right-drag: pan (speed proportional to distance for consistent feel)
        const panSpeed = orbitState.radius * 0.001;
        const right = new THREE.Vector3();
        const up    = new THREE.Vector3();
        three.camera.getWorldDirection(new THREE.Vector3()); // dummy call to ensure matrix is up-to-date
        right.crossVectors(
            three.camera.getWorldDirection(new THREE.Vector3()).negate(),
            three.camera.up
        ).normalize();
        up.copy(three.camera.up).normalize();

        orbitState.target.addScaledVector(right, -dx * panSpeed);
        orbitState.target.addScaledVector(up,     dy * panSpeed);
    }

    updateCamera();
}

function handlePointerUp(_msg) {
    orbitState.isDragging = false;
    orbitState.isPanning  = false;
}

// ─── Wheel (zoom) ──────────────────────────────────────────────────────────

function handleWheel({ deltaY }) {
    const zoomFactor = deltaY > 0 ? 1.1 : (1 / 1.1);
    orbitState.radius = Math.max(0.01, orbitState.radius * zoomFactor);
    updateCamera();
}

// ─── Resize ────────────────────────────────────────────────────────────────

function handleResize({ width, height, pixelRatio }) {
    three.camera.aspect = width / height;
    three.camera.updateProjectionMatrix();
    // false = do NOT touch canvas.style
    three.renderer.setSize(width, height, false);
    three.renderer.setPixelRatio(pixelRatio);
}

// ─── File drop handler ─────────────────────────────────────────────────────

async function handleLoadFile({ data, filename }) {
    sendStatus(`Loading: ${filename}…`);
    try {
        await loadUSDFromData(new Uint8Array(data), filename);
    } catch (err) {
        sendError(`Failed to load ${filename}: ${err.message}`);
        console.error('[Worker] loadFile error:', err);
    }
}

// ─── Render loop ───────────────────────────────────────────────────────────

function animate() {
    // requestAnimationFrame is available in dedicated workers with OffscreenCanvas
    // (Chrome 69+, Firefox 105+). Fallback to setTimeout for broader support.
    if (typeof requestAnimationFrame === 'function') {
        requestAnimationFrame(animate);
    } else {
        setTimeout(animate, 16);
    }

    three.renderer.render(three.scene, three.camera);
}

// ─── Helpers: postMessage shortcuts ───────────────────────────────────────

function sendStatus(message) {
    console.log('[Worker]', message);
    self.postMessage({ type: 'status', message });
}

function sendError(message) {
    console.error('[Worker] ERROR:', message);
    self.postMessage({ type: 'error', message });
}

function readSceneEntityCounts(usd) {
    const safeNumber = (fn) => {
        if (typeof fn !== 'function') return 0;
        try {
            const value = fn();
            return Number.isFinite(value) ? value : 0;
        } catch {
            return 0;
        }
    };
    const safeArrayCount = (fn) => {
        if (typeof fn !== 'function') return 0;
        try {
            const value = fn();
            return Array.isArray(value) ? value.length : 0;
        } catch {
            return 0;
        }
    };
    return {
        lights: safeNumber(usd?.numLights && usd.numLights.bind(usd)),
        cameras: safeNumber(usd?.numCameras && usd.numCameras.bind(usd)),
        nodes: safeNumber(usd?.numNodes && usd.numNodes.bind(usd)),
        pointInstancers: safeNumber(usd?.numPointInstancers && usd.numPointInstancers.bind(usd)),
        pointInstanceDraws: safeNumber(usd?.numPointInstanceDraws && usd.numPointInstanceDraws.bind(usd)),
        skeletons: safeNumber(usd?.numSkeletons && usd.numSkeletons.bind(usd)),
        animations: safeNumber(usd?.numAnimations && usd.numAnimations.bind(usd)),
        unsupportedRenderables: safeNumber(usd?.numUnsupportedRenderables && usd.numUnsupportedRenderables.bind(usd)) ||
            safeArrayCount(usd?.getUnsupportedRenderables && usd.getUnsupportedRenderables.bind(usd))
    };
}

function sendLoaded(meshCount, materialCount, upAxis, sceneEntityCounts = {}) {
    console.log(
        `[Worker] Loaded: ${meshCount} meshes, ${materialCount} materials, ` +
        `lights=${sceneEntityCounts.lights || 0}, cameras=${sceneEntityCounts.cameras || 0}, nodes=${sceneEntityCounts.nodes || 0}, ` +
        `pointInstancers=${sceneEntityCounts.pointInstancers || 0}, pointInstanceDraws=${sceneEntityCounts.pointInstanceDraws || 0}, ` +
        `skeletons=${sceneEntityCounts.skeletons || 0}, animations=${sceneEntityCounts.animations || 0}, ` +
        `unsupportedRenderables=${sceneEntityCounts.unsupportedRenderables || 0}, upAxis=${upAxis}`
    );
    self.postMessage({
        type: 'loaded',
        meshCount,
        materialCount,
        upAxis,
        lights: sceneEntityCounts.lights || 0,
        cameras: sceneEntityCounts.cameras || 0,
        nodes: sceneEntityCounts.nodes || 0,
        pointInstancers: sceneEntityCounts.pointInstancers || 0,
        pointInstanceDraws: sceneEntityCounts.pointInstanceDraws || 0,
        skeletons: sceneEntityCounts.skeletons || 0,
        animations: sceneEntityCounts.animations || 0,
        unsupportedRenderables: sceneEntityCounts.unsupportedRenderables || 0
    });
}
