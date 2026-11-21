// progress-offscreenwebgl.js -- Main thread entry point for OffscreenCanvas progress demo.
//
// Responsibilities:
//   1. Check OffscreenCanvas support.
//   2. Transfer canvas control to a dedicated Web Worker.
//   3. Forward pointer / wheel / resize events to the worker.
//   4. Handle file-drop / file-picker -> send ArrayBuffer to worker.
//   5. Receive progress / status / loaded / error messages from worker -> update DOM.
//   6. Display progress bar, memory graph, and stage indicators.

// ============================================================================
// DOM references (mutable: canvas is replaced on worker respawn)
// ============================================================================

import { getAssetUriFromURL, mountBackendSelector } from './src/lightusd/LoaderConfigUtils.js';

let canvas = document.getElementById('gl');
const statusEl = document.getElementById('status');
const modelInfoEl = document.getElementById('model-info');
const meshCountEl = document.getElementById('mesh-count');
const materialCountEl = document.getElementById('material-count');
const textureCountEl = document.getElementById('texture-count');
const lightCountEl = document.getElementById('light-count');
const cameraCountEl = document.getElementById('camera-count');
const nodeCountEl = document.getElementById('node-count');
const pointInstancerCountEl = document.getElementById('point-instancer-count');
const pointInstanceDrawCountEl = document.getElementById('point-instance-draw-count');
const skeletonCountEl = document.getElementById('skeleton-count');
const animationCountEl = document.getElementById('animation-count');
const unsupportedRenderableCountEl = document.getElementById('unsupported-renderable-count');
const currentFileEl = document.getElementById('current-file');
const fpsValueEl = document.getElementById('fps-value');
const loadStatsEl = document.getElementById('load-stats');
const loadBtn = document.getElementById('load-btn');
const sampleBtn = document.getElementById('sample-btn');
const upAxisBtn = document.getElementById('upaxis-btn');
const fitBtn = document.getElementById('fit-btn');
const fileInput = document.getElementById('file-input');
const unsupportedOverlay = document.getElementById('unsupported-overlay');

function getStartupUSDModelURI(params = new URLSearchParams(window.location.search)) {
    return getAssetUriFromURL(params, ['usd']);
}

function getBackendFromURL(params = new URLSearchParams(window.location.search)) {
    const backend = params.get('backend');
    return (backend === 'core' || backend === 'auto' || backend === 'legacy')
        ? backend
        : 'core';
}

function getDisplayNameFromURI(uri) {
    try {
        const parsed = new URL(uri, window.location.href);
        return parsed.pathname.split('/').filter(Boolean).pop() || uri;
    } catch {
        return uri.split('/').pop() || uri;
    }
}

// Progress UI DOM elements (cached to avoid repeated lookups)
const progressContainer = document.getElementById('progress-container');
const progressBar = document.getElementById('progress-bar');
const progressPercentageEl = document.getElementById('progress-percentage');
const progressStageEl = document.getElementById('progress-stage');
const progressDetailsEl = document.getElementById('progress-details');
const textureProgressContainer = document.getElementById('texture-progress-container');
const textureProgressBar = document.getElementById('texture-progress-bar');
const textureProgressCountEl = document.getElementById('texture-progress-count');
const textureProgressDetailsEl = document.getElementById('texture-progress-details');
const toastEl = document.getElementById('toast');
const progressStopBtn = document.getElementById('progress-stop-btn');
const progressPauseBtn = document.getElementById('progress-pause-btn');
const progressDebugToggle = document.getElementById('progress-debug-toggle');
const progressDebugDelay = document.getElementById('progress-debug-delay');

// Memory panel DOM elements
const memUsedEl = document.getElementById('mem-used');
const memTotalEl = document.getElementById('mem-total');
const memLimitEl = document.getElementById('mem-limit');
const memPointsEl = document.getElementById('mem-points');
const memWarningEl = document.getElementById('memory-warning');

// Set to true to auto-load a sample model on page load (for development)
let debugLoadOnInit = true;

// Sample models
const SAMPLE_MODELS = [
    'assets/fancy-teapot-mtlx.usdz'
];
const startupUSDURI = getStartupUSDModelURI();

// Scene state (tracked on main thread for UI)
const sceneState = {
    hasModel: false,
    upAxis: 'Y',
    applyUpAxisConversion: false
};

// ============================================================================
// Worker lifecycle (mutable — replaced on respawn after OOM)
// ============================================================================

let worker = null;
let preserveStatus = false;  // When true, ignore worker 'status' messages (keep error visible)
const WORKER_URL = new URL('./progress-offscreenwebgl.worker.js', import.meta.url);

// Loading watchdog: detects worker stuck (e.g. WASM OOM blocking event loop)
const LOADING_WATCHDOG_TIMEOUT_MS = 10000;
let loadingWatchdogTimer = null;

function onWatchdogTimeout() {
    loadingWatchdogTimer = null;
    console.warn('[main] Loading watchdog timeout — worker stuck, respawning...');
    statusEl.textContent = 'Error: Loading timed out — WASM out of memory. Worker restarted.';
    statusEl.className = 'error';
    preserveStatus = true;
    hideProgress();
    showToast('WASM out of memory — worker restarted. Try a smaller file.', 5000);
    respawnWorker();
}

function startLoadingWatchdog() {
    clearLoadingWatchdog();
    loadingWatchdogTimer = setTimeout(onWatchdogTimeout, LOADING_WATCHDOG_TIMEOUT_MS);
}

function resetLoadingWatchdog() {
    if (loadingWatchdogTimer !== null) {
        clearTimeout(loadingWatchdogTimer);
        loadingWatchdogTimer = setTimeout(onWatchdogTimeout, LOADING_WATCHDOG_TIMEOUT_MS);
    }
}

function clearLoadingWatchdog() {
    if (loadingWatchdogTimer !== null) {
        clearTimeout(loadingWatchdogTimer);
        loadingWatchdogTimer = null;
    }
}

// ============================================================================
// Browser capability check
// ============================================================================

if (typeof canvas.transferControlToOffscreen !== 'function') {
    unsupportedOverlay.classList.add('visible');
    throw new Error('OffscreenCanvas not supported');
}

// ============================================================================
// Worker spawn / respawn
// ============================================================================

/**
 * Replace the <canvas> element (transferControlToOffscreen is one-shot per element).
 * Returns the new canvas.
 */
function replaceCanvas() {
    const parent = canvas.parentElement;
    const newCanvas = document.createElement('canvas');
    newCanvas.id = 'gl';
    // Copy CSS classes if any
    newCanvas.className = canvas.className;
    parent.replaceChild(newCanvas, canvas);
    canvas = newCanvas;
    attachCanvasEvents(canvas);
    resizeObserver.observe(canvas);
    return canvas;
}

/**
 * Spawn a new worker, transfer offscreen canvas, and wire up listeners.
 * Called once on startup and again after respawn.
 */
function spawnWorker() {
    worker = new Worker(WORKER_URL, { type: 'module' });

    const offscreen = canvas.transferControlToOffscreen();

    worker.postMessage(
        {
            type: 'init',
            canvas: offscreen,
            width: canvas.clientWidth,
            height: canvas.clientHeight,
            pixelRatio: window.devicePixelRatio || 1,
        },
        [offscreen]
    );

    worker.addEventListener('message', onWorkerMessage);
    worker.addEventListener('error', onWorkerError);

    // Sync debug delay setting to the new worker
    const debugEnabled = progressDebugToggle.checked;
    const debugMs = debugEnabled ? parseInt(progressDebugDelay.value, 10) || 0 : 0;
    worker.postMessage({ type: 'setDebugDelay', delayMs: debugMs });
}

/**
 * Terminate the stuck worker, replace the canvas, and spawn a fresh worker.
 * This fully resets the WASM VM — the new worker loads a fresh WASM instance.
 */
function respawnWorker() {
    clearLoadingWatchdog();

    // Terminate the stuck worker (kills its event loop, frees WASM memory)
    if (worker) {
        worker.terminate();
        worker = null;
    }

    // Reset UI state
    sceneState.hasModel = false;
    modelInfoEl.style.display = 'none';
    updateUpAxisButton();
    updateFitButton();

    // Replace canvas (transferControlToOffscreen is one-shot)
    replaceCanvas();

    // Spawn fresh worker (caller sets status text before calling respawnWorker)
    spawnWorker();
}

// ============================================================================
// Progress UI Functions
// ============================================================================

let lastProgressUpdate = 0;
const PROGRESS_UPDATE_INTERVAL = 50;

const STAGE_ORDER = ['downloading', 'parsing', 'building', 'materials', 'textures', 'complete'];

const STAGE_LABELS = {
    'downloading': 'Downloading file...',
    'parsing': 'Parsing USD (Worker)...',
    'building': 'Building Three.js scene...',
    'materials': 'Converting materials...',
    'textures': 'Decoding textures...',
    'complete': 'Complete!'
};

// Cache stage item DOM elements
const stageElements = {};
STAGE_ORDER.forEach(stage => {
    const item = document.querySelector(`.progress-stage-item[data-stage="${stage}"]`);
    if (item) {
        stageElements[stage] = { item, icon: item.querySelector('.stage-icon') };
    }
});

function showProgress() {
    progressContainer.classList.add('visible');
    progressPauseBtn.textContent = 'Pause';
    progressPauseBtn.classList.remove('paused');
    progressBar.classList.remove('paused');
    resetProgressStages();
}

function hideProgress() {
    progressContainer.classList.remove('visible');
    progressPauseBtn.textContent = 'Pause';
    progressPauseBtn.classList.remove('paused');
    progressBar.classList.remove('paused');
}

function resetProgressStages() {
    for (const stage of STAGE_ORDER) {
        const el = stageElements[stage];
        if (!el) continue;
        el.item.classList.remove('active', 'completed');
        el.icon.classList.remove('active', 'completed');
        el.icon.classList.add('pending');
    }

    progressBar.style.transform = 'scaleX(0)';
    progressBar.classList.remove('complete');
    progressPercentageEl.textContent = '0%';
    lastProgressUpdate = 0;
}

function updateProgressUI({ stage, percentage, message }) {
    const pct = Math.round(percentage);

    // Throttle updates — only record data point when throttled
    const now = performance.now();
    if (now - lastProgressUpdate < PROGRESS_UPDATE_INTERVAL && pct < 100) {
        recordMemoryPoint(stage, pct, false);
        return;
    }
    lastProgressUpdate = now;

    // Update progress bar
    progressBar.style.transform = `scaleX(${pct / 100})`;

    if (pct >= 100 || stage === 'complete') {
        progressBar.classList.add('complete');
    }

    progressPercentageEl.textContent = `${pct}%`;
    progressStageEl.textContent = STAGE_LABELS[stage] || stage;
    progressDetailsEl.textContent = message || '';

    updateStageIcons(stage);
    recordMemoryPoint(stage, pct, true);
}

function updateStageIcons(currentStage) {
    const currentIndex = STAGE_ORDER.indexOf(currentStage);

    STAGE_ORDER.forEach((stage, index) => {
        const el = stageElements[stage];
        if (!el) return;

        el.item.classList.remove('active', 'completed');
        el.icon.classList.remove('active', 'completed', 'pending');

        if (index < currentIndex) {
            el.item.classList.add('completed');
            el.icon.classList.add('completed');
            el.icon.textContent = '\u2713';
        } else if (index === currentIndex) {
            el.item.classList.add('active');
            el.icon.classList.add('active');
            el.icon.textContent = String(index + 1);
        } else {
            el.icon.classList.add('pending');
            el.icon.textContent = String(index + 1);
        }
    });
}

// ============================================================================
// Texture Progress UI
// ============================================================================

function showTextureProgress() {
    if (textureProgressContainer) {
        textureProgressContainer.classList.add('visible');
        if (textureProgressBar) textureProgressBar.style.transform = 'scaleX(0)';
    }
}

function hideTextureProgress() {
    if (textureProgressContainer) textureProgressContainer.classList.remove('visible');
}

function updateTextureProgressUI(info) {
    const { loaded, total, failed, percentage, currentTexture, isComplete } = info;

    if (textureProgressCountEl) {
        const failedText = failed > 0 ? ` (${failed} failed)` : '';
        textureProgressCountEl.textContent = `${loaded}/${total}${failedText}`;
    }

    if (textureProgressBar) {
        textureProgressBar.style.transform = `scaleX(${percentage / 100})`;
    }

    if (textureProgressDetailsEl) {
        if (isComplete) {
            textureProgressDetailsEl.textContent = failed > 0
                ? `Complete with ${failed} failures`
                : 'All textures loaded';
        } else if (currentTexture) {
            textureProgressDetailsEl.textContent = `Loading: ${currentTexture}`;
        } else {
            textureProgressDetailsEl.textContent = 'Starting texture loading...';
        }
    }

    if (isComplete) {
        setTimeout(() => hideTextureProgress(), 2000);
    }
}

// ============================================================================
// Memory Tracking
// ============================================================================

const memoryState = {
    available: false,
    dataPoints: [],
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

function initMemoryGraph() {
    memoryState.canvas = document.getElementById('memory-graph');
    if (!memoryState.canvas) return;

    const rect = memoryState.canvas.parentElement.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    memoryState.canvas.width = rect.width * dpr;
    memoryState.canvas.height = 120 * dpr;
    memoryState.canvas.style.width = rect.width + 'px';
    memoryState.canvas.style.height = '120px';

    memoryState.ctx = memoryState.canvas.getContext('2d');
    memoryState.ctx.scale(dpr, dpr);

    memoryState.available = !!(performance && performance.memory);
    if (!memoryState.available && memWarningEl) {
        memWarningEl.classList.add('visible');
    }
}

function formatBytes(bytes) {
    if (!Number.isFinite(bytes)) return 'n/a';
    if (bytes === 0) return '0 B';
    const sign = bytes < 0 ? '-' : '';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const value = Math.abs(bytes);
    const i = Math.min(Math.floor(Math.log(value) / Math.log(k)), sizes.length - 1);
    return sign + parseFloat((value / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
}

function getMemoryUsage() {
    if (!memoryState.available) return { used: 0, total: 0, limit: 0 };
    const mem = performance.memory;
    return { used: mem.usedJSHeapSize, total: mem.totalJSHeapSize, limit: mem.jsHeapSizeLimit };
}

function recordMemoryPoint(stage, percentage, drawGraph = true) {
    const mem = getMemoryUsage();
    const time = performance.now() - memoryState.startTime;

    memoryState.dataPoints.push({ time, used: mem.used, total: mem.total, limit: mem.limit, stage, percentage });

    if (memoryState.dataPoints.length > memoryState.maxDataPoints) {
        memoryState.dataPoints.shift();
    }

    updateMemoryStats(mem);
    if (drawGraph) drawMemoryGraph();
}

function updateMemoryStats(mem) {
    if (memUsedEl) memUsedEl.textContent = formatBytes(mem.used);
    if (memTotalEl) memTotalEl.textContent = formatBytes(mem.total);
    if (memLimitEl) memLimitEl.textContent = formatBytes(mem.limit);
    if (memPointsEl) memPointsEl.textContent = memoryState.dataPoints.length;

    if (memUsedEl) {
        const usageRatio = mem.used / mem.limit;
        memUsedEl.classList.remove('warning', 'critical');
        if (usageRatio > 0.8) memUsedEl.classList.add('critical');
        else if (usageRatio > 0.6) memUsedEl.classList.add('warning');
    }
}

function drawMemoryGraph() {
    const ctx = memoryState.ctx;
    const cvs = memoryState.canvas;
    if (!ctx || !cvs) return;

    const width = cvs.width / (window.devicePixelRatio || 1);
    const height = cvs.height / (window.devicePixelRatio || 1);
    const padding = { top: 10, right: 10, bottom: 20, left: 50 };
    const graphWidth = width - padding.left - padding.right;
    const graphHeight = height - padding.top - padding.bottom;

    ctx.fillStyle = '#111';
    ctx.fillRect(0, 0, width, height);

    if (memoryState.dataPoints.length < 2) return;

    const points = memoryState.dataPoints;
    const maxMem = points.reduce((max, p) => Math.max(max, p.used, p.total), 0);
    const limitMem = points[0].limit || maxMem * 1.2;
    const yMax = Math.max(maxMem * 1.1, limitMem);
    const timeRange = points[points.length - 1].time - points[0].time;

    // Grid
    ctx.strokeStyle = memoryState.colors.grid;
    ctx.lineWidth = 0.5;
    for (let i = 0; i <= 4; i++) {
        const y = padding.top + (graphHeight / 4) * i;
        ctx.beginPath();
        ctx.moveTo(padding.left, y);
        ctx.lineTo(width - padding.right, y);
        ctx.stroke();
    }

    // Limit line
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

    const drawLine = (dataKey, color) => {
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.beginPath();
        points.forEach((point, i) => {
            const x = padding.left + (graphWidth * (point.time - points[0].time) / (timeRange || 1));
            const y = padding.top + graphHeight * (1 - point[dataKey] / yMax);
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        });
        ctx.stroke();
    };

    drawLine('total', memoryState.colors.total);
    drawLine('used', memoryState.colors.used);

    // Stage change markers
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

    // Y axis labels
    ctx.fillStyle = memoryState.colors.text;
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++) {
        const y = padding.top + (graphHeight / 4) * i;
        const value = yMax * (1 - i / 4);
        ctx.fillText(formatBytes(value), padding.left - 5, y);
    }

    // Current stage label
    if (points.length > 0) {
        const lastPoint = points[points.length - 1];
        ctx.fillStyle = '#FF9800';
        ctx.font = '10px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(lastPoint.stage || '', width / 2, height - 5);
    }
}

function resetMemoryTracking() {
    memoryState.dataPoints = [];
    memoryState.startTime = performance.now();

    if (memUsedEl) memUsedEl.textContent = '--';
    if (memTotalEl) memTotalEl.textContent = '--';
    if (memLimitEl) memLimitEl.textContent = '--';
    if (memPointsEl) memPointsEl.textContent = '0';

    if (memoryState.ctx && memoryState.canvas) {
        const width = memoryState.canvas.width / (window.devicePixelRatio || 1);
        const height = memoryState.canvas.height / (window.devicePixelRatio || 1);
        memoryState.ctx.fillStyle = '#111';
        memoryState.ctx.fillRect(0, 0, width, height);
    }
}

let currentLoadStats = null;

function formatDurationMs(ms) {
    if (!Number.isFinite(ms)) return 'n/a';
    return ms < 1000 ? `${ms.toFixed(0)} ms` : `${(ms / 1000).toFixed(2)} s`;
}

function beginLoadStats(fileSize = null, filename = '-') {
    currentLoadStats = {
        startTime: performance.now(),
        fileSize,
        fetchMs: null,
        totalMs: null,
        memoryBefore: getMemoryUsage(),
        memoryAfter: null
    };
    if (currentFileEl) currentFileEl.textContent = filename;
    updateLoadStatsPanel('Loading...');
    return currentLoadStats;
}

function updateLoadStatsPanel(overrideText = null) {
    if (!loadStatsEl || !currentLoadStats) return;
    if (overrideText) {
        loadStatsEl.textContent = overrideText;
        return;
    }
    const stats = currentLoadStats;
    const before = stats.memoryBefore || {};
    const after = stats.memoryAfter || {};
    const jsDelta = Number.isFinite(after.used) && Number.isFinite(before.used)
        ? ` (${after.used - before.used >= 0 ? '+' : ''}${formatBytes(after.used - before.used)})`
        : '';
    loadStatsEl.textContent = [
        `File: ${formatBytes(stats.fileSize || 0)}`,
        `Fetch/read: ${stats.fetchMs === null ? 'n/a' : formatDurationMs(stats.fetchMs)}`,
        `Worker total: ${formatDurationMs(stats.totalMs)}`,
        `JS heap: ${Number.isFinite(after.used) ? formatBytes(after.used) + jsDelta : 'n/a'}`
    ].join('\n');
}

function finishLoadStats() {
    if (!currentLoadStats) return;
    currentLoadStats.totalMs = performance.now() - currentLoadStats.startTime;
    currentLoadStats.memoryAfter = getMemoryUsage();
    updateLoadStatsPanel();
}

function failLoadStats() {
    if (!currentLoadStats) return;
    currentLoadStats.totalMs = performance.now() - currentLoadStats.startTime;
    currentLoadStats.memoryAfter = getMemoryUsage();
    updateLoadStatsPanel('Failed');
}

// ============================================================================
// Toast
// ============================================================================

function showToast(message, duration = 3000) {
    toastEl.textContent = message;
    toastEl.classList.add('visible');
    setTimeout(() => toastEl.classList.remove('visible'), duration);
}

// ============================================================================
// UpAxis / Fit buttons
// ============================================================================

function updateUpAxisButton() {
    if (sceneState.upAxis === 'Z') {
        upAxisBtn.style.display = 'flex';
        if (sceneState.applyUpAxisConversion) {
            upAxisBtn.classList.add('active');
            upAxisBtn.title = 'Z-up to Y-up: ON (click to disable)';
        } else {
            upAxisBtn.classList.remove('active');
            upAxisBtn.title = 'Z-up to Y-up: OFF (click to enable)';
        }
    } else {
        upAxisBtn.style.display = 'none';
    }
}

function updateFitButton() {
    fitBtn.style.display = sceneState.hasModel ? 'flex' : 'none';
}

// ============================================================================
// Worker message handlers (shared by initial spawn and respawns)
// ============================================================================

function onWorkerMessage(e) {
    const msg = e.data;

    switch (msg.type) {
        case 'status':
            if (!preserveStatus) {
                statusEl.textContent = msg.message;
                statusEl.className = '';
            }
            // Worker signals ready after WASM init + env load
            if (debugLoadOnInit && msg.message.startsWith('Ready')) {
                debugLoadOnInit = false;  // Only auto-load once (not on respawn)
                if (startupUSDURI) {
                    loadModelFromURL(startupUSDURI);
                } else {
                    loadSampleModel();
                }
            }
            break;

        case 'progress':
            resetLoadingWatchdog();
            updateProgressUI({
                stage: msg.stage,
                percentage: msg.percentage,
                message: msg.message
            });
            break;

        case 'loaded':
            clearLoadingWatchdog();
            statusEl.textContent = `Loaded: ${msg.meshCount} meshes, ${msg.materialCount} materials`;
            statusEl.className = '';
            meshCountEl.textContent = msg.meshCount;
            materialCountEl.textContent = msg.materialCount;
            textureCountEl.textContent = msg.textureCount || 0;
            if (lightCountEl) lightCountEl.textContent = msg.lights || 0;
            if (cameraCountEl) cameraCountEl.textContent = msg.cameras || 0;
            if (nodeCountEl) nodeCountEl.textContent = msg.nodes || 0;
            if (pointInstancerCountEl) pointInstancerCountEl.textContent = msg.pointInstancers || 0;
            if (pointInstanceDrawCountEl) pointInstanceDrawCountEl.textContent = msg.pointInstanceDraws || 0;
            if (skeletonCountEl) skeletonCountEl.textContent = msg.skeletons || 0;
            if (animationCountEl) animationCountEl.textContent = msg.animations || 0;
            if (unsupportedRenderableCountEl) unsupportedRenderableCountEl.textContent = msg.unsupportedRenderables || 0;
            modelInfoEl.style.display = 'block';

            sceneState.hasModel = true;
            sceneState.upAxis = msg.upAxis || 'Y';
            sceneState.applyUpAxisConversion = (sceneState.upAxis === 'Z');
            updateUpAxisButton();
            updateFitButton();

			finishLoadStats();
			hideProgress();
			window.renderComplete = true;
			break;

        case 'fps':
            if (fpsValueEl) fpsValueEl.textContent = msg.fps.toFixed(1);
            break;

        case 'texture_progress':
            if (msg.isStart) {
                showTextureProgress();
            }
            updateTextureProgressUI(msg);
            if (msg.isComplete) {
                textureCountEl.textContent = msg.loaded;
            }
            break;

        case 'error':
            clearLoadingWatchdog();
            statusEl.textContent = `Error: ${msg.message}`;
            statusEl.className = 'error';
            failLoadStats();
            hideProgress();
            showToast(`Failed to load: ${msg.message}`);
            console.error('[Worker error]', msg.message);
            break;

        case 'paused':
            clearLoadingWatchdog();  // Don't timeout while paused
            progressStageEl.textContent = 'Paused';
            progressBar.classList.add('paused');
            break;

        case 'resumed':
            startLoadingWatchdog();  // Restart watchdog
            progressBar.classList.remove('paused');
            break;

        case 'debugDelay': {
            // Worker has debug delay enabled — extend watchdog to avoid false timeouts
            const totalDelay = msg.delayMs * msg.phases + LOADING_WATCHDOG_TIMEOUT_MS;
            console.log(`[main] Debug delay active: ${msg.delayMs}ms × ${msg.phases} phases, watchdog extended to ${totalDelay}ms`);
            clearLoadingWatchdog();
            loadingWatchdogTimer = setTimeout(onWatchdogTimeout, totalDelay);
            break;
        }
    }
}

function onWorkerError(e) {
    clearLoadingWatchdog();
    statusEl.textContent = `Worker error: ${e.message}`;
    statusEl.className = 'error';
    hideProgress();
    console.error('[Worker uncaught error]', e);
}

// ============================================================================
// Canvas events (forwarded to worker via closure over `worker` variable)
// ============================================================================

function attachCanvasEvents(cvs) {
    cvs.addEventListener('pointerdown', (e) => {
        cvs.setPointerCapture(e.pointerId);
        worker.postMessage({ type: 'pointerdown', x: e.clientX, y: e.clientY, button: e.button });
    });

    cvs.addEventListener('pointermove', (e) => {
        if (e.buttons === 0) return;  // Skip no-op messages when not dragging
        worker.postMessage({ type: 'pointermove', x: e.clientX, y: e.clientY, buttons: e.buttons });
    });

    cvs.addEventListener('pointerup', (e) => {
        worker.postMessage({ type: 'pointerup', x: e.clientX, y: e.clientY });
    });

    cvs.addEventListener('pointercancel', (e) => {
        worker.postMessage({ type: 'pointerup', x: e.clientX, y: e.clientY });
    });

    cvs.addEventListener('wheel', (e) => {
        e.preventDefault();
        worker.postMessage({ type: 'wheel', deltaY: e.deltaY });
    }, { passive: false });
}

// ============================================================================
// Resize
// ============================================================================

const resizeObserver = new ResizeObserver((entries) => {
    for (const entry of entries) {
        const { width, height } = entry.contentRect;
        worker.postMessage({
            type: 'resize',
            width,
            height,
            pixelRatio: window.devicePixelRatio || 1,
        });
    }
});

// ============================================================================
// File loading helpers
// ============================================================================

async function sendFileToWorker(file) {
    try {
        preserveStatus = false;
        showProgress();
        resetMemoryTracking();
        const stats = beginLoadStats(null, file.name);
        updateProgressUI({ stage: 'downloading', percentage: 0, message: `Reading: ${file.name}...` });

        const readStart = performance.now();
        const arrayBuffer = await file.arrayBuffer();
        stats.fetchMs = performance.now() - readStart;
        stats.fileSize = arrayBuffer.byteLength;
        updateLoadStatsPanel();

        updateProgressUI({ stage: 'downloading', percentage: 30, message: `Sending to worker...` });

        startLoadingWatchdog();
        worker.postMessage(
            { type: 'loadFile', data: arrayBuffer, filename: file.name, backend: getBackendFromURL() },
            [arrayBuffer]
        );
    } catch (err) {
        clearLoadingWatchdog();
        statusEl.textContent = `Failed to read file: ${err.message}`;
        statusEl.className = 'error';
        failLoadStats();
        hideProgress();
        console.error('[main] Failed to read file:', err);
    }
}

async function loadModelFromURL(url) {
    preserveStatus = false;
    showProgress();
    resetMemoryTracking();
    const displayName = getDisplayNameFromURI(url);
    const stats = beginLoadStats(null, displayName);
    updateProgressUI({ stage: 'downloading', percentage: 0, message: `Downloading ${url}...` });

    try {
        const fetchStart = performance.now();
        const response = await fetch(url);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);

        const contentLength = response.headers.get('content-length');
        const total = contentLength ? parseInt(contentLength, 10) : 0;

        let loaded = 0;
        const reader = response.body.getReader();
        const chunks = [];

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            chunks.push(value);
            loaded += value.length;

            const pct = total > 0 ? Math.round((loaded / total) * 100) : 0;
            updateProgressUI({
                stage: 'downloading',
                percentage: pct * 0.3,
                message: `Downloading... ${pct}%`
            });
        }

        const totalLength = chunks.reduce((acc, c) => acc + c.length, 0);
        const binary = new Uint8Array(totalLength);
        let offset = 0;
        for (const chunk of chunks) {
            binary.set(chunk, offset);
            offset += chunk.length;
        }
        stats.fetchMs = performance.now() - fetchStart;
        stats.fileSize = binary.byteLength;
        updateLoadStatsPanel();

        updateProgressUI({ stage: 'downloading', percentage: 30, message: 'Sending to worker...' });

        startLoadingWatchdog();
        worker.postMessage(
            { type: 'loadFile', data: binary.buffer, filename: url, backend: getBackendFromURL() },
            [binary.buffer]
        );
    } catch (err) {
        clearLoadingWatchdog();
        statusEl.textContent = `Failed to download: ${err.message}`;
        statusEl.className = 'error';
        failLoadStats();
        hideProgress();
        showToast(`Failed to load: ${err.message}`);
    }
}

async function loadSampleModel() {
    const url = SAMPLE_MODELS[Math.floor(Math.random() * SAMPLE_MODELS.length)];
    await loadModelFromURL(url);
}

// ============================================================================
// File picker
// ============================================================================

loadBtn.addEventListener('click', () => fileInput.click());

fileInput.addEventListener('change', async () => {
    const file = fileInput.files?.[0];
    if (!file) return;
    await sendFileToWorker(file);
    fileInput.value = '';
});

sampleBtn.addEventListener('click', () => loadSampleModel());

upAxisBtn.addEventListener('click', () => {
    sceneState.applyUpAxisConversion = !sceneState.applyUpAxisConversion;
    updateUpAxisButton();
    worker.postMessage({
        type: 'toggleUpAxis',
        apply: sceneState.applyUpAxisConversion
    });
});

fitBtn.addEventListener('click', () => {
    worker.postMessage({ type: 'fitCamera' });
    showToast('Camera fitted to scene');
});

progressStopBtn.addEventListener('click', () => {
    console.log('[main] User stopped loading — respawning worker...');
    statusEl.textContent = 'Loading stopped by user. Worker restarted.';
    statusEl.className = 'error';
    preserveStatus = true;
    hideProgress();
    showToast('Loading stopped — worker restarted');
    respawnWorker();
});

progressPauseBtn.addEventListener('click', () => {
    if (progressPauseBtn.classList.contains('paused')) {
        // Resume
        worker.postMessage({ type: 'resume' });
        progressPauseBtn.textContent = 'Pause';
        progressPauseBtn.classList.remove('paused');
    } else {
        // Pause
        worker.postMessage({ type: 'pause' });
        progressPauseBtn.textContent = 'Resume';
        progressPauseBtn.classList.add('paused');
    }
});

// ============================================================================
// Debug delay controls
// ============================================================================

function syncDebugDelay() {
    const enabled = progressDebugToggle.checked;
    const ms = enabled ? parseInt(progressDebugDelay.value, 10) || 0 : 0;
    progressDebugDelay.disabled = !enabled;
    if (worker) worker.postMessage({ type: 'setDebugDelay', delayMs: ms });
}

progressDebugToggle.addEventListener('change', syncDebugDelay);
progressDebugDelay.addEventListener('change', syncDebugDelay);

// ============================================================================
// Drag-and-drop
// ============================================================================

document.body.addEventListener('dragover', (e) => {
    e.preventDefault();
    document.body.classList.add('drag-over');
});

document.body.addEventListener('dragleave', (e) => {
    if (!e.relatedTarget || !document.body.contains(e.relatedTarget)) {
        document.body.classList.remove('drag-over');
    }
});

document.body.addEventListener('drop', async (e) => {
    e.preventDefault();
    document.body.classList.remove('drag-over');
    const file = e.dataTransfer?.files?.[0];
    if (!file) return;

    if (/\.(usd|usda|usdc|usdz)$/i.test(file.name)) {
        await sendFileToWorker(file);
    } else {
        showToast('Please drop a USD file (.usd, .usda, .usdc, .usdz)');
    }
});

// ============================================================================
// Initialization
// ============================================================================

initMemoryGraph();
attachCanvasEvents(canvas);
resizeObserver.observe(canvas);
// Backend switch reloads the page (the worker binds its WASM module at init).
mountBackendSelector(document.getElementById('info-panel'));
spawnWorker();
