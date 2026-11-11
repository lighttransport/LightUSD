import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { EXRLoader } from 'three/examples/jsm/loaders/EXRLoader.js';
import GUI from 'three/examples/jsm/libs/lil-gui.module.min.js';
import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils, TextureLoadingManager } from 'lightusd/LightUSDLoaderUtils.js';
import {
	buildThreeNode,
	isScene,
	readSceneMeta
} from 'lightusd/RenderSceneUtils.js';
import {
	getAssetUriFromURL,
	getBackendFromURL,
	LOADER_BACKEND_CHOICES,
	makeStaticParseOptions,
	setBackendAndReload
} from 'lightusd/LoaderConfigUtils.js';
import { buildSkeletonDataFromUSD } from 'lightusd/USDSkeletonData.js';
import { extractSkinnedMeshData } from 'lightusd/USDSceneSkinningData.js';
import { applyUSDSceneSkinningPipeline } from 'lightusd/USDSceneSkinningPipeline.js';
import { buildNodeIndexMap } from 'lightusd/USDAnimationConverter.js';
import { extractUSDSceneAnimations } from 'lightusd/USDSceneAnimationPipeline.js';

const LOADER_BACKEND = getBackendFromURL();
const VERBOSE_LOAD_LOGS = new URLSearchParams(window.location.search).get('traceLoad') === 'true';
const animationLoadLogger = VERBOSE_LOAD_LOGS ? console : {
	log() {},
	warn: console.warn.bind(console),
	error: console.error.bind(console)
};
const MAX_PROGRESS_HISTORY = 200;
const LOAD_PROGRESS_UPDATE_INTERVAL_MS = 100;
const activeLoadProgress = { stage: 'idle', percentage: 0, message: '' };
const loadProgressHistory = [];
let lastLoadProgressUpdateMs = -Infinity;
let progressHideTimer = null;

function showLoadProgress(stage, percentage, message, force = false) {
	const now = performance.now();
	const nextStage = stage || activeLoadProgress.stage;
	const stageChanged = nextStage !== activeLoadProgress.stage;
	if (!force && !stageChanged && now - lastLoadProgressUpdateMs < LOAD_PROGRESS_UPDATE_INTERVAL_MS) {
		return;
	}
	lastLoadProgressUpdateMs = now;
	activeLoadProgress.stage = nextStage;
	activeLoadProgress.percentage = Math.max(0, Math.min(100,
		Number.isFinite(percentage) ? percentage : activeLoadProgress.percentage));
	activeLoadProgress.message = message || activeLoadProgress.message;
	loadProgressHistory.push({ time: now, ...activeLoadProgress });
	if (loadProgressHistory.length > MAX_PROGRESS_HISTORY) {
		loadProgressHistory.splice(0, loadProgressHistory.length - MAX_PROGRESS_HISTORY);
	}

	const panel = document.getElementById('progressPanel');
	const label = document.getElementById('progressLabel');
	const bar = document.getElementById('progressBar');
	if (panel) panel.style.display = 'block';
	if (label) {
		label.textContent = `${Math.round(activeLoadProgress.percentage)}%: ${activeLoadProgress.message}`;
	}
	if (bar) bar.style.transform = `scaleX(${activeLoadProgress.percentage / 100})`;
}

function hideLoadProgress(delayMs = 1200) {
	if (progressHideTimer) clearTimeout(progressHideTimer);
	progressHideTimer = setTimeout(() => {
		const panel = document.getElementById('progressPanel');
		if (panel && activeLoadProgress.percentage >= 100) panel.style.display = 'none';
	}, delayMs);
}

function reportCoreParseProgress(info = {}) {
	const percentage = Number.isFinite(info.percentage) ? info.percentage : 10;
	const message = info.message || info.stage || 'Parsing USD scene...';
	const completeCount = Number(info.crateCurrent ?? info.meshCurrent);
	const totalCount = Number(info.crateTotal ?? info.meshTotal);
	const compactCount = `${Math.min(completeCount, totalCount)}/${totalCount}`;
	const spacedCount = `${Math.min(completeCount, totalCount)} / ${totalCount}`;
	const hasCount = message.includes(compactCount) || message.includes(spacedCount);
	const count = !hasCount && Number.isFinite(completeCount) &&
		Number.isFinite(totalCount) && totalCount > 0 ? ` (${spacedCount})` : '';
	showLoadProgress(info.stage || 'parsing', percentage, `${message}${count}`,
		percentage >= 70);
}

window.animationLoadProgress = { active: activeLoadProgress, history: loadProgressHistory };

function getStartupUSDModelURI(params = new URLSearchParams(window.location.search)) {
	return getAssetUriFromURL(params, ['usd']);
}

function getDisplayNameFromURI(uri) {
	try {
		const parsed = new URL(uri, window.location.href);
		return parsed.pathname.split('/').filter(Boolean).pop() || uri;
	} catch {
		return uri.split('/').pop() || uri;
	}
}

function formatDurationMs(ms) {
	if (!Number.isFinite(ms)) return 'n/a';
	return ms < 1000 ? `${ms.toFixed(0)} ms` : `${(ms / 1000).toFixed(2)} s`;
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
	const wasmHeap = currentLoader?.native_?.HEAPU8?.buffer?.byteLength;
	return {
		jsHeap: Number.isFinite(jsHeap) ? jsHeap : null,
		wasmHeap: Number.isFinite(wasmHeap) ? wasmHeap : null
	};
}

function formatMemoryUse(before, after, key) {
	const current = after?.[key];
	if (!Number.isFinite(current)) return 'n/a';
	const previous = before?.[key];
	if (!Number.isFinite(previous)) return formatBytes(current);
	const delta = current - previous;
	return `${formatBytes(current)} (${delta > 0 ? '+' : ''}${formatBytes(delta)})`;
}

function formatTextureStats(stats) {
	if (!stats || !Number.isFinite(stats.textureTotal) || stats.textureTotal <= 0) {
		return 'queued 0';
	}
	const countText = `${stats.textureLoaded}/${stats.textureTotal}` +
		(stats.textureFailed ? ` (${stats.textureFailed} failed)` : '');
	return stats.textureComplete && Number.isFinite(stats.textureMs) ?
		`${countText}, ${formatDurationMs(stats.textureMs)}` :
		`${countText}, loading`;
}

function formatNativeTimings(stats) {
	const timings = stats?.nativeTimings;
	if (!timings) return null;
	return `Native: archive ${formatDurationMs(timings.archiveMs)}, ` +
		`crate ${formatDurationMs(timings.nativeBeginMs)}, ` +
		`entities ${formatDurationMs(timings.entityCopyMs)}, ` +
		`meshes ${formatDurationMs(timings.meshCopyMs)}, ` +
		`animations ${formatDurationMs(timings.animationCopyMs)}`;
}

function beginLoadStats(fileSize = null) {
	const stats = {
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
		totalMs: null,
		memoryBefore: captureMemorySnapshot(),
		memoryAfter: null
	};
	window.animationLoadStats = stats;
	showLoadProgress('start', 0, 'Preparing load...', true);
	updateLoadStatsPanel(stats, 'Loading...');
	return stats;
}

function updateLoadStatsPanel(stats, overrideText = null) {
	const el = document.getElementById('loadStats');
	if (!el) return;
	el.style.display = 'block';
	if (overrideText) {
		el.textContent = overrideText;
		return;
	}
	const lines = [
		`File: ${formatBytes(stats.fileSize)}`,
		`Fetch/read: ${stats.fetchMs === null ? 'n/a' : formatDurationMs(stats.fetchMs)}`,
		`Parse/load: ${formatDurationMs(stats.parseMs)}`,
		`Process/build: ${formatDurationMs(stats.processMs)}`,
		`Textures: ${formatTextureStats(stats)}`,
		`Total: ${formatDurationMs(stats.totalMs)}`,
		`JS heap: ${formatMemoryUse(stats.memoryBefore, stats.memoryAfter, 'jsHeap')}`,
		`WASM heap: ${formatMemoryUse(stats.memoryBefore, stats.memoryAfter, 'wasmHeap')}`
	];
	const nativeTimings = formatNativeTimings(stats);
	if (nativeTimings) lines.splice(3, 0, nativeTimings);
	el.textContent = lines.join('\n');
}

function failLoadStats(stats) {
	if (!stats) return;
	stats.totalMs = performance.now() - stats.startTime;
	stats.memoryAfter = captureMemorySnapshot();
	updateLoadStatsPanel(stats, 'Failed');
	showLoadProgress('failed', 100, 'Load failed', true);
}

function setCurrentFileName(filename) {
	const currentFileEl = document.getElementById('currentFile');
	if (currentFileEl) currentFileEl.textContent = filename || '-';
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
	if (stats.textureTotal > 0) {
		const completed = stats.textureLoaded + stats.textureFailed;
		const percentage = 90 + Math.min(10, (completed / stats.textureTotal) * 10);
		showLoadProgress('textures', percentage,
			`Loading textures ${completed} / ${stats.textureTotal}`,
			stats.textureComplete);
		if (stats.textureComplete) hideLoadProgress();
	}
	updateLoadStatsPanel(stats);
}

function startTrackedTextureLoading(manager, stats, label = 'textureQueue') {
	if (!manager || !Number.isFinite(manager.total) || manager.total <= 0) {
		updateTextureStats(stats, { loaded: 0, failed: 0, total: 0, complete: true, ms: 0 });
		return null;
	}
	currentTextureLoadingManager = manager;
	const start = performance.now();
	updateTextureStats(stats, {
		loaded: manager.loaded || 0,
		failed: manager.failed || 0,
		total: manager.total || 0
	});
	const largeAsset = Number.isFinite(stats?.fileSize) && stats.fileSize >= 128 * 1024 * 1024;
	return manager.startLoading({
		// Bound simultaneous image decode buffers on texture-heavy USDZ files.
		// Texture upload is asynchronous and is not part of process/build time.
		concurrency: largeAsset ? 4 : 8,
		yieldInterval: 16,
		onTextureLoaded: (material) => { material.needsUpdate = true; },
		onProgress: (info) => updateTextureStats(stats, info)
	}).then((status) => {
		const elapsed = performance.now() - start;
		const loaded = status.loaded || 0;
		const failed = status.failed || 0;
		const total = status.total || 0;
		if (currentTextureLoadingManager === manager) currentTextureLoadingManager = null;
		if (typeof manager.reset === 'function') manager.reset();
		updateTextureStats(stats, { loaded, failed, total, complete: true, ms: elapsed });
		console.log(`[animation] ${label}:done ${loaded}/${total} failed=${failed} ${formatDurationMs(elapsed)}`);
		return status;
	}).catch((err) => {
		console.warn(`[animation] ${label} failed:`, err);
		const elapsed = performance.now() - start;
		const status = manager.getStatus ? manager.getStatus() : null;
		const loaded = status?.loaded || manager.loaded || 0;
		const failed = status?.failed || manager.failed || 0;
		const total = status?.total || manager.total || 0;
		if (currentTextureLoadingManager === manager) currentTextureLoadingManager = null;
		if (typeof manager.reset === 'function') manager.reset();
		updateTextureStats(stats, { loaded, failed, total, complete: true, ms: elapsed });
		return status;
	});
}

function cleanupCurrentTextureLoading() {
	if (!currentTextureLoadingManager) return;
	try {
		currentTextureLoadingManager.abort();
		if (typeof currentTextureLoadingManager.reset === 'function') {
			currentTextureLoadingManager.reset();
		}
	} catch (_) {
		// Ignore stale texture queue cleanup errors.
	}
	currentTextureLoadingManager = null;
}

// Scene setup
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x1a1a1a);

// Camera
const camera = new THREE.PerspectiveCamera(
	75,
	window.innerWidth / window.innerHeight,
	0.1,
	10000
);
camera.position.set(50, 50, 50);
camera.lookAt(0, 0, 0);

// Renderer
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
document.body.appendChild(renderer.domElement);

// Orbit controls
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.dampingFactor = 0.05;

// Lighting
const ambientLight = new THREE.AmbientLight(0x404040, 1);
scene.add(ambientLight);

const directionalLight = new THREE.DirectionalLight(0xffffff, 1);
directionalLight.position.set(50, 100, 50);
directionalLight.castShadow = true;
directionalLight.shadow.mapSize.width = 2048;
directionalLight.shadow.mapSize.height = 2048;
directionalLight.shadow.camera.left = -100;
directionalLight.shadow.camera.right = 100;
directionalLight.shadow.camera.top = 100;
directionalLight.shadow.camera.bottom = -100;
directionalLight.shadow.camera.near = 0.5;
directionalLight.shadow.camera.far = 500;
directionalLight.shadow.bias = -0.0001; // Reduce shadow acne
scene.add(directionalLight);

// Ground plane
const groundGeometry = new THREE.PlaneGeometry(200, 200);
const groundMaterial = new THREE.MeshStandardMaterial({
	color: 0x333333,
	roughness: 0.8,
	metalness: 0.2
});
const ground = new THREE.Mesh(groundGeometry, groundMaterial);
ground.rotation.x = -Math.PI / 2;
ground.receiveShadow = true;
scene.add(ground);

// Grid helper
let gridHelper = new THREE.GridHelper(200, 20, 0x666666, 0x444444);
scene.add(gridHelper);

// Axis helper at center
const axisHelper = new THREE.AxesHelper(50); // Size 50
axisHelper.name = 'AxisHelper';
scene.add(axisHelper);

// Virtual root object for USD scene (name = "/")
const usdSceneRoot = new THREE.Group();
usdSceneRoot.name = "/";
scene.add(usdSceneRoot);

// Store reference to the actual USD content node (child of usdSceneRoot)
// This is needed for creating the animation mixer on the correct root
let usdContentNode = null;

// Store USD animations from the file
let usdAnimations = [];

// Store the current file's upAxis (Y or Z)
let currentFileUpAxis = "Y";

// Store the current scene metadata
let currentSceneMetadata = {
	upAxis: "Y",
	metersPerUnit: 1.0,
	framesPerSecond: 24.0,
	timeCodesPerSecond: 24.0,
	startTimeCode: null,
	endTimeCode: null,
	autoPlay: true,
	comment: "",
	copyright: ""
};

// Store currently selected object for transform display
let selectedObject = null;

// ===========================================
// Environment Map and Material Settings
// ===========================================

// PMREM generator for environment maps
let pmremGenerator = null;

// Current environment map
let envMap = null;

// Texture cache for material conversion
let textureCache = new Map();

// LightUSD loader and scene references for cleanup
let currentLoader = null;
let currentUSDScene = null;
let currentTextureLoadingManager = null;
let usdDomeLightData = null; // Store DomeLight data from USD file

// Environment map presets
const ENV_PRESETS = {
	'usd_dome': 'usd', // Special marker for USD DomeLight (if available)
	'goegap_1k': 'assets/textures/goegap_1k.hdr',
	'env_sunsky_sunset': 'assets/textures/env_sunsky_sunset.hdr',
	'studio': null, // Will use synthetic studio lighting
	'constant_color': 'constant' // Special marker for constant color environment
};

// Material and environment settings
const materialSettings = {
	materialType: 'auto', // 'auto', 'openpbr', 'usdpreviewsurface'
	envMapPreset: 'goegap_1k',
	envMapIntensity: 1.0,
	envConstantColor: '#ffffff', // Color for constant color environment
	envColorspace: 'sRGB', // 'sRGB' (convert to linear) or 'linear' (no conversion)
	showEnvBackground: false,
	exposure: 1.0,
	toneMapping: 'aces'
};

// Store bounding box helpers for each object
const objectBBoxHelpers = new Map(); // uuid -> BoxHelper

// Raycaster for object selection
const raycaster = new THREE.Raycaster();
const mouse = new THREE.Vector2();

// Highlight selected object
let selectionHelper = null;

// ===========================================
// Cached objects for animation loop optimization
// Avoid per-frame allocations for better GC performance
// ===========================================

// Cached bounding boxes for selection (reused each frame)
let selectionLocalBBox = new THREE.Box3();  // Local-space bbox of selected object
let selectionWorldBBox = new THREE.Box3();  // Reusable world-space bbox

// Cache local bounding boxes for objects with bbox helpers
// uuid -> { localBBox: Box3, worldBBox: Box3 (reusable) }
const objectLocalBBoxCache = new Map();

// Pre-allocated Set for collecting unique animation actions (reused each loop)
const uniqueActionsSet = new Set();

// ===========================================
// GC-Free Direct Animation System
// Bypasses Three.js AnimationMixer for zero-allocation updates
// ===========================================

// Animation data storage: Map<object.uuid, AnimationData>
const directAnimationData = new Map();

// Pre-allocated interpolation temporaries (reused every frame)
const _quatA = new THREE.Quaternion();
const _quatB = new THREE.Quaternion();

// Toggle for GC-free mode
let useDirectAnimation = true;

/**
 * Binary search to find the keyframe index for a given time
 */
function findKeyframeIndex(times, t) {
	const len = times.length;
	if (len === 0) return -1;
	if (t <= times[0]) return 0;
	if (t >= times[len - 1]) return len - 1;

	let lo = 0, hi = len - 1;
	while (lo < hi - 1) {
		const mid = (lo + hi) >>> 1;
		if (times[mid] <= t) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return lo;
}

/**
 * Interpolate Vector3 from keyframes (GC-free)
 */
function interpolateVec3(times, values, t, target) {
	const idx = findKeyframeIndex(times, t);
	if (idx < 0) return;

	const i0 = idx * 3;
	if (idx >= times.length - 1 || t <= times[idx]) {
		target.set(values[i0], values[i0 + 1], values[i0 + 2]);
	} else {
		const t0 = times[idx];
		const t1 = times[idx + 1];
		const alpha = (t - t0) / (t1 - t0);
		const i1 = (idx + 1) * 3;
		target.set(
			values[i0] + (values[i1] - values[i0]) * alpha,
			values[i0 + 1] + (values[i1 + 1] - values[i0 + 1]) * alpha,
			values[i0 + 2] + (values[i1 + 2] - values[i0 + 2]) * alpha
		);
	}
}

/**
 * Interpolate Quaternion from keyframes (GC-free, uses slerp)
 */
function interpolateQuat(times, values, t, target) {
	const idx = findKeyframeIndex(times, t);
	if (idx < 0) return;

	const i0 = idx * 4;
	if (idx >= times.length - 1 || t <= times[idx]) {
		target.set(values[i0], values[i0 + 1], values[i0 + 2], values[i0 + 3]);
	} else {
		const t0 = times[idx];
		const t1 = times[idx + 1];
		const alpha = (t - t0) / (t1 - t0);
		const i1 = (idx + 1) * 4;
		_quatA.set(values[i0], values[i0 + 1], values[i0 + 2], values[i0 + 3]);
		_quatB.set(values[i1], values[i1 + 1], values[i1 + 2], values[i1 + 3]);
		target.slerpQuaternions(_quatA, _quatB, alpha);
	}
}

/**
 * Update all animations using direct property assignment (GC-free)
 */
function updateDirectAnimations(time) {
	for (const [uuid, animData] of directAnimationData) {
		const obj = animData.object;
		if (!obj) continue;

		if (animData.position) {
			interpolateVec3(animData.position.times, animData.position.values, time, obj.position);
		}
		if (animData.rotation) {
			interpolateQuat(animData.rotation.times, animData.rotation.values, time, obj.quaternion);
		}
		if (animData.scale) {
			interpolateVec3(animData.scale.times, animData.scale.values, time, obj.scale);
		}
	}
}

/**
 * Build direct animation data from USD loader (call at load time)
 */
function buildDirectAnimationData(usdLoader, sceneRoot) {
	directAnimationData.clear();

	const numAnimations = usdLoader.numAnimations();

	// Build node index map
	const nodeIndexMap = new Map();
	let nodeIndex = 0;
	sceneRoot.traverse((obj) => {
		nodeIndexMap.set(nodeIndex, obj);
		nodeIndex++;
	});

	for (let i = 0; i < numAnimations; i++) {
		const usdAnimation = usdLoader.getAnimation(i);

		// Handle channel-based animation
		if (usdAnimation.channels && usdAnimation.samplers) {
			for (const channel of usdAnimation.channels) {
				const targetType = channel.target_type || 'SceneNode';
				if (targetType !== 'SceneNode') continue;

				const sampler = usdAnimation.samplers[channel.sampler];
				if (!sampler || !sampler.times || !sampler.values) continue;

				const targetObject = nodeIndexMap.get(channel.target_node);
				if (!targetObject) continue;

				const times = sampler.times instanceof Float32Array
					? sampler.times : new Float32Array(sampler.times);
				const values = sampler.values instanceof Float32Array
					? sampler.values : new Float32Array(sampler.values);

				let animData = directAnimationData.get(targetObject.uuid);
				if (!animData) {
					animData = { object: targetObject };
					directAnimationData.set(targetObject.uuid, animData);
				}

				switch (channel.path) {
					case 'Translation':
						animData.position = { times, values };
						break;
					case 'Rotation':
						animData.rotation = { times, values };
						break;
					case 'Scale':
						animData.scale = { times, values };
						break;
				}
			}
		}

		// Handle track-based animation (legacy fallback only when channels are absent).
		// Skip if channel-based data was already processed above — the track-based
		// path finds targets by name, which selects the wrong node (Mesh instead of
		// Group) when both share the same name (common in Blender USD exports).
		if (!(usdAnimation.channels && usdAnimation.channels.length > 0) &&
			usdAnimation.tracks && usdAnimation.tracks.length > 0) {
			let targetObject = null;
			if (usdAnimation.name) {
				let searchName = usdAnimation.name.replace(/_xform$/, '').replace(/_anim$/, '');
				sceneRoot.traverse((obj) => {
					if (obj.name === searchName || (obj.name && obj.name.startsWith(searchName))) {
						targetObject = obj;
					}
				});
			}
			if (!targetObject) continue;

			let animData = directAnimationData.get(targetObject.uuid);
			if (!animData) {
				animData = { object: targetObject };
				directAnimationData.set(targetObject.uuid, animData);
			}

			for (const track of usdAnimation.tracks) {
				if (!track.times || !track.values) continue;

				const times = track.times instanceof Float32Array
					? track.times : new Float32Array(track.times);
				const values = track.values instanceof Float32Array
					? track.values : new Float32Array(track.values);

				switch (track.path) {
					case 'Translation':
						animData.position = { times, values };
						break;
					case 'Rotation':
						animData.rotation = { times, values };
						break;
					case 'Scale':
						animData.scale = { times, values };
						break;
				}
			}
		}
	}

}

// ===========================================
// USD Animation Extraction Functions
// ===========================================

/**
 * Convert USD animation data to Three.js AnimationClip
 * Supports both channel/sampler and track-based animation structures
 * @param {Object} usdLoader - LightUSD loader instance
 * @param {THREE.Object3D} sceneRoot - Three.js scene containing the loaded geometry
 * @returns {Array<THREE.AnimationClip>} Array of Three.js AnimationClips
 */
function convertUSDAnimationsToThreeJS(usdLoader, sceneRoot) {
	const animationClips = [];

	// Get number of animations
	const numAnimations = usdLoader.numAnimations();

	// Get summary of all animations
	const animationInfos = usdLoader.getAllAnimationInfos();

	// Build node index map for faster lookup
	const nodeIndexMap = new Map();
	let nodeIndex = 0;
	sceneRoot.traverse((obj) => {
		nodeIndexMap.set(nodeIndex, obj);
		nodeIndex++;
	});

	// Convert each animation to Three.js format
	for (let i = 0; i < numAnimations; i++) {
		const usdAnimation = usdLoader.getAnimation(i);

		// Prefer channel-based format (uses correct target_node indices).
		// The WASM binding always provides both tracks AND channels/samplers,
		// but the tracks format finds targets by name which breaks when
		// Group and Mesh share the same name (common in Blender exports).
		// The channel-based path uses nodeIndexMap for reliable targeting.
		if (usdAnimation.channels && usdAnimation.samplers && usdAnimation.channels.length > 0) {
			// Skip to channel-based processing below
		} else if (usdAnimation.tracks && usdAnimation.tracks.length > 0) {

			// Process track-based animation
			const keyframeTracks = [];

			// Find the target object - for track animations, usually the first child after scene root
			let targetObject = sceneRoot;
			// Try to find the animated object by name from the animation
			// Animation name format: "object_name_xform" or "object_name"
			// Remove "_xform" suffix if present, then try exact match
			if (usdAnimation.name) {
				let searchName = usdAnimation.name;
				// Remove common suffixes
				searchName = searchName.replace(/_xform$/, '');
				searchName = searchName.replace(/_anim$/, '');

				// First try exact match
				let found = false;
				sceneRoot.traverse((obj) => {
					if (obj.name === searchName) {
						targetObject = obj;
						found = true;
					}
				});

				// If no exact match, try matching without the mesh suffix
				if (!found) {
					sceneRoot.traverse((obj) => {
						if (obj.name && obj.name.startsWith(searchName)) {
							targetObject = obj;
							found = true;
						}
					});
				}
			}

			// If we can't find it by name, use the first mesh or group
			if (targetObject === sceneRoot) {
				console.warn(`Could not find target object for animation "${usdAnimation.name}", using first mesh/group`);
				sceneRoot.traverse((obj) => {
					if ((obj.isMesh || obj.isGroup) && obj !== sceneRoot) {
						targetObject = obj;
						return; // Stop traversal once we find the first mesh/group
					}
				});
			}

			const targetName = targetObject.name || 'AnimatedObject';
			const targetUUID = targetObject.uuid;

			// Process each track
			for (const track of usdAnimation.tracks) {
				if (!track.times || !track.values) {
					console.warn('Track missing times or values');
					continue;
				}

				// Convert times and values to arrays
				const times = Array.isArray(track.times) ? track.times : Array.from(track.times);
				const values = Array.isArray(track.values) ? track.values : Array.from(track.values);
				const interpolation = getUSDInterpolationMode(track.interpolation);

				// Create appropriate Three.js KeyframeTrack based on path
				let keyframeTrack;

				// Use UUID-based targeting for reliability (same as channel-based animations)
				// Format: "<uuid>.<property>" (PropertyBinding checks uuid === nodeName)
				switch (track.path) {
					case 'CustomProperty':
						if (debugAnimationTracking) {
							console.debug(`Skipping USD custom property animation: ${track.propertyName || '(unnamed)'}`);
						}
						continue;

					case 'translation':
					case 'Translation':
						keyframeTrack = new THREE.VectorKeyframeTrack(
							`${targetUUID}.position`,
							times,
							values,
							interpolation
						);
						break;

					case 'rotation':
					case 'Rotation':
						// Rotation is stored as quaternions (x, y, z, w)
						keyframeTrack = new THREE.QuaternionKeyframeTrack(
							`${targetUUID}.quaternion`,
							times,
							values,
							interpolation
						);
						break;

					case 'scale':
					case 'Scale':
						keyframeTrack = new THREE.VectorKeyframeTrack(
							`${targetUUID}.scale`,
							times,
							values,
							interpolation
						);
						break;

					default:
						console.warn(`Unknown track path: ${track.path}`);
						continue;
				}

				if (keyframeTrack) {
					keyframeTracks.push(keyframeTrack);
				}
			}

			// Create Three.js AnimationClip from tracks
			if (keyframeTracks.length > 0) {
				const clip = new THREE.AnimationClip(
					usdAnimation.name || `Animation_${i}`,
					usdAnimation.duration || -1, // -1 will auto-calculate from tracks
					keyframeTracks
				);

				animationClips.push(clip);
			}

			continue; // Skip to next animation
		}

		// Handle channel-based animation (newer format)
		if (!usdAnimation.channels || !usdAnimation.samplers) {
			console.warn(`Animation ${i} missing channels/samplers and tracks`);
			continue;
		}

		// Filter for node animations only (skip skeletal animations)
		const nodeChannels = usdAnimation.channels.filter(channel => {
			const targetType = channel.target_type || 'SceneNode'; // Default to SceneNode for backward compat
			return targetType === 'SceneNode';
		});

		if (nodeChannels.length === 0) {
			continue; // Skip skeletal-only animations
		}

		// Create Three.js KeyframeTracks from USD animation channels
		const keyframeTracks = [];

		for (const channel of nodeChannels) {
			// Get sampler data
			const sampler = usdAnimation.samplers[channel.sampler];
			if (!sampler || !sampler.times || !sampler.values) {
				console.warn(`Invalid sampler for channel`);
				continue;
			}

			// Find the Three.js object for this channel
			const targetObject = nodeIndexMap.get(channel.target_node);
			if (!targetObject) {
				continue;
			}

			// Convert times and values to arrays
			const times = Array.isArray(sampler.times) ? sampler.times : Array.from(sampler.times);
			const values = Array.isArray(sampler.values) ? sampler.values : Array.from(sampler.values);

			// Create appropriate Three.js KeyframeTrack based on path
			let keyframeTrack;
			// Use UUID for reliable hierarchical animation targeting
			// Three.js AnimationMixer supports both name-based and UUID-based targeting
			const targetUUID = targetObject.uuid;
			const interpolation = getUSDInterpolationMode(sampler.interpolation);

			// Three.js AnimationMixer can target objects by UUID or by name
			// Using UUID is more reliable for hierarchical animations
			// Format: "<uuid>.<property>" (PropertyBinding checks uuid === nodeName)
			switch (channel.path) {
				case 'CustomProperty':
					if (debugAnimationTracking) {
						console.debug(`Skipping USD custom property animation: ${channel.propertyName || '(unnamed)'}`);
					}
					continue;

				case 'Translation':
					keyframeTrack = new THREE.VectorKeyframeTrack(
						`${targetUUID}.position`,
						times,
						values,
						interpolation
					);
					break;

				case 'Rotation':
					// Rotation is stored as quaternions (x, y, z, w)
					keyframeTrack = new THREE.QuaternionKeyframeTrack(
						`${targetUUID}.quaternion`,
						times,
						values,
						interpolation
					);
					break;

				case 'Scale':
					keyframeTrack = new THREE.VectorKeyframeTrack(
						`${targetUUID}.scale`,
						times,
						values,
						interpolation
					);
					break;

				case 'Weights':
					// For morph targets
					keyframeTrack = new THREE.NumberKeyframeTrack(
						`.uuid[${targetUUID}].morphTargetInfluences`,
						times,
						values,
						interpolation
					);
					break;

				default:
					console.warn(`Unknown animation path: ${channel.path}`);
					continue;
			}

			if (keyframeTrack) {
				keyframeTracks.push(keyframeTrack);
			}
		}

		// Create Three.js AnimationClip
		if (keyframeTracks.length > 0) {
			const clip = new THREE.AnimationClip(
				usdAnimation.name || `Animation_${i}`,
				usdAnimation.duration || -1, // -1 will auto-calculate from tracks
				keyframeTracks
			);

			animationClips.push(clip);
		}
	}

	return animationClips;
}

/**
 * Convert USD interpolation mode to Three.js InterpolateMode
 * @param {string} interpolation - USD interpolation mode (Linear, Step, CubicSpline)
 * @returns {number} Three.js InterpolateMode constant
 */
function getUSDInterpolationMode(interpolation) {
	switch (interpolation) {
		case 'Step':
		case 'STEP':
			return THREE.InterpolateDiscrete;
		case 'CubicSpline':
		case 'CUBICSPLINE':
			return THREE.InterpolateSmooth;
		case 'Linear':
		case 'LINEAR':
		default:
			return THREE.InterpolateLinear;
	}
}

// ===========================================
// Environment Map Functions
// ===========================================

/**
 * Initialize PMREM generator and renderer settings for PBR
 */
function initializePBRRenderer() {
	// Create PMREM generator for environment maps
	pmremGenerator = new THREE.PMREMGenerator(renderer);
	pmremGenerator.compileEquirectangularShader();

	// Set up tone mapping
	renderer.toneMapping = THREE.ACESFilmicToneMapping;
	renderer.toneMappingExposure = materialSettings.exposure;
	renderer.outputColorSpace = THREE.SRGBColorSpace;

	console.log('PBR renderer initialized with ACES tone mapping');
}

// ===========================================
// Colorspace Conversion Utilities
// ===========================================

/**
 * Convert sRGB component to linear
 * @param {number} c - sRGB component value [0, 1]
 * @returns {number} Linear component value [0, 1]
 */
function sRGBComponentToLinear(c) {
	if (c <= 0.04045) {
		return c / 12.92;
	} else {
		return Math.pow((c + 0.055) / 1.055, 2.4);
	}
}

/**
 * Parse hex color and convert to RGB [0, 1] with optional linear conversion
 * @param {string} hexColor - Hex color string (e.g., '#ff8800')
 * @param {boolean} toLinear - If true, convert from sRGB to linear
 * @returns {object} {r, g, b} values in [0, 1]
 */
function parseHexColor(hexColor, toLinear = false) {
	// Remove # if present
	const hex = hexColor.replace('#', '');

	// Parse RGB components
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
 * Convert RGB [0, 1] to hex color string for canvas
 * @param {number} r - Red [0, 1]
 * @param {number} g - Green [0, 1]
 * @param {number} b - Blue [0, 1]
 * @returns {string} Hex color string
 */
function rgbToHex(r, g, b) {
	const toHex = (c) => {
		const clamped = Math.max(0, Math.min(1, c));
		const val = Math.round(clamped * 255);
		return val.toString(16).padStart(2, '0');
	};
	return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
}

// ===========================================
// Environment Loading
// ===========================================

/**
 * Load environment map from preset
 * @param {string} preset - Environment preset name
 */
async function loadEnvironment(preset) {
	materialSettings.envMapPreset = preset;
	const path = ENV_PRESETS[preset];

	if (!path) {
		// Studio lighting - create synthetic environment
		envMap = createStudioEnvironment();
		applyEnvironment();
		console.log('Using synthetic studio environment');
		return;
	}

	if (path === 'usd') {
		// USD DomeLight - use stored DomeLight data
		if (usdDomeLightData) {
			console.log('Using USD DomeLight environment');
			// Environment map already loaded by loadDomeLightFromUSD
		} else {
			console.warn('USD DomeLight selected but no DomeLight data available');
			envMap = createStudioEnvironment();
			applyEnvironment();
		}
		return;
	}

	if (path === 'constant') {
		// Constant color environment
		envMap = createConstantColorEnvironment(materialSettings.envConstantColor, materialSettings.envColorspace);
		applyEnvironment();
		console.log('Using constant color environment');
		return;
	}

	console.log(`Loading environment: ${preset}...`);
	try {
		const hdrLoader = new HDRLoader();
		const texture = await hdrLoader.loadAsync(path);
		envMap = pmremGenerator.fromEquirectangular(texture).texture;
		texture.dispose();
		applyEnvironment();
		console.log(`Environment loaded: ${preset}`);
	} catch (error) {
		console.error('Failed to load environment:', error);
		// Fall back to synthetic
		envMap = createStudioEnvironment();
		applyEnvironment();
	}
}

/**
 * Create a synthetic studio environment
 * @returns {THREE.Texture} Environment texture
 */
function createStudioEnvironment() {
	const canvas = document.createElement('canvas');
	canvas.width = 256;
	canvas.height = 256;
	const ctx = canvas.getContext('2d');

	// Create gradient (light from top)
	const gradient = ctx.createLinearGradient(0, 0, 0, 256);
	gradient.addColorStop(0, '#ffffff');
	gradient.addColorStop(0.5, '#cccccc');
	gradient.addColorStop(1, '#666666');
	ctx.fillStyle = gradient;
	ctx.fillRect(0, 0, 256, 256);

	const texture = new THREE.CanvasTexture(canvas);
	texture.mapping = THREE.EquirectangularReflectionMapping;
	return pmremGenerator.fromEquirectangular(texture).texture;
}

/**
 * Create a constant color environment
 * @param {string} color - Hex color string
 * @param {string} colorspace - 'sRGB' or 'linear'
 * @returns {THREE.Texture} Environment texture
 */
function createConstantColorEnvironment(color, colorspace = 'sRGB') {
	const canvas = document.createElement('canvas');
	canvas.width = 256;
	canvas.height = 256;
	const ctx = canvas.getContext('2d');

	// Parse and potentially convert color based on colorspace
	let fillColor = color;
	if (colorspace === 'sRGB') {
		// Convert sRGB to linear for proper PBR workflow
		const rgb = parseHexColor(color, true); // true = convert to linear
		fillColor = rgbToHex(rgb.r, rgb.g, rgb.b);
	}
	// else: linear colorspace - use color as-is (no conversion)

	// Fill with solid color
	ctx.fillStyle = fillColor;
	ctx.fillRect(0, 0, 256, 256);

	const texture = new THREE.CanvasTexture(canvas);
	texture.mapping = THREE.EquirectangularReflectionMapping;

	// Set colorspace based on setting
	if (colorspace === 'sRGB') {
		texture.colorSpace = THREE.LinearSRGBColorSpace;
	} else {
		texture.colorSpace = THREE.LinearSRGBColorSpace; // Already linear
	}

	return pmremGenerator.fromEquirectangular(texture).texture;
}

/**
 * Apply the current environment map to the scene
 */
function applyEnvironment() {
	scene.environment = envMap;
	updateEnvBackground();
	updateEnvIntensity();

	// Update envMap reference on all existing materials
	usdSceneRoot.traverse((child) => {
		if (child.isMesh && child.material) {
			const materials = Array.isArray(child.material) ? child.material : [child.material];
			materials.forEach(mat => {
				mat.envMap = envMap;
				mat.needsUpdate = true;  // Flag material for shader recompilation
			});
		}
	});
}

/**
 * Update environment background visibility
 */
function updateEnvBackground() {
	if (materialSettings.showEnvBackground && envMap) {
		scene.background = envMap;
	} else {
		scene.background = new THREE.Color(0x1a1a1a);
	}
}

/**
 * Update constant color environment when color changes
 */
function updateConstantColorEnvironment() {
	// Only update if constant color environment is selected
	if (materialSettings.envMapPreset === 'constant_color') {
		envMap = createConstantColorEnvironment(materialSettings.envConstantColor, materialSettings.envColorspace);
		applyEnvironment();
	}
}

/**
 * Update environment map intensity on all materials
 */
function updateEnvIntensity() {
	usdSceneRoot.traverse((child) => {
		if (child.isMesh && child.material) {
			const materials = Array.isArray(child.material) ? child.material : [child.material];
			materials.forEach(mat => {
				if (mat.envMapIntensity !== undefined) {
					mat.envMapIntensity = materialSettings.envMapIntensity;
				}
			});
		}
	});
}

/**
 * Update tone mapping type
 * @param {string} value - Tone mapping type
 */
function updateToneMapping(value) {
	const mappings = {
		'none': THREE.NoToneMapping,
		'linear': THREE.LinearToneMapping,
		'reinhard': THREE.ReinhardToneMapping,
		'cineon': THREE.CineonToneMapping,
		'aces': THREE.ACESFilmicToneMapping,
		'agx': THREE.AgXToneMapping,
		'neutral': THREE.NeutralToneMapping
	};
	renderer.toneMapping = mappings[value] || THREE.ACESFilmicToneMapping;
}

// ===========================================
// DomeLight Environment Map Loading
// ===========================================

/**
 * Load DomeLight from USD and apply to scene
 * Uses LightUSDLoaderUtils.loadDomeLightFromUSD for the heavy lifting
 * @param {Object} usdScene - USD scene object from LightUSDLoader
 * @returns {Promise<Object|null>} DomeLight data or null if not found
 */
async function loadDomeLightFromUSD(usdScene) {
	const result = await LightUSDLoaderUtils.loadDomeLightFromUSD(usdScene, pmremGenerator);

	if (result) {
		// Apply result to app state
		envMap = result.texture;
		materialSettings.envMapIntensity = result.intensity;
		materialSettings.envMapPreset = 'usd_dome';

		if (result.colorHex) {
			materialSettings.envConstantColor = result.colorHex;
		}

		applyEnvironment();

		// Update UI to reflect loaded DomeLight settings
		if (typeof envIntensityController !== 'undefined') {
			envIntensityController.updateDisplay();
		}
		if (typeof envPresetController !== 'undefined') {
			envPresetController.updateDisplay();
		}

		// Store DomeLight data for reference
		usdDomeLightData = {
			name: result.name,
			textureFile: result.textureFile,
			envmapTextureId: result.envmapTextureId,
			intensity: result.intensity,
			color: result.color,
			exposure: result.exposure,
			envMap: envMap
		};

		return usdDomeLightData;
	}

	return null;
}

/**
 * Reload all materials with current settings
 */
async function reloadMaterials() {
	if (!usdContentNode) return;

	console.log(`Reloading materials with type: ${materialSettings.materialType}`);

	// Get the current USD scene loader
	const loader = new LightUSDLoader();
	await loader.init({ useZstdCompressedWasm: false, useMemory64: false });

	// Clear texture cache for fresh reload
	textureCache.clear();

	// Traverse and update materials on all meshes
	usdContentNode.traverse(async (child) => {
		if (child.isMesh && child.userData.materialData) {
			try {
				const newMaterial = await LightUSDLoaderUtils.convertMaterial(
					child.userData.materialData,
					child.userData.usdScene,
					{
						preferredMaterialType: materialSettings.materialType,
						envMap: envMap,
						envMapIntensity: materialSettings.envMapIntensity,
						textureCache: textureCache
					}
				);

				// Preserve shadow settings
				if (child.material) {
					child.material.dispose();
				}
				child.material = newMaterial;
				child.material.needsUpdate = true;

				// Apply double-sided if enabled
				child.material.side = (animationParams.doubleSided ||
					child.userData?.usdMesh?.doubleSided) ? THREE.DoubleSide : THREE.FrontSide;
			} catch (e) {
				console.warn(`Failed to reload material for ${child.name}:`, e);
			}
		}
	});

	console.log('Materials reloaded');
}

// Load USD model asynchronously
async function loadUSDModel() {
	cleanupCurrentTextureLoading();

	// Initialize PBR renderer if not already done
	if (!pmremGenerator) {
		initializePBRRenderer();
		// Load default environment
		await loadEnvironment(materialSettings.envMapPreset);
	}

	const loader = new LightUSDLoader();

	// Initialize the loader (wait for WASM module to load)
	// Use memory64: false for browser compatibility
	// Use useZstdCompressedWasm: false since compressed WASM is not available
	await loader.init({ useZstdCompressedWasm: false, useMemory64: true });
	currentLoader = loader; // Store reference for cleanup

	// USD FILES
	const usd_filename = "./assets/suzanne-xform.usdc";

	// Load USD scene
	const usd_scene = await loader.loadAsync(usd_filename);
	currentUSDScene = usd_scene; // Store reference for cleanup

	// Get the default root node from USD
	const usdRootNode = usd_scene.getDefaultRootNode();

	// Get scene metadata from the USD file
	const sceneMetadata = usd_scene.getSceneMetadata ? usd_scene.getSceneMetadata() : {};
	const fileUpAxis = sceneMetadata.upAxis || "Y";
	currentFileUpAxis = fileUpAxis; // Store globally for toggle function

	// Store metadata globally
	currentSceneMetadata = {
		upAxis: fileUpAxis,
		metersPerUnit: sceneMetadata.metersPerUnit || 1.0,
		framesPerSecond: sceneMetadata.framesPerSecond || 24.0,
		timeCodesPerSecond: sceneMetadata.timeCodesPerSecond || 24.0,
		startTimeCode: sceneMetadata.startTimeCode,
		endTimeCode: sceneMetadata.endTimeCode,
		autoPlay: sceneMetadata.autoPlay !== undefined ? sceneMetadata.autoPlay : true,
		comment: sceneMetadata.comment || "",
		copyright: sceneMetadata.copyright || ""
	};

	// Update metadata UI
	updateMetadataUI();

	// Try to load DomeLight environment from USD
	try {
		const domeLightData = await loadDomeLightFromUSD(usd_scene);
		if (domeLightData) {
			if (envPresetController) {
				envPresetController.updateDisplay();
			}
		}
	} catch (error) {
		console.warn('Error checking for DomeLight:', error);
	}

	// Create default material with environment map
	const defaultMtl = new THREE.MeshPhysicalMaterial({
		color: 0x888888,
		roughness: 0.5,
		metalness: 0.0,
		envMap: envMap,
		envMapIntensity: materialSettings.envMapIntensity
	});

	// Clear texture cache for fresh load
	textureCache.clear();

	const options = {
		overrideMaterial: false,
		envMap: envMap,
		envMapIntensity: materialSettings.envMapIntensity,
		preferredMaterialType: materialSettings.materialType,
		textureCache: textureCache,
		storeMaterialData: true
	};

	// Build Three.js node from USD with MaterialX/OpenPBR support
	const threeNode = await LightUSDLoaderUtils.buildThreeNode(usdRootNode, defaultMtl, usd_scene, options);

	// Store USD scene reference for material reloading
	threeNode.traverse((child) => {
		if (child.isMesh) {
			child.userData.usdScene = usd_scene;
		}
	});

	// Store reference to USD content node for mixer creation
	usdContentNode = threeNode;

	// Clear existing USD scene
	while (usdSceneRoot.children.length > 0) {
		usdSceneRoot.remove(usdSceneRoot.children[0]);
	}

	// Add loaded USD scene to usdSceneRoot
	usdSceneRoot.add(threeNode);

	// Always reset the shared scene root when loading a new file. Otherwise a
	// Y-up scene loaded after a Z-up scene inherits the previous -90° rotation.
	usdSceneRoot.rotation.x =
		(animationParams.applyUpAxisConversion && fileUpAxis === "Z") ? -Math.PI / 2 : 0;

	// Apply scene scale and update shadow frustum based on model bounds
	animationParams.applySceneScale();

	// Traverse and enable shadows for all meshes
	usdSceneRoot.traverse((child) => {
		if (child.isMesh) {
			child.castShadow = true;
			child.receiveShadow = true;
		}
	});

	// Extract USD animations if available
	try {
		const animationInfos = usd_scene.getAllAnimationInfos();
		// IMPORTANT: Pass threeNode (the USD root) for correct node index mapping
		// The node indices in USD animations reference nodes within the USD scene hierarchy
		usdAnimations = convertUSDAnimationsToThreeJS(usd_scene, threeNode);

		// Build GC-free direct animation data
		buildDirectAnimationData(usd_scene, threeNode);

		if (usdAnimations.length > 0) {
			console.log(`Extracted ${usdAnimations.length} animations from USD file`);

			// Animation parameters updated automatically via playAllUSDAnimations()

			// Log animation details
			usdAnimations.forEach((clip, index) => {
				const info = animationInfos[index];
				let typeStr = '';
				if (info) {
					const types = [];
					if (info.has_skeletal_animation) types.push('skeletal');
					if (info.has_node_animation) types.push('node');
					if (types.length > 0) typeStr = ` [${types.join('+')}]`;
				}
				// console.log(`Animation ${index}: ${clip.name}, duration: ${clip.duration}s, tracks: ${clip.tracks.length}${typeStr}`);
			});

			// Set time range from metadata or first USD animation
			let timeRangeSource = "animation";
			let beginTime = 0;
			let endTime = 0;

			// Prefer metadata startTimeCode/endTimeCode if available
			if (currentSceneMetadata.startTimeCode !== null && currentSceneMetadata.startTimeCode !== undefined &&
			    currentSceneMetadata.endTimeCode !== null && currentSceneMetadata.endTimeCode !== undefined) {
				beginTime = currentSceneMetadata.startTimeCode;
				endTime = currentSceneMetadata.endTimeCode;
				timeRangeSource = "metadata";
			} else {
				// Fallback to first animation clip duration
				const firstClip = usdAnimations[0];
				if (firstClip && firstClip.duration > 0) {
					beginTime = 0;
					endTime = firstClip.duration;
				}
			}

			if (endTime > beginTime) {
				animationParams.beginTime = beginTime;
				animationParams.endTime = endTime;
				animationParams.duration = endTime - beginTime;
				animationParams.time = beginTime; // Reset time to beginning
				// console.log(`Set time range from ${timeRangeSource}: ${beginTime}s - ${endTime}s`);

				// Update GUI controllers if they exist
				updateTimeRangeGUIControllers(endTime);
			}


		// Set playback speed (FPS) from framesPerSecond metadata
		const fps = currentSceneMetadata.framesPerSecond || 24.0;
		animationParams.speed = fps;
		// console.log(`Set animation speed (FPS) from metadata: ${fps}`);
			// Setup all USD animations (paused by default)
			playAllUSDAnimations();
			console.log(`✅ Scene ready! ${usdAnimations.length} animation(s) loaded and paused. Click Play to start.`);
		} else {
			// No USD animations found
			console.log('No USD animations found in this USD file');
			console.log('✅ Scene ready! (no animations)');

			// Still build scene graph UI for static scenes
			buildSceneGraphUI();
		}
	} catch (error) {
		console.log('No animations found in USD file or animation extraction not supported:', error);
	}
}

// Debug: Dump scene hierarchy
function dumpSceneHierarchy(root, prefix = '', level = 0) {
	const indent = '  '.repeat(level);
	// console.log(`${prefix}${indent}"${root.name || 'unnamed'}" [${root.type}] uuid=${root.uuid}`);

	if (root.children && root.children.length > 0) {
		root.children.forEach((child, index) => {
			const isLast = index === root.children.length - 1;
			const childPrefix = isLast ? '└─ ' : '├─ ';
			dumpSceneHierarchy(child, childPrefix, level + 1);
		});
	}
}

// Toggle bounding box for an object
function toggleBoundingBox(obj, show) {
	if (show) {
		// Create bbox helper if it doesn't exist
		if (!objectBBoxHelpers.has(obj.uuid)) {
			// Compute and cache local bounding box (only done once)
			const localBBox = new THREE.Box3();
			const worldBBox = new THREE.Box3();
			computeLocalBoundingBox(obj, localBBox);

			// Compute initial world bbox
			applyRigidTransformToBBox(localBBox, obj, worldBBox);

			// Create a BoxHelper or Box3Helper
			const helper = new THREE.Box3Helper(worldBBox, 0x00ff00); // Green color
			helper.name = `bbox_${obj.name || obj.uuid}`;

			// Store the helper
			objectBBoxHelpers.set(obj.uuid, helper);

			// Cache the local bbox and a reusable world bbox for this object
			objectLocalBBoxCache.set(obj.uuid, {
				localBBox: localBBox,
				worldBBox: worldBBox,
				object: obj  // Store reference to avoid scene traversal
			});

			// Add to scene
			scene.add(helper);

			// console.log(`BBox created for "${obj.name}":`, {
			// 	min: worldBBox.min,
			// 	max: worldBBox.max,
			// 	size: worldBBox.getSize(new THREE.Vector3())
			// });
		} else {
			// Make it visible
			const helper = objectBBoxHelpers.get(obj.uuid);
			helper.visible = true;
		}
	} else {
		// Hide the bbox helper
		if (objectBBoxHelpers.has(obj.uuid)) {
			const helper = objectBBoxHelpers.get(obj.uuid);
			helper.visible = false;
		}
	}
}

// Update bounding box helper for an object (call this during animation)
// Uses cached local bbox + rigid transform for O(1) update instead of O(n) vertex traversal
function updateBoundingBox(obj) {
	const cache = objectLocalBBoxCache.get(obj.uuid);
	if (cache) {
		const helper = objectBBoxHelpers.get(obj.uuid);
		if (helper && helper.visible) {
			// Apply current rigid transform to cached local bbox
			applyRigidTransformToBBox(cache.localBBox, obj, cache.worldBBox);
			helper.box.copy(cache.worldBBox);
		}
	}
}

// Build scene graph tree UI with animation controls
// Performance limit: Skip building UI for scenes with too many objects
const SCENE_GRAPH_UI_MAX_OBJECTS = 100;

function buildSceneGraphUI() {
	if (!window.sceneGraphFolder) return;

	// Clear existing controls
	window.sceneGraphFolder.controllers.forEach(c => c.destroy());
	window.sceneGraphFolder.folders.forEach(f => f.destroy());

	// Count total objects in the USD scene to avoid creating too many GUI elements
	let objectCount = 0;
	if (usdSceneRoot) {
		usdSceneRoot.traverse(() => objectCount++);
	}

	// Skip building detailed scene graph UI for large scenes (performance optimization)
	// Instead, show a simplified UI with click-to-select functionality
	if (objectCount > SCENE_GRAPH_UI_MAX_OBJECTS) {
		console.warn(`[Performance] Scene has ${objectCount} objects, using simplified selection UI (limit: ${SCENE_GRAPH_UI_MAX_OBJECTS})`);

		// Show scene info
		const sceneInfo = {
			objectCount: objectCount,
			meshCount: 0,
			animatedCount: objectAnimationActions.size
		};
		usdSceneRoot.traverse(obj => { if (obj.isMesh) sceneInfo.meshCount++; });

		window.sceneGraphFolder.add(sceneInfo, 'objectCount').name('Total Objects').disable();
		window.sceneGraphFolder.add(sceneInfo, 'meshCount').name('Meshes').disable();
		window.sceneGraphFolder.add(sceneInfo, 'animatedCount').name('Animated').disable();

		// Add instruction
		const instruction = { text: 'Click on 3D objects to select them' };
		window.sceneGraphFolder.add(instruction, 'text').name('💡 Tip').disable();

		window.sceneGraphFolder.show();
		return;
	}

	// Recursively add objects to the tree
	function addObjectToUI(obj, parentFolder) {
		const objectName = obj.name || `${obj.type}_${obj.uuid.slice(0, 8)}`;
		const hasChildren = obj.children && obj.children.length > 0;
		const isAnimated = objectAnimationActions.has(obj.uuid);

		if (hasChildren) {
			// Create a folder for objects with children
			const folder = parentFolder.addFolder(objectName + (isAnimated ? ' 🎬' : ''));

			// Add select button to show transform info
			const selectControl = {
				select: function() {
					selectObject(obj);
				}
			};
			folder.add(selectControl, 'select').name('👁️ Select');

			// Add animation toggle if this object is animated
			if (isAnimated) {
				const animControl = {
					enabled: true,
					toggleAnimation: function() {
						const animData = objectAnimationActions.get(obj.uuid);
						if (animData) {
							animData.enabled = this.enabled;
							if (this.enabled) {
								// Re-enable by setting weight to 1
								animData.action.setEffectiveWeight(1.0);
							} else {
								// Disable by setting weight to 0
								animData.action.setEffectiveWeight(0.0);
							}
							// console.log(`${objectName} animation: ${this.enabled ? 'enabled' : 'disabled'}`);
						}
					}
				};
				folder.add(animControl, 'enabled')
					.name('🎬 Animate')
					.onChange(() => animControl.toggleAnimation());
			}

			// Add bounding box toggle
			const bboxControl = {
				showBBox: false,
				toggleBBox: function() {
					toggleBoundingBox(obj, this.showBBox);
				}
			};
			folder.add(bboxControl, 'showBBox')
				.name('📦 BBox')
				.onChange(() => bboxControl.toggleBBox());

			// Recursively add children
			obj.children.forEach(child => {
				addObjectToUI(child, folder);
			});
		} else {
			// Leaf node - add select button
			const selectControl = {
				select: function() {
					selectObject(obj);
				}
			};
			parentFolder.add(selectControl, 'select').name(`👁️ ${objectName}`);

			// Add animation toggle if this object is animated
			if (isAnimated) {
				const animControl = {
					enabled: true,
					label: objectName + ' 🎬',
					toggleAnimation: function() {
						const animData = objectAnimationActions.get(obj.uuid);
						if (animData) {
							animData.enabled = this.enabled;
							if (this.enabled) {
								animData.action.setEffectiveWeight(1.0);
							} else {
								animData.action.setEffectiveWeight(0.0);
							}
							// console.log(`${objectName} animation: ${this.enabled ? 'enabled' : 'disabled'}`);
						}
					}
				};
				parentFolder.add(animControl, 'enabled')
					.name(animControl.label)
					.onChange(() => animControl.toggleAnimation());
			}

			// Add bounding box toggle for leaf nodes too
			const bboxControl = {
				showBBox: false,
				label: '📦 BBox',
				toggleBBox: function() {
					toggleBoundingBox(obj, this.showBBox);
				}
			};
			parentFolder.add(bboxControl, 'showBBox')
				.name(bboxControl.label)
				.onChange(() => bboxControl.toggleBBox());
		}
	}

	// Start building from usdSceneRoot
	if (usdSceneRoot && usdSceneRoot.children.length > 0) {
		// Add the USD scene root and its children
		addObjectToUI(usdSceneRoot, window.sceneGraphFolder);
		window.sceneGraphFolder.show();
		// console.log('Scene graph UI built');
	}
}

// Select an object and display its transform info
function selectObject(obj) {
	selectedObject = obj;

	// Remove previous selection helper
	if (selectionHelper) {
		scene.remove(selectionHelper);
		if (selectionHelper.geometry) selectionHelper.geometry.dispose();
		if (selectionHelper.material) selectionHelper.material.dispose();
		selectionHelper = null;
	}

	// Create selection helper (wireframe box)
	if (obj.isMesh || obj.isGroup) {
		// Compute and cache the local bounding box for rigid transform optimization
		// For meshes, compute bbox from geometry in local space
		// For groups, compute from children but store relative to this object
		computeLocalBoundingBox(obj, selectionLocalBBox);

		// Compute initial world bbox by applying current transform
		applyRigidTransformToBBox(selectionLocalBBox, obj, selectionWorldBBox);

		selectionHelper = new THREE.Box3Helper(selectionWorldBBox, 0xffff00); // Yellow color
		selectionHelper.name = 'SelectionHelper';
		scene.add(selectionHelper);
	}

	// Update transform info UI
	updateTransformInfoUI(obj);

	// console.log('Selected object:', obj.name, obj);
}

/**
 * Compute local bounding box for an object (in object's local coordinate space)
 * This only needs to be called once when the object is selected, not every frame.
 * @param {THREE.Object3D} obj - The object to compute bbox for
 * @param {THREE.Box3} targetBBox - Box3 to store the result (reused to avoid allocation)
 */
function computeLocalBoundingBox(obj, targetBBox) {
	targetBBox.makeEmpty();

	if (obj.isMesh && obj.geometry) {
		// For meshes, use geometry's bounding box (already in local space)
		if (!obj.geometry.boundingBox) {
			obj.geometry.computeBoundingBox();
		}
		targetBBox.copy(obj.geometry.boundingBox);
	} else {
		// For groups/other objects, compute from children in local space
		// We need to temporarily reset the object's world matrix to identity
		// to get the local-space bounding box
		const tempMatrix = new THREE.Matrix4();
		const invWorldMatrix = new THREE.Matrix4();

		obj.updateWorldMatrix(true, true);
		invWorldMatrix.copy(obj.matrixWorld).invert();

		obj.traverse((child) => {
			if (child.isMesh && child.geometry) {
				if (!child.geometry.boundingBox) {
					child.geometry.computeBoundingBox();
				}
				// Transform child's local bbox to the selected object's local space
				const childBBox = child.geometry.boundingBox.clone();
				tempMatrix.copy(child.matrixWorld).premultiply(invWorldMatrix);
				childBBox.applyMatrix4(tempMatrix);
				targetBBox.union(childBBox);
			}
		});
	}
}

/**
 * Apply rigid transform (translation, rotation, scale) to a local bounding box
 * to compute the world-space bounding box.
 * This is much faster than setFromObject() which traverses all vertices.
 * @param {THREE.Box3} localBBox - Local-space bounding box
 * @param {THREE.Object3D} obj - Object whose world matrix to apply
 * @param {THREE.Box3} targetBBox - Box3 to store the world-space result (reused)
 */
function applyRigidTransformToBBox(localBBox, obj, targetBBox) {
	// Ensure world matrix is up to date
	obj.updateWorldMatrix(true, false);

	// Copy local bbox and apply world transform
	targetBBox.copy(localBBox).applyMatrix4(obj.matrixWorld);
}

// Update transform info UI
function updateTransformInfoUI(obj) {
	if (!window.transformInfoFolder) return;

	// Clear existing controls
	window.transformInfoFolder.controllers.forEach(c => c.destroy());

	if (!obj) {
		window.transformInfoFolder.hide();
		return;
	}

	// Create display object
	const transformInfo = {
		name: obj.name || 'unnamed',
		type: obj.type,
		posX: obj.position.x.toFixed(4),
		posY: obj.position.y.toFixed(4),
		posZ: obj.position.z.toFixed(4),
		rotX: (obj.rotation.x * 180 / Math.PI).toFixed(2) + '°',
		rotY: (obj.rotation.y * 180 / Math.PI).toFixed(2) + '°',
		rotZ: (obj.rotation.z * 180 / Math.PI).toFixed(2) + '°',
		scaleX: obj.scale.x.toFixed(4),
		scaleY: obj.scale.y.toFixed(4),
		scaleZ: obj.scale.z.toFixed(4),
	};

	// Add USD metadata if available
	if (obj.userData['primMeta.absPath']) {
		transformInfo.usdPath = obj.userData['primMeta.absPath'];
	}
	if (obj.userData['primMeta.displayName']) {
		transformInfo.displayName = obj.userData['primMeta.displayName'];
	}

	// Add read-only controllers (no .listen() needed - values are set once on selection)
	window.transformInfoFolder.add(transformInfo, 'name').name('Name').disable();
	window.transformInfoFolder.add(transformInfo, 'type').name('Type').disable();

	if (transformInfo.usdPath) {
		window.transformInfoFolder.add(transformInfo, 'usdPath').name('USD Path').disable();
	}
	if (transformInfo.displayName) {
		window.transformInfoFolder.add(transformInfo, 'displayName').name('Display Name').disable();
	}

	window.transformInfoFolder.add(transformInfo, 'posX').name('Position X').disable();
	window.transformInfoFolder.add(transformInfo, 'posY').name('Position Y').disable();
	window.transformInfoFolder.add(transformInfo, 'posZ').name('Position Z').disable();

	window.transformInfoFolder.add(transformInfo, 'rotX').name('Rotation X').disable();
	window.transformInfoFolder.add(transformInfo, 'rotY').name('Rotation Y').disable();
	window.transformInfoFolder.add(transformInfo, 'rotZ').name('Rotation Z').disable();

	window.transformInfoFolder.add(transformInfo, 'scaleX').name('Scale X').disable();
	window.transformInfoFolder.add(transformInfo, 'scaleY').name('Scale Y').disable();
	window.transformInfoFolder.add(transformInfo, 'scaleZ').name('Scale Z').disable();

	// Add material info for meshes
	if (obj.isMesh && obj.material) {
		const mats = Array.isArray(obj.material) ? obj.material : [obj.material];
		const matInfo = {
			materialCount: mats.length,
			materialType: mats[0].type,
			materialName: mats[0].name || 'unnamed'
		};

		// Material properties
		if (mats[0].color) {
			matInfo.color = '#' + mats[0].color.getHexString();
		}
		if (mats[0].roughness !== undefined) {
			matInfo.roughness = mats[0].roughness.toFixed(2);
		}
		if (mats[0].metalness !== undefined) {
			matInfo.metalness = mats[0].metalness.toFixed(2);
		}
		if (mats[0].opacity !== undefined && mats[0].opacity < 1) {
			matInfo.opacity = mats[0].opacity.toFixed(2);
		}
		if (mats[0].map) {
			matInfo.hasTexture = 'Yes';
		}
		if (mats[0].emissiveMap || (mats[0].emissive && mats[0].emissive.getHex() !== 0)) {
			matInfo.hasEmissive = 'Yes';
		}

		window.transformInfoFolder.add(matInfo, 'materialType').name('Material Type').disable();
		window.transformInfoFolder.add(matInfo, 'materialName').name('Material Name').disable();
		if (matInfo.color) window.transformInfoFolder.addColor(matInfo, 'color').name('Color').disable();
		if (matInfo.roughness) window.transformInfoFolder.add(matInfo, 'roughness').name('Roughness').disable();
		if (matInfo.metalness) window.transformInfoFolder.add(matInfo, 'metalness').name('Metalness').disable();
		if (matInfo.opacity) window.transformInfoFolder.add(matInfo, 'opacity').name('Opacity').disable();
		if (matInfo.hasTexture) window.transformInfoFolder.add(matInfo, 'hasTexture').name('Texture').disable();
		if (matInfo.hasEmissive) window.transformInfoFolder.add(matInfo, 'hasEmissive').name('Emissive').disable();
	}

	// Add animation control for animated objects
	const isAnimated = objectAnimationActions.has(obj.uuid);
	if (isAnimated) {
		const animData = objectAnimationActions.get(obj.uuid);
		const animControl = {
			animated: true,
			enabled: animData.enabled,
			toggleAnimation: function() {
				animData.enabled = this.enabled;
				if (this.enabled) {
					animData.action.setEffectiveWeight(1.0);
				} else {
					animData.action.setEffectiveWeight(0.0);
				}
				// console.log(`Animation for "${obj.name}": ${this.enabled ? 'enabled' : 'disabled'}`);
			}
		};
		window.transformInfoFolder.add(animControl, 'enabled')
			.name('🎬 Animation')
			.onChange(() => animControl.toggleAnimation());
	}

	window.transformInfoFolder.show();
	// console.log('Transform info updated for:', obj.name);
}

// Store per-object animation actions for individual control
const objectAnimationActions = new Map(); // uuid -> { action, enabled }

// Cache for AnimationClip actions to prevent duplicate Interpolant allocations
// Key: clip name, Value: cached action from mixer.clipAction()
const clipActionCache = new Map();

/**
 * Get or create a cached AnimationAction for a clip.
 * This prevents duplicate Interpolant/ArrayBuffer allocations when the same clip is used multiple times.
 * @param {THREE.AnimationMixer} mixer - The animation mixer
 * @param {THREE.AnimationClip} clip - The animation clip
 * @returns {THREE.AnimationAction} The cached or newly created action
 */
function getCachedClipAction(mixer, clip) {
	const cacheKey = clip.name || clip.uuid;

	if (clipActionCache.has(cacheKey)) {
		return clipActionCache.get(cacheKey);
	}

	const action = mixer.clipAction(clip);
	clipActionCache.set(cacheKey, action);
	return action;
}

// Play all USD animations (all channels applied together)
function playAllUSDAnimations() {
	if (usdAnimations.length === 0) return;

	// Ensure mixer exists - create on usdContentNode (the actual USD root) for correct UUID resolution
	// The mixer MUST be created on the same root that was used for animation extraction
	if (!mixer && usdContentNode) {
		mixer = new THREE.AnimationMixer(usdContentNode);
	}

	// Stop all current animations
	objectAnimationActions.forEach(({action}) => action.stop());
	objectAnimationActions.clear();
	clipActionCache.clear(); // Clear cached actions to prevent stale references
	directAnimationData.clear(); // Clear GC-free animation data

	// All USD animation clips contain channels for different objects
	// We need to play all clips together

	usdAnimations.forEach((clip, clipIndex) => {
		if (mixer && clip) {
			// Validate that all tracks can find their targets before creating the action
			let allTracksValid = true;
			const invalidTracks = [];

			clip.tracks.forEach(track => {
				// Track name format: "<uuid>.<property>"
				const parts = track.name.split('.');
				if (parts.length >= 2) {
					const targetName = parts[0];
					let found = false;
					// Check in usdContentNode which is the mixer root
					if (usdContentNode) {
						usdContentNode.traverse(obj => {
							if (obj.uuid === targetName || obj.name === targetName) {
								found = true;
							}
						});
					}
					if (!found) {
						allTracksValid = false;
						invalidTracks.push({ track: track.name, targetName: targetName });
					}
				}
			});

			if (!allTracksValid) {
				console.warn(`⚠️ Clip ${clipIndex} "${clip.name}" has ${invalidTracks.length} track(s) with invalid targets:`);
				invalidTracks.forEach(({track, targetName}) => {
					console.warn(`  - Track "${track}" references target ${targetName.slice(0, 32)} which doesn't exist in scene`);
				});
				console.warn(`  Skipping this clip to avoid errors.`);
				return; // Skip this clip
			}

			// Use cached action to prevent duplicate Interpolant allocations
			const action = getCachedClipAction(mixer, clip);
			action.loop = THREE.LoopRepeat;
			action.play();
			action.paused = true; // Start paused - will be unpaused when user clicks Play

			// Group tracks by target object for per-object control
			clip.tracks.forEach(track => {
				// Track name format: "<uuid>.<property>"
				const parts = track.name.split('.');
				if (parts.length >= 2) {
					const targetName = parts[0];
					let found = false;
					// Traverse usdContentNode which is the mixer root
					if (usdContentNode) {
						usdContentNode.traverse(obj => {
							if (obj.uuid === targetName || obj.name === targetName) {
								found = true;
								// Store action reference for this object
								if (!objectAnimationActions.has(obj.uuid)) {
									objectAnimationActions.set(obj.uuid, {
										action: action,
										enabled: true,
										objectName: obj.name,
										object: obj
									});
								}
							}
						});
					}
					if (!found) {
						console.warn(`    ✗ Target not found for track "${track.name}"`);
					}
				}
			});
		}
	});

	// Store the first action as the main animation action for time control
	// Use cached action to prevent duplicate Interpolant allocations
	if (usdAnimations.length > 0 && mixer) {
		animationAction = getCachedClipAction(mixer, usdAnimations[0]);
	}

	// Pre-warm the animation system to force early allocation of internal buffers
	// This ensures PropertyBindings and Interpolants are fully initialized before the main loop
	if (mixer) {
		// Run a few update cycles to warm up all internal structures
		for (let i = 0; i < 3; i++) {
			mixer.update(0.016); // ~60fps frame time
		}
		// Reset time to beginning
		for (const action of clipActionCache.values()) {
			action.time = animationParams.beginTime;
		}
		mixer.update(0); // Reset mixer state
		// console.log('Animation system pre-warmed');
	}

	// Build scene graph UI with per-object animation controls
	buildSceneGraphUI();
}

// Animation mixer and actions
let mixer = null;
let animationAction = null;

// Animation mixer update throttling (reduces GC pressure with many clips)
let mixerFrameCounter = 0;
let accumulatedMixerTime = 0;

// Debug: Track object transforms during animation
let debugAnimationTracking = false;
let debugFrameCounter = 0;
const DEBUG_LOG_INTERVAL = 60; // Log every 60 frames (about 1 second at 60fps)

function debugLogObjectTransforms() {
	if (!debugAnimationTracking || !usdSceneRoot) return;

	debugFrameCounter++;
	if (debugFrameCounter % DEBUG_LOG_INTERVAL !== 0) return;

	console.log('=== Animation Transform Debug ===');
	console.log(`Time: ${animationParams.time.toFixed(3)}s`);

	usdSceneRoot.traverse((obj) => {
		// Log transforms of named objects or objects with animation
		if (obj.name && obj.name !== '' && obj !== usdSceneRoot) {
			const pos = obj.position;
			const rot = obj.rotation;
			const scale = obj.scale;
			console.log(`  "${obj.name}" (${obj.type}):`, {
				position: `[${pos.x.toFixed(3)}, ${pos.y.toFixed(3)}, ${pos.z.toFixed(3)}]`,
				rotation: `[${rot.x.toFixed(3)}, ${rot.y.toFixed(3)}, ${rot.z.toFixed(3)}]`,
				scale: `[${scale.x.toFixed(3)}, ${scale.y.toFixed(3)}, ${scale.z.toFixed(3)}]`,
				uuid: obj.uuid
			});
		}
	});
	console.log('================================');
}

// Animation parameters
const animationParams = {
	isPlaying: false, // Start paused - animation starts after scene is fully loaded
	playPause: function() {
		this.isPlaying = !this.isPlaying;

		// Reset mixer throttle state when toggling
		mixerFrameCounter = 0;
		accumulatedMixerTime = 0;

		// Pause/unpause all animation actions
		if (mixer) {
			// Reuse pre-allocated Set and for...of to avoid allocations
			uniqueActionsSet.clear();
			for (const [, {action, enabled}] of objectAnimationActions) {
				if (action && enabled) {
					uniqueActionsSet.add(action);
				}
			}

			// Set paused state on all unique actions
			for (const action of uniqueActionsSet) {
				action.paused = !this.isPlaying;
			}
		}

		// Also update the main action if it exists (fallback)
		if (animationAction) {
			animationAction.paused = !this.isPlaying;
		}
	},
	reset: function() {
		animationParams.time = animationParams.beginTime;
		animationParams.playbackSpeed = 1.0;
		animationParams.speed = currentSceneMetadata.timeCodesPerSecond;

		// Reset mixer throttle state
		mixerFrameCounter = 0;
		accumulatedMixerTime = 0;

		// Reset all animation actions
		if (mixer) {
			// Reuse pre-allocated Set and for...of to avoid allocations
			uniqueActionsSet.clear();
			for (const [, {action, enabled}] of objectAnimationActions) {
				if (action && enabled) {
					uniqueActionsSet.add(action);
				}
			}

			// Set time on all unique actions
			for (const action of uniqueActionsSet) {
				action.time = animationParams.beginTime;
			}
		}

		// Also reset the main action if it exists (fallback)
		if (animationAction) {
			animationAction.time = animationParams.beginTime;
		}

		// Update GUI controllers (defined after this object, but available at call time)
		if (typeof speedController !== 'undefined') speedController.updateDisplay();
		if (timelineController) timelineController.updateDisplay();
	},
	time: 0,
	beginTime: 0,
	endTime: 10,
	duration: 10, // timecodes
	// Realtime playback: effective speed = timeCodesPerSecond * playbackSpeed
	// timeCodesPerSecond comes from USD metadata (set at load time)
	speed: 24.0, // effective FPS (= timeCodesPerSecond * playbackSpeed), read-only display
	playbackSpeed: 1.0, // user-controlled multiplier (1.0 = realtime)

	// Rendering options
	shadowsEnabled: true,
	toggleShadows: function() {
		renderer.shadowMap.enabled = this.shadowsEnabled;
		directionalLight.castShadow = this.shadowsEnabled;
		ground.receiveShadow = this.shadowsEnabled;

		// Update all loaded USD objects
		usdSceneRoot.traverse((child) => {
			if (child.isMesh) {
				child.castShadow = this.shadowsEnabled && !child.userData.skipShadows;
				child.receiveShadow = this.shadowsEnabled && !child.userData.skipShadows;
			}
		});
	},

	// Up axis conversion (Z-up to Y-up)
	applyUpAxisConversion: true,
	toggleUpAxisConversion: function() {
		if (this.applyUpAxisConversion && currentFileUpAxis === "Z") {
			// Apply Z-up to Y-up conversion (-90 degrees around X axis)
			usdSceneRoot.rotation.x = -Math.PI / 2;
			// console.log(`[toggleUpAxisConversion] Applied Z-up to Y-up rotation (file upAxis="${currentFileUpAxis}"): usdSceneRoot.rotation.x =`, usdSceneRoot.rotation.x);
		} else {
			// Reset rotation (either disabled or file is already Y-up)
			usdSceneRoot.rotation.x = 0;
			if (this.applyUpAxisConversion && currentFileUpAxis !== "Z") {
				// console.log(`[toggleUpAxisConversion] No rotation needed (file upAxis="${currentFileUpAxis}"): usdSceneRoot.rotation.x =`, usdSceneRoot.rotation.x);
			} else {
				// console.log(`[toggleUpAxisConversion] Reset rotation (conversion disabled): usdSceneRoot.rotation.x =`, usdSceneRoot.rotation.x);
			}
		}
	},

	// Double-sided rendering
	doubleSided: false,
	toggleDoubleSided: function() {
		// Update all loaded USD objects
		usdSceneRoot.traverse((child) => {
			if (child.isMesh && child.material) {
				const side = (this.doubleSided || child.userData?.usdMesh?.doubleSided)
					? THREE.DoubleSide : THREE.FrontSide;
				if (Array.isArray(child.material)) {
					child.material.forEach(mat => {
						mat.side = side;
						mat.needsUpdate = true;
					});
				} else {
					child.material.side = side;
					child.material.needsUpdate = true;
				}
				// Also update original material if stored
				if (child.userData.originalMaterial) {
					if (Array.isArray(child.userData.originalMaterial)) {
						child.userData.originalMaterial.forEach(mat => {
							mat.side = side;
						});
					} else {
						child.userData.originalMaterial.side = side;
					}
				}
			}
		});

		// Update ground plane
		if (ground.material) {
			ground.material.side = this.doubleSided ? THREE.DoubleSide : THREE.FrontSide;
			ground.material.needsUpdate = true;
			if (ground.userData.originalMaterial) {
				ground.userData.originalMaterial.side = this.doubleSided ? THREE.DoubleSide : THREE.FrontSide;
			}
		}
	},

	// Normal visualization
	showNormals: false,
	toggleNormalVisualization: function() {
		// Update all loaded USD objects
		usdSceneRoot.traverse((child) => {
			if (child.isMesh && child.material) {
				// Store original materials if switching to normal view
				if (this.showNormals && !child.userData.originalMaterial) {
					child.userData.originalMaterial = child.material;
					// Create normal material
					const normalMat = new THREE.MeshNormalMaterial({
						side: this.doubleSided ? THREE.DoubleSide : THREE.FrontSide,
						flatShading: false
					});
					child.material = normalMat;
				}
				// Restore original materials if switching back
				else if (!this.showNormals && child.userData.originalMaterial) {
					child.material = child.userData.originalMaterial;
					child.userData.originalMaterial = null;
				}
			}
		});

		// Update ground plane
		if (ground.material) {
			if (this.showNormals && !ground.userData.originalMaterial) {
				ground.userData.originalMaterial = ground.material;
				ground.material = new THREE.MeshNormalMaterial({
					side: this.doubleSided ? THREE.DoubleSide : THREE.FrontSide
				});
			} else if (!this.showNormals && ground.userData.originalMaterial) {
				ground.material = ground.userData.originalMaterial;
				ground.userData.originalMaterial = null;
			}
		}
	},

	// Curves can be numerous and expensive, and are usually guide/debug data.
	showCurves: false,
	toggleCurves: function() {
		usdSceneRoot.traverse((child) => {
			if (child.userData?.usdCurves) child.visible = this.showCurves;
		});
	},

	// Scene scaling
	sceneScale: 1.0,
	applyMetersPerUnit: true, // Apply metersPerUnit scaling from USD metadata
	applySceneScale: function() {
		// Calculate effective scale: user scale * metersPerUnit (if enabled)
		let effectiveScale = this.sceneScale;
		if (this.applyMetersPerUnit && currentSceneMetadata.metersPerUnit) {
			effectiveScale *= currentSceneMetadata.metersPerUnit;
			// console.log(`Applying metersPerUnit: ${currentSceneMetadata.metersPerUnit} (effective scale: ${effectiveScale})`);
		}

		usdSceneRoot.scale.set(effectiveScale, effectiveScale, effectiveScale);

		// Calculate shadow camera frustum based on actual scene bounds
		// This ensures shadows work correctly regardless of model size
		if (usdContentNode) {
			// Compute bounding box of the USD content
			const bbox = new THREE.Box3().setFromObject(usdContentNode);

			// Apply current scale to the bounding box
			const scaledMin = bbox.min.clone().multiplyScalar(effectiveScale);
			const scaledMax = bbox.max.clone().multiplyScalar(effectiveScale);

			// Add padding (20% extra) to ensure shadows aren't clipped
			const padding = 1.2;
			const size = scaledMax.clone().sub(scaledMin).multiplyScalar(padding / 2);

			// Set frustum to cover the entire scaled scene
			const maxSize = Math.max(size.x, size.y, size.z);
			directionalLight.shadow.camera.left = -maxSize;
			directionalLight.shadow.camera.right = maxSize;
			directionalLight.shadow.camera.top = maxSize;
			directionalLight.shadow.camera.bottom = -maxSize;
			directionalLight.shadow.camera.near = 0.5;
			directionalLight.shadow.camera.far = maxSize * 4; // Far enough to cover tall objects

			// Update the shadow camera projection matrix
			directionalLight.shadow.camera.updateProjectionMatrix();

			// console.log(`Shadow camera frustum updated for scale ${effectiveScale}:`, {
			// 	bbox: { min: scaledMin, max: scaledMax },
			// 	frustumSize: maxSize,
			// 	far: maxSize * 4
			// });
		} else {
			// Fallback if usdContentNode not yet loaded
			const baseFrustumSize = 100;
			const frustumSize = baseFrustumSize * effectiveScale;

			directionalLight.shadow.camera.left = -frustumSize;
			directionalLight.shadow.camera.right = frustumSize;
			directionalLight.shadow.camera.top = frustumSize;
			directionalLight.shadow.camera.bottom = -frustumSize;
			directionalLight.shadow.camera.near = 0.5;
			directionalLight.shadow.camera.far = 500 * effectiveScale;

			directionalLight.shadow.camera.updateProjectionMatrix();

			// console.log(`Shadow camera frustum updated (fallback) for scale ${effectiveScale}: [-${frustumSize}, ${frustumSize}]`);
		}
	},
	setScalePreset_0_1: function() {
		this.sceneScale = 0.1;
		this.applySceneScale();
		if (typeof sceneScaleController !== 'undefined') sceneScaleController.updateDisplay();
	},
	setScalePreset_1_0: function() {
		this.sceneScale = 1.0;
		this.applySceneScale();
		if (typeof sceneScaleController !== 'undefined') sceneScaleController.updateDisplay();
	},
	setScalePreset_10_0: function() {
		this.sceneScale = 10.0;
		this.applySceneScale();
		if (typeof sceneScaleController !== 'undefined') sceneScaleController.updateDisplay();
	},

	// Mixer update interval (reduces GC with many animation clips)
	// 1 = every frame, 2 = every 2nd frame, etc.
	mixerUpdateInterval: 2,

	// GC-free direct animation mode (bypasses Three.js AnimationMixer)
	useDirectAnimation: true,
	toggleDirectAnimation: function() {
		useDirectAnimation = this.useDirectAnimation;
		console.log(`Direct animation mode: ${useDirectAnimation ? 'ON (GC-free)' : 'OFF (using AnimationMixer)'}`);
	},

	// Debug animation tracking
	debugAnimationLog: false,
	toggleDebugAnimationLog: function() {
		debugAnimationTracking = this.debugAnimationLog;
		if (this.debugAnimationLog) {
			console.log('Animation debug logging enabled');
			debugFrameCounter = 0; // Reset counter to log immediately
		} else {
			console.log('Animation debug logging disabled');
		}
	},

	// Show all helpers toggle
	showHelpers: true,
	toggleAllHelpers: function() {
		// Update individual toggles to match
		this.showAxisHelper = this.showHelpers;
		this.showGroundPlane = this.showHelpers;
		this.showGrid = this.showHelpers;

		// Apply changes
		axisHelper.visible = this.showAxisHelper;
		ground.visible = this.showGroundPlane;
		gridHelper.visible = this.showGrid;

		// console.log(`All helpers ${this.showHelpers ? 'shown' : 'hidden'}`);
	},

	// Ground plane Y position
	groundPlaneY: 0.0,
	showGroundPlane: true,
	showGrid: true,
	applyGroundPlaneY: function() {
		ground.position.y = this.groundPlaneY;
		gridHelper.position.y = this.groundPlaneY;
		// console.log(`Ground plane Y position set to: ${this.groundPlaneY}`);
	},
	toggleGroundPlane: function() {
		ground.visible = this.showGroundPlane;
		// Update master toggle if needed
		this.updateShowHelpersMasterToggle();
	},
	toggleGrid: function() {
		gridHelper.visible = this.showGrid;
		// Update master toggle if needed
		this.updateShowHelpersMasterToggle();
	},
	updateShowHelpersMasterToggle: function() {
		// Update master toggle to reflect if all helpers are shown
		this.showHelpers = this.showAxisHelper && this.showGroundPlane && this.showGrid;
	},
	fitGroundToScene: function() {
		// Calculate scene bounding box
		const bbox = new THREE.Box3();
		if (usdContentNode && usdContentNode.children.length > 0) {
			bbox.setFromObject(usdContentNode);
		} else if (usdSceneRoot && usdSceneRoot.children.length > 0) {
			bbox.setFromObject(usdSceneRoot);
		} else {
			console.warn('No scene content to fit ground to');
			return;
		}

		if (bbox.isEmpty()) {
			console.warn('Scene bounding box is empty');
			return;
		}

		// Set ground plane to the minimum Y of the bounding box
		this.groundPlaneY = bbox.min.y;
		this.applyGroundPlaneY();
		if (typeof groundPlaneYController !== 'undefined') groundPlaneYController.updateDisplay();
		// console.log(`Ground plane fitted to scene bottom: Y = ${this.groundPlaneY.toFixed(4)}`);
	},

	// Fit to scene
	fitToScene: function() {
		fitToScene();
	},

	// Update functions
	updateDuration: function() {
		this.duration = this.endTime - this.beginTime;
		if (typeof durationController !== 'undefined') durationController.updateDisplay();
	}
};

// GUI setup
const gui = new GUI();
gui.title('Animation Controls');
// Backend switch reloads the page: the loader binds its WASM module at init.
gui.add({ backend: LOADER_BACKEND }, 'backend', LOADER_BACKEND_CHOICES)
	.name('Loader Backend').onChange(setBackendAndReload);

// Store references to GUI controllers for dynamic updates
let timelineController = null;
let beginTimeController = null;
let endTimeController = null;
let envPresetController = null;

// Playback controls
const playbackFolder = gui.addFolder('Playback');
playbackFolder.add(animationParams, 'playPause').name('Play / Pause [Space]');
playbackFolder.add(animationParams, 'reset').name('Reset');
const speedController = playbackFolder.add(animationParams, 'playbackSpeed', 0.1, 5.0, 0.1).name('Speed');
playbackFolder.add(animationParams, 'speed').name('Speed (FPS)').disable();
timelineController = playbackFolder.add(animationParams, 'time', 0, 30, 0.01)
	.name('Timeline')
	.onChange((value) => {
		// When user manually scrubs the timeline, update all animation actions
		if (mixer) {
			// OPTIMIZED: Reuse pre-allocated Set and for...of to avoid allocations
			uniqueActionsSet.clear();
			for (const [, {action, enabled}] of objectAnimationActions) {
				if (action && enabled) {
					uniqueActionsSet.add(action);
				}
			}

			const wasPaused = !animationParams.isPlaying;

			// Stop and reset mixer's time
			mixer.timeScale = 1.0;
			mixer.time = 0;

			// Configure all actions for the target time
			for (const action of uniqueActionsSet) {
				action.paused = false;
				action.enabled = true;
				action.time = value;
				action.weight = 1.0;
			}

			// Also update the main action if it exists
			if (animationAction && !uniqueActionsSet.has(animationAction)) {
				animationAction.paused = false;
				animationAction.enabled = true;
				animationAction.time = value;
				animationAction.weight = 1.0;
			}

			// Force mixer to evaluate by calling update
			mixer.update(0.0001);

			// Compensate for the small delta we added
			for (const action of uniqueActionsSet) {
				action.time = value;
			}
			if (animationAction) {
				animationAction.time = value;
			}

			// Restore paused state if needed
			if (wasPaused) {
				for (const action of uniqueActionsSet) {
					action.paused = true;
				}
				if (animationAction) {
					animationAction.paused = true;
				}
			}
		}
	});

// Time range controls (nested inside Playback folder)
beginTimeController = playbackFolder.add(animationParams, 'beginTime', 0, 29, 0.1)
	.name('Begin TimeCode')
	.onChange(() => {
		if (animationParams.beginTime >= animationParams.endTime) {
			animationParams.beginTime = animationParams.endTime - 0.1;
		}
		animationParams.updateDuration();
	});
endTimeController = playbackFolder.add(animationParams, 'endTime', 0.1, 30, 0.1)
	.name('End TimeCode')
	.onChange(() => {
		if (animationParams.endTime <= animationParams.beginTime) {
			animationParams.endTime = animationParams.beginTime + 0.1;
		}
		animationParams.updateDuration();
	});
// Store reference for manual update when duration changes
const durationController = playbackFolder.add(animationParams, 'duration', 0.1, 30, 0.1)
	.name('Duration')
	.disable();

playbackFolder.open();

// Rendering controls
const renderingFolder = gui.addFolder('Rendering');
renderingFolder.add(animationParams, 'shadowsEnabled')
	.name('Shadows')
	.onChange(() => animationParams.toggleShadows());
renderingFolder.add(animationParams, 'applyUpAxisConversion')
	.name('Z-up to Y-up')
	.onChange(() => animationParams.toggleUpAxisConversion());
renderingFolder.add(animationParams, 'doubleSided')
	.name('Double-Sided')
	.onChange(() => animationParams.toggleDoubleSided());
renderingFolder.add(animationParams, 'showNormals')
	.name('Show Normals')
	.onChange(() => animationParams.toggleNormalVisualization());
renderingFolder.add(animationParams, 'showCurves')
	.name('Show Curves')
	.onChange(() => animationParams.toggleCurves());

// Add master helpers toggle
renderingFolder.add(animationParams, 'showHelpers')
	.name('🔧 Show Helpers (All)')
	.onChange(() => animationParams.toggleAllHelpers());

// Add axis helper toggle
animationParams.showAxisHelper = true;
animationParams.toggleAxisHelper = function() {
	axisHelper.visible = this.showAxisHelper;
	// Update master toggle if needed
	this.updateShowHelpersMasterToggle();
};
renderingFolder.add(animationParams, 'showAxisHelper')
	.name('Show Axis')
	.onChange(() => animationParams.toggleAxisHelper());

// Ground plane controls
const groundFolder = renderingFolder.addFolder('Ground Plane');
groundFolder.add(animationParams, 'showGroundPlane')
	.name('Show Ground')
	.onChange(() => animationParams.toggleGroundPlane());
groundFolder.add(animationParams, 'showGrid')
	.name('Show Grid')
	.onChange(() => animationParams.toggleGrid());
// Store reference for manual update when fitGroundToScene is called
const groundPlaneYController = groundFolder.add(animationParams, 'groundPlaneY', -1000, 1000, 0.01)
	.name('Y Position')
	.onChange(() => animationParams.applyGroundPlaneY());
groundFolder.add(animationParams, 'fitGroundToScene')
	.name('Fit to Scene Bottom');
groundFolder.open();

renderingFolder.add(animationParams, 'fitToScene')
	.name('Fit to Scene');

// Scene scaling controls
const scaleFolder = renderingFolder.addFolder('Scene Scale');
// Store reference for manual update when scale presets are used
const sceneScaleController = scaleFolder.add(animationParams, 'sceneScale', 0.01, 100, 0.01)
	.name('Scale')
	.onChange(() => animationParams.applySceneScale());
scaleFolder.add(animationParams, 'applyMetersPerUnit')
	.name('Apply metersPerUnit')
	.onChange(() => animationParams.applySceneScale());
scaleFolder.add(animationParams, 'setScalePreset_0_1').name('Scale: 1/10 (0.1x)');
scaleFolder.add(animationParams, 'setScalePreset_1_0').name('Scale: 1/1 (1.0x)');
scaleFolder.add(animationParams, 'setScalePreset_10_0').name('Scale: 10/1 (10x)');
scaleFolder.open();

renderingFolder.open();

// ===========================================
// Material & Environment GUI
// ===========================================
const materialFolder = gui.addFolder('Material & Environment');

// Material type selector
materialFolder.add(materialSettings, 'materialType', ['auto', 'openpbr', 'usdpreviewsurface'])
	.name('Material Type')
	.onChange(() => reloadMaterials());

// Environment preset selector
envPresetController = materialFolder.add(materialSettings, 'envMapPreset', Object.keys(ENV_PRESETS))
	.name('Environment')
	.onChange((value) => loadEnvironment(value));

// Constant color environment color picker
materialFolder.addColor(materialSettings, 'envConstantColor')
	.name('Env Color')
	.onChange(() => updateConstantColorEnvironment());

// Environment colorspace workflow
materialFolder.add(materialSettings, 'envColorspace', ['sRGB', 'linear'])
	.name('Env Colorspace')
	.onChange(() => updateConstantColorEnvironment());

// Environment intensity - store reference to update when loading USD DomeLight
const envIntensityController = materialFolder.add(materialSettings, 'envMapIntensity', 0, 3, 0.1)
	.name('Env Intensity')
	.onChange(() => updateEnvIntensity());

// Show environment as background
materialFolder.add(materialSettings, 'showEnvBackground')
	.name('Show Env Background')
	.onChange(() => updateEnvBackground());

// Exposure control
materialFolder.add(materialSettings, 'exposure', 0, 3, 0.1)
	.name('Exposure')
	.onChange((value) => {
		renderer.toneMappingExposure = value;
	});

// Tone mapping selector
materialFolder.add(materialSettings, 'toneMapping', ['none', 'linear', 'reinhard', 'cineon', 'aces', 'agx', 'neutral'])
	.name('Tone Mapping')
	.onChange((value) => updateToneMapping(value));

// Reload materials button
materialFolder.add({ reload: () => reloadMaterials() }, 'reload')
	.name('Reload Materials');

materialFolder.open();

// Scene Metadata - will be populated dynamically
const metadataFolder = gui.addFolder('Scene Metadata');
window.metadataFolder = metadataFolder;
metadataFolder.hide(); // Hide until scene is loaded

// Transform Info - will be populated when object is selected
const transformInfoFolder = gui.addFolder('Transform Info');
window.transformInfoFolder = transformInfoFolder;
transformInfoFolder.hide(); // Hide until object is selected

// Scene Graph Tree - will be populated dynamically
const sceneGraphFolder = gui.addFolder('Scene Graph');
window.sceneGraphFolder = sceneGraphFolder;
sceneGraphFolder.hide(); // Hide until scene is loaded

// Function to update time range GUI controllers when animation is loaded
function updateTimeRangeGUIControllers(maxDuration) {
	const newMax = Math.max(maxDuration, 30); // Ensure minimum of 30s for usability

	// Update timeline controller
	if (timelineController) {
		timelineController.max(newMax);
		timelineController.updateDisplay();
	}

	// Update begin time controller
	if (beginTimeController) {
		beginTimeController.max(newMax - 0.1);
		beginTimeController.updateDisplay();
	}

	// Update end time controller
	if (endTimeController) {
		endTimeController.max(newMax);
		endTimeController.updateDisplay();
	}

	// console.log(`Updated GUI time range to 0-${newMax}s`);
}

// Function to update scene metadata UI
function updateMetadataUI() {
	if (!window.metadataFolder) return;

	// Clear existing controls
	window.metadataFolder.controllers.forEach(c => c.destroy());

	// Create read-only display object
	const metadataDisplay = {
		upAxis: currentSceneMetadata.upAxis,
		metersPerUnit: currentSceneMetadata.metersPerUnit,
		framesPerSecond: currentSceneMetadata.framesPerSecond,
		timeCodesPerSecond: currentSceneMetadata.timeCodesPerSecond,
		startTimeCode: currentSceneMetadata.startTimeCode !== null ? currentSceneMetadata.startTimeCode.toFixed(2) : "N/A",
		endTimeCode: currentSceneMetadata.endTimeCode !== null ? currentSceneMetadata.endTimeCode.toFixed(2) : "N/A",
		autoPlay: currentSceneMetadata.autoPlay,
		comment: currentSceneMetadata.comment || "N/A",
		copyright: currentSceneMetadata.copyright || "N/A"
	};

	// Add read-only controllers (no .listen() needed - values are static)
	window.metadataFolder.add(metadataDisplay, 'upAxis').name('Up Axis').disable();
	window.metadataFolder.add(metadataDisplay, 'metersPerUnit').name('Meters Per Unit').disable();
	window.metadataFolder.add(metadataDisplay, 'framesPerSecond').name('FPS').disable();
	window.metadataFolder.add(metadataDisplay, 'timeCodesPerSecond').name('Timecodes/sec').disable();
	window.metadataFolder.add(metadataDisplay, 'startTimeCode').name('Start TimeCode').disable();
	window.metadataFolder.add(metadataDisplay, 'endTimeCode').name('End TimeCode').disable();
	window.metadataFolder.add(metadataDisplay, 'autoPlay').name('Auto Play').disable();

	if (currentSceneMetadata.comment) {
		window.metadataFolder.add(metadataDisplay, 'comment').name('Comment').disable();
	}
	if (currentSceneMetadata.copyright) {
		window.metadataFolder.add(metadataDisplay, 'copyright').name('Copyright').disable();
	}

	window.metadataFolder.show();
	// console.log('Scene metadata UI updated');
}

// Fit camera, grid, and shadows to scene bounds
function fitToScene() {
	// Compute bounding box of USD scene content
	const bbox = new THREE.Box3();

	if (usdContentNode && usdContentNode.children.length > 0) {
		bbox.setFromObject(usdContentNode);
	} else if (usdSceneRoot && usdSceneRoot.children.length > 0) {
		bbox.setFromObject(usdSceneRoot);
	} else {
		console.warn('No scene content to fit to');
		return;
	}

	// Check if bounding box is valid
	if (bbox.isEmpty()) {
		console.warn('Scene bounding box is empty');
		return;
	}

	// Get bounding box dimensions
	const size = new THREE.Vector3();
	const center = new THREE.Vector3();
	bbox.getSize(size);
	bbox.getCenter(center);

	// console.log('Scene bounds:', {
	// 	min: bbox.min,
	// 	max: bbox.max,
	// 	size: size,
	// 	center: center
	// });

	// Calculate the maximum dimension
	const maxDim = Math.max(size.x, size.y, size.z);

	// Calculate camera distance to fit the scene
	// Use field of view to determine appropriate distance
	const fov = camera.fov * (Math.PI / 180); // Convert to radians
	const cameraDistance = Math.abs(maxDim / Math.sin(fov / 2)) * 0.7; // 0.7 for some padding

	// Position camera at a nice viewing angle (similar to current position ratio)
	const cameraOffset = new THREE.Vector3(1, 1, 1).normalize().multiplyScalar(cameraDistance);
	const newCameraPos = center.clone().add(cameraOffset);

	// Update camera position
	camera.position.copy(newCameraPos);

	// Update OrbitControls target to look at scene center
	controls.target.copy(center);
	controls.update();

	// console.log('Camera fitted:', {
	// 	position: newCameraPos,
	// 	target: center,
	// 	distance: cameraDistance
	// });

	// Update grid size to match scene bounds (with some padding)
	const gridSize = Math.ceil(maxDim * 2.5); // 2.5x padding for context
	const gridDivisions = 20;

	// Remove old grid
	scene.remove(gridHelper);

	// Create new grid at the bottom of the scene (bbox minimum Y)
	gridHelper = new THREE.GridHelper(gridSize, gridDivisions, 0x666666, 0x444444);
	gridHelper.position.y = bbox.min.y;
	scene.add(gridHelper);

	// console.log('Grid updated:', {
	// 	size: gridSize,
	// 	divisions: gridDivisions,
	// 	divisionSize: gridSize / gridDivisions,
	// 	groundY: bbox.min.y
	// });

	// Update ground plane to match grid
	ground.geometry.dispose();
	ground.geometry = new THREE.PlaneGeometry(gridSize, gridSize);
	ground.position.y = bbox.min.y;

	// Update ground plane Y parameter in UI
	animationParams.groundPlaneY = bbox.min.y;

	// Update shadow frustum to cover the scene (with padding)
	const shadowSize = maxDim * 1.5; // 1.5x padding for shadows
	directionalLight.shadow.camera.left = -shadowSize;
	directionalLight.shadow.camera.right = shadowSize;
	directionalLight.shadow.camera.top = shadowSize;
	directionalLight.shadow.camera.bottom = -shadowSize;
	directionalLight.shadow.camera.near = 0.5;
	directionalLight.shadow.camera.far = cameraDistance * 3;
	directionalLight.shadow.camera.updateProjectionMatrix();

	// Position directional light relative to scene
	const lightDistance = cameraDistance * 0.8;
	directionalLight.position.set(
		center.x + lightDistance * 0.5,
		center.y + lightDistance,
		center.z + lightDistance * 0.5
	);

	// console.log('Shadows updated:', {
	// 	frustumSize: shadowSize,
	// 	lightPosition: directionalLight.position,
	// 	far: cameraDistance * 3
	// });

	// console.log('✓ Fit to scene complete');
}

// Info folder
const infoFolder = gui.addFolder('Info');
const info = {
	fps: 0,
	objects: scene.children.length
};
// Store controller reference for manual update (avoiding .listen() overhead)
const fpsController = infoFolder.add(info, 'fps').name('FPS').disable();
infoFolder.add(info, 'objects').name('Objects').disable();
infoFolder.add(animationParams, 'useDirectAnimation')
	.name('Direct Anim (GC-free)')
	.onChange(() => animationParams.toggleDirectAnimation());
infoFolder.add(animationParams, 'mixerUpdateInterval', 1, 4, 1)
	.name('Mixer Skip Frames');
infoFolder.add(animationParams, 'debugAnimationLog')
	.name('Debug Animation Log')
	.onChange(() => animationParams.toggleDebugAnimationLog());
infoFolder.open();

// Window resize handler
window.addEventListener('resize', onWindowResize, false);

function onWindowResize() {
	camera.aspect = window.innerWidth / window.innerHeight;
	camera.updateProjectionMatrix();
	renderer.setSize(window.innerWidth, window.innerHeight);
}

// Mouse click handler for object selection
window.addEventListener('click', onMouseClick, false);

function onMouseClick(event) {
	// Ignore clicks on GUI
	const guiElement = document.querySelector('.lil-gui');
	if (guiElement && guiElement.contains(event.target)) {
		return;
	}

	// Calculate mouse position in normalized device coordinates (-1 to +1)
	mouse.x = (event.clientX / window.innerWidth) * 2 - 1;
	mouse.y = -(event.clientY / window.innerHeight) * 2 + 1;

	// Update the picking ray with the camera and mouse position
	raycaster.setFromCamera(mouse, camera);

	// Calculate objects intersecting the picking ray
	// Only check USD scene objects, not helpers/grid/ground
	const intersectables = [];
	if (usdSceneRoot) {
		usdSceneRoot.traverse((obj) => {
			if (obj.isMesh) {
				intersectables.push(obj);
			}
		});
	}

	const intersects = raycaster.intersectObjects(intersectables, false);

	if (intersects.length > 0) {
		// Select the first intersected object
		const selectedObj = intersects[0].object;
		selectObject(selectedObj);
		// console.log('Clicked object:', selectedObj.name);
	} else {
		// Deselect if clicking on empty space
		if (selectedObject) {
			selectedObject = null;
			if (selectionHelper) {
				scene.remove(selectionHelper);
				if (selectionHelper.geometry) selectionHelper.geometry.dispose();
				if (selectionHelper.material) selectionHelper.material.dispose();
				selectionHelper = null;
			}
			updateTransformInfoUI(null);
			// console.log('Deselected object');
		}
	}
}

// Function to load a USD file from ArrayBuffer
async function loadUSDFromArrayBuffer(arrayBuffer, filename, stats = null) {
	window.renderComplete = false;
	stats = stats || beginLoadStats(arrayBuffer.byteLength);
	if (!Number.isFinite(stats.fileSize)) {
		stats.fileSize = arrayBuffer.byteLength;
	}
	setCurrentFileName(filename);

	// Initialize PBR renderer if not already done
	if (!pmremGenerator) {
		initializePBRRenderer();
		// Load default environment
		await loadEnvironment(materialSettings.envMapPreset);
	}

	cleanupCurrentTextureLoading();

	// Clear existing USD scene
	while (usdSceneRoot.children.length > 0) {
		const child = usdSceneRoot.children[0];
		// Dispose geometries and materials
		child.traverse((obj) => {
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
		usdSceneRoot.remove(child);
	}

	// Clear selection
	selectedObject = null;
	if (selectionHelper) {
		scene.remove(selectionHelper);
		if (selectionHelper.geometry) selectionHelper.geometry.dispose();
		if (selectionHelper.material) selectionHelper.material.dispose();
		selectionHelper = null;
	}
	updateTransformInfoUI(null);

	// Clear bounding box helpers
	objectBBoxHelpers.forEach((helper) => {
		scene.remove(helper);
		helper.geometry.dispose();
		if (helper.material) {
			helper.material.dispose();
		}
	});
	objectBBoxHelpers.clear();

	// Clear cached local bounding boxes
	objectLocalBBoxCache.clear();

	// Stop animation playback
	animationParams.isPlaying = false;

	// Stop and clear animation mixer
	if (mixer) {
		mixer.stopAllAction();
		mixer = null;
	}
	animationAction = null;
	clipActionCache.clear(); // Clear cached actions
	directAnimationData.clear(); // Clear GC-free animation data

	// Reset animations
	usdAnimations = [];

	// Dispose textures in cache
	textureCache.forEach((texture) => {
		if (texture && texture.dispose) {
			texture.dispose();
		}
	});
	textureCache.clear();

	// Clean up WASM memory from previous load
	if (currentUSDScene) {
		try {
			// Try to delete the USD scene if it has a delete method
			if (typeof currentUSDScene.delete === 'function') {
				currentUSDScene.delete();
				// console.log('USD scene deleted');
			}
		} catch (e) {
			console.warn('Could not delete USD scene:', e);
		}
		currentUSDScene = null;
	}

	// Clean up previous loader
	if (currentLoader) {
		try {
			// Try to access native loader for memory cleanup
			if (currentLoader.native_ && typeof currentLoader.native_.reset === 'function') {
				currentLoader.native_.reset();
				// console.log('WASM memory reset via native loader');
			} else if (currentLoader.native_ && typeof currentLoader.native_.clearAssets === 'function') {
				currentLoader.native_.clearAssets();
				// console.log('WASM assets cleared via native loader');
			}
		} catch (e) {
			console.warn('Could not reset WASM memory:', e);
		}
		currentLoader = null;
	}

	// Clear USD DomeLight data
	usdDomeLightData = null;

	const loader = new LightUSDLoader();
	await loader.init({ useZstdCompressedWasm: false, useMemory64: false });
	currentLoader = loader; // Store reference for cleanup

	console.log(`Loading USD from file: ${filename} (${(arrayBuffer.byteLength / 1024).toFixed(2)} KB)`);

	const parseStart = performance.now();
	let usd_scene;
	if (LOADER_BACKEND === 'core' || LOADER_BACKEND === 'auto') {
		showLoadProgress('parsing', 10, 'Preparing core backend input...', true);
		usd_scene = await new Promise((resolve, reject) => {
			loader.parse(
				new Uint8Array(arrayBuffer),
				filename,
				resolve,
				reject,
				makeStaticParseOptions({
					backend: LOADER_BACKEND,
					onProgress: reportCoreParseProgress,
					progressBase: 10,
					progressRange: 60
				})
			);
		});
	} else {
		const blob = new Blob([arrayBuffer]);
		const blobUrl = URL.createObjectURL(blob);
		try {
			usd_scene = await loader.loadAsync(blobUrl);
		} finally {
			URL.revokeObjectURL(blobUrl);
		}
	}
	stats.parseMs = performance.now() - parseStart;
	currentUSDScene = usd_scene; // Store reference for cleanup
	stats.nativeStats = usd_scene?.getStats?.() || null;
	stats.nativeTimings = stats.nativeStats?.timings || null;
	const processStart = performance.now();

	if (isScene(usd_scene)) {
		showLoadProgress('processing', 70, 'Preparing scene metadata and animation...', true);
		const sceneMetadata = readSceneMeta(usd_scene);
		const fileUpAxis = sceneMetadata.upAxis || "Y";
		const rawSceneMetadata = usd_scene.getSceneMetadata ? usd_scene.getSceneMetadata() : {};
		const timeCodesPerSecond = rawSceneMetadata.timeCodesPerSecond || 24.0;
		currentFileUpAxis = fileUpAxis;
		currentSceneMetadata = {
			upAxis: fileUpAxis,
			metersPerUnit: sceneMetadata.metersPerUnit || 1.0,
			framesPerSecond: rawSceneMetadata.framesPerSecond || timeCodesPerSecond,
			timeCodesPerSecond,
			startTimeCode: rawSceneMetadata.startTimeCode ?? null,
			endTimeCode: rawSceneMetadata.endTimeCode ?? null,
			autoPlay: false,
			comment: "core backend render",
			copyright: ""
		};
		updateMetadataUI();

		const {
			hasSkinnedMeshData,
			allSkinnedMeshUSDData,
			skinnedMeshDataByName
		} = extractSkinnedMeshData(usd_scene, { logger: console });
		const skeletonBuild = buildSkeletonDataFromUSD(usd_scene, {
			logger: console,
			hasSkinnedMeshData
		});
		const built = buildThreeNode(usd_scene, {
			skipTextures: false,
			lazyTextures: true,
			showCurves: animationParams.showCurves,
			pruneEmptyNodes: true,
			onProgress: (info = {}) => {
				const local = Math.max(0, Math.min(100, Number(info.percentage) || 0));
				showLoadProgress('building', 72 + local * 0.16,
					info.message || `Building scene ${info.builtMeshes || 0} / ${info.totalMeshes || 0}`);
			},
			// Animations/skinning are extracted from the adapter AFTER the
			// build; the default releaseBuildData would wipe them.
			releaseBuildData: false
		});
		// Start texture decode only after geometry, skinning and animation are
		// ready. Running four-to-sixteen decoders here used to compete with the
		// synchronous build and inflated its peak memory.
		const coreTextureManager = built.textureManager;
		// Core animation target_node is a RenderScene node-table index; use the
		// table map from buildThreeNode, not a DFS walk of the built tree.
		const nodeIndexMap = built.nodeIndexMap || buildNodeIndexMap(built.node);
		const skinningResult = applyUSDSceneSkinningPipeline({
			threeNode: built.node,
			characterGroup: usdSceneRoot,
			helperScene: scene,
			skeletonDataArray: skeletonBuild.skeletonDataArray,
			allSkinnedMeshUSDData,
			skinnedMeshDataByName,
			usdScene: usd_scene,
			showMesh: true,
			showSkeleton: false,
			useWASMBoneTexture: false,
			logger: console
		});
		usdContentNode = built.node;

		// Match the legacy animation path: prefer an authored DomeLight over the
		// demo's startup HDRI. The core path previously returned without this
		// step, leaving exported scenes under the much brighter goegap environment
		// even though the RenderScene contained the same DomeLight.
		// Load after the Three.js hierarchy is attached so applyEnvironment()
		// updates every generated material as well as scene.environment.
		try {
			const domeLightData = await loadDomeLightFromUSD(usd_scene);
			if (domeLightData && envPresetController) {
				envPresetController.updateDisplay();
			}
		} catch (error) {
			console.warn('Error checking core scene for DomeLight:', error);
		}

		// usdSceneRoot is reused between loads, so reset stale Z-up conversion for
		// Y-up files as well as applying it for Z-up files.
		usdSceneRoot.rotation.x =
			(animationParams.applyUpAxisConversion && fileUpAxis === "Z") ? -Math.PI / 2 : 0;
		animationParams.applySceneScale();
		for (const child of skinningResult.allSceneMeshes || []) {
			if (child.isMesh) {
				const materials = Array.isArray(child.material) ? child.material : [child.material];
				const opacityMapped = materials.some((material) =>
					!!material?.userData?.coreTexturePaths?.opacity);
				// VFX cards (lasers, beams, projected shadows) should not render
				// another full-card pass into the directional shadow map.
				child.userData.skipShadows = opacityMapped;
				child.castShadow = !opacityMapped;
				child.receiveShadow = !opacityMapped;
			}
		}
		try {
			const animData = extractUSDSceneAnimations(usd_scene, {
				boneMaps: skeletonBuild.boneMaps,
				nodeIndexMap,
				threeRoot: built.node,
				timeCodesPerSecond,
				// Large animated stages can contain thousands of clips. Retaining a
				// console line per clip is measurable time and memory in DevTools.
				logger: animationLoadLogger
			});
			usdAnimations = [
				...animData.usdAnimations,
				...animData.usdNodeAnimations
			];
			if (usdAnimations.length > 0) {
				let beginTime = 0;
				let endTime = 0;
				if (currentSceneMetadata.startTimeCode !== null && currentSceneMetadata.startTimeCode !== undefined &&
				    currentSceneMetadata.endTimeCode !== null && currentSceneMetadata.endTimeCode !== undefined) {
					beginTime = currentSceneMetadata.startTimeCode;
					endTime = currentSceneMetadata.endTimeCode;
				} else {
					for (const clip of usdAnimations) {
						if (clip && clip.duration > endTime) endTime = clip.duration;
					}
				}
				if (endTime > beginTime) {
					animationParams.beginTime = beginTime;
					animationParams.endTime = endTime;
					animationParams.duration = endTime - beginTime;
					animationParams.time = beginTime;
					updateTimeRangeGUIControllers(endTime);
				}
				animationParams.speed = currentSceneMetadata.framesPerSecond || timeCodesPerSecond;
				playAllUSDAnimations();
				console.log(`[animation] core backend loaded ${usdAnimations.length} animation clip(s)`);
			} else {
				buildSceneGraphUI();
			}
		} catch (error) {
			console.warn('[animation] next animation extraction failed:', error);
			buildSceneGraphUI();
		}
		if (typeof usd_scene.releaseBuildData === 'function') {
			usd_scene.releaseBuildData();
		}
		stats.processMs = performance.now() - processStart;
		stats.totalMs = performance.now() - stats.startTime;
		stats.memoryAfter = captureMemorySnapshot();
		updateLoadStatsPanel(stats);
		window.renderComplete = true;
		if (coreTextureManager && coreTextureManager.total > 0) {
			showLoadProgress('textures', 90,
				`Loading textures 0 / ${coreTextureManager.total}`, true);
			startTrackedTextureLoading(coreTextureManager, stats, 'coreTextureQueue');
		} else {
			showLoadProgress('complete', 100, 'Load complete', true);
			hideLoadProgress();
		}
		return;
	}

	// Get the default root node from USD
	const usdRootNode = usd_scene.getDefaultRootNode();

	// Get scene metadata from the USD file
	const sceneMetadata = usd_scene.getSceneMetadata ? usd_scene.getSceneMetadata() : {};
	const fileUpAxis = sceneMetadata.upAxis || "Y";
	currentFileUpAxis = fileUpAxis; // Store globally for toggle function

	// Store metadata globally
	currentSceneMetadata = {
		upAxis: fileUpAxis,
		metersPerUnit: sceneMetadata.metersPerUnit || 1.0,
		framesPerSecond: sceneMetadata.framesPerSecond || 24.0,
		timeCodesPerSecond: sceneMetadata.timeCodesPerSecond || 24.0,
		startTimeCode: sceneMetadata.startTimeCode,
		endTimeCode: sceneMetadata.endTimeCode,
		autoPlay: sceneMetadata.autoPlay !== undefined ? sceneMetadata.autoPlay : true,
		comment: sceneMetadata.comment || "",
		copyright: sceneMetadata.copyright || ""
	};

	// Update metadata UI
	updateMetadataUI();

	// Try to load DomeLight environment from USD
	try {
		const domeLightData = await loadDomeLightFromUSD(usd_scene);
		if (domeLightData) {
			if (envPresetController) {
				envPresetController.updateDisplay();
			}
		}
	} catch (error) {
		console.warn('Error checking for DomeLight:', error);
	}

	// Create default material with environment map
	const defaultMtl = new THREE.MeshPhysicalMaterial({
		color: 0x888888,
		roughness: 0.5,
		metalness: 0.0,
		envMap: envMap,
		envMapIntensity: materialSettings.envMapIntensity
	});

	// Clear texture cache for fresh load
	textureCache.clear();

	const options = {
		overrideMaterial: false,
		envMap: envMap,
		envMapIntensity: materialSettings.envMapIntensity,
		preferredMaterialType: materialSettings.materialType,
		textureCache: textureCache,
		textureLoadingManager: new TextureLoadingManager(),
		storeMaterialData: true
	};

	// Build Three.js node from USD with MaterialX/OpenPBR support
	const threeNode = await LightUSDLoaderUtils.buildThreeNode(usdRootNode, defaultMtl, usd_scene, options);
	if (options.textureLoadingManager) {
		startTrackedTextureLoading(options.textureLoadingManager, stats, 'textureQueue');
	}

	// Store USD scene reference for material reloading
	threeNode.traverse((child) => {
		if (child.isMesh) {
			child.userData.usdScene = usd_scene;
		}
	});

	// Store reference to USD content node for mixer creation
	usdContentNode = threeNode;

	// Add loaded USD scene to usdSceneRoot
	usdSceneRoot.add(threeNode);

	// Debug: Log initial transforms of all objects
	// console.log('=== Initial Object Transforms ===');
	// threeNode.traverse((obj) => {
	// 	if (obj.name && obj.name !== '') {
	// 		console.log(`Object "${obj.name}": position=[${obj.position.x.toFixed(3)}, ${obj.position.y.toFixed(3)}, ${obj.position.z.toFixed(3)}], scale=[${obj.scale.x.toFixed(3)}, ${obj.scale.y.toFixed(3)}, ${obj.scale.z.toFixed(3)}]`);
	// 	}
	// });
	// console.log('=================================');

	// Always assign the shared scene-root rotation so a Y-up file loaded after
	// a Z-up file does not retain the previous file's conversion.
	usdSceneRoot.rotation.x =
		(animationParams.applyUpAxisConversion && fileUpAxis === "Z") ? -Math.PI / 2 : 0;
	if (animationParams.applyUpAxisConversion && fileUpAxis !== "Y" && fileUpAxis !== "Z") {
		console.warn(`[loadUSDFromArrayBuffer] File upAxis is "${fileUpAxis}" (not Y or Z), no rotation applied`);
	}

	// Apply scene scale and update shadow frustum based on model bounds
	animationParams.applySceneScale();

	// Traverse and enable shadows for all meshes
	usdSceneRoot.traverse((child) => {
		if (child.isMesh) {
			child.castShadow = true;
			child.receiveShadow = true;
		}
	});

	// Extract USD animations if available
	try {
		const animationInfos = usd_scene.getAllAnimationInfos();
		// IMPORTANT: Pass threeNode (the USD root) for correct node index mapping
		// The node indices in USD animations reference nodes within the USD scene hierarchy
		usdAnimations = convertUSDAnimationsToThreeJS(usd_scene, threeNode);

		// Build GC-free direct animation data
		buildDirectAnimationData(usd_scene, threeNode);

		if (usdAnimations.length > 0) {
			console.log(`Extracted ${usdAnimations.length} animations from USD file`);

			// Animation parameters updated automatically via playAllUSDAnimations()

			// Log animation details
			usdAnimations.forEach((clip, index) => {
				const info = animationInfos[index];
				let typeStr = '';
				if (info) {
					const types = [];
					if (info.has_skeletal_animation) types.push('skeletal');
					if (info.has_node_animation) types.push('node');
					if (types.length > 0) typeStr = ` [${types.join('+')}]`;
				}
				// console.log(`Animation ${index}: ${clip.name}, duration: ${clip.duration}s, tracks: ${clip.tracks.length}${typeStr}`);
			});

			// Set time range from metadata or first USD animation
			let timeRangeSource = "animation";
			let beginTime = 0;
			let endTime = 0;

			// Prefer metadata startTimeCode/endTimeCode if available
			if (currentSceneMetadata.startTimeCode !== null && currentSceneMetadata.startTimeCode !== undefined &&
			    currentSceneMetadata.endTimeCode !== null && currentSceneMetadata.endTimeCode !== undefined) {
				beginTime = currentSceneMetadata.startTimeCode;
				endTime = currentSceneMetadata.endTimeCode;
				timeRangeSource = "metadata";
			} else {
				// Fallback to first animation clip duration
				const firstClip = usdAnimations[0];
				if (firstClip && firstClip.duration > 0) {
					beginTime = 0;
					endTime = firstClip.duration;
				}
			}

			if (endTime > beginTime) {
				animationParams.beginTime = beginTime;
				animationParams.endTime = endTime;
				animationParams.duration = endTime - beginTime;
				animationParams.time = beginTime; // Reset time to beginning
				// console.log(`Set time range from ${timeRangeSource}: ${beginTime}s - ${endTime}s`);

				// Update GUI controllers if they exist
				updateTimeRangeGUIControllers(endTime);
			}


		// Set playback speed (FPS) from framesPerSecond metadata
		const fps = currentSceneMetadata.framesPerSecond || 24.0;
		animationParams.speed = fps;
		// console.log(`Set animation speed (FPS) from metadata: ${fps}`);
			// Setup all USD animations (paused by default)
			playAllUSDAnimations();
			console.log(`✅ Scene ready! ${usdAnimations.length} animation(s) loaded and paused. Click Play to start.`);
		} else {
			// No USD animations found
			console.log('No USD animations found in USD file');
			console.log('✅ Scene ready! (no animations)');

			// Still build scene graph UI for static scenes
			buildSceneGraphUI();
		}
	} catch (error) {
		console.log('No animations found in USD file or animation extraction not supported:', error);

		// Still build scene graph UI for static scenes
		buildSceneGraphUI();
	}

	stats.processMs = performance.now() - processStart;
	stats.totalMs = performance.now() - stats.startTime;
	stats.memoryAfter = captureMemorySnapshot();
	updateLoadStatsPanel(stats);
	window.renderComplete = true;
}

// Listen for file upload events
window.addEventListener('loadUSDFile', async (event) => {
	const file = event.detail.file;
	if (!file) return;

	const stats = beginLoadStats();
	try {
		showLoadProgress('fetch', 2, 'Reading USD file...', true);
		const readStart = performance.now();
		const arrayBuffer = await file.arrayBuffer();
		stats.fetchMs = performance.now() - readStart;
		stats.fileSize = arrayBuffer.byteLength;
		showLoadProgress('fetch', 10, 'USD file read', true);
		await loadUSDFromArrayBuffer(arrayBuffer, file.name, stats);
		console.log('USD file loaded successfully:', file.name);

		// Hide loading indicator
		if (window.hideLoadingIndicator) {
			window.hideLoadingIndicator();
		}
	} catch (error) {
		failLoadStats(stats);
		console.error('Failed to load USD file:', error);
		alert('Failed to load USD file: ' + error.message);

		// Hide loading indicator on error too
		if (window.hideLoadingIndicator) {
			window.hideLoadingIndicator();
		}
	}
});

async function loadUSDFromURI(uri) {
	const displayName = getDisplayNameFromURI(uri);
	const stats = beginLoadStats();
	setCurrentFileName(displayName);
	const loadingIndicator = document.getElementById('loadingIndicator');
	if (loadingIndicator) loadingIndicator.classList.add('active');
	try {
		showLoadProgress('fetch', 2, 'Fetching USD file...', true);
		const fetchStart = performance.now();
		const response = await fetch(uri);
		if (!response.ok) {
			throw new Error(`Failed to fetch ${uri}: ${response.statusText}`);
		}
		const arrayBuffer = await response.arrayBuffer();
		stats.fetchMs = performance.now() - fetchStart;
		stats.fileSize = arrayBuffer.byteLength;
		showLoadProgress('fetch', 10, 'USD file fetched', true);
		await loadUSDFromArrayBuffer(arrayBuffer, displayName, stats);
	} catch (error) {
		failLoadStats(stats);
		console.error(`Failed to load USD file (${uri}):`, error);
		alert('Failed to load USD file: ' + error.message);
	} finally {
		if (window.hideLoadingIndicator) {
			window.hideLoadingIndicator();
		}
	}
}

document.body.addEventListener('dragover', (event) => {
	event.preventDefault();
	document.body.classList.add('drag-over');
});

document.body.addEventListener('dragleave', (event) => {
	if (!event.relatedTarget || !document.body.contains(event.relatedTarget)) {
		document.body.classList.remove('drag-over');
	}
});

document.body.addEventListener('drop', (event) => {
	event.preventDefault();
	document.body.classList.remove('drag-over');
	const file = event.dataTransfer?.files?.[0];
	if (!file) return;
	if (!/\.(usd|usda|usdc|usdz)$/i.test(file.name)) {
		alert('Please drop a USD file (.usd, .usda, .usdc, .usdz)');
		return;
	}
	window.dispatchEvent(new CustomEvent('loadUSDFile', { detail: { file } }));
});

// Listen for default model reload
window.addEventListener('loadDefaultModel', async () => {
	try {
		await loadUSDModel();
		console.log('Default model reloaded');
	} catch (error) {
		console.error('Failed to reload default model:', error);
	}
});

// Load USD model
const startupUSDURI = getStartupUSDModelURI();
(startupUSDURI ? loadUSDFromURI(startupUSDURI) : loadUSDModel()).catch((error) => {
	console.error('Failed to load USD model:', error);
	alert('Failed to load USD file: ' + error.message);
});

// FPS calculation
let lastTime = performance.now();
let frames = 0;
let fpsUpdateTime = 0;

// Animation loop
// OPTIMIZED: Uses cached objects to avoid per-frame allocations and scene traversals
function animate() {
	requestAnimationFrame(animate);

	const currentTime = performance.now();
	const deltaTime = (currentTime - lastTime) / 1000; // Convert to seconds
	lastTime = currentTime;

	// Update FPS
	frames++;
	fpsUpdateTime += deltaTime;
	if (fpsUpdateTime >= 0.5) {
		info.fps = Math.round(frames / fpsUpdateTime);
		fpsController.updateDisplay(); // Manual update instead of .listen()
		const fpsValueEl = document.getElementById('fpsValue');
		if (fpsValueEl) fpsValueEl.textContent = String(info.fps);
		frames = 0;
		fpsUpdateTime = 0;
	}

	// Update animation time with begin/end range
	// Effective speed = timeCodesPerSecond * playbackSpeed (realtime at 1.0x)
	if (animationParams.isPlaying) {
		const effectiveSpeed = currentSceneMetadata.timeCodesPerSecond * animationParams.playbackSpeed;
		animationParams.speed = effectiveSpeed; // keep display value in sync
		animationParams.time += deltaTime * effectiveSpeed;

		// Loop within begin/end range
		if (animationParams.time > animationParams.endTime) {
			animationParams.time = animationParams.beginTime;

			// Sync all animation actions to the new time
			if (mixer) {
				// OPTIMIZED: Reuse pre-allocated Set and for...of to avoid closure allocation
				uniqueActionsSet.clear();
				for (const [, {action, enabled}] of objectAnimationActions) {
					if (action && enabled) {
						uniqueActionsSet.add(action);
					}
				}

				// Set time on all unique actions
				for (const action of uniqueActionsSet) {
					action.time = animationParams.beginTime;
				}
			}

			// Also update the main action if it exists (fallback)
			if (animationAction) {
				animationAction.time = animationParams.beginTime;
			}
		}
		if (animationParams.time < animationParams.beginTime) {
			animationParams.time = animationParams.beginTime;
		}

		// Manual timeline update instead of .listen()
		// Throttle to every 3 frames to reduce allocations from lil-gui
		if (timelineController && (frames % 3 === 0)) {
			timelineController.updateDisplay();
		}
	}

	// Update animations
	if (animationParams.isPlaying) {
		if (useDirectAnimation && directAnimationData.size > 0) {
			// GC-free direct animation: directly set object transforms
			updateDirectAnimations(animationParams.time);
		} else if (mixer && animationAction) {
			// Fallback to Three.js AnimationMixer (has GC overhead)
			accumulatedMixerTime += deltaTime * (currentSceneMetadata.timeCodesPerSecond * animationParams.playbackSpeed);
			mixerFrameCounter++;

			if (mixerFrameCounter >= animationParams.mixerUpdateInterval) {
				mixer.update(accumulatedMixerTime);
				accumulatedMixerTime = 0;
				mixerFrameCounter = 0;
			}
		}
	}

	// Debug: Log object transforms periodically
	debugLogObjectTransforms();

	// Update bounding boxes for objects that are being displayed
	// OPTIMIZED: Use for...of to avoid closure allocation
	for (const [uuid, cache] of objectLocalBBoxCache) {
		const helper = objectBBoxHelpers.get(uuid);
		if (helper && helper.visible) {
			// Apply current rigid transform to cached local bbox (O(1) operation)
			applyRigidTransformToBBox(cache.localBBox, cache.object, cache.worldBBox);
			helper.box.copy(cache.worldBBox);
		}
	}

	// Update selection helper to follow selected object
	// OPTIMIZED: Use cached local bbox + rigid transform instead of setFromObject()
	if (selectionHelper && selectedObject) {
		applyRigidTransformToBBox(selectionLocalBBox, selectedObject, selectionWorldBBox);
		selectionHelper.box.copy(selectionWorldBBox);
	}

	// Update controls
	controls.update();

	// Render
	renderer.render(scene, camera);
}

// Start animation
animate();

// ===========================================
// Keyboard Shortcuts
// ===========================================

window.addEventListener('keydown', (event) => {
	// Ignore if typing in an input field
	if (event.target.tagName === 'INPUT' || event.target.tagName === 'TEXTAREA') return;

	switch (event.code) {
		case 'Space':
			event.preventDefault();
			animationParams.playPause();
			break;
	}
});

// ===========================================
// Debug: Expose key objects globally for performance profiling
// ===========================================
window.renderer = renderer;
window.scene = scene;
window.camera = camera;
window.directionalLight = directionalLight;
// Expose key objects for external access (e.g., parent frame integration)
window.usdSceneRoot = usdSceneRoot;
window.mixer = () => mixer;
window.animationParams = animationParams;
window.THREE = THREE;
