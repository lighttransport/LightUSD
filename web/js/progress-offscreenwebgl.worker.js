// progress-offscreenwebgl.worker.js -- Web Worker that owns the WebGL context
// via OffscreenCanvas, with progress reporting back to the main thread.
//
// Combines offscreengl.worker.js rendering with progress-demo.js progress stages.

// ---- document polyfill (must be at top) ----
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
        createElementNS(_ns, tag) {
            return this.createElement(tag);
        },
    };
}

import * as THREE from 'three';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { LightUSDLoader } from './src/lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils, TextureLoadingManager } from './src/lightusd/LightUSDLoaderUtils.js';
import { setLightUSD as setMaterialXLightUSD } from './src/lightusd/LightUSDMaterialX.js';
import {
    buildNextThreeNode,
    isNextScene,
    readNextSceneMeta
} from './src/lightusd/NextRenderSceneUtils.js';

// ---- Worker-compatible image loading patch ----
THREE.ImageLoader.prototype.load = function (url, onLoad, _onProgress, onError) {
    fetch(url)
        .then(r => {
            if (!r.ok) throw new Error(`HTTP ${r.status} loading ${url}`);
            return r.blob();
        })
        .then(blob => createImageBitmap(blob, {
            imageOrientation: 'flipY',
            colorSpaceConversion: 'none',
        }))
        .then(bitmap => { if (onLoad) onLoad(bitmap); })
        .catch(err => {
            console.warn('[Worker] ImageLoader error:', err);
            if (onError) onError(err);
        });
};

// ============================================================================
// Constants
// ============================================================================

const USE_MEMORY64    = false;

// ============================================================================
// Coroutine pause & debug delay support
// ============================================================================
// Wraps requestAnimationFrame so that:
//   - When paused, the coroutine's co_await yieldToEventLoop() stays suspended
//   - When DEBUG_PARSE_DELAY_MS > 0, each yield point sleeps that long
// The EM_JS yieldToEventLoop_impl uses requestAnimationFrame(resolve), so
// intercepting RAF controls the coroutine timing. No C++ changes needed.

// Artificial delay (ms) at each coroutine yield point.
// Controlled by the Debug toggle in the progress dialog.
// Set from main thread via 'setDebugDelay' message.
let DEBUG_PARSE_DELAY_MS = 500;

const _origRAF = self.requestAnimationFrame?.bind(self);
let _coroutinePaused = false;
let _pausedCallbacks = [];   // queued RAF callbacks while paused
let _isLoadingPhase = false; // only delay during USD loading, not render loop

if (_origRAF) {
    self.requestAnimationFrame = (cb) => {
        if (_coroutinePaused) {
            _pausedCallbacks.push(cb);
            return -1;  // dummy handle
        }
        if (_isLoadingPhase && DEBUG_PARSE_DELAY_MS > 0) {
            // Delay this yield point — the coroutine stays suspended
            setTimeout(() => _origRAF(cb), DEBUG_PARSE_DELAY_MS);
            return -1;
        }
        return _origRAF(cb);
    };
}

function pauseCoroutine() {
    _coroutinePaused = true;
    console.log('[Worker] Coroutine paused');
    self.postMessage({ type: 'paused' });
}

function resumeCoroutine() {
    _coroutinePaused = false;
    console.log('[Worker] Coroutine resumed');
    // Flush queued callbacks — each one resumes a co_await
    const cbs = _pausedCallbacks.splice(0);
    for (const cb of cbs) {
        if (_isLoadingPhase && DEBUG_PARSE_DELAY_MS > 0) {
            setTimeout(() => _origRAF(cb), DEBUG_PARSE_DELAY_MS);
        } else {
            _origRAF(cb);
        }
    }
    self.postMessage({ type: 'resumed' });
}

/**
 * Clear pause state and flush queued RAF callbacks.
 * Called when loading finishes (success or error) to ensure the render loop
 * is never left stalled.
 */
function clearPauseState() {
    if (!_coroutinePaused) return;
    _coroutinePaused = false;
    const cbs = _pausedCallbacks.splice(0);
    for (const cb of cbs) _origRAF(cb);
    self.postMessage({ type: 'resumed' });
}

const CAMERA_FOV      = 45;
const CAMERA_NEAR     = 0.1;
const CAMERA_FAR      = 1000;
const CAMERA_PADDING  = 1.2;
const BG_COLOR        = 0x1a1a1a;

// ============================================================================
// State
// ============================================================================

const three = {
    renderer: null,
    scene: null,
    camera: null,
    pmremGenerator: null,
    envMap: null,
};

const loaderState = {
    loader: null,
    nativeLoader: null,
};

const sceneState = {
    root: null,
    materials: [],
    textureCache: new Map(),
    textureLoadingManager: null,
    textureCount: 0,
    meshCount: 0,
    upAxis: 'Y',
};

// Pre-allocated scratch vectors for hot-path (pointer move)
const _scratchRight = new THREE.Vector3();
const _scratchUp = new THREE.Vector3();
const _scratchDir = new THREE.Vector3();

// Manual orbit camera
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

// ============================================================================
// Main message dispatch
// ============================================================================

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
        case 'toggleUpAxis':
            handleToggleUpAxis(msg);
            break;
        case 'fitCamera':
            fitCameraToScene();
            break;
        case 'pause':
            pauseCoroutine();
            break;
        case 'resume':
            resumeCoroutine();
            break;
        case 'setDebugDelay':
            DEBUG_PARSE_DELAY_MS = msg.delayMs;
            console.log(`[Worker] Debug delay set to ${DEBUG_PARSE_DELAY_MS}ms`);
            break;
        default:
            console.warn('[Worker] Unknown message type:', msg.type);
    }
});

// ============================================================================
// Init
// ============================================================================

async function handleInit({ canvas, width, height, pixelRatio }) {
    sendStatus('Initializing renderer...');
    try {
        initThreeJS(canvas, width, height, pixelRatio);
    } catch (err) {
        sendError(`WebGL init failed: ${err.message}`);
        return;
    }

    sendStatus('Initializing LightUSD WASM...');
    try {
        await initLoader();
    } catch (err) {
        sendError(`WASM init failed: ${err.message}`);
        return;
    }

    sendStatus('Loading environment...');
    await loadEnvironment();

    sendStatus('Ready - Load a USD file to begin');
    animate();
}

// ============================================================================
// Three.js setup
// ============================================================================

function initThreeJS(offscreenCanvas, width, height, pixelRatio) {
    three.scene = new THREE.Scene();
    three.scene.background = new THREE.Color(BG_COLOR);

    three.camera = new THREE.PerspectiveCamera(CAMERA_FOV, width / height, CAMERA_NEAR, CAMERA_FAR);
    three.camera.position.set(3, 2, 5);

    three.renderer = new THREE.WebGLRenderer({
        canvas: offscreenCanvas,
        antialias: true,
    });
    three.renderer.setSize(width, height, false);
    three.renderer.setPixelRatio(pixelRatio);
    three.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    three.renderer.toneMappingExposure = 1.0;
    three.renderer.outputColorSpace = THREE.SRGBColorSpace;

    // Lights
    const ambientLight = new THREE.AmbientLight(0xffffff, 0.5);
    three.scene.add(ambientLight);
    const directionalLight = new THREE.DirectionalLight(0xffffff, 1.0);
    directionalLight.position.set(5, 10, 7.5);
    three.scene.add(directionalLight);

    // Grid
    const grid = new THREE.GridHelper(10, 10, 0x444444, 0x333333);
    three.scene.add(grid);

    three.pmremGenerator = new THREE.PMREMGenerator(three.renderer);
    three.pmremGenerator.compileEquirectangularShader();
}

// ============================================================================
// LightUSD WASM loader
// ============================================================================

async function initLoader() {
    loaderState.loader = new LightUSDLoader(null, {
        maxMemoryLimitMB: 512,
        onTydraProgress: (info) => {
            const meshProgress = info.meshTotal > 0
                ? `${info.meshCurrent}/${info.meshTotal}`
                : '';
            const meshName = info.meshName ? info.meshName.split('/').pop() : '';
            const detail = meshProgress
                ? `Converting: ${meshProgress} ${meshName}`
                : `Converting: ${info.stage}`;

            if (_isLoadingPhase) {
                // Coroutine path: Tydra runs inside the 'meshes' phase (60-80%)
                sendProgress('building', 60 + (info.progress * 20), detail);
            } else {
                // Sync path: Tydra runs during parse (30-80%)
                sendProgress('parsing', 30 + (info.progress * 50), detail);
            }
        },
        onTydraComplete: (info) => {
            console.log(`[Worker Tydra] Complete: ${info.meshCount} meshes, ${info.materialCount} materials, ${info.textureCount} textures`);
            sendProgress('building', 80, `Building ${info.meshCount} meshes...`);
        }
    });
    await loaderState.loader.init({ useMemory64: USE_MEMORY64 });

    const wasmModule = loaderState.loader.native_;
    LightUSDLoaderUtils.setLightUSD(wasmModule);
    setMaterialXLightUSD(wasmModule);
}

// ============================================================================
// Environment (HDR)
// ============================================================================

async function loadEnvironment() {
    try {
        const hdrLoader = new HDRLoader();
        const texture = await hdrLoader.loadAsync('./assets/textures/goegap_1k.hdr');
        three.envMap = three.pmremGenerator.fromEquirectangular(texture).texture;
        texture.dispose();
    } catch {
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

    const texture = new THREE.CanvasTexture(oc);
    texture.mapping = THREE.EquirectangularReflectionMapping;
    const envTexture = three.pmremGenerator.fromEquirectangular(texture).texture;
    texture.dispose();
    return envTexture;
}

function applyEnv() {
    three.scene.environment = three.envMap;
    three.scene.background = three.envMap;
    sceneState.materials.forEach(mat => {
        mat.envMap = three.envMap;
        mat.needsUpdate = true;
    });
}

// ============================================================================
// USD Loading with progress
// ============================================================================

async function handleLoadFile({ data, filename, backend = 'next' }) {
    sendStatus(`Loading: ${filename}...`);

    try {
        await loadUSDFromData(new Uint8Array(data), filename, backend);
    } catch (err) {
        sendError(`Failed to load ${filename}: ${err.message}`);
        console.error('[Worker] loadFile error:', err);
    } finally {
        clearPauseState();
    }
}

// Map C++ coroutine phase names to progress stage/percentage
const COROUTINE_PHASE_MAP = {
    detecting: { stage: 'parsing', pct: 30, label: 'Detecting format...' },
    parsing:   { stage: 'parsing', pct: 35, label: 'Parsing USD data...' },
    setup:     { stage: 'parsing', pct: 50, label: 'Setting up scene...' },
    assets:    { stage: 'building', pct: 55, label: 'Processing assets...' },
    meshes:    { stage: 'building', pct: 60, label: 'Converting meshes...' },
    complete:  { stage: 'building', pct: 80, label: 'Parse complete' },
};

async function loadUSDFromData(data, filename, backend = 'next') {
    clearScene();

    sendProgress('parsing', 30, `Parsing: ${filename}...`);

    const useNext = backend === 'next' || backend === 'auto';
    const useAsync = !useNext && loaderState.loader.hasAsyncSupport();
    console.log(`[Worker] Coroutine async: ${useAsync ? 'ON' : 'OFF'}` +
        (useAsync && DEBUG_PARSE_DELAY_MS > 0 ? `, debug delay: ${DEBUG_PARSE_DELAY_MS}ms/phase` : ''));

    // Tell main thread the expected total delay so it can adjust the watchdog
    if (useAsync && DEBUG_PARSE_DELAY_MS > 0) {
        self.postMessage({ type: 'debugDelay', delayMs: DEBUG_PARSE_DELAY_MS, phases: 10 });
    }

    // Parse USD — use coroutine async path if available for smooth progress
    _isLoadingPhase = true;
    let usd;
    try {
        if (useAsync) {
            usd = await loaderState.loader.parseAsync(data, filename, {
                onPhaseStart: (info) => {
                    const mapped = COROUTINE_PHASE_MAP[info.phase];
                    if (mapped) {
                        sendProgress(mapped.stage, mapped.pct, mapped.label);
                    } else {
                        sendProgress('parsing', 30 + info.progress * 50,
                            `Phase: ${info.phase}`);
                    }
                }
            });
        } else {
            usd = await new Promise((resolve, reject) => {
                loaderState.loader.parse(data, filename, resolve, reject, {
                    backend,
                    materialDedup: false,
                    mergeMeshes: false,
                    mergeMeshesBakeTransform: false,
                    flattenRenderTree: false
                });
            });
        }
    } catch (err) {
        const msg = err?.message || String(err);
        if (msg.includes('resize_heap') || msg.includes('enlarge memory') ||
            msg.includes('Cannot enlarge') || msg.includes('OOM') ||
            msg.includes('out of memory') || msg.includes('the limit is')) {
            const sizeMB = Math.round(data.byteLength / (1024 * 1024));
            throw new Error(
                `Out of WASM memory loading ${filename} (${sizeMB} MB). ` +
                `The 32-bit WASM build is limited to ~2 GB heap. ` +
                `Try a smaller file, or rebuild with USE_MEMORY64=true for 64-bit WASM.`
            );
        }
        throw err;
    } finally {
        _isLoadingPhase = false;
    }

    loaderState.nativeLoader = usd;

    if (isNextScene(usd)) {
        const metadata = readNextSceneMeta(usd);
        sceneState.upAxis = metadata.upAxis || 'Y';
        sendProgress('building', 80, 'Building next render scene...');
        const built = buildNextThreeNode(usd, {
            skipTextures: false,
            lazyTextures: true,
            onProgress: (info) => {
                const pct = 80 + Math.min(1, Math.max(0, info.percentage / 100)) * 14;
                sendProgress('building', pct, info.message ||
                    `Building next scene (${info.builtMeshes}/${info.totalMeshes})...`);
            },
            progressInterval: 20
        });
        sceneState.root = built.node;
        sceneState.textureLoadingManager = built.textureManager;
        sceneState.meshCount = usd.numMeshes ? usd.numMeshes() : 0;
        sceneState.materials = [];
        built.node.traverse((obj) => {
            if (obj.isMesh) {
                const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
                for (const mat of mats) {
                    if (mat && !sceneState.materials.includes(mat)) {
                        sceneState.materials.push(mat);
                    }
                }
            }
        });
        three.scene.add(sceneState.root);
        if (sceneState.root && sceneState.upAxis === 'Z') {
            sceneState.root.rotation.x = -Math.PI / 2;
        }
        fitCameraToScene();
        if (sceneState.textureLoadingManager && sceneState.textureLoadingManager.total > 0) {
            const texManager = sceneState.textureLoadingManager;
            sendTextureProgress({ loaded: 0, total: texManager.total, percentage: 0, isStart: true });
            texManager.startLoading({
                onProgress: (info) => sendTextureProgress(info),
                onTextureLoaded: (material) => { material.needsUpdate = true; },
                concurrency: 16,
                yieldInterval: 16
            }).then(status => {
                sceneState.textureCount = status.loaded;
                sendTextureProgress({
                    ...status,
                    percentage: 100,
                    isComplete: true
                });
            }).catch(err => {
                console.warn('[Worker] Next texture loading failed:', err);
            });
        }
        console.log('[Worker] next backend rendered geometry/materials from RenderScene; legacy node APIs are skipped.');
        const sceneEntityCounts = readSceneEntityCounts(usd);
        sendLoaded(
            sceneState.meshCount,
            sceneState.materials.length,
            sceneState.textureCount,
            sceneState.upAxis,
            sceneEntityCounts
        );
        clearPauseState();
        return;
    }

    // Read metadata
    const metadata = usd.getSceneMetadata ? usd.getSceneMetadata() : {};
    sceneState.upAxis = metadata.upAxis || 'Y';

    // Build scene with progress (coroutine ended at ~80%, JS building starts at 80%)
    sendProgress('building', 80, 'Building Three.js scene...');
    await buildSceneWithProgress(usd);

    // Try to load DomeLight environment
    try {
        const result = await LightUSDLoaderUtils.loadDomeLightFromUSD(
            usd,
            three.pmremGenerator
        );
        if (result) {
            three.envMap = result.texture;
            applyEnv();
        }
    } catch (err) {
        console.warn('[Worker] DomeLight load error (non-fatal):', err);
    }

    // Apply Z-up -> Y-up
    if (sceneState.root && sceneState.upAxis === 'Z') {
        sceneState.root.rotation.x = -Math.PI / 2;
    }

    fitCameraToScene();

    // Start delayed texture loading
    if (sceneState.textureLoadingManager && sceneState.textureLoadingManager.total > 0) {
        const texManager = sceneState.textureLoadingManager;
        console.log(`[Worker] Starting delayed texture loading: ${texManager.total} textures queued`);

        sendTextureProgress({ loaded: 0, total: texManager.total, percentage: 0, isStart: true });

        texManager.startLoading({
            onProgress: (info) => {
                sendTextureProgress(info);
            },
            onTextureLoaded: (material, _mapProperty, _texture) => {
                material.needsUpdate = true;
            },
            concurrency: LightUSDLoaderUtils.defaultTextureConcurrency(),
            yieldInterval: 16
        }).then(status => {
            console.log(`[Worker] Texture loading complete: ${status.loaded}/${status.total}`);
            sceneState.textureCount = status.loaded;
            sendTextureProgress({
                loaded: status.loaded,
                total: status.total,
                failed: status.failed,
                percentage: 100,
                isComplete: true
            });
        }).catch(err => {
            console.error('[Worker] Texture loading error:', err);
        });
    }

    // Send loaded
    const sceneEntityCounts = readSceneEntityCounts(loaderState.nativeLoader);
    sendLoaded(
        sceneState.meshCount,
        sceneState.materials.length,
        sceneState.textureCount,
        sceneState.upAxis,
        sceneEntityCounts
    );

    clearPauseState();
}

// ============================================================================
// Scene building with progress
// ============================================================================

function debugDelay() {
    if (DEBUG_PARSE_DELAY_MS > 0) {
        return new Promise(resolve => setTimeout(resolve, DEBUG_PARSE_DELAY_MS));
    }
    return Promise.resolve();
}

async function buildSceneWithProgress(usd) {
    const rootNode = usd.getDefaultRootNode();
    sceneState.meshCount = 0;
    sceneState.textureCount = 0;
    sceneState.materials = [];
    sceneState.textureCache.clear();

    // Create texture loading manager for delayed texture loading
    sceneState.textureLoadingManager = new TextureLoadingManager();

    const totalMeshes = usd.numMeshes ? usd.numMeshes() : 0;

    sendProgress('building', 80, `Building Three.js meshes (0/${totalMeshes})...`);
    await debugDelay();

    const defaultMtl = new THREE.MeshPhysicalMaterial({
        color: 0x888888,
        roughness: 0.5,
        metalness: 0.0,
        envMap: three.envMap,
    });

    if (rootNode) {
        sceneState.root = await LightUSDLoaderUtils.buildThreeNode(
            rootNode,
            defaultMtl,
            usd,
            {
                overrideMaterial: false,
                envMap: three.envMap,
                envMapIntensity: 1.0,
                preferredMaterialType: 'auto',
                textureCache: sceneState.textureCache,
                textureLoadingManager: sceneState.textureLoadingManager,
                onProgress: (info) => {
                    // Map buildThreeNode progress to 80-88%
                    const mappedPercentage = 80 + (info.percentage * 0.08);
                    sendProgress('building', Math.min(88, mappedPercentage), info.message);
                }
            }
        );
    } else {
        sceneState.root = new THREE.Group();
    }

    three.scene.add(sceneState.root);

    // Count meshes & collect materials
    sendProgress('materials', 88, 'Converting materials...');
    await debugDelay();
    const matSet = new Set();
    sceneState.root.traverse(obj => {
        if (obj.isMesh) {
            sceneState.meshCount++;
            if (obj.material) {
                const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
                mats.forEach(m => matSet.add(m));
            }
        }
    });
    sceneState.materials = Array.from(matSet);

    // Count textures
    sendProgress('textures', 94, `Decoding textures from ${sceneState.materials.length} materials...`);
    await debugDelay();
    const textureProps = ['map', 'normalMap', 'roughnessMap', 'metalnessMap', 'emissiveMap', 'aoMap', 'alphaMap'];
    for (const mat of sceneState.materials) {
        textureProps.forEach(prop => {
            if (mat[prop]) sceneState.textureCount++;
        });
    }

    sendProgress('complete', 100,
        `Complete! ${sceneState.meshCount} meshes, ${sceneState.materials.length} materials, ${sceneState.textureCount} textures`
    );
}

// ============================================================================
// Scene cleanup
// ============================================================================

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
    sceneState.meshCount = 0;
    sceneState.textureCount = 0;
    sceneState.textureCache.forEach(t => t?.dispose());
    sceneState.textureCache.clear();

    if (sceneState.textureLoadingManager) {
        sceneState.textureLoadingManager.abort();
        sceneState.textureLoadingManager.reset();
        sceneState.textureLoadingManager = null;
    }

    if (loaderState.nativeLoader) {
        try { loaderState.nativeLoader.reset(); } catch (_) {
            try { loaderState.nativeLoader.clearAssets(); } catch (_2) { /* ignore */ }
        }
        loaderState.nativeLoader = null;
    }
}

// ============================================================================
// Camera
// ============================================================================

function fitCameraToScene() {
    if (!sceneState.root) return;

    sceneState.root.updateMatrixWorld(true);

    const box = new THREE.Box3().setFromObject(sceneState.root);
    if (box.isEmpty()) return;

    const size   = box.getSize(new THREE.Vector3());
    const center = box.getCenter(new THREE.Vector3());

    const sphereR = size.length() * 0.5;
    const fovRad  = CAMERA_FOV * (Math.PI / 180);
    const aspect  = three.camera.aspect;
    const hFov    = 2 * Math.atan(Math.tan(fovRad / 2) * aspect);
    const distV   = sphereR / Math.sin(fovRad / 2);
    const distH   = sphereR / Math.sin(hFov / 2);
    const dist    = Math.max(distV, distH) * CAMERA_PADDING;

    orbitState.target.copy(center);
    orbitState.radius = dist;
    orbitState.theta  = 0.3;
    orbitState.phi    = 1.2;

    three.camera.near = Math.max(0.01, dist / 100);
    three.camera.far  = Math.max(1000, dist * 10);
    three.camera.updateProjectionMatrix();

    updateCamera();
}

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

// ============================================================================
// Pointer / orbit / pan handlers
// ============================================================================

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
        orbitState.theta -= dx * 0.01;
        orbitState.phi    = Math.max(0.01, Math.min(Math.PI - 0.01, orbitState.phi + dy * 0.01));
    } else if (orbitState.isPanning && buttons & 2) {
        const panSpeed = orbitState.radius * 0.001;
        three.camera.getWorldDirection(_scratchDir);
        _scratchRight.crossVectors(_scratchDir.negate(), three.camera.up).normalize();
        _scratchUp.copy(three.camera.up).normalize();

        orbitState.target.addScaledVector(_scratchRight, -dx * panSpeed);
        orbitState.target.addScaledVector(_scratchUp,     dy * panSpeed);
    }

    updateCamera();
}

function handlePointerUp(_msg) {
    orbitState.isDragging = false;
    orbitState.isPanning  = false;
}

// ============================================================================
// Wheel (zoom)
// ============================================================================

function handleWheel({ deltaY }) {
    const zoomFactor = deltaY > 0 ? 1.1 : (1 / 1.1);
    orbitState.radius = Math.max(0.01, orbitState.radius * zoomFactor);
    updateCamera();
}

// ============================================================================
// Resize
// ============================================================================

function handleResize({ width, height, pixelRatio }) {
    three.camera.aspect = width / height;
    three.camera.updateProjectionMatrix();
    three.renderer.setSize(width, height, false);
    three.renderer.setPixelRatio(pixelRatio);
}

// ============================================================================
// UpAxis toggle
// ============================================================================

function handleToggleUpAxis({ apply }) {
    if (!sceneState.root) return;
    if (apply && sceneState.upAxis === 'Z') {
        sceneState.root.rotation.x = -Math.PI / 2;
    } else {
        sceneState.root.rotation.x = 0;
    }
}

// ============================================================================
// Render loop
// ============================================================================

const fpsState = {
    lastUpdateMs: performance.now(),
    frames: 0
};

function animate() {
    // Use _origRAF directly — the wrapped requestAnimationFrame is only for
    // coroutine yield points (pause/delay). The render loop must always run.
    if (_origRAF) {
        _origRAF(animate);
    } else {
        setTimeout(animate, 16);
    }
    three.renderer.render(three.scene, three.camera);
    fpsState.frames++;
    const now = performance.now();
    if (now - fpsState.lastUpdateMs >= 500) {
        self.postMessage({
            type: 'fps',
            fps: fpsState.frames * 1000 / (now - fpsState.lastUpdateMs)
        });
        fpsState.frames = 0;
        fpsState.lastUpdateMs = now;
    }
}

// ============================================================================
// Helpers: postMessage shortcuts
// ============================================================================

function sendStatus(message) {
    console.log('[Worker]', message);
    self.postMessage({ type: 'status', message });
}

function sendError(message) {
    console.error('[Worker] ERROR:', message);
    self.postMessage({ type: 'error', message });
}

function sendProgress(stage, percentage, message) {
    self.postMessage({ type: 'progress', stage, percentage, message });
}

function sendTextureProgress(info) {
    self.postMessage({ type: 'texture_progress', ...info });
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
        lights: safeNumber(usd && usd.numLights && usd.numLights.bind(usd)),
        cameras: safeNumber(usd && usd.numCameras && usd.numCameras.bind(usd)),
        nodes: safeNumber(usd && usd.numNodes && usd.numNodes.bind(usd)),
        pointInstancers: safeNumber(usd && usd.numPointInstancers && usd.numPointInstancers.bind(usd)),
        pointInstanceDraws: safeNumber(usd && usd.numPointInstanceDraws && usd.numPointInstanceDraws.bind(usd)),
        skeletons: safeNumber(usd && usd.numSkeletons && usd.numSkeletons.bind(usd)),
        animations: safeNumber(usd && usd.numAnimations && usd.numAnimations.bind(usd)),
        unsupportedRenderables: safeNumber(usd && usd.numUnsupportedRenderables && usd.numUnsupportedRenderables.bind(usd)) ||
            safeArrayCount(usd && usd.getUnsupportedRenderables && usd.getUnsupportedRenderables.bind(usd))
    };
}

function sendLoaded(meshCount, materialCount, textureCount, upAxis, sceneEntityCounts = {}) {
    console.log(
        `[Worker] Loaded: ${meshCount} meshes, ${materialCount} materials, ${textureCount} textures, ` +
        `lights=${sceneEntityCounts.lights || 0}, cameras=${sceneEntityCounts.cameras || 0}, ` +
        `nodes=${sceneEntityCounts.nodes || 0}, pointInstancers=${sceneEntityCounts.pointInstancers || 0}, ` +
        `pointInstanceDraws=${sceneEntityCounts.pointInstanceDraws || 0}, skeletons=${sceneEntityCounts.skeletons || 0}, ` +
        `animations=${sceneEntityCounts.animations || 0}, ` +
        `unsupportedRenderables=${sceneEntityCounts.unsupportedRenderables || 0}, upAxis=${upAxis}`
    );
    self.postMessage({
        type: 'loaded',
        meshCount,
        materialCount,
        textureCount,
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

// ============================================================================
// Global error handlers (safety net for uncaught errors, e.g. Emscripten abort)
// ============================================================================

self.addEventListener('error', (e) => {
    const msg = e.message || 'Unknown worker error';
    console.error('[Worker] Uncaught error:', msg);
    self.postMessage({ type: 'error', message: msg });
});

self.addEventListener('unhandledrejection', (e) => {
    const msg = e.reason?.message || String(e.reason) || 'Unhandled rejection in worker';
    console.error('[Worker] Unhandled rejection:', msg);
    self.postMessage({ type: 'error', message: msg });
});
