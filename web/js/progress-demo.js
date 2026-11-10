// LightUSD Progress Demo with OpenPBR Material Support
// Demonstrates progress callbacks for USD loading with detailed stage reporting
// Supports both main-thread and Web Worker loading modes

import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDWorkerLoader } from 'lightusd/LightUSDWorkerLoader.js';
import { LightUSDLoaderUtils, TextureLoadingManager } from 'lightusd/LightUSDLoaderUtils.js';
import { setLightUSD as setMaterialXLightUSD } from 'lightusd/LightUSDMaterialX.js';
import { OpenPBRMaterial } from 'lightusd/LightUSDOpenPBRSimple.js';
import { getBackendFromURL, makeStaticNextParseOptions } from 'lightusd/LoaderConfigUtils.js';
import {
    buildNextThreeNode,
    isNextScene,
    nextCountsFromScene,
    readNextSceneMeta
} from 'lightusd/NextRenderSceneUtils.js';

// ============================================================================
// Constants
// ============================================================================

const DEFAULT_BACKGROUND_COLOR = 0x1a1a1a;
const CAMERA_PADDING = 1.2;

// Sample models for testing (point these at your own .usdz/.usda under assets/)
const SAMPLE_MODELS = [
    'assets/physics-robot-arm.usda'
];

// ============================================================================
// Global State
// ============================================================================

const threeState = {
    scene: null,
    camera: null,
    renderer: null,
    controls: null
};

const loaderState = {
    loader: null,
    workerLoader: null,
    nativeLoader: null,
    useWorker: true  // Use Web Worker by default for responsive UI
};

const sceneState = {
    root: null,
    materials: [],
    textureCount: 0,
    meshCount: 0,
    upAxis: 'Y',
    lightsCount: 0,
    camerasCount: 0,
    nodesCount: 0,
    pointInstancerCount: 0,
    pointInstanceDrawCount: 0,
    skeletonCount: 0,
    animationCount: 0,
    unsupportedRenderableCount: 0,
    textureLoadingManager: null  // For delayed texture loading
};

// Settings
const settings = {
    applyUpAxisConversion: false,
    backend: getBackendFromURL(new URLSearchParams(window.location.search), 'next')
};

// Progress state for tracking stages
const progressState = {
    currentStage: null,
    stageProgress: {}
};

// Memory tracking state
const memoryState = {
    available: false,
    dataPoints: [],       // Array of {time, used, total, limit, stage, percentage}
    maxDataPoints: 100,
    startTime: 0,
    canvas: null,
    ctx: null,
    colors: {
        used: '#4CAF50',
        total: '#2196F3',
        limit: 'rgba(244, 67, 54, 0.3)',
        grid: 'rgba(255, 255, 255, 0.1)',
        text: '#888',
        stageLine: 'rgba(255, 152, 0, 0.5)'
    }
};

const loadStatsState = {
    current: null,
    fps: 0,
    frames: 0,
    fpsLastTime: 0
};

// ============================================================================
// Memory Tracking Functions
// ============================================================================

/**
 * Initialize memory graph canvas
 */
function initMemoryGraph() {
    memoryState.canvas = document.getElementById('memory-graph');
    if (!memoryState.canvas) return;

    // Set canvas size with device pixel ratio for sharp rendering
    const rect = memoryState.canvas.parentElement.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    memoryState.canvas.width = rect.width * dpr;
    memoryState.canvas.height = 120 * dpr;
    memoryState.canvas.style.width = rect.width + 'px';
    memoryState.canvas.style.height = '120px';

    memoryState.ctx = memoryState.canvas.getContext('2d');
    memoryState.ctx.scale(dpr, dpr);

    // Check if memory API is available
    memoryState.available = !!(performance && performance.memory);
    if (!memoryState.available) {
        document.getElementById('memory-warning').classList.add('visible');
    }
}

/**
 * Get current memory usage (Chrome only)
 */
function getMemoryUsage() {
    if (!memoryState.available) {
        return { used: 0, total: 0, limit: 0 };
    }

    const mem = performance.memory;
    return {
        used: mem.usedJSHeapSize,
        total: mem.totalJSHeapSize,
        limit: mem.jsHeapSizeLimit
    };
}

/**
 * Format bytes to human readable string
 */
function formatBytes(bytes) {
    if (bytes === 0) return '0 B';
    if (!Number.isFinite(bytes)) return '--';
    const sign = bytes < 0 ? '-' : '';
    bytes = Math.abs(bytes);
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return sign + parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
}

function formatMs(ms) {
    return Number.isFinite(ms) ? `${Math.round(ms)} ms` : '--';
}

function wasmHeapBytes() {
    try {
        return loaderState.loader?.native_?.HEAPU8?.buffer?.byteLength || 0;
    } catch (_) {
        return 0;
    }
}

function captureStatsMemory() {
    const mem = getMemoryUsage();
    return {
        jsHeap: mem.used,
        wasmHeap: wasmHeapBytes()
    };
}

function sourceName(source, isFile) {
    if (isFile) return source?.name || 'local.usd';
    const clean = String(source || '').split(/[?#]/)[0];
    return decodeURIComponent(clean.slice(clean.lastIndexOf('/') + 1)) || 'scene.usd';
}

function urlParam(params) {
    return params.get('uri') || params.get('url') || params.get('src') || params.get('model') || params.get('usd') || '';
}

function beginLoadStats(source, isFile) {
    const before = captureStatsMemory();
    const stats = {
        filename: sourceName(source, isFile),
        backend: settings.backend,
        startTime: performance.now(),
        fileSize: isFile ? (source?.size || 0) : 0,
        readMs: NaN,
        parseMs: NaN,
        processMs: NaN,
        totalMs: NaN,
        memoryBefore: before,
        memoryAfter: before
    };
    loadStatsState.current = stats;
    updateLoadStatsPanel(stats);
    return stats;
}

function finishLoadStats(stats) {
    stats.totalMs = performance.now() - stats.startTime;
    stats.memoryAfter = captureStatsMemory();
    updateLoadStatsPanel(stats);
}

function updateLoadStatsPanel(stats = loadStatsState.current) {
    if (!stats) return;
    const setText = (id, value) => {
        const el = document.getElementById(id);
        if (el) el.textContent = value;
    };
    setText('current-file', stats.filename || '-');
    setText('backend-name', stats.backend || settings.backend);
    setText('fps-value', loadStatsState.fps ? String(loadStatsState.fps) : '--');
    setText('file-size', stats.fileSize ? formatBytes(stats.fileSize) : '--');
    setText('read-time', formatMs(stats.readMs));
    setText('parse-time', formatMs(stats.parseMs));
    setText('process-time', formatMs(stats.processMs));
    setText('total-time', formatMs(stats.totalMs));
    const jsDelta = stats.memoryAfter.jsHeap - stats.memoryBefore.jsHeap;
    const wasmDelta = stats.memoryAfter.wasmHeap - stats.memoryBefore.wasmHeap;
    setText('js-heap-stat', `${formatBytes(stats.memoryAfter.jsHeap)} (${jsDelta >= 0 ? '+' : ''}${formatBytes(jsDelta)})`);
    setText('wasm-heap-stat', `${formatBytes(stats.memoryAfter.wasmHeap)} (${wasmDelta >= 0 ? '+' : ''}${formatBytes(wasmDelta)})`);
}

/**
 * Record memory data point
 */
function recordMemoryPoint(stage, percentage) {
    const mem = getMemoryUsage();
    const time = performance.now() - memoryState.startTime;

    memoryState.dataPoints.push({
        time,
        used: mem.used,
        total: mem.total,
        limit: mem.limit,
        stage,
        percentage
    });

    // Keep only last N points
    if (memoryState.dataPoints.length > memoryState.maxDataPoints) {
        memoryState.dataPoints.shift();
    }

    // Update UI
    updateMemoryStats(mem);
    drawMemoryGraph();
}

/**
 * Update memory statistics display
 */
function updateMemoryStats(mem) {
    const usedEl = document.getElementById('mem-used');
    const totalEl = document.getElementById('mem-total');
    const limitEl = document.getElementById('mem-limit');
    const pointsEl = document.getElementById('mem-points');

    if (usedEl) usedEl.textContent = formatBytes(mem.used);
    if (totalEl) totalEl.textContent = formatBytes(mem.total);
    if (limitEl) limitEl.textContent = formatBytes(mem.limit);
    if (pointsEl) pointsEl.textContent = memoryState.dataPoints.length;

    // Add warning colors based on usage
    if (usedEl) {
        const usageRatio = mem.used / mem.limit;
        usedEl.classList.remove('warning', 'critical');
        if (usageRatio > 0.8) {
            usedEl.classList.add('critical');
        } else if (usageRatio > 0.6) {
            usedEl.classList.add('warning');
        }
    }
}

/**
 * Draw memory usage graph using Canvas2D
 */
function drawMemoryGraph() {
    const ctx = memoryState.ctx;
    const canvas = memoryState.canvas;
    if (!ctx || !canvas) return;

    const width = canvas.width / (window.devicePixelRatio || 1);
    const height = canvas.height / (window.devicePixelRatio || 1);
    const padding = { top: 10, right: 10, bottom: 20, left: 50 };
    const graphWidth = width - padding.left - padding.right;
    const graphHeight = height - padding.top - padding.bottom;

    // Clear canvas
    ctx.fillStyle = '#111';
    ctx.fillRect(0, 0, width, height);

    if (memoryState.dataPoints.length < 2) return;

    // Find data range
    const points = memoryState.dataPoints;
    const maxMem = Math.max(...points.map(p => Math.max(p.used, p.total)));
    const limitMem = points[0].limit || maxMem * 1.2;
    const yMax = Math.max(maxMem * 1.1, limitMem);
    const timeRange = points[points.length - 1].time - points[0].time;

    // Draw grid
    ctx.strokeStyle = memoryState.colors.grid;
    ctx.lineWidth = 0.5;
    for (let i = 0; i <= 4; i++) {
        const y = padding.top + (graphHeight / 4) * i;
        ctx.beginPath();
        ctx.moveTo(padding.left, y);
        ctx.lineTo(width - padding.right, y);
        ctx.stroke();
    }

    // Draw limit line
    if (limitMem > 0) {
        const limitY = padding.top + graphHeight * (1 - limitMem / yMax);
        ctx.strokeStyle = memoryState.colors.limit;
        ctx.lineWidth = 1;
        ctx.setLineDash([5, 5]);
        ctx.beginPath();
        ctx.moveTo(padding.left, limitY);
        ctx.lineTo(width - padding.right, limitY);
        ctx.stroke();
        ctx.setLineDash([]);
    }

    // Helper to draw a line
    const drawLine = (dataKey, color) => {
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.beginPath();

        points.forEach((point, i) => {
            const x = padding.left + (graphWidth * (point.time - points[0].time) / (timeRange || 1));
            const y = padding.top + graphHeight * (1 - point[dataKey] / yMax);

            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        });

        ctx.stroke();
    };

    // Draw total heap line
    drawLine('total', memoryState.colors.total);

    // Draw used heap line
    drawLine('used', memoryState.colors.used);

    // Draw stage change markers
    let lastStage = null;
    ctx.strokeStyle = memoryState.colors.stageLine;
    ctx.lineWidth = 1;
    ctx.setLineDash([2, 2]);
    points.forEach((point) => {
        if (point.stage !== lastStage) {
            const x = padding.left + (graphWidth * (point.time - points[0].time) / (timeRange || 1));
            ctx.beginPath();
            ctx.moveTo(x, padding.top);
            ctx.lineTo(x, height - padding.bottom);
            ctx.stroke();
            lastStage = point.stage;
        }
    });
    ctx.setLineDash([]);

    // Draw Y axis labels
    ctx.fillStyle = memoryState.colors.text;
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++) {
        const y = padding.top + (graphHeight / 4) * i;
        const value = yMax * (1 - i / 4);
        ctx.fillText(formatBytes(value), padding.left - 5, y);
    }

    // Draw current stage label
    if (points.length > 0) {
        const lastPoint = points[points.length - 1];
        ctx.fillStyle = '#FF9800';
        ctx.font = '10px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(lastPoint.stage || '', width / 2, height - 5);
    }
}

/**
 * Reset memory tracking for new load
 */
function resetMemoryTracking() {
    memoryState.dataPoints = [];
    memoryState.startTime = performance.now();

    // Clear stats display
    const usedEl = document.getElementById('mem-used');
    const totalEl = document.getElementById('mem-total');
    const limitEl = document.getElementById('mem-limit');
    const pointsEl = document.getElementById('mem-points');

    if (usedEl) usedEl.textContent = '--';
    if (totalEl) totalEl.textContent = '--';
    if (limitEl) limitEl.textContent = '--';
    if (pointsEl) pointsEl.textContent = '0';

    // Clear graph
    if (memoryState.ctx && memoryState.canvas) {
        const width = memoryState.canvas.width / (window.devicePixelRatio || 1);
        const height = memoryState.canvas.height / (window.devicePixelRatio || 1);
        memoryState.ctx.fillStyle = '#111';
        memoryState.ctx.fillRect(0, 0, width, height);
    }
}

// ============================================================================
// Progress UI Functions
// ============================================================================

function showProgress() {
    const container = document.getElementById('progress-container');
    container.classList.add('visible');
    resetProgressStages();
}

function hideProgress() {
    const container = document.getElementById('progress-container');
    container.classList.remove('visible');
}

function resetProgressStages() {
    const stageItems = document.querySelectorAll('.progress-stage-item');
    stageItems.forEach(item => {
        item.classList.remove('active', 'completed');
        const icon = item.querySelector('.stage-icon');
        icon.classList.remove('active', 'completed');
        icon.classList.add('pending');
    });
    progressState.currentStage = null;
    progressState.stageProgress = {};

    // Reset progress bar to 0
    const progressBar = document.getElementById('progress-bar');
    progressBar.style.transform = 'scaleX(0)';
    progressBar.classList.remove('complete');
    document.getElementById('progress-percentage').textContent = '0%';

    // Reset throttle timer
    lastProgressUpdate = 0;
}

// Track last update time for throttling
let lastProgressUpdate = 0;
const PROGRESS_UPDATE_INTERVAL = 50; // ms - throttle updates to allow browser to paint

/**
 * Force browser to schedule a repaint.
 * This uses a technique of reading layout properties which forces a reflow,
 * combined with requestAnimationFrame queueing.
 */
function forceRepaint() {
    // Force layout reflow by reading offsetHeight
    void document.body.offsetHeight;

    // Queue a microtask to potentially allow compositor to paint
    queueMicrotask(() => {
        void document.body.offsetHeight;
    });
}

// ============================================================================
// Texture Progress UI Functions
// ============================================================================

function showTextureProgress() {
    const container = document.getElementById('texture-progress-container');
    if (container) {
        container.classList.add('visible');
        // Reset progress bar
        const bar = document.getElementById('texture-progress-bar');
        if (bar) bar.style.transform = 'scaleX(0)';
        updateTextureProgressUI({ loaded: 0, total: 0, percentage: 0, currentTexture: null });
    }
}

function hideTextureProgress() {
    const container = document.getElementById('texture-progress-container');
    if (container) container.classList.remove('visible');
}

function updateTextureProgressUI(info) {
    const { loaded, total, failed, percentage, currentTexture, isComplete } = info;

    // Update count
    const countEl = document.getElementById('texture-progress-count');
    if (countEl) {
        const failedText = failed > 0 ? ` (${failed} failed)` : '';
        countEl.textContent = `${loaded}/${total}${failedText}`;
    }

    // Update progress bar
    const bar = document.getElementById('texture-progress-bar');
    if (bar) {
        bar.style.transform = `scaleX(${percentage / 100})`;
    }

    // Update details
    const detailsEl = document.getElementById('texture-progress-details');
    if (detailsEl) {
        if (isComplete) {
            detailsEl.textContent = failed > 0
                ? `Complete with ${failed} failures`
                : 'All textures loaded';
        } else if (currentTexture) {
            detailsEl.textContent = `Loading: ${currentTexture}`;
        } else {
            detailsEl.textContent = 'Starting texture loading...';
        }
    }

    // Auto-hide when complete after a delay
    if (isComplete) {
        setTimeout(() => {
            hideTextureProgress();
        }, 2000);
    }
}

function updateProgressUI(progress) {
    const percentage = Math.round(progress.percentage);
    const stage = progress.stage;
    const message = progress.message || '';

    // Throttle updates to give browser time to repaint
    const now = performance.now();
    if (now - lastProgressUpdate < PROGRESS_UPDATE_INTERVAL && percentage < 100) {
        // Skip this update, but still record memory
        recordMemoryPoint(stage, percentage);
        return;
    }
    lastProgressUpdate = now;

    // Update progress bar using transform (can be animated by compositor)
    const progressBar = document.getElementById('progress-bar');
    progressBar.style.transform = `scaleX(${percentage / 100})`;

    // Add 'complete' class when done to stop shimmer animation
    if (percentage >= 100 || stage === 'complete') {
        progressBar.classList.add('complete');
    }

    // Update percentage text
    document.getElementById('progress-percentage').textContent = `${percentage}%`;

    // Update stage text
    const stageLabels = {
        'downloading': 'Downloading file...',
        'parsing': 'Parsing USD (WASM)...',
        'building': 'Building Three.js scene...',
        'textures': 'Processing textures...',
        'materials': 'Converting materials...',
        'complete': 'Complete!'
    };
    document.getElementById('progress-stage').textContent = stageLabels[stage] || stage;
    document.getElementById('progress-details').textContent = message;

    // Update stage icons
    updateStageIcons(stage);

    // Record memory at each progress callback
    recordMemoryPoint(stage, percentage);

    // Force browser repaint attempt
    forceRepaint();

    // Log progress for debugging (helps verify callbacks are being called)
    console.log(`[Progress] ${stage}: ${percentage}% - ${message}`);
}

function updateStageIcons(currentStage) {
    const stageOrder = ['downloading', 'parsing', 'building', 'textures', 'materials', 'complete'];
    const currentIndex = stageOrder.indexOf(currentStage);

    stageOrder.forEach((stage, index) => {
        const item = document.querySelector(`.progress-stage-item[data-stage="${stage}"]`);
        if (!item) return;

        const icon = item.querySelector('.stage-icon');
        item.classList.remove('active', 'completed');
        icon.classList.remove('active', 'completed', 'pending');

        if (index < currentIndex) {
            // Completed stages
            item.classList.add('completed');
            icon.classList.add('completed');
            icon.textContent = '\u2713'; // Checkmark
        } else if (index === currentIndex) {
            // Current stage
            item.classList.add('active');
            icon.classList.add('active');
            icon.textContent = String(index + 1);
        } else {
            // Pending stages
            icon.classList.add('pending');
            icon.textContent = String(index + 1);
        }
    });
}

// ============================================================================
// OpenPBR Material Helpers
// ============================================================================

function extractOpenPBRValue(param, defaultValue = 0) {
    if (param === undefined || param === null) return defaultValue;
    if (typeof param === 'number') return param;
    if (Array.isArray(param)) return param[0] ?? defaultValue;
    if (typeof param === 'object') {
        if ('value' in param) return param.value;
        if ('default' in param) return param.default;
    }
    return defaultValue;
}

function extractOpenPBRColor(param, defaultColor = [1, 1, 1]) {
    if (param === undefined || param === null) return new THREE.Color(...defaultColor);
    if (Array.isArray(param)) return new THREE.Color(param[0], param[1], param[2]);
    if (typeof param === 'object') {
        if ('value' in param && Array.isArray(param.value)) {
            return new THREE.Color(param.value[0], param.value[1], param.value[2]);
        }
    }
    return new THREE.Color(...defaultColor);
}

function hasOpenPBRTexture(param) {
    if (!param || typeof param !== 'object') return false;
    return !!(param.textureId || param.texture || param.file);
}

function getOpenPBRTextureId(param) {
    if (!param || typeof param !== 'object') return null;
    return param.textureId || param.texture || param.file || null;
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

    material.userData.textures = {};
    material.userData.materialType = 'OpenPBRMaterial';

    return material;
}

/**
 * Convert material data to OpenPBRMaterial with progress reporting
 * @param {Object} matData - Material data from USD
 * @param {Object} nativeLoader - LightUSD native loader
 * @param {Function} onProgress - Progress callback
 * @returns {Promise<OpenPBRMaterial>}
 */
async function convertToOpenPBRMaterialWithProgress(matData, nativeLoader, onProgress = null) {
    const openPBR = matData.openPBR || matData.openPBRShader || matData;
    const material = createBaseOpenPBRMaterial(openPBR);
    const geometrySection = openPBR.geometry || {};

    if (!nativeLoader) return material;

    // Collect all texture loading tasks
    const textureLoads = [];
    const textureParams = [
        { param: openPBR.base_color, mapName: 'map', label: 'base color' },
        { param: openPBR.specular_roughness, mapName: 'roughnessMap', label: 'roughness' },
        { param: openPBR.base_metalness, mapName: 'metalnessMap', label: 'metalness' },
        { param: openPBR.emission_color, mapName: 'emissiveMap', label: 'emissive' },
        { param: geometrySection.normal || openPBR.normal || openPBR.geometry_normal, mapName: 'normalMap', label: 'normal' },
        { param: geometrySection.ambient_occlusion || openPBR.ambient_occlusion, mapName: 'aoMap', label: 'AO' }
    ];

    textureParams.forEach(({ param, mapName, label }) => {
        if (hasOpenPBRTexture(param)) {
            textureLoads.push({ param, mapName, label });
        }
    });

    // Load textures with progress
    let loadedCount = 0;
    for (const { param, mapName, label } of textureLoads) {
        try {
            const texId = getOpenPBRTextureId(param);
            const texture = await LightUSDLoaderUtils.getTextureFromUSD(nativeLoader, texId);
            if (texture) {
                material[mapName] = texture;
                material.userData.textures[mapName] = { textureId: texId, texture };

                // Apply normal map scale if applicable
                if (mapName === 'normalMap' && material.uniforms?.normalScale) {
                    const normalMapScale = extractOpenPBRValue(geometrySection.normal_map_scale || openPBR.normal_map_scale, 1.0);
                    material.uniforms.normalScale.value.set(normalMapScale, normalMapScale);
                }
            }
        } catch (err) {
            console.warn(`Failed to load ${label} texture:`, err);
        }

        loadedCount++;
        if (onProgress) {
            onProgress({
                loaded: loadedCount,
                total: textureLoads.length,
                label
            });
        }
    }

    return material;
}

// ============================================================================
// Material Conversion
// ============================================================================

/**
 * Check if material is OpenPBR type
 */
function isOpenPBRMaterial(matData) {
    if (!matData) return false;
    return !!(matData.openPBR || matData.openPBRShader);
}

/**
 * Convert material with progress reporting
 */
async function convertMaterial(matData, nativeLoader, onProgress) {
    if (isOpenPBRMaterial(matData)) {
        return await convertToOpenPBRMaterialWithProgress(matData, nativeLoader, onProgress);
    }

    // Fallback to MeshPhysicalMaterial for UsdPreviewSurface
    const material = new THREE.MeshPhysicalMaterial({
        color: 0x808080,
        metalness: 0.0,
        roughness: 0.5
    });

    const preview = matData.usdPreviewSurface || matData;
    if (preview.diffuseColor) {
        material.color = new THREE.Color(...preview.diffuseColor);
    }
    if (preview.metallic !== undefined) {
        material.metalness = preview.metallic;
    }
    if (preview.roughness !== undefined) {
        material.roughness = preview.roughness;
    }

    return material;
}

// ============================================================================
// Memory Cleanup
// ============================================================================

/**
 * Clean up scene resources to free memory before loading a new USD file
 * This disposes Three.js objects and clears WASM memory
 */
function cleanupScene() {
    // Dispose Three.js scene objects (geometry, materials, textures)
    if (sceneState.root) {
        sceneState.root.traverse(obj => {
            if (obj.isMesh) {
                // Dispose geometry
                if (obj.geometry) {
                    obj.geometry.dispose();
                }
                // Dispose materials
                if (obj.material) {
                    if (Array.isArray(obj.material)) {
                        obj.material.forEach(mat => disposeMaterial(mat));
                    } else {
                        disposeMaterial(obj.material);
                    }
                }
            }
        });
        threeState.scene.remove(sceneState.root);
        sceneState.root = null;
    }

    // Clear materials array
    sceneState.materials = [];
    sceneState.meshCount = 0;
    sceneState.textureCount = 0;
    sceneState.lightsCount = 0;
    sceneState.camerasCount = 0;
    sceneState.nodesCount = 0;
    sceneState.pointInstancerCount = 0;
    sceneState.pointInstanceDrawCount = 0;
    sceneState.skeletonCount = 0;
    sceneState.animationCount = 0;
    sceneState.unsupportedRenderableCount = 0;

    // Abort and reset texture loading manager
    if (sceneState.textureLoadingManager) {
        sceneState.textureLoadingManager.abort();
        sceneState.textureLoadingManager.reset();
        sceneState.textureLoadingManager = null;
    }
    hideTextureProgress();

    // Clear WASM memory - native loader
    if (loaderState.nativeLoader) {
        try {
            // Try reset() first (clears all internal state)
            if (typeof loaderState.nativeLoader.reset === 'function') {
                loaderState.nativeLoader.reset();
            }
        } catch (e) {
            try {
                // Fall back to clearAssets() if available
                if (typeof loaderState.nativeLoader.clearAssets === 'function') {
                    loaderState.nativeLoader.clearAssets();
                }
            } catch (e2) {
                console.warn('Could not clear WASM memory:', e2);
            }
        }
        loaderState.nativeLoader = null;
    }

    // Clear WASM memory - worker loader
    // Dispose and recreate worker to free WASM memory in the worker thread
    if (loaderState.workerLoader) {
        try {
            loaderState.workerLoader.dispose();
        } catch (e) {
            console.warn('Could not dispose worker loader:', e);
        }
        loaderState.workerLoader = null;
    }

    // Clear main-thread loader (will be recreated on next load)
    // This ensures WASM module can be garbage collected
    if (loaderState.loader) {
        loaderState.loader = null;
    }

    // Reset scene state
    sceneState.upAxis = 'Y';
    settings.applyUpAxisConversion = false;

    // Update UI buttons
    updateFitButton();
    updateUpAxisButton();

    console.log('[Progress Demo] Scene cleaned up, memory freed');
}

/**
 * Dispose a Three.js material and its textures
 */
function disposeMaterial(material) {
    if (!material) return;

    // Dispose all texture maps
    const textureProps = [
        'map', 'normalMap', 'roughnessMap', 'metalnessMap',
        'emissiveMap', 'aoMap', 'alphaMap', 'envMap',
        'lightMap', 'bumpMap', 'displacementMap', 'specularMap'
    ];

    textureProps.forEach(prop => {
        if (material[prop]) {
            material[prop].dispose();
            material[prop] = null;
        }
    });

    // Dispose the material itself
    if (typeof material.dispose === 'function') {
        material.dispose();
    }
}

// ============================================================================
// Scene Building with Progress
// ============================================================================

/**
 * Build Three.js scene from USD with detailed progress reporting.
 * This function handles the JS layer processing (Three.js mesh/material/texture creation)
 * which CAN show real-time progress since it's async JavaScript (not blocked by WASM).
 *
 * Uses delayed texture loading mode: scene renders first without textures,
 * then textures load in background with separate progress bar.
 */
async function buildSceneWithProgress(usd, onProgress) {
    if (isNextScene(usd)) {
        sceneState.meshCount = 0;
        sceneState.textureCount = 0;
        sceneState.lightsCount = 0;
        sceneState.camerasCount = 0;
        sceneState.nodesCount = 0;
        sceneState.pointInstancerCount = 0;
        sceneState.pointInstanceDrawCount = 0;
        sceneState.skeletonCount = 0;
        sceneState.unsupportedRenderableCount = 0;
        sceneState.materials = [];
        onProgress({
            stage: 'building',
            percentage: 50,
            message: 'Building next render scene...'
        });
        await new Promise(r => requestAnimationFrame(r));
        const built = buildNextThreeNode(usd, {
            skipTextures: false,
            lazyTextures: true,
            onProgress: (info) => {
                const mappedPercentage = 50 + ((info.percentage || 0) * 0.35);
                onProgress({
                    stage: 'building',
                    percentage: Math.min(85, mappedPercentage),
                    message: info.message || 'Building next render scene...'
                });
            }
        });
        sceneState.textureLoadingManager = built.textureManager;
        const counts = nextCountsFromScene(usd);
        sceneState.meshCount = counts.meshes || 0;
        sceneState.textureCount = counts.textures || 0;
        sceneState.lightsCount = counts.lights || 0;
        sceneState.camerasCount = counts.cameras || 0;
        sceneState.nodesCount = counts.nodes || 0;
    sceneState.pointInstancerCount = counts.pointInstancers || 0;
    sceneState.pointInstanceDrawCount = counts.pointInstanceDraws || 0;
    sceneState.skeletonCount = counts.skeletons || 0;
    sceneState.animationCount = counts.animations || 0;
    sceneState.unsupportedRenderableCount = counts.unsupportedRenderables || 0;
        const materialSet = new Set();
        built.node.traverse((obj) => {
            if (!obj.isMesh || !obj.material) return;
            if (Array.isArray(obj.material)) obj.material.forEach((m) => materialSet.add(m));
            else materialSet.add(obj.material);
        });
        sceneState.materials = Array.from(materialSet);
        onProgress({
            stage: 'complete',
            percentage: 100,
            message: `Complete! ${sceneState.meshCount} meshes, ${sceneState.materials.length} materials, ${sceneState.textureCount} textures`
        });
        return built.node;
    }

    const rootNode = usd.getDefaultRootNode();
    if (!rootNode) {
        throw new Error('No root node found in USD');
    }

    sceneState.meshCount = 0;
    sceneState.textureCount = 0;
    sceneState.lightsCount = 0;
    sceneState.camerasCount = 0;
    sceneState.nodesCount = 0;
    sceneState.pointInstancerCount = 0;
    sceneState.pointInstanceDrawCount = 0;
    sceneState.skeletonCount = 0;
    sceneState.animationCount = 0;
    sceneState.unsupportedRenderableCount = 0;
    sceneState.materials = [];

    // Create texture loading manager for delayed texture loading
    sceneState.textureLoadingManager = new TextureLoadingManager();

    // Get mesh/material counts from native loader for accurate progress
    const totalMeshes = usd.numMeshes ? usd.numMeshes() : 0;
    const totalMaterials = usd.numMaterials ? usd.numMaterials() : 0;
    const totalTextures = usd.numTextures ? usd.numTextures() : 0;
    sceneState.lightsCount = usd?.numLights ? usd.numLights() : 0;
    sceneState.camerasCount = usd?.numCameras ? usd.numCameras() : 0;
    sceneState.nodesCount = usd?.numNodes ? usd.numNodes() : 0;
    sceneState.pointInstancerCount = usd?.numPointInstancers ? usd.numPointInstancers() : 0;
    sceneState.pointInstanceDrawCount = usd?.numPointInstanceDraws ? usd.numPointInstanceDraws() : 0;
    sceneState.skeletonCount = usd?.numSkeletons ? usd.numSkeletons() : 0;
    sceneState.animationCount = usd?.numAnimations ? usd.numAnimations() : 0;
    sceneState.unsupportedRenderableCount = typeof usd?.numUnsupportedRenderables === 'function'
        ? usd.numUnsupportedRenderables()
        : (typeof usd?.getUnsupportedRenderables === 'function'
            ? usd.getUnsupportedRenderables().length || 0
            : 0);

    // Phase 1: Building Three.js meshes with per-mesh progress
    // This is JS layer processing - progress WILL update in real-time!
    // Using delayed texture mode - textures are queued, not loaded immediately
    onProgress({
        stage: 'building',
        percentage: 50,
        message: `Building Three.js meshes (0/${totalMeshes})...`
    });

    // Allow initial progress to render
    await new Promise(r => requestAnimationFrame(r));

    const threeRoot = await LightUSDLoaderUtils.buildThreeNode(
        rootNode,
        null,
        usd,
        {
            // Enable delayed texture loading - textures will be queued
            textureLoadingManager: sceneState.textureLoadingManager,

            // Progress callback - called during JS layer mesh building
            // This WILL show real-time updates because buildThreeNode yields to browser
            onProgress: (info) => {
                // Map buildThreeNode progress (0-100%) to our range (50-80%)
                const mappedPercentage = 50 + (info.percentage * 0.3);
                onProgress({
                    stage: 'building',
                    percentage: Math.min(80, mappedPercentage),
                    message: info.message
                });
            }
        }
    );

    // Phase 2: Count meshes and materials from built scene
    onProgress({
        stage: 'materials',
        percentage: 80,
        message: `Counting materials...`
    });

    // Yield to show materials stage
    await new Promise(r => requestAnimationFrame(r));

    const materialSet = new Set();
    let meshIndex = 0;
    threeRoot.traverse((obj) => {
        if (obj.isMesh) {
            sceneState.meshCount++;
            meshIndex++;

            if (obj.material) {
                if (Array.isArray(obj.material)) {
                    obj.material.forEach(m => materialSet.add(m));
                } else {
                    materialSet.add(obj.material);
                }
            }
        }
    });

    sceneState.materials = Array.from(materialSet);

    // Phase 3: Count textures from materials with progress
    onProgress({
        stage: 'textures',
        percentage: 85,
        message: `Counting textures (0/${sceneState.materials.length} materials)...`
    });

    // Yield to show textures stage
    await new Promise(r => requestAnimationFrame(r));

    const textureProps = ['map', 'normalMap', 'roughnessMap', 'metalnessMap', 'emissiveMap', 'aoMap', 'alphaMap'];
    let materialIndex = 0;

    for (const mat of sceneState.materials) {
        materialIndex++;

        textureProps.forEach(prop => {
            if (mat[prop]) sceneState.textureCount++;
        });

        // Update progress every few materials
        if (materialIndex % 5 === 0 || materialIndex === sceneState.materials.length) {
            const texProgress = 85 + (materialIndex / sceneState.materials.length) * 10;
            onProgress({
                stage: 'textures',
                percentage: Math.min(95, texProgress),
                message: `Processing materials (${materialIndex}/${sceneState.materials.length}), ${sceneState.textureCount} textures found`
            });

            // Yield periodically to allow UI updates
            await new Promise(r => requestAnimationFrame(r));
        }
    }

    // Report final counts
    onProgress({
        stage: 'complete',
        percentage: 100,
        message: `Complete! ${sceneState.meshCount} meshes, ${sceneState.materials.length} materials, ${sceneState.textureCount} textures`
    });

    return threeRoot;
}

// ============================================================================
// UpAxis Conversion
// ============================================================================

/**
 * Load scene metadata including upAxis from USD file
 */
function loadSceneMetadata() {
    if (isNextScene(loaderState.nativeLoader)) {
        const metadata = readNextSceneMeta(loaderState.nativeLoader);
        sceneState.upAxis = metadata.upAxis || 'Y';
        return;
    }
    const metadata = loaderState.nativeLoader.getSceneMetadata ? loaderState.nativeLoader.getSceneMetadata() : {};
    sceneState.upAxis = metadata.upAxis || 'Y';
}

/**
 * Initialize upAxis conversion setting based on USD file's upAxis
 * Automatically enable Z-up to Y-up conversion when USD file has upAxis = 'Z'
 */
function initUpAxisConversion() {
    if (sceneState.upAxis === 'Z') {
        settings.applyUpAxisConversion = true;
    } else {
        settings.applyUpAxisConversion = false;
    }
    updateUpAxisButton();
    applyUpAxisConversion();
}

/**
 * Apply or remove Z-up to Y-up rotation based on settings
 */
function applyUpAxisConversion() {
    if (!sceneState.root) return;

    if (settings.applyUpAxisConversion && sceneState.upAxis === 'Z') {
        sceneState.root.rotation.x = -Math.PI / 2;
    } else {
        sceneState.root.rotation.x = 0;
    }
}

// ============================================================================
// Worker Mode Toggle
// ============================================================================

/**
 * Toggle between Web Worker and main-thread loading modes
 */
function toggleWorkerMode() {
    loaderState.useWorker = !loaderState.useWorker;
    updateWorkerButton();

    // Persist preference
    try {
        localStorage.setItem('lightusd-use-worker', loaderState.useWorker ? 'true' : 'false');
    } catch (e) {
        // Ignore localStorage errors
    }

    // Show toast notification
    const mode = loaderState.useWorker ? 'Web Worker' : 'Main Thread';
    showToast(`Loading mode: ${mode}`);
}

/**
 * Update the worker toggle button appearance
 */
function updateWorkerButton() {
    const btn = document.getElementById('worker-btn');
    if (!btn) return;

    if (loaderState.useWorker) {
        btn.classList.add('active');
        btn.title = 'Web Worker mode: ON (responsive UI during loading)';
    } else {
        btn.classList.remove('active');
        btn.title = 'Main Thread mode (may freeze UI during loading)';
    }
}

/**
 * Initialize worker mode from saved preference
 */
function initWorkerMode() {
    try {
        const saved = localStorage.getItem('lightusd-use-worker');
        if (saved !== null) {
            loaderState.useWorker = saved === 'true';
        }
    } catch (e) {
        // Ignore localStorage errors
    }
    updateWorkerButton();
}

/**
 * Toggle Z-up to Y-up conversion
 */
function toggleUpAxisConversion() {
    settings.applyUpAxisConversion = !settings.applyUpAxisConversion;
    updateUpAxisButton();
    applyUpAxisConversion();
}

/**
 * Update the up-axis button appearance
 */
function updateUpAxisButton() {
    const btn = document.getElementById('upaxis-btn');
    if (!btn) return;

    if (sceneState.upAxis === 'Z') {
        btn.style.display = 'flex';
        if (settings.applyUpAxisConversion) {
            btn.classList.add('active');
            btn.title = 'Z-up to Y-up: ON (click to disable)';
        } else {
            btn.classList.remove('active');
            btn.title = 'Z-up to Y-up: OFF (click to enable)';
        }
    } else {
        btn.style.display = 'none';
    }
}

/**
 * Update the fit button visibility
 */
function updateFitButton() {
    const btn = document.getElementById('fit-btn');
    if (!btn) return;

    // Show fit button only when a model is loaded
    btn.style.display = sceneState.root ? 'flex' : 'none';
}

// ============================================================================
// Loading Functions
// ============================================================================

/**
 * Load USD using Web Worker (keeps main thread responsive)
 */
async function loadWithWorker(source, isFile, stats) {
    console.log('[Progress Demo] Using Web Worker for responsive loading');

    // Initialize worker loader if needed
    if (!loaderState.workerLoader) {
        loaderState.workerLoader = new LightUSDWorkerLoader({
            onProgress: (info) => {
                // Worker reports progress - UI updates immediately since main thread is free!
                const phaseMap = {
                    'downloading': { stage: 'downloading', pctBase: 0, pctRange: 30 },
                    'parsing': { stage: 'parsing', pctBase: 30, pctRange: 30 },
                    'extracting': { stage: 'parsing', pctBase: 60, pctRange: 10 },
                    'extracting_meshes': { stage: 'parsing', pctBase: 60, pctRange: 15 },
                    'complete': { stage: 'building', pctBase: 80, pctRange: 0 }
                };
                const mapped = phaseMap[info.phase] || { stage: 'parsing', pctBase: 30, pctRange: 50 };
                const pct = mapped.pctBase + (info.progress * mapped.pctRange);

                updateProgressUI({
                    stage: mapped.stage,
                    percentage: pct,
                    message: info.message || `${info.phase}...`
                });
            },
            onTydraProgress: (info) => {
                // Tydra conversion progress - this updates smoothly since it's from worker!
                const meshProgress = info.meshTotal > 0
                    ? `${info.meshCurrent}/${info.meshTotal}`
                    : '';
                const meshName = info.meshName ? info.meshName.split('/').pop() : '';

                updateProgressUI({
                    stage: 'parsing',
                    percentage: 30 + (info.progress * 50),
                    message: meshProgress
                        ? `Converting: ${meshProgress} ${meshName}`
                        : `Converting: ${info.stage}`
                });
            },
            onTydraComplete: (info) => {
                console.log(`[Worker] Tydra complete: ${info.meshCount} meshes`);
                updateProgressUI({
                    stage: 'building',
                    percentage: 80,
                    message: `Building ${info.meshCount} meshes...`
                });
            }
        });

        await loaderState.workerLoader.init();
    }

    let usd;
    const workerParseOptions = makeStaticNextParseOptions({ backend: settings.backend });
    const parseStart = performance.now();
    if (isFile) {
        // Load from File object
        updateProgressUI({ stage: 'downloading', percentage: 0, message: 'Reading file...' });
        const readStart = performance.now();
        const arrayBuffer = await source.arrayBuffer();
        stats.readMs = performance.now() - readStart;
        stats.fileSize = arrayBuffer.byteLength;
        updateLoadStatsPanel(stats);
        usd = await loaderState.workerLoader.parse(new Uint8Array(arrayBuffer), source.name, workerParseOptions);
    } else {
        // Load from URL - worker handles download progress
        usd = await loaderState.workerLoader.load(source, workerParseOptions);
    }
    stats.parseMs = performance.now() - parseStart;
    updateLoadStatsPanel(stats);

    return usd;
}

/**
 * Load USD on main thread (legacy mode, may freeze UI during parsing)
 */
async function loadWithMainThread(source, isFile, stats) {
    console.log('[Progress Demo] Using main thread loading (UI may freeze during parsing)');

    // Initialize loader if needed
    if (!loaderState.loader) {
        loaderState.loader = new LightUSDLoader(undefined, {
            onTydraProgress: (info) => {
                const meshProgress = info.meshTotal > 0
                    ? `${info.meshCurrent}/${info.meshTotal}`
                    : '';
                const meshName = info.meshName ? info.meshName.split('/').pop() : '';

                updateProgressUI({
                    stage: 'parsing',
                    percentage: 30 + (info.progress * 50),
                    message: meshProgress
                        ? `Converting: ${meshProgress} ${meshName}`
                        : `Converting: ${info.stage}`
                });
            },
            onTydraComplete: (info) => {
                console.log(`[Tydra] Complete: ${info.meshCount} meshes`);
                updateProgressUI({
                    stage: 'building',
                    percentage: 80,
                    message: `Building ${info.meshCount} meshes...`
                });
            }
        });
        await loaderState.loader.init();
        setMaterialXLightUSD(loaderState.loader);
    }

    let usd;
    const parseOptions = makeStaticNextParseOptions({ backend: settings.backend });
    const parseStart = performance.now();
    if (isFile) {
        updateProgressUI({ stage: 'downloading', percentage: 0, message: 'Reading file...' });
        const readStart = performance.now();
        const arrayBuffer = await source.arrayBuffer();
        stats.readMs = performance.now() - readStart;
        stats.fileSize = arrayBuffer.byteLength;
        updateLoadStatsPanel(stats);
        updateProgressUI({ stage: 'parsing', percentage: 30, message: 'Parsing USD...' });
        usd = await new Promise((resolve, reject) => {
            loaderState.loader.parse(new Uint8Array(arrayBuffer), source.name, resolve, reject, parseOptions);
        });
    } else {
        updateProgressUI({ stage: 'downloading', percentage: 0, message: `Fetching ${sourceName(source, false)}...` });
        const fetchStart = performance.now();
        const response = await fetch(source, { cache: 'no-store' });
        if (!response.ok && response.status !== 206) {
            throw new Error(`HTTP ${response.status} ${response.statusText}`);
        }
        const arrayBuffer = await response.arrayBuffer();
        stats.readMs = performance.now() - fetchStart;
        stats.fileSize = arrayBuffer.byteLength;
        updateLoadStatsPanel(stats);
        updateProgressUI({ stage: 'parsing', percentage: 30, message: 'Parsing USD...' });
        usd = await new Promise((resolve, reject) => {
            loaderState.loader.parse(new Uint8Array(arrayBuffer), sourceName(source, false), resolve, reject, parseOptions);
        });
    }
    stats.parseMs = performance.now() - parseStart;
    updateLoadStatsPanel(stats);

    return usd;
}

/**
 * Load USD file with full progress reporting
 * Uses Web Worker by default for responsive UI during heavy parsing
 */
async function loadUSDWithProgress(source, isFile = false) {
    showProgress();
    resetMemoryTracking();
    updateProgressUI({ stage: 'downloading', percentage: 0, message: 'Starting...' });

    // Clean up previous scene to free memory before loading new file
    cleanupScene();
    const stats = beginLoadStats(source, isFile);

    try {
        let usd;

        // Use Web Worker for responsive UI during parsing. LightUSDWorker
        // handles both backends (it dynamically imports the next-only module
        // when the load options request backend=next).
        if (loaderState.useWorker) {
            usd = await loadWithWorker(source, isFile, stats);
        } else {
            usd = await loadWithMainThread(source, isFile, stats);
        }

        loaderState.nativeLoader = usd;

        // Load scene metadata (upAxis, etc.)
        loadSceneMetadata();

        // Build scene with progress
        const processStart = performance.now();
        const threeRoot = await buildSceneWithProgress(usd, updateProgressUI);
        stats.processMs = performance.now() - processStart;

        // Add to scene
        if (sceneState.root) {
            threeState.scene.remove(sceneState.root);
        }
        sceneState.root = threeRoot;
        threeState.scene.add(threeRoot);

        // Apply Z-up to Y-up conversion if needed
        initUpAxisConversion();

        // Fit camera to scene
        fitCameraToObject(threeRoot);

        // Update UI
        updateModelInfo();
        finishLoadStats(stats);
        updateStatus(`Model loaded (backend: ${isNextScene(usd) ? 'next' : 'legacy'}, upAxis: ${sceneState.upAxis})`);

        hideProgress();

        // Force a render frame BEFORE starting texture loading
        // This ensures the scene without textures is displayed first
        await new Promise(r => requestAnimationFrame(r));
        threeState.renderer.render(threeState.scene, threeState.camera);

        // Start delayed texture loading if there are queued textures
        if (sceneState.textureLoadingManager && sceneState.textureLoadingManager.total > 0) {
            const texManager = sceneState.textureLoadingManager;
            const totalTextures = texManager.total;

            console.log(`[Progress Demo] Starting delayed texture loading: ${totalTextures} textures queued`);
            showTextureProgress();

            // Start texture loading with progress callbacks
            // This runs asynchronously - scene is already visible and interactive
            texManager.startLoading({
                onProgress: updateTextureProgressUI,
                onTextureLoaded: (material, mapProperty, texture) => {
                    // Material is automatically updated by the manager
                    // Force re-render to show the new texture
                    material.needsUpdate = true;
                },
                concurrency: 2,      // Load 2 textures at a time
                yieldInterval: 16    // Yield to browser every frame (~60fps)
            }).then(status => {
                console.log(`[Progress Demo] Texture loading complete: ${status.loaded}/${status.total} loaded, ${status.failed} failed`);

                // Update texture count in model info
                sceneState.textureCount = status.loaded;
                document.getElementById('texture-count').textContent = sceneState.textureCount;
            }).catch(err => {
                console.error('[Progress Demo] Texture loading error:', err);
                hideTextureProgress();
            });
        }

    } catch (error) {
        console.error('Failed to load USD:', error);
        updateStatus(`Error: ${error.message}`);
        hideProgress();
        showToast(`Failed to load: ${error.message}`);
    }
}

// ============================================================================
// Three.js Setup
// ============================================================================

function initThreeJS() {
    const container = document.getElementById('canvas-container');

    // Scene
    threeState.scene = new THREE.Scene();
    threeState.scene.background = new THREE.Color(DEFAULT_BACKGROUND_COLOR);

    // Camera
    const aspect = container.clientWidth / container.clientHeight;
    threeState.camera = new THREE.PerspectiveCamera(45, aspect, 0.01, 1000);
    threeState.camera.position.set(3, 2, 3);

    // Renderer
    threeState.renderer = new THREE.WebGLRenderer({ antialias: true });
    threeState.renderer.setSize(container.clientWidth, container.clientHeight);
    threeState.renderer.setPixelRatio(window.devicePixelRatio);
    threeState.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    threeState.renderer.toneMappingExposure = 1.0;
    threeState.renderer.outputColorSpace = THREE.SRGBColorSpace;
    container.appendChild(threeState.renderer.domElement);

    // Controls
    threeState.controls = new OrbitControls(threeState.camera, threeState.renderer.domElement);
    threeState.controls.enableDamping = true;
    threeState.controls.dampingFactor = 0.05;

    // Lights
    const ambientLight = new THREE.AmbientLight(0xffffff, 0.5);
    threeState.scene.add(ambientLight);

    const directionalLight = new THREE.DirectionalLight(0xffffff, 1.0);
    directionalLight.position.set(5, 10, 7.5);
    threeState.scene.add(directionalLight);

    // Grid helper
    const grid = new THREE.GridHelper(10, 10, 0x444444, 0x333333);
    threeState.scene.add(grid);

    // Handle resize
    window.addEventListener('resize', onWindowResize);

    // Start animation loop
    animate();
}

function onWindowResize() {
    const container = document.getElementById('canvas-container');
    threeState.camera.aspect = container.clientWidth / container.clientHeight;
    threeState.camera.updateProjectionMatrix();
    threeState.renderer.setSize(container.clientWidth, container.clientHeight);
}

function animate() {
    requestAnimationFrame(animate);
    const now = performance.now();
    if (!loadStatsState.fpsLastTime) loadStatsState.fpsLastTime = now;
    loadStatsState.frames++;
    if (now - loadStatsState.fpsLastTime >= 500) {
        loadStatsState.fps = Math.round((loadStatsState.frames * 1000) / (now - loadStatsState.fpsLastTime));
        loadStatsState.frames = 0;
        loadStatsState.fpsLastTime = now;
        updateLoadStatsPanel();
    }
    threeState.controls.update();
    threeState.renderer.render(threeState.scene, threeState.camera);
}

/**
 * Fit camera to view an object or the entire scene
 * Uses bounding sphere for better camera positioning with aspect ratio consideration
 */
function fitCameraToObject(object) {
    if (!object) return;

    // Ensure world matrices are updated (important after upAxis rotation)
    object.updateMatrixWorld(true);

    const box = new THREE.Box3().setFromObject(object);
    if (box.isEmpty()) return;

    const size = box.getSize(new THREE.Vector3());
    const center = box.getCenter(new THREE.Vector3());

    // Use bounding sphere radius for better coverage
    const boundingSphereRadius = size.length() * 0.5;
    const fov = threeState.camera.fov * (Math.PI / 180);
    const aspectRatio = threeState.camera.aspect;

    // Calculate distance based on both horizontal and vertical FOV
    const fovH = 2 * Math.atan(Math.tan(fov / 2) * aspectRatio);
    const distanceV = boundingSphereRadius / Math.sin(fov / 2);
    const distanceH = boundingSphereRadius / Math.sin(fovH / 2);
    const distance = Math.max(distanceV, distanceH) * CAMERA_PADDING;

    // Position camera at an angle for better viewing
    const cameraOffset = new THREE.Vector3(0.5, 0.35, 1).normalize().multiplyScalar(distance);
    threeState.camera.position.copy(center).add(cameraOffset);
    threeState.controls.target.copy(center);
    threeState.controls.update();

    // Update near/far planes based on scene size
    const maxDim = Math.max(size.x, size.y, size.z);
    threeState.camera.near = maxDim * 0.001;
    threeState.camera.far = maxDim * 100;
    threeState.camera.updateProjectionMatrix();
}

/**
 * Fit camera to the current scene
 */
function fitToScene() {
    if (!sceneState.root) {
        showToast('No model loaded');
        return;
    }

    fitCameraToObject(sceneState.root);
    showToast('Camera fitted to scene');
}

// ============================================================================
// UI Functions
// ============================================================================

function updateStatus(message) {
    document.getElementById('status').textContent = message;
}

function updateModelInfo() {
    document.getElementById('model-info').style.display = 'block';
    document.getElementById('current-file').textContent = loadStatsState.current?.filename || '-';
    document.getElementById('backend-name').textContent = settings.backend;
    document.getElementById('mesh-count').textContent = sceneState.meshCount;
    document.getElementById('material-count').textContent = sceneState.materials.length;
    document.getElementById('texture-count').textContent = sceneState.textureCount;
    document.getElementById('light-count').textContent = sceneState.lightsCount;
    document.getElementById('camera-count').textContent = sceneState.camerasCount;
    document.getElementById('node-count').textContent = sceneState.nodesCount;
    document.getElementById('point-instancer-count').textContent = sceneState.pointInstancerCount;
    document.getElementById('point-instance-draw-count').textContent = sceneState.pointInstanceDrawCount;
    document.getElementById('skeleton-count').textContent = sceneState.skeletonCount;
    document.getElementById('animation-count').textContent = sceneState.animationCount;
    document.getElementById('unsupported-renderable-count').textContent = sceneState.unsupportedRenderableCount;

    // Update toolbar buttons visibility
    updateFitButton();
}

function showToast(message, duration = 3000) {
    const toast = document.getElementById('toast');
    toast.textContent = message;
    toast.classList.add('visible');
    setTimeout(() => toast.classList.remove('visible'), duration);
}

// ============================================================================
// File Loading
// ============================================================================

function loadFile() {
    document.getElementById('file-input').click();
}

function handleFileSelect(event) {
    const file = event.target.files[0];
    if (file) {
        loadUSDWithProgress(file, true);
    }
    event.target.value = '';
}

function loadSampleModel() {
    const randomIndex = Math.floor(Math.random() * SAMPLE_MODELS.length);
    loadUSDWithProgress(SAMPLE_MODELS[randomIndex]);
}

// ============================================================================
// Drag and Drop
// ============================================================================

function setupDragAndDrop() {
    const container = document.getElementById('canvas-container');

    container.addEventListener('dragover', (e) => {
        e.preventDefault();
        container.classList.add('drag-over');
    });

    container.addEventListener('dragleave', () => {
        container.classList.remove('drag-over');
    });

    container.addEventListener('drop', (e) => {
        e.preventDefault();
        container.classList.remove('drag-over');

        const file = e.dataTransfer.files[0];
        if (file && /\.(usd|usda|usdc|usdz)$/i.test(file.name)) {
            loadUSDWithProgress(file, true);
        } else {
            showToast('Please drop a USD file (.usd, .usda, .usdc, .usdz)');
        }
    });
}

// ============================================================================
// Initialization
// ============================================================================

async function init() {
    updateStatus('Initializing...');

    initThreeJS();
    setupDragAndDrop();
    initMemoryGraph();
    initWorkerMode();

    // Setup file input handler
    document.getElementById('file-input').addEventListener('change', handleFileSelect);
    const backendSelect = document.getElementById('backend-select');
    if (backendSelect) {
        backendSelect.value = settings.backend;
        backendSelect.addEventListener('change', () => {
            settings.backend = backendSelect.value;
            updateLoadStatsPanel();
            updateStatus(`Backend set to ${settings.backend}`);
        });
    }

    const params = new URLSearchParams(window.location.search);
    const initialUrl = urlParam(params);
    if (initialUrl) {
        loadUSDWithProgress(initialUrl, false);
        return;
    }

    updateStatus('Ready - Load a USD file to begin');
    updateLoadStatsPanel({
        filename: '-',
        backend: settings.backend,
        fileSize: NaN,
        readMs: NaN,
        parseMs: NaN,
        processMs: NaN,
        totalMs: NaN,
        memoryBefore: captureStatsMemory(),
        memoryAfter: captureStatsMemory()
    });
}

// Export for HTML onclick handlers
window.loadFile = loadFile;
window.loadSampleModel = loadSampleModel;
window.toggleUpAxisConversion = toggleUpAxisConversion;
window.toggleWorkerMode = toggleWorkerMode;
window.fitToScene = fitToScene;

// Start the app
init();
