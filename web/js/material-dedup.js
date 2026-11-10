// LightUSD Material / Mesh Dedup demo.
//
// Demonstrates the Tydra render-scene optimizations exposed through the WASM
// binding: material/texture deduplication, mesh merging (with transform
// baking) and render-tree flattening. Each toggle re-runs the native
// conversion (LightUSDLoader.parse with the corresponding options) and rebuilds
// the Three.js scene so the mesh / material / draw-call reduction is visible
// live.
//
// Also shows global scene scaling that honors the USD `metersPerUnit` stage
// metadata and up-axis (Z-up -> Y-up) conversion.
//
// Textures are always decoded lazily on the JS side (the native conversion does
// no texture decode — see convertScene), and stream in after the untextured
// scene renders.
//
// Recommended combinations:
//   - General viewer (no per-object picking): native Material Dedup + Mesh
//     Merge (bake). Fewest materials and draw calls; geometry is concatenated
//     so per-instance identity is lost. This is the default.
//   - Picking / selection needed: native Material Dedup + Mesh Merge OFF +
//     three.js "Batch By Material". BatchedMesh cuts draw calls while keeping
//     per-instance transforms and identity (raycast).
//   - "JS Material Dedup" is for the structural (textures-off) view only: it is
//     redundant with native material dedup, and with lazy textures it collapses
//     materials before their textures load (texture bleed). It is force-skipped
//     while textures are enabled (see applyJsPostProcess) and off by default.
//
// Patterned after animation.js / skin-anim.js / materialx.js / progress-demo.js.

import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import GUI from 'three/examples/jsm/libs/lil-gui.module.min.js';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils, TextureLoadingManager } from 'lightusd/LightUSDLoaderUtils.js';
import { dedupMaterialsByContent, batchByMaterial } from 'lightusd/MeshBatching.js';
import { getAssetUriFromURL } from 'lightusd/LoaderConfigUtils.js';
import { parseUSDZEntries } from './src/usdzconvert.js';
import {
	buildNextThreeNode,
	findNextArchiveEntry,
	isNextScene,
	nextArchiveUDIMLayout,
	nextCountsFromScene,
	readNextSceneMeta
} from 'lightusd/NextRenderSceneUtils.js';

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

let renderer, scene, camera, controls, gui;
let usdSceneRoot; // group that holds the converted USD scene (scaled/oriented)
let loader = null; // reused LightUSDLoader instance
let loaderModuleBackend = null; // Core module, retained across loads.
let currentUsd = null; // current native scene (embind object)
let raycaster, pointerNdc;
let pickedObject = null;

let rawBytes = null; // Uint8Array of the active model
let currentName = 'sample scene';
let currentIsGeneratedSample = true;

// metersPerUnit / upAxis from the active model's stage metadata.
let sceneMeta = { upAxis: 'Y', metersPerUnit: 1.0 };

// Baseline counts captured with all optimizations OFF, for comparison.
let baseline = null;

// Native render-scene counts of the current selection (so three.js-only
// post-process toggles can refresh stats without re-converting).
let currentCounts = null;

// Result of the last three.js post-process pass (material dedup / batching).
let jsPostStats = null;

// Background lazy texture loader for the currently displayed scene.
let textureManager = null;
let activeLoadStats = null;
let conversionWorker = null;
let conversionWorkerSeq = 0;
let conversionWorkerActiveBytes = null;
const CONVERSION_WORKER_URL = new URL('./material-dedup.worker.js', import.meta.url);
const IMAGE_RE = /\.(png|jpg|jpeg|webp|gif|bmp|tif|tiff|exr|hdr|avif)$/i;
const WORKER_PREFETCH_IMAGE_RE = /\.(png|jpg|jpeg|webp|gif)$/i;
const MAX_WORKER_PREFETCH_IMAGES = 128;
const MAX_WORKER_PREFETCH_BYTES = 256 * 1024 * 1024;
let activeProgress = {
	visible: false,
	stage: '',
	percentage: 0,
	message: ''
};
const progressHistory = [];
const MAX_PROGRESS_HISTORY = 8192;

const textureCache = new Map();
const frameState = {
	lastFpsUpdateMs: performance.now(),
	lastFrameMs: performance.now(),
	frameCount: 0,
	movementKeys: new Set()
};

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
	const wasmHeap = loader?.native?.HEAPU8?.buffer?.byteLength ||
		loader?.native_?.HEAPU8?.buffer?.byteLength;
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

function showProgress(stage, percentage, message) {
	const panel = document.getElementById('progressPanel');
	const label = document.getElementById('progressLabel');
	const bar = document.getElementById('progressBar');
	if (!panel || !label || !bar) return;
	const pct = Math.max(0, Math.min(100,
		Number.isFinite(percentage) ? percentage : activeProgress.percentage || 0));
	activeProgress = {
		visible: true,
		stage: stage || activeProgress.stage,
		percentage: pct,
		message: message || activeProgress.message
	};
	progressHistory.push({
		timeMs: performance.now(),
		stage: activeProgress.stage,
		percentage: activeProgress.percentage,
		message: activeProgress.message
	});
	if (progressHistory.length > MAX_PROGRESS_HISTORY) {
		progressHistory.splice(0, progressHistory.length - MAX_PROGRESS_HISTORY);
	}
	panel.style.display = 'block';
	label.textContent = message || stage || 'Loading...';
	bar.style.transform = `scaleX(${pct / 100})`;
}

function hideProgress(delayMs = 1200) {
	const panel = document.getElementById('progressPanel');
	if (!panel) return;
	if (delayMs > 0) {
		setTimeout(() => {
			if (activeProgress.percentage >= 100) panel.style.display = 'none';
		}, delayMs);
	} else {
		panel.style.display = 'none';
	}
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
	activeLoadStats = stats;
	updateLoadStatsPanel(stats, 'Loading...');
	showProgress('start', 0, 'Preparing load...');
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
	el.textContent = [
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

function formatTextureStats(stats) {
	if (!stats || !Number.isFinite(stats.textureTotal) || stats.textureTotal <= 0) {
		return params.loadTextures ? 'queued 0' : 'disabled';
	}
	const countText = `${stats.textureLoaded}/${stats.textureTotal}` +
		(stats.textureFailed ? ` (${stats.textureFailed} failed)` : '');
	if (stats.textureComplete && Number.isFinite(stats.textureMs)) {
		return `${countText}, ${formatDurationMs(stats.textureMs)}`;
	}
	return `${countText}, loading`;
}

function finishLoadStats(stats) {
	if (!stats) return;
	stats.totalMs = performance.now() - stats.startTime;
	stats.memoryAfter = captureMemorySnapshot();
	updateLoadStatsPanel(stats);
}

function failLoadStats(stats) {
	if (!stats) return;
	stats.totalMs = performance.now() - stats.startTime;
	stats.memoryAfter = captureMemorySnapshot();
	updateLoadStatsPanel(stats, 'Failed');
	showProgress('failed', 100, 'Load failed');
}

function updateTextureLoadStats(info = {}) {
	const stats = activeLoadStats;
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

// Optimization + scene-transform parameters (driven by lil-gui).
const params = {
	backend: 'next',

	// Tydra render-scene optimizations.
	materialDedup: true,
	mergeMeshes: true,
	mergeMeshesBakeTransform: true,
	flattenRenderTree: false,

	// Scene transform.
	globalScale: 1.0,
	honorMetersPerUnit: true,
	upAxisConversion: true,

	// Match usdview's default: draw back faces, while preserving the USD
	// doubleSided distinction for back-face normal orientation. Enabling this
	// option switches to BackUnlessDoubleSided-style culling.
	cullBackfaces: false,

	// Apply textures, decoded lazily on the JS side (raw bytes are pulled from
	// the asset on demand by getImageCopy; the scene renders untextured first,
	// then textures stream in). On by default.
	loadTextures: true,

	// Run next-backend native conversion in a dedicated Worker so crate-reader
	// progress can repaint while synchronous WASM is busy.
	useWorker: true,

	// three.js-side post-process (applied after the scene is built, no
	// re-conversion). Independent of the native Tydra optimizations above.
	jsMaterialDedup: false,
	jsBatchByMaterial: false,

	// Sample scene generator.
	sampleGrid: 6,

	// Actions.
	resetOptimizations: () => {
		params.materialDedup = false;
		params.mergeMeshes = false;
		params.mergeMeshesBakeTransform = true;
		params.flattenRenderTree = false;
		refreshGuiControllers();
		rebuild();
	},
	enableAllOptimizations: () => {
		params.materialDedup = true;
		params.mergeMeshes = true;
		params.mergeMeshesBakeTransform = true;
		params.flattenRenderTree = true;
		refreshGuiControllers();
		rebuild();
	}
};

const guiControllers = [];

// ---------------------------------------------------------------------------
// Sample scene generation
// ---------------------------------------------------------------------------

// Build a USDA scene: a gridN x gridN grid of unit cubes. Each cube gets its
// own Material prim, but the parameters are drawn from a small palette, so many
// materials are byte-identical duplicates -> great fodder for material dedup,
// and cubes sharing a (deduped) material -> great fodder for mesh merge.
function buildSampleUSDA(gridN) {
	const palette = [
		[0.85, 0.25, 0.2],
		[0.2, 0.6, 0.85],
		[0.3, 0.8, 0.35],
		[0.9, 0.75, 0.2]
	];
	const h = 0.4; // half-extent
	const spacing = 1.4;

	const cubePoints = [
		[-h, -h, -h], [h, -h, -h], [h, h, -h], [-h, h, -h],
		[-h, -h, h], [h, -h, h], [h, h, h], [-h, h, h]
	];
	const faceCounts = [4, 4, 4, 4, 4, 4];
	const faceIndices = [
		0, 1, 2, 3,
		4, 7, 6, 5,
		0, 4, 5, 1,
		1, 5, 6, 2,
		2, 6, 7, 3,
		3, 7, 4, 0
	];

	const pointsStr = cubePoints.map((p) => `(${p[0]}, ${p[1]}, ${p[2]})`).join(', ');
	const fcStr = faceCounts.join(', ');
	const fiStr = faceIndices.join(', ');

	const lines = [];
	lines.push('#usda 1.0');
	lines.push('(');
	lines.push('    upAxis = "Y"');
	lines.push('    metersPerUnit = 1.0');
	lines.push('    doc = "Generated dedup demo scene"');
	lines.push(')');
	lines.push('');
	lines.push('def Xform "Root"');
	lines.push('{');

	const half = (gridN - 1) / 2;
	let idx = 0;
	for (let r = 0; r < gridN; r++) {
		for (let c = 0; c < gridN; c++) {
			const pal = palette[idx % palette.length];
			const tx = (c - half) * spacing;
			const tz = (r - half) * spacing;
			const matName = `Mat_${idx}`;
			const meshName = `Cube_${idx}`;

			// One Material prim per cube; only `palette.length` distinct
			// parameter sets, so material dedup collapses them.
			lines.push(`    def Material "${matName}"`);
			lines.push('    {');
			lines.push(`        token outputs:surface.connect = </Root/${matName}/Surface.outputs:surface>`);
			lines.push('        def Shader "Surface"');
			lines.push('        {');
			lines.push('            uniform token info:id = "UsdPreviewSurface"');
			lines.push(`            color3f inputs:diffuseColor = (${pal[0]}, ${pal[1]}, ${pal[2]})`);
			lines.push('            float inputs:metallic = 0.0');
			lines.push('            float inputs:roughness = 0.55');
			lines.push('            token outputs:surface');
			lines.push('        }');
			lines.push('    }');
			lines.push('');

			lines.push(`    def Mesh "${meshName}"`);
			lines.push('    {');
			lines.push(`        rel material:binding = </Root/${matName}>`);
			lines.push(`        point3f[] points = [${pointsStr}]`);
			lines.push(`        int[] faceVertexCounts = [${fcStr}]`);
			lines.push(`        int[] faceVertexIndices = [${fiStr}]`);
			lines.push(`        double3 xformOp:translate = (${tx}, 0, ${tz})`);
			lines.push('        uniform token[] xformOpOrder = ["xformOp:translate"]');
			lines.push('    }');
			lines.push('');
			idx++;
		}
	}

	lines.push('}');
	lines.push('');
	return lines.join('\n');
}

// ---------------------------------------------------------------------------
// Three.js setup
// ---------------------------------------------------------------------------

function initThree() {
	scene = new THREE.Scene();
	scene.background = new THREE.Color(0x202428);

	camera = new THREE.PerspectiveCamera(
		50, window.innerWidth / window.innerHeight, 0.01, 5000);
	camera.position.set(8, 7, 10);

	renderer = new THREE.WebGLRenderer({ antialias: true });
	renderer.setPixelRatio(window.devicePixelRatio);
	renderer.setSize(window.innerWidth, window.innerHeight);
	renderer.outputColorSpace = THREE.SRGBColorSpace;
	document.body.appendChild(renderer.domElement);
	raycaster = new THREE.Raycaster();
	pointerNdc = new THREE.Vector2();
	renderer.domElement.addEventListener('pointerdown', onCanvasPointerDown);

	controls = new OrbitControls(camera, renderer.domElement);
	controls.enableDamping = true;
	controls.target.set(0, 0, 0);

	// Lighting (no HDR needed for this structural demo).
	const hemi = new THREE.HemisphereLight(0xffffff, 0x444455, 1.1);
	scene.add(hemi);
	const dir = new THREE.DirectionalLight(0xffffff, 1.4);
	dir.position.set(5, 10, 7);
	scene.add(dir);
	const dir2 = new THREE.DirectionalLight(0x88aaff, 0.5);
	dir2.position.set(-6, 4, -5);
	scene.add(dir2);

	const grid = new THREE.GridHelper(40, 40, 0x556677, 0x334455);
	grid.position.y = -0.5;
	scene.add(grid);

	usdSceneRoot = new THREE.Group();
	scene.add(usdSceneRoot);

	window.addEventListener('resize', onResize);
	animate();
}

function onResize() {
	camera.aspect = window.innerWidth / window.innerHeight;
	camera.updateProjectionMatrix();
	renderer.setSize(window.innerWidth, window.innerHeight);
}

function animate() {
	requestAnimationFrame(animate);
	frameState.frameCount++;
	const now = performance.now();
	const deltaSeconds = Math.min(0.1, Math.max(0, (now - frameState.lastFrameMs) / 1000));
	frameState.lastFrameMs = now;
	updateCameraPivotMovement(deltaSeconds);
	if (now - frameState.lastFpsUpdateMs >= 500) {
		const fps = frameState.frameCount * 1000 / (now - frameState.lastFpsUpdateMs);
		const fpsEl = document.getElementById('fpsValue');
		if (fpsEl) fpsEl.textContent = fps.toFixed(1);
		frameState.frameCount = 0;
		frameState.lastFpsUpdateMs = now;
	}
	controls.update();
	renderer.render(scene, camera);
}

function isEditableKeyboardTarget(target) {
	if (!target) return false;
	const tag = target.tagName?.toLowerCase();
	return target.isContentEditable || tag === 'input' || tag === 'textarea' || tag === 'select';
}

function updateCameraPivotMovement(deltaSeconds) {
	if (!camera || !controls || frameState.movementKeys.size === 0 || deltaSeconds <= 0) return;
	const forwardAmount = (frameState.movementKeys.has('KeyW') ? 1 : 0) -
		(frameState.movementKeys.has('KeyS') ? 1 : 0);
	const rightAmount = (frameState.movementKeys.has('KeyD') ? 1 : 0) -
		(frameState.movementKeys.has('KeyA') ? 1 : 0);
	if (forwardAmount === 0 && rightAmount === 0) return;

	// Translate camera and OrbitControls target together. Movement stays on the
	// scene's Y-up ground plane, independent of camera pitch.
	const forward = new THREE.Vector3();
	camera.getWorldDirection(forward);
	forward.y = 0;
	if (forward.lengthSq() < 1e-8) forward.set(0, 0, -1);
	forward.normalize();
	const right = new THREE.Vector3().crossVectors(forward, camera.up).normalize();
	const motion = forward.multiplyScalar(forwardAmount).addScaledVector(right, rightAmount);
	if (motion.lengthSq() > 1) motion.normalize();
	const orbitDistance = Math.max(0.25, camera.position.distanceTo(controls.target));
	const fast = frameState.movementKeys.has('ShiftLeft') ||
		frameState.movementKeys.has('ShiftRight');
	const speed = Math.max(0.25, orbitDistance * 0.8) * (fast ? 3 : 1);
	motion.multiplyScalar(speed * deltaSeconds);
	camera.position.add(motion);
	controls.target.add(motion);
}

function setupCameraMovementKeys() {
	const movementCodes = new Set(['KeyW', 'KeyA', 'KeyS', 'KeyD', 'ShiftLeft', 'ShiftRight']);
	window.addEventListener('keydown', (event) => {
		if (isEditableKeyboardTarget(event.target) || !movementCodes.has(event.code)) return;
		frameState.movementKeys.add(event.code);
		event.preventDefault();
	});
	window.addEventListener('keyup', (event) => {
		if (!movementCodes.has(event.code)) return;
		frameState.movementKeys.delete(event.code);
	});
	window.addEventListener('blur', () => frameState.movementKeys.clear());
}

// ---------------------------------------------------------------------------
// Conversion + scene build
// ---------------------------------------------------------------------------

async function ensureLoader(_backend = 'next') {
	const moduleBackend = 'next';
	if (loader && loaderModuleBackend === moduleBackend) return loader;
	loader = new LightUSDLoader();
	await loader.init({
		useZstdCompressedWasm: false,
		useMemory64: false,
		backend: moduleBackend
	});
	loaderModuleBackend = moduleBackend;
	return loader;
}

function formatNativeProgressMessage(label, info = {}) {
	const stage = info.stage || info.phase || 'native';
	const meshTotal = Number(info.meshTotal);
	const meshCurrent = Number(info.meshCurrent);
	if (Number.isFinite(meshTotal) && meshTotal > 0) {
		return `${label}: ${stage} ${Math.min(meshCurrent, meshTotal)}/${meshTotal}`;
	}
	const materialTotal = Number(info.materialsTotal);
	const materialCurrent = Number(info.materialsCurrent);
	if (Number.isFinite(materialTotal) && materialTotal > 0) {
		return `${label}: ${stage} materials ${Math.min(materialCurrent, materialTotal)}/${materialTotal}`;
	}
	return `${label}: ${stage}`;
}

function progressCountFromInfo(info = {}) {
	const pairs = [
		['crateCurrent', 'crateTotal'],
		['archiveCurrent', 'archiveTotal'],
		['meshCurrent', 'meshTotal'],
		['materialsCurrent', 'materialsTotal'],
		['builtMeshes', 'totalMeshes'],
		['loaded', 'total'],
		['current', 'total']
	];
	for (const [currentKey, totalKey] of pairs) {
		const current = Number(info[currentKey]);
		const total = Number(info[totalKey]);
		if (Number.isFinite(current) && Number.isFinite(total) && total > 0) {
			return { current: Math.max(0, Math.min(current, total)), total };
		}
	}
	return null;
}

function formatProgressPercent(value) {
	if (!Number.isFinite(value)) return '0%';
	const clamped = Math.max(0, Math.min(100, value));
	return Math.abs(clamped - Math.round(clamped)) < 0.05
		? `${Math.round(clamped)}%`
		: `${clamped.toFixed(1)}%`;
}

function escapeRegExp(text) {
	return String(text).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function cleanCountFromMessage(message, count) {
	let text = String(message || '');
	if (!count) return text;
	const current = escapeRegExp(Math.round(count.current));
	const total = escapeRegExp(Math.round(count.total));
	text = text
		.replace(new RegExp(`\\s*\\(${current}\\s*/\\s*${total}\\)`, 'g'), '')
		.replace(new RegExp(`\\s+${current}\\s*/\\s*${total}`, 'g'), '')
		.replace(/\s+([:,.])/g, '$1')
		.replace(/\s{2,}/g, ' ')
		.trim();
	return text;
}

function formatCountedProgressMessage(message, info = {}, localPct = null) {
	const count = progressCountFromInfo(info);
	if (!count) return message;
	const countPct = (count.current / count.total) * 100;
	const pct = Number.isFinite(countPct) ? countPct : Number(localPct);
	const clean = cleanCountFromMessage(message, count);
	return `${formatProgressPercent(pct)} (${Math.round(count.current)} / ${Math.round(count.total)}): ${clean}`;
}

async function parseWithOptions(bytes, name, options, progressOptions = {}) {
	const label = progressOptions.label || 'Converting';
	const base = Number.isFinite(progressOptions.base) ? progressOptions.base : 20;
	const range = Number.isFinite(progressOptions.range) ? progressOptions.range : 30;
	const backend = 'next';
	let syntheticStep = 0;
	const report = (info = {}) => {
		let localPct = Number(info.localPercentage);
		if (!Number.isFinite(localPct)) {
			if (Number.isFinite(info.progress)) {
				localPct = Math.max(0, Math.min(100, Number(info.progress) * 100));
			} else if (Number.isFinite(info.percentage)) {
				localPct = Math.max(0, Math.min(100, Number(info.percentage)));
			} else if (Number.isFinite(info.meshCurrent) && Number.isFinite(info.meshTotal) &&
					info.meshTotal > 0) {
				localPct = Math.max(0, Math.min(100, (info.meshCurrent / info.meshTotal) * 100));
			} else {
				syntheticStep = Math.min(96, syntheticStep + 4);
				localPct = syntheticStep;
			}
		}
		const pct = base + (Math.max(0, Math.min(100, localPct)) / 100) * range;
		const message = info.message
			? (String(info.message).startsWith(label)
				? info.message
				: `${label}: ${info.message}`)
			: formatNativeProgressMessage(label, info);
		showProgress(info.stage || info.phase || 'native', pct,
			formatCountedProgressMessage(message, info, localPct));
	};

	report({ stage: 'start', localPercentage: 0, message: `${label}: starting...` });
	if (backend === 'next' && params.useWorker && typeof Worker !== 'undefined') {
		try {
			return await parseNextInWorker(bytes, name, options, progressOptions, report);
		} catch (error) {
			if (error?.workerConversionError) throw error;
			console.warn('[material-dedup] next worker unavailable; falling back to main thread', error);
			conversionWorkerActiveBytes = null;
		}
	}

	if (typeof loader.parseAsync === 'function') {
		return loader.parseAsync(bytes, name, {
			...options,
			onLightUSDDebug: report,
			onTydraProgress: report,
			onPhaseStart: (info = {}) => {
				report({
					stage: info.phase || 'native',
					localPercentage: Math.max(0, Math.min(100, (Number(info.progress) || 0) * 100)),
					message: `${label}: ${info.phase || 'native'}`
				});
			}
		});
	}

	return new Promise((resolve, reject) => {
		loader.parse(bytes, name, resolve, reject, {
			...options,
			onProgress: report,
			onLightUSDDebug: report,
			onTydraProgress: report,
			progressBase: base,
			progressRange: range
		});
	});
}

function getConversionWorker() {
	if (conversionWorker) return conversionWorker;
	conversionWorker = new Worker(CONVERSION_WORKER_URL, { type: 'module' });
	conversionWorker.addEventListener('error', (event) => {
		console.error('[material-dedup] conversion worker error:', event);
		conversionWorkerActiveBytes = null;
	});
	return conversionWorker;
}

function resetConversionWorkerCache() {
	conversionWorkerActiveBytes = null;
	if (conversionWorker) {
		try { conversionWorker.postMessage({ type: 'clear' }); } catch (_) {}
	}
}

function collectWorkerTexturePaths(meshes) {
	const paths = new Set();
	const add = (path) => {
		const key = normWorkerTexturePath(path);
		if (key) paths.add(key);
	};
	for (const mesh of meshes || []) {
		for (const path of Object.values(mesh.texturePaths || {})) add(path);
		for (const material of mesh.materials || []) {
			for (const path of Object.values(material.texturePaths || {})) add(path);
		}
	}
	return paths;
}

function isWorkerUDIMPath(path) {
	return /<UDIM>|%04d|%\(UDIM\)d/.test(String(path || ''));
}

function workerUDIMTilePath(pattern, id) {
	return String(pattern || '')
		.replace('<UDIM>', String(id))
		.replace('%04d', String(id))
		.replace('%(UDIM)d', String(id));
}

function workerTextureMimeType(path) {
	const lower = String(path || '').toLowerCase();
	if (/\.jpe?g$/.test(lower)) return 'image/jpeg';
	if (/\.png$/.test(lower)) return 'image/png';
	if (/\.webp$/.test(lower)) return 'image/webp';
	if (/\.gif$/.test(lower)) return 'image/gif';
	return '';
}

function startWorkerTexturePrefetch(sourceBytes) {
	const prefetched = new Map();
	const state = { aborted: false };
	const jobs = [];
	prefetched.abort = () => {
		state.aborted = true;
		for (const job of jobs) job.resolveImage(null);
		prefetched.clear();
	};
	const source = sourceBytes instanceof Uint8Array
		? sourceBytes
		: new Uint8Array(sourceBytes);
	if (source.length < 4 || source[0] !== 0x50 || source[1] !== 0x4b ||
			source[2] !== 0x03 || source[3] !== 0x04) {
		return prefetched;
	}

	let images;
	try {
		images = parseUSDZEntries(source).filter((entry) =>
			WORKER_PREFETCH_IMAGE_RE.test(entry.name));
	} catch (_) {
		return prefetched;
	}
	const totalBytes = images.reduce((sum, entry) => sum + entry.data.byteLength, 0);
	// Bound speculative work for texture-heavy production scenes. Those keep
	// the established demand-driven loader; medium archives can overlap image
	// decode with the otherwise independent WASM conversion worker.
	if (images.length === 0 || images.length > MAX_WORKER_PREFETCH_IMAGES ||
			totalBytes > MAX_WORKER_PREFETCH_BYTES) {
		return prefetched;
	}

	for (const entry of images) {
		const key = normWorkerTexturePath(entry.name);
		let resolveImage;
		const promise = new Promise((resolve) => { resolveImage = resolve; });
		prefetched.set(key, promise);
		jobs.push({ entry, resolveImage });
	}
	let nextJob = 0;
	const loadOne = async () => {
		while (!state.aborted && nextJob < jobs.length) {
			const { entry, resolveImage } = jobs[nextJob++];
			const mimeType = workerTextureMimeType(entry.name);
			const blob = new Blob([entry.data], mimeType ? { type: mimeType } : undefined);
			const url = URL.createObjectURL(blob);
			try {
				const image = await new THREE.ImageLoader().loadAsync(url);
				resolveImage(state.aborted ? null : { image, flipY: true });
			} catch (_) {
				// Demand loading below remains the authoritative error path.
				resolveImage(null);
			} finally {
				URL.revokeObjectURL(url);
			}
		}
	};
	const cores = Number.isFinite(navigator.hardwareConcurrency)
		? navigator.hardwareConcurrency
		: 8;
	const concurrency = Math.max(4, Math.min(16, cores || 8, jobs.length));
	void Promise.all(Array.from({ length: concurrency }, loadOne));
	return prefetched;
}

function makeWorkerArchiveEntries(sourceBytes, meshes) {
	const archiveEntries = new Map();
	if (!sourceBytes) return archiveEntries;
	const bytes = sourceBytes instanceof Uint8Array
		? sourceBytes
		: new Uint8Array(sourceBytes);
	// returnArchiveEntries means "retain embedded textures if this input is an
	// archive", not that every input is USDZ. Generated samples and direct
	// .usda/.usdc loads have no ZIP container and must not reach the EOCD parser.
	if (bytes.length < 4 || bytes[0] !== 0x50 || bytes[1] !== 0x4b ||
			bytes[2] !== 0x03 || bytes[3] !== 0x04) {
		return archiveEntries;
	}
	const referenced = collectWorkerTexturePaths(meshes);
	const isReferenced = (key) => {
		if (referenced.has(key)) return true;
		for (const ref of referenced) {
			if (key.endsWith('/' + ref) || ref.endsWith('/' + key)) return true;
			if (isWorkerUDIMPath(ref)) {
				for (let id = 1001; id <= 1100; ++id) {
					const tile = normWorkerTexturePath(workerUDIMTilePath(ref, id));
					if (key === tile || key.endsWith('/' + tile) || tile.endsWith('/' + key)) {
						return true;
					}
				}
			}
		}
		return false;
	};
	for (const entry of parseUSDZEntries(bytes)) {
		const key = normWorkerTexturePath(entry.name);
		if (!key || !IMAGE_RE.test(key)) continue;
		if (isReferenced(key)) {
			// USDZ entries are STORE-only views into sourceBytes. Retaining those
			// views avoids copying and transferring the full texture payload from
			// the conversion worker before the first frame can be built.
			archiveEntries.set(key, entry.data);
		}
	}
	return archiveEntries;
}

function makeWorkerNextScene(payload = {}, sourceBytes = null,
		includeArchiveEntries = false, prefetchedImages = null) {
	const meshes = payload.meshes || [];
	const archiveEntries = includeArchiveEntries
		? makeWorkerArchiveEntries(sourceBytes, meshes)
		: new Map();
	const materialKeys = new Set();
	const textureKeys = new Set();
	for (const mesh of meshes) {
		if (mesh.materialKey) materialKeys.add(mesh.materialKey);
		for (const path of Object.values(mesh.texturePaths || {})) {
			if (path) textureKeys.add(normWorkerTexturePath(path));
		}
		for (const material of mesh.materials || []) {
			if (material.materialKey) materialKeys.add(material.materialKey);
			for (const path of Object.values(material.texturePaths || {})) {
				if (path) textureKeys.add(normWorkerTexturePath(path));
			}
		}
	}
	return {
		__backend: 'next',
		__workerConverted: true,
		filename: payload.filename || '',
		meshes,
		stats: payload.stats || {},
		sceneMetadata: {
			upAxis: payload.sceneMetadata?.upAxis || 'Y',
			metersPerUnit: (typeof payload.sceneMetadata?.metersPerUnit === 'number' &&
				payload.sceneMetadata.metersPerUnit > 0)
				? payload.sceneMetadata.metersPerUnit
				: 1.0
		},
		archiveEntries,
		materialKeys,
		textureKeys,
		getArchiveTextureBytes(path) {
			return findNextArchiveEntry(archiveEntries, path)?.bytes || null;
		},
		getArchiveUDIMTiles(path) {
			return nextArchiveUDIMLayout(archiveEntries, path);
		},
		getPrefetchedArchiveImage(path) {
			const key = normWorkerTexturePath(path);
			if (!prefetchedImages) return null;
			if (prefetchedImages.has(key)) return prefetchedImages.get(key);
			for (const [candidate, image] of prefetchedImages) {
				if (candidate.endsWith('/' + key) || key.endsWith('/' + candidate)) {
					return image;
				}
			}
			return null;
		},
		releaseArchiveTextureBytes() {
			archiveEntries.clear();
			if (prefetchedImages) prefetchedImages.abort();
		},
		releaseBuildData() {
			this.meshes = [];
			materialKeys.clear();
			textureKeys.clear();
		},
		getSceneMetadata() {
			return this.sceneMetadata;
		},
		numMeshes() {
			return this.stats?.optimizedMeshes ?? this.meshes.length;
		},
		numMaterials() {
			return this.stats?.optimizedMaterials ?? materialKeys.size ?? this.meshes.length;
		},
		numTextures() {
			return this.stats?.optimizedTextures ?? textureKeys.size;
		},
		getStats() {
			return this.stats || {};
		},
		delete() {
			this.end();
		},
		end() {
			this.meshes = [];
			archiveEntries.clear();
			if (prefetchedImages) prefetchedImages.abort();
			materialKeys.clear();
			textureKeys.clear();
		}
	};
}

function normWorkerTexturePath(path) {
	return String(path || '').replace(/^[./]+/, '');
}

function workerConversionInput(bytes, name) {
	const source = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
	if (source.length < 4 || source[0] !== 0x50 || source[1] !== 0x4b ||
			source[2] !== 0x03 || source[3] !== 0x04) {
		return { bytes: source, name };
	}

	try {
		const entries = parseUSDZEntries(source);
		const layers = entries.filter((entry) => /\.(usd|usda|usdc)$/i.test(entry.name));
		// A self-contained USDZ does not need its image payload in the conversion
		// worker. Transfer only the root layer; the original archive remains on
		// the main thread for lazy texture loading. Multi-layer packages may use
		// references/value clips, so retain the full archive for those.
		if (layers.length === 1) {
			return { bytes: layers[0].data, name: layers[0].name };
		}
	} catch (_) {
		// Let LightUSDLoader produce the authoritative malformed-archive error.
	}
	return { bytes: source, name };
}

function parseNextInWorker(bytes, name, options, progressOptions, report) {
	const worker = getConversionWorker();
	const id = ++conversionWorkerSeq;
	const prefetchedImages = options.returnArchiveEntries
		? startWorkerTexturePrefetch(bytes)
		: null;
	const transfer = [];
	const message = {
		type: 'convert',
		id,
		name,
		options,
		progressBase: Number.isFinite(progressOptions.base) ? progressOptions.base : 0,
		progressRange: Number.isFinite(progressOptions.range) ? progressOptions.range : 100
	};
	if (conversionWorkerActiveBytes !== bytes) {
		const input = workerConversionInput(bytes, name);
		const copy = input.bytes.slice();
		message.name = input.name;
		message.bytes = copy.buffer;
		transfer.push(copy.buffer);
		conversionWorkerActiveBytes = bytes;
	}
	return new Promise((resolve, reject) => {
		const cleanup = () => {
			worker.removeEventListener('message', onMessage);
			worker.removeEventListener('error', onError);
		};
		const onError = (event) => {
			cleanup();
			if (prefetchedImages) prefetchedImages.abort();
			conversionWorkerActiveBytes = null;
			const error = new Error(event.message || 'conversion worker failed');
			error.workerStartupError = true;
			reject(error);
		};
		const onMessage = (event) => {
			const msg = event.data || {};
			if (msg.id !== id) return;
			if (msg.type === 'progress') {
				report(msg.info || {});
			} else if (msg.type === 'result') {
				cleanup();
				resolve(makeWorkerNextScene(msg.payload, bytes,
					!!options.returnArchiveEntries, prefetchedImages));
			} else if (msg.type === 'error') {
				cleanup();
				if (prefetchedImages) prefetchedImages.abort();
				const error = new Error(msg.error || 'worker conversion failed');
				error.workerConversionError = true;
				reject(error);
			}
		};
		worker.addEventListener('message', onMessage);
		worker.addEventListener('error', onError);
		worker.postMessage(message, transfer);
	});
}

function countsFromUsd(usd) {
	if (isNextScene(usd)) return nextCountsFromScene(usd);
	return {
		meshes: usd.numMeshes ? usd.numMeshes() : 0,
		materials: usd.numMaterials ? usd.numMaterials() : 0,
		textures: usd.numTextures ? usd.numTextures() : 0
	};
}

// Read scene metadata (upAxis / metersPerUnit) defensively.
function readSceneMeta(usd) {
	if (isNextScene(usd)) return readNextSceneMeta(usd);
	const md = (usd.getSceneMetadata) ? usd.getSceneMetadata() : {};
	return {
		upAxis: md.upAxis || 'Y',
		metersPerUnit: (typeof md.metersPerUnit === 'number' && md.metersPerUnit > 0)
			? md.metersPerUnit : 1.0
	};
}

// Apply global scale (optionally folding in metersPerUnit) and up-axis fix.
function applySceneTransform() {
	let effectiveScale = params.globalScale;
	if (params.honorMetersPerUnit && sceneMeta.metersPerUnit) {
		effectiveScale *= sceneMeta.metersPerUnit;
	}
	usdSceneRoot.scale.setScalar(effectiveScale);

	usdSceneRoot.rotation.x =
		(params.upAxisConversion && sceneMeta.upAxis === 'Z') ? -Math.PI / 2 : 0;
}

// three.js normally couples DoubleSide with both disabling culling and flipping
// the shading normal on back faces. Hydra treats these as separate decisions:
// usdview defaults to CullStyleNothing, but flips back-face normals only when
// the gprim is effectively double-sided. Keep that distinction in this WebGL
// demo by undefining Three's DOUBLE_SIDED shader path for effective
// single-sided meshes while culling is disabled.
function setSingleSidedBackfaceNormals(material, enabled) {
	if (!material) return;
	material.userData ||= {};
	const patchKey = '__lightusdSingleSidedBackfacePatch';
	const state = material.userData[patchKey];
	if (enabled) {
		if (state) return;
		const originalOnBeforeCompile = material.onBeforeCompile;
		const originalProgramCacheKey = material.customProgramCacheKey;
		material.userData[patchKey] = {
			onBeforeCompile: originalOnBeforeCompile,
			customProgramCacheKey: originalProgramCacheKey
		};
		material.onBeforeCompile = function(shader, activeRenderer) {
			originalOnBeforeCompile.call(this, shader, activeRenderer);
			// WebGLProgram adds #define DOUBLE_SIDED before this source. Undefine
			// it here so rasterization stays unculled but the normal is not
			// reoriented for gl_FrontFacing.
			shader.fragmentShader = '#undef DOUBLE_SIDED\n' + shader.fragmentShader;
		};
		material.customProgramCacheKey = function() {
			return `${originalProgramCacheKey.call(this)}|lightusd-single-sided-backface-normal`;
		};
	} else if (state) {
		material.onBeforeCompile = state.onBeforeCompile;
		material.customProgramCacheKey = state.customProgramCacheKey;
		delete material.userData[patchKey];
	}
	material.needsUpdate = true;
}

// Apply usdview-style CullStyleNothing (default) or
// BackUnlessDoubleSided (when the UI option is enabled). `doubleSided` here is
// the renderer-effective value: authored USD opinions win, while the next
// backend may infer true for an unauthored planar opacity billboard.
function applyCullBackfaces() {
	usdSceneRoot.traverse((o) => {
		if (!o.isMesh || !o.material) return;
		const mats = Array.isArray(o.material) ? o.material : [o.material];
		for (const m of mats) {
			if (!m) continue;
			m.userData ||= {};
			// Three renders transparent DoubleSide materials in two passes unless
			// forceSinglePass is set. CullStyleNothing is one unculled raster pass,
			// so avoid doubling transparent draw calls while this view policy is
			// active. Restore the material's original preference when culling is on.
			const singlePassKey = '__lightusdOriginalForceSinglePass';
			if (!Object.prototype.hasOwnProperty.call(m.userData, singlePassKey)) {
				m.userData[singlePassKey] = m.forceSinglePass;
			}
			m.forceSinglePass = params.cullBackfaces
				? m.userData[singlePassKey] : true;
			// BatchedMesh does not retain one top-level usdMesh record. Its
			// material was already keyed by effective sideness, so remember that
			// initial state before this view policy changes Material.side.
			if (m.userData.__lightusdEffectiveDoubleSided === undefined) {
				m.userData.__lightusdEffectiveDoubleSided =
					typeof o.userData?.usdMesh?.doubleSided === 'boolean'
						? o.userData.usdMesh.doubleSided
						: m.side === THREE.DoubleSide;
			}
			const doubleSided = m.userData.__lightusdEffectiveDoubleSided === true;
			const side = (!params.cullBackfaces || doubleSided)
				? THREE.DoubleSide : THREE.FrontSide;
			setSingleSidedBackfaceNormals(
				m, !params.cullBackfaces && !doubleSided);
			if (m && m.side !== side) {
				m.side = side;
				m.needsUpdate = true;
			}
		}
	});
}

// Fit the camera to the current USD scene bounds so loaded models (which may
// be authored at any scale) are framed regardless of metersPerUnit.
//
// Robust to outliers: scenes often contain a huge sky dome or ground plane
// whose bounds dwarf the actual content. We compute per-mesh world bounds,
// drop meshes far larger than the median, and frame the remainder.
function frameCameraToScene() {
	usdSceneRoot.updateMatrixWorld(true);

	const boxes = [];
	usdSceneRoot.traverse((o) => {
		if (!o.isMesh || !o.geometry) return;
		if (!o.geometry.boundingBox) o.geometry.computeBoundingBox();
		if (!o.geometry.boundingBox) return;
		const b = o.geometry.boundingBox.clone().applyMatrix4(o.matrixWorld);
		if (!b.isEmpty()) boxes.push(b);
	});

	let box = new THREE.Box3();
	if (boxes.length === 0) {
		box.setFromObject(usdSceneRoot);
	} else {
		// Median per-mesh diagonal; exclude meshes >8x that (sky dome/ground).
		const diags = boxes
			.map((b) => b.getSize(new THREE.Vector3()).length())
			.sort((a, b) => a - b);
		const median = diags[Math.floor(diags.length / 2)] || 0;
		const limit = median * 8;
		let used = 0;
		for (let i = 0; i < boxes.length; i++) {
			if (median > 0 && boxes[i].getSize(new THREE.Vector3()).length() > limit) {
				continue;
			}
			box.union(boxes[i]);
			used++;
		}
		if (used === 0 || box.isEmpty()) {
			box = new THREE.Box3().setFromObject(usdSceneRoot);
		}
	}
	if (box.isEmpty()) return;

	const center = new THREE.Vector3();
	const size = new THREE.Vector3();
	box.getCenter(center);
	box.getSize(size);
	const radius = Math.max(size.x, size.y, size.z) || 1;
	camera.position.set(center.x + radius * 0.8, center.y + radius * 0.6,
		center.z + radius * 1.1);
	camera.near = Math.max(radius / 1000, 1e-4);
	camera.far = radius * 20;
	camera.updateProjectionMatrix();
	controls.target.copy(center);
	controls.update();
}

function collectMaterialTextures(material, textures) {
	if (!material) return;
	for (const key of Object.keys(material)) {
		const value = material[key];
		if (value && value.isTexture) {
			textures.add(value);
		}
	}
	if (material.uniforms) {
		for (const uniform of Object.values(material.uniforms)) {
			const value = uniform?.value;
			if (value && value.isTexture) {
				textures.add(value);
			}
		}
	}
}

function disposeMaterial(material) {
	if (!material) return;
	const textures = new Set();
	collectMaterialTextures(material, textures);
	for (const texture of textures) {
		if (texture.image && typeof texture.image.close === 'function') {
			try { texture.image.close(); } catch (e) { /* ignore */ }
		}
		texture.dispose();
	}
	material.dispose();
}

function disposeTextureCache() {
	for (const value of textureCache.values()) {
		if (value && value.isTexture) {
			if (value.image && typeof value.image.close === 'function') {
				try { value.image.close(); } catch (e) { /* ignore */ }
			}
			value.dispose();
		}
	}
	textureCache.clear();
}

function disposeSceneRoot() {
	showPickedObject(null);
	for (let i = usdSceneRoot.children.length - 1; i >= 0; i--) {
		const child = usdSceneRoot.children[i];
		usdSceneRoot.remove(child);
		child.traverse((o) => {
			// BatchedMesh owns internal GPU buffers; dispose() releases them.
			if (o.isBatchedMesh && typeof o.dispose === 'function') o.dispose();
			if (o.geometry) o.geometry.dispose();
			if (o.material) {
				const mats = Array.isArray(o.material) ? o.material : [o.material];
				mats.forEach(disposeMaterial);
			}
		});
	}
}

function freeUsd(usd) {
	if (usd && typeof usd.delete === 'function') {
		try { usd.delete(); } catch (e) { /* ignore */ }
	}
}

function releaseCurrentUSDResources({ clearRawBytes = false } = {}) {
	abortTextureManager();
	disposeSceneRoot();
	disposeTextureCache();
	freeUsd(currentUsd);
	currentUsd = null;
	currentCounts = null;
	jsPostStats = null;
	if (clearRawBytes) {
		rawBytes = null;
		resetConversionWorkerCache();
	}
}

// Convert `bytes` with the given optimization options and return the native
// scene + counts. Does NOT mount it into the Three.js scene.
async function convertScene(bytes, name, opts) {
	const requestedBackend = 'next';
	const backend = requestedBackend;
	await ensureLoader(backend);
	const usd = await parseWithOptions(bytes, name, {
		backend,
		materialDedup: !!opts.materialDedup,
		mergeMeshes: !!opts.mergeMeshes,
		mergeMeshesBakeTransform: !!opts.mergeMeshesBakeTransform,
		flattenRenderTree: !!opts.flattenRenderTree,
		// Never decode textures during native conversion: that bulk-decodes the
		// whole (undeduplicated) image set into the wasm heap and OOMs texture-
		// heavy scenes on wasm32. Instead the JS layer pulls raw bytes per image
		// on demand (getImageCopy → ensureImageBufferLoaded_) and decodes them
		// lazily via TextureLoadingManager.
		loadTextureInNative: false,
		returnArchiveEntries: !!opts.loadTextures
	}, opts.progress || {});
	return { usd, counts: countsFromUsd(usd) };
}

// Apply the enabled three.js-side post-process passes to a freshly built node.
// Material dedup runs first so batchByMaterial can group by material identity.
function applyJsPostProcess(node) {
	jsPostStats = null;
	let dedup = null;
	let batch = null;
	// JS material dedup keys on material content INCLUDING texture refs. With
	// lazy textures the maps aren't loaded yet, so it would over-merge materials
	// that differ only by texture and orphan their queued texture loads. Skip it
	// whenever textures are enabled (native material dedup already covers it).
	if (params.jsMaterialDedup && !params.loadTextures) {
		dedup = dedupMaterialsByContent(node);
	}
	if (params.jsBatchByMaterial) {
		batch = batchByMaterial(node);
	}
	if (dedup || batch) {
		jsPostStats = { dedup, batch };
	}
}

// Build a Three.js node from a native scene and mount it under usdSceneRoot.
// `postProcess` enables the three.js-side optimization passes (only meaningful
// for the displayed/current scene, not the transient baseline mount).
// `lazyTextures` queues textures for background decoding via a returned
// TextureLoadingManager (the caller starts loading after the first render).
async function mountScene(usd,
		{ skipTextures = true, postProcess = false, lazyTextures = false, progressBase = 55, progressRange = 25 } = {}) {
	abortTextureManager();
	disposeSceneRoot();
	disposeTextureCache();

	const reportBuildProgress = (info = {}) => {
		const localPct = Math.max(0, Math.min(100, Number(info.percentage) || 0));
		const pct = progressBase + (localPct / 100) * progressRange;
		const message = formatCountedProgressMessage(
			info.message || 'Building scene...', info, localPct);
		showProgress('building', pct, message);
	};

	if (isNextScene(usd)) {
		const built = buildNextThreeNode(usd, {
			skipTextures,
			lazyTextures,
			onProgress: reportBuildProgress
		});
		if (postProcess) {
			applyJsPostProcess(built.node);
		} else {
			jsPostStats = null;
		}
		usdSceneRoot.add(built.node);
		applySceneTransform();
		applyCullBackfaces();
		textureManager = built.textureManager;
		updateDebugHandle();
		return;
	}

	const usdRootNode = usd.getDefaultRootNode();
	const defaultMtl = new THREE.MeshStandardMaterial({
		color: 0x888888, roughness: 0.6, metalness: 0.0
	});
	// Lazy texture mode: queue textures instead of loading them inline, so the
	// scene appears immediately and textures stream in afterwards.
	const manager = (lazyTextures && !skipTextures) ? new TextureLoadingManager() : null;

	const threeNode = await LightUSDLoaderUtils.buildThreeNode(
		usdRootNode, defaultMtl, usd, {
			overrideMaterial: false,
			preferredMaterialType: 'auto',
			textureCache,
			storeMaterialData: true,
			onProgress: reportBuildProgress,
			// When textures are disabled this is a geometry/count-focused view;
			// skipping texture loads also avoids 404 spam from unresolved URIs.
			skipTextures,
			textureLoadingManager: manager
		});

	if (postProcess) {
		applyJsPostProcess(threeNode);
	} else {
		jsPostStats = null;
	}

	usdSceneRoot.add(threeNode);
	applySceneTransform();
	applyCullBackfaces();
	textureManager = manager;
	updateDebugHandle();
}

function abortTextureManager() {
	if (textureManager) {
		try { textureManager.abort(); textureManager.reset(); } catch (e) { /* ignore */ }
		textureManager = null;
		updateDebugHandle();
	}
}

// Kick off background texture decoding/upload for the current scene (after the
// untextured scene is already visible). Loads a few at a time, yielding to the
// browser so the UI stays responsive.
function startLazyTextureLoading() {
	if (!textureManager || textureManager.total <= 0) return;
	const mgr = textureManager;
	const textureStart = performance.now();
	updateTextureLoadStats({
		loaded: mgr.loaded || 0,
		failed: mgr.failed || 0,
		total: mgr.total || 0
	});
	updateDebugHandle();
	mgr.startLoading({
		concurrency: LightUSDLoaderUtils.defaultTextureConcurrency(),
		yieldInterval: 16,
		onTextureLoaded: (material) => { material.needsUpdate = true; },
		onProgress: (info) => {
			if (textureManager !== mgr) return;
				updateTextureLoadStats(info);
				const localPct = Number.isFinite(info.percentage)
					? info.percentage
					: ((info.loaded + (info.failed || 0)) / Math.max(1, info.total || 1)) * 100;
				showProgress('textures',
					80 + Math.max(0, Math.min(100, localPct)) * 0.2,
					formatCountedProgressMessage(
						`Loading textures${info.failed ? ` (${info.failed} failed)` : ''}`,
						info,
						localPct));
				setStatus(`${describeOptions()} · textures ${info.loaded}/${info.total}` +
					(info.failed ? ` (${info.failed} failed)` : ''));
			}
		}).then(() => {
			if (textureManager !== mgr) return;
			const elapsed = performance.now() - textureStart;
		updateTextureLoadStats({
			loaded: mgr.loaded || 0,
			failed: mgr.failed || 0,
			total: mgr.total || 0,
			complete: true,
				ms: elapsed
			});
			setStatus(`${describeOptions()} · textures complete ${mgr.loaded}/${mgr.total}` +
				(mgr.failed ? ` (${mgr.failed} failed)` : '') +
				` in ${formatDurationMs(elapsed)}`);
			showProgress('complete', 100, 'Textures complete');
			hideProgress();
			updateDebugHandle();
		})
			.catch((err) => {
				if (textureManager !== mgr) return;
				console.warn('[material-dedup] texture loading:', err);
				showProgress('failed', 100, 'Texture loading failed');
			});
}

// Deterministically measure the built USD scene by walking usdSceneRoot. This
// avoids relying on renderer.info (which only reflects the last drawn frame and
// undercounts on the first post-rebuild frame). `draws` counts renderable
// submeshes — one per mesh, or one per geometry group for multi-material
// meshes — which is the draw-call count Three.js will submit for the model.
function measureSceneInfo() {
	let draws = 0;
	let triangles = 0;
	usdSceneRoot.traverse((o) => {
		if (!o.isMesh || !o.geometry) return;
		if (o.isBatchedMesh) {
			// A BatchedMesh is a single multi-draw call covering all its
			// instances; its geometry already holds the combined index buffer.
			draws += 1;
			const idx = o.geometry.getIndex();
			if (idx) triangles += Math.floor(idx.count / 3);
			return;
		}
		const groups = o.geometry.groups ? o.geometry.groups.length : 0;
		draws += groups > 0 ? groups : 1;
		const idx = o.geometry.getIndex();
		const count = idx ? idx.count
			: (o.geometry.attributes.position ? o.geometry.attributes.position.count : 0);
		triangles += Math.floor(count / 3);
	});
	return { draws, triangles };
}

// Full (re)load of a model: capture the all-optimizations-OFF baseline, then
// build the scene with the currently selected options.
async function loadModel(bytes, name, stats = null) {
	window.renderComplete = false;
	window.renderError = null;
	updateDebugHandle();
	releaseCurrentUSDResources({ clearRawBytes: true });
	baseline = null;
	rawBytes = bytes;
	currentName = name;
	document.getElementById('currentFile').textContent = name;
	setStatus('Converting baseline (no optimizations)…');
	showProgress('parsing', 20, 'Converting baseline...');

	let baseResult = null;
	try {
		const parseStart = performance.now();
		// Every next-backend result carries exact source mesh, material and
		// texture counts. Mesh merging preserves triangles, and its merged/group
		// counters give the exact draw-call reduction. Avoid a second full
		// conversion used only to populate the baseline column. This applies to
		// both main-thread and worker conversion; limiting it to workers made
		// large main-thread loads take almost exactly twice as long.
		if (params.backend === 'next' && !params.jsBatchByMaterial) {
			let convertedAt = null;
			const built = await rebuild({
				deriveNextBaseline: true,
				onConverted: () => {
					convertedAt = performance.now();
					if (stats) stats.parseMs = convertedAt - parseStart;
				}
			});
			if (!built) throw new Error('next scene conversion failed');
			if (stats) stats.processMs = performance.now() - convertedAt;
			frameCameraToScene();
			finishLoadStats(stats);
			window.renderComplete = true;
			if (!textureManager || textureManager.total <= 0) {
				showProgress('complete', 100, 'Load complete');
				hideProgress();
			}
			updateDebugHandle();
			return;
		}

		// Baseline: everything off (bake-transform is irrelevant when not merging).
		baseResult = await convertScene(bytes, name, {
			materialDedup: false,
			mergeMeshes: false,
			mergeMeshesBakeTransform: false,
			flattenRenderTree: false,
			progress: { label: 'Converting baseline', base: 18, range: 20 }
		});
		if (stats) stats.parseMs = performance.now() - parseStart;
		showProgress('building', 38, 'Building baseline scene...');
		sceneMeta = readSceneMeta(baseResult.usd);
		// Baseline is for counts only; never decode/apply its (undeduplicated)
		// texture set — it can be enormous and is immediately replaced below.
		await mountScene(baseResult.usd, { skipTextures: true, progressBase: 30, progressRange: 10 });
		const baseInfo = measureSceneInfo();
		baseline = {
			meshes: baseResult.counts.meshes,
			materials: baseResult.counts.materials,
			textures: baseResult.counts.textures,
			draws: baseInfo.draws,
			triangles: baseInfo.triangles
		};
		freeUsd(baseResult.usd);
		baseResult = null;

		// Now the current selection, then frame the camera once for this model.
		const processStart = performance.now();
		showProgress('processing', 45, 'Converting with current options...');
		await rebuild();
		if (stats) stats.processMs = performance.now() - processStart;
		frameCameraToScene();
		finishLoadStats(stats);
		window.renderComplete = true;
		if (!textureManager || textureManager.total <= 0) {
			showProgress('complete', 100, 'Load complete');
			hideProgress();
		}
		updateDebugHandle();
	} catch (err) {
		console.error(err);
		failLoadStats(stats);
		window.renderError = err && err.message ? err.message : String(err);
		updateDebugHandle();
		setStatus('Error: ' + (err && err.message ? err.message : String(err)));
		if (baseResult) {
			freeUsd(baseResult.usd);
		}
		releaseCurrentUSDResources({ clearRawBytes: true });
	}
}

// Re-convert + rebuild with the current optimization options (baseline kept).
async function rebuild({ deriveNextBaseline = false, onConverted = null } = {}) {
	if (!rawBytes) return;
	setStatus('Converting with current options…');
	showProgress('processing', 45, 'Converting with current options...');
	// Stop texture loads, dispose Three.js/GPU objects, and free the previous
	// native scene before parsing the next variant. This keeps repeated reloads
	// and optimization toggles from retaining multiple large USD scenes at once.
	releaseCurrentUSDResources();
	let result = null;
	try {
		result = await convertScene(rawBytes, currentName, {
			...params,
			progress: { label: 'Converting current scene', base: 45, range: 10 }
		});
		const usd = result.usd;
		const counts = result.counts;
		if (typeof onConverted === 'function') onConverted(result);
		sceneMeta = readSceneMeta(usd);
		currentUsd = usd;
		result = null;
		currentCounts = counts;
		updateDebugHandle();
		await mountScene(usd, {
			skipTextures: !params.loadTextures, postProcess: true,
			lazyTextures: params.loadTextures,
			progressBase: 55,
			progressRange: params.loadTextures ? 25 : 40 });
		const info = measureSceneInfo();
		if (deriveNextBaseline) {
			const native = counts.stats || {};
			const merged = Number(native.mergedMeshes) || 0;
			const groups = Number(native.mergeGroups) || 0;
			baseline = {
				meshes: Number.isFinite(native.sourceMeshes) ? native.sourceMeshes : counts.meshes,
				materials: Number.isFinite(native.sourceMaterials) ? native.sourceMaterials : counts.materials,
				textures: Number.isFinite(native.sourceTextures) ? native.sourceTextures : counts.textures,
				draws: info.draws + Math.max(0, merged - groups),
				triangles: info.triangles
			};
		}
		updateStatsUI(counts, info);
		setStatus(describeOptions());
		startLazyTextureLoading();
		// Without queued textures nothing else finishes the progress display:
		// the scene-build phase tops out below 100% (base+range mapping) and
		// hideProgress() only hides at >= 100. (With textures, the texture
		// loader emits 'Textures complete'.)
		if (!textureManager || textureManager.total <= 0) {
			showProgress('complete', 100, 'Update complete');
			hideProgress();
		}
		updateDebugHandle();
		return { counts, info };
	} catch (err) {
		if (result) {
			freeUsd(result.usd);
		}
		releaseCurrentUSDResources();
		console.error(err);
		setStatus('Error: ' + (err && err.message ? err.message : String(err)));
		showProgress('failed', 100, 'Update failed');
		updateDebugHandle();
		return null;
	}
}

// Re-apply only the three.js post-process passes on the already-converted
// scene (no native re-conversion). Used when toggling the JS-side options.
async function reapplyThreePostProcess() {
	if (!currentUsd || !currentCounts) return;
	// buildNextThreeNode releases the adapter's copied mesh payload after the
	// first mount to keep large scenes within the wasm32/browser memory budget.
	// Such an adapter cannot be mounted a second time: doing so produced an
	// empty scene when Load Textures or a JS post-process option was toggled.
	// Reconvert from the retained source bytes so texture archive entries and
	// mesh/material data match the newly selected options.
	if (isNextScene(currentUsd) &&
			(!Array.isArray(currentUsd.meshes) || currentUsd.meshes.length === 0)) {
		await rebuild();
		return;
	}
	setStatus('Applying three.js post-process…');
	showProgress('postprocess', 65, 'Applying three.js post-process...');
	try {
		await mountScene(currentUsd, {
			skipTextures: !params.loadTextures, postProcess: true,
			lazyTextures: params.loadTextures,
			progressBase: 65,
			progressRange: params.loadTextures ? 15 : 30 });
		const info = measureSceneInfo();
		updateStatsUI(currentCounts, info);
		setStatus(describeOptions());
		startLazyTextureLoading();
		if (!textureManager || textureManager.total <= 0) {
			showProgress('complete', 100, 'Update complete');
			hideProgress();
		}
	} catch (err) {
		console.error(err);
		setStatus('Error: ' + (err && err.message ? err.message : String(err)));
		showProgress('failed', 100, 'Update failed');
	}
}

function describeOptions() {
	const on = [];
	const backend = currentUsd?.__backend || 'next';
	const workerNote = currentUsd?.__workerConverted ? '/worker' : '';
	const backendNote = `backend ${backend}` +
		workerNote +
		(currentUsd?.__backendFallbackReason ? ` (fallback: ${currentUsd.__backendFallbackReason})` : '');
	if (params.materialDedup) on.push('material-dedup');
	if (params.mergeMeshes) on.push(params.mergeMeshesBakeTransform ? 'merge-meshes(bake)' : 'merge-meshes');
	if (params.flattenRenderTree) on.push('flatten-tree');
	if (params.cullBackfaces) on.push('cull-backfaces');
	const scaleNote = params.honorMetersPerUnit && sceneMeta.metersPerUnit !== 1.0
		? `, scale ×${(params.globalScale * sceneMeta.metersPerUnit).toFixed(3)} (mpu ${sceneMeta.metersPerUnit})`
		: '';

	const js = [];
	if (jsPostStats && jsPostStats.dedup) {
		const d = jsPostStats.dedup;
		js.push(`js-mat-dedup ${d.uniqueMaterials} unique / ${d.replacedBindings} rebound`);
	}
	if (jsPostStats && jsPostStats.batch) {
		const b = jsPostStats.batch;
		js.push(`batched ${b.batchedMeshCount} meshes → ${b.batchCount} BatchedMesh`);
	}
	const jsNote = js.length ? ` · three.js: ${js.join(', ')}` : '';
	const nativeStats = currentCounts?.stats;
	const nativeNote = nativeStats && backend === 'next'
		? ` · native ${nativeStats.sourceMeshes ?? '?'}→${nativeStats.optimizedMeshes ?? '?'} meshes, ` +
			`${nativeStats.sourceMaterials ?? '?'}→${nativeStats.optimizedMaterials ?? '?'} mats, ` +
			`${nativeStats.sourceTextures ?? '?'}→${nativeStats.optimizedTextures ?? '?'} tex`
		: '';

	return `${backendNote} · ` + (on.length ? on.join(', ') : 'no optimizations') +
		` · upAxis ${sceneMeta.upAxis}${scaleNote}${nativeNote}${jsNote}`;
}

// ---------------------------------------------------------------------------
// Stats UI
// ---------------------------------------------------------------------------

function setStatus(msg) {
	document.getElementById('status').textContent = msg;
}

function updateDebugHandle() {
	window.__materialDedupDebug = {
		scene,
		camera,
		controls,
		root: usdSceneRoot,
		textureManager,
		textureCache,
		currentUsd,
		currentCounts,
		nativeStats: currentCounts?.stats || null,
		loadStats: activeLoadStats,
		progress: activeProgress,
		progressHistory,
		pickedObject,
		params
	};
}

function compactValue(value, max = 180) {
	if (value === undefined || value === null || value === '') return '';
	const text = typeof value === 'string' ? value : JSON.stringify(value);
	return text.length > max ? `${text.slice(0, max - 1)}…` : text;
}

function materialTextureLines(material) {
	const lines = [];
	const mapKeys = [
		'map', 'normalMap', 'roughnessMap', 'metalnessMap', 'aoMap',
		'emissiveMap', 'alphaMap', 'specularColorMap', 'displacementMap'
	];
	for (const key of mapKeys) {
		const texture = material?.[key];
		if (texture?.isTexture) {
			const image = texture.image;
			const size = image && image.width && image.height
				? ` ${image.width}x${image.height}` : '';
			lines.push(`${key}: loaded${size}`);
		}
	}
	const nextPaths = material?.userData?.nextTexturePaths;
	if (nextPaths) {
		for (const [key, path] of Object.entries(nextPaths)) {
			if (path) lines.push(`${key} path: ${path}`);
		}
	}
	return lines;
}

function materialSummary(material, fallbackEntry = null) {
	const lines = [];
	const entryMaterial = fallbackEntry?.material || null;
	const nextMaterial = material?.userData?.nextMaterial || null;
	const id = nextMaterial?.id ?? fallbackEntry?.materialId ?? material?.userData?.materialId;
	const key = nextMaterial?.key ?? fallbackEntry?.materialKey;
	const primPath = nextMaterial?.primPath ?? entryMaterial?.primPath;
	lines.push(`material type: ${material?.type || '(none)'}`);
	if (Number.isFinite(id)) lines.push(`material id: ${id}`);
	if (key) lines.push(`material key: ${compactValue(key)}`);
	if (primPath) lines.push(`material prim: ${primPath}`);
	if (material?.color) {
		lines.push(`color: #${material.color.getHexString()}`);
	}
	if (Number.isFinite(material?.metalness)) lines.push(`metalness: ${material.metalness}`);
	if (Number.isFinite(material?.roughness)) lines.push(`roughness: ${material.roughness}`);
	if (Number.isFinite(material?.opacity)) lines.push(`opacity: ${material.opacity}`);
	const textureLines = materialTextureLines(material);
	const entryPaths = fallbackEntry?.texturePaths;
	if (entryPaths) {
		for (const [slot, path] of Object.entries(entryPaths)) {
			if (path && !textureLines.some((line) => line.includes(path))) {
				textureLines.push(`${slot} path: ${path}`);
			}
		}
	}
	if (textureLines.length) {
		lines.push('textures:');
		for (const line of textureLines) lines.push(`  ${line}`);
	}
	const rawData = material?.userData?.rawData;
	if (rawData) {
		const rawName = rawData.name || rawData.primName || rawData.absPath ||
			rawData.abs_path || rawData.displayName || rawData.display_name;
		if (rawName) lines.push(`raw material: ${rawName}`);
		if (rawData.typeString || material.userData.typeString) {
			lines.push(`raw type: ${rawData.typeString || material.userData.typeString}`);
		}
	}
	return lines;
}

function materialIndexForIntersection(object, hit) {
	const groups = object.geometry?.groups || [];
	if (!groups.length || !Number.isFinite(hit?.faceIndex)) return -1;
	const triOffset = hit.faceIndex * 3;
	for (const group of groups) {
		if (triOffset >= group.start && triOffset < group.start + group.count) {
			return group.materialIndex;
		}
	}
	return -1;
}

function showPickedObject(object, hit = null) {
	pickedObject = object || null;
	const panel = document.getElementById('pickInfo');
	const details = document.getElementById('pickDetails');
	if (!panel || !details) return;
	if (!object) {
		panel.style.display = 'none';
		details.textContent = 'Click a mesh to inspect material and texture bindings.';
		updateDebugHandle();
		return;
	}

	const usdMesh = object.userData?.usdMesh || {};
	const geometry = object.geometry;
	const position = geometry?.attributes?.position;
	const index = geometry?.getIndex?.();
	const materialIndex = materialIndexForIntersection(object, hit);
	const material = Array.isArray(object.material)
		? object.material[Math.max(0, materialIndex)]
		: object.material;
	const fallbackEntry = materialIndex >= 0 && Array.isArray(usdMesh.materials)
		? usdMesh.materials[materialIndex]
		: {
			materialId: usdMesh.materialId,
			materialKey: usdMesh.materialKey,
			material: usdMesh.material || {},
			texturePaths: usdMesh.texturePaths || {}
		};
	const triangleCount = index
		? Math.floor(index.count / 3)
		: Math.floor((position?.count || 0) / 3);
	const lines = [
		`object: ${object.name || '(unnamed)'}`,
		`mesh index: ${Number.isFinite(usdMesh.index) ? usdMesh.index : '(unknown)'}`,
		`mesh name: ${usdMesh.primName || object.name || '(unknown)'}`,
		`mesh path: ${usdMesh.primPath || '(not exposed)'}`,
		`vertices: ${position?.count ?? 0}`,
		`triangles: ${triangleCount}`,
		`groups: ${geometry?.groups?.length || 0}`,
		`picked group material: ${materialIndex >= 0 ? materialIndex : '(single/default)'}`
	];
	lines.push('');
	lines.push(...materialSummary(material, fallbackEntry));
	panel.style.display = 'block';
	details.textContent = lines.join('\n');
	updateDebugHandle();
}

function onCanvasPointerDown(event) {
	if (!raycaster || !pointerNdc || !camera || !usdSceneRoot) return;
	const rect = renderer.domElement.getBoundingClientRect();
	pointerNdc.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
	pointerNdc.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;
	raycaster.setFromCamera(pointerNdc, camera);
	const hits = raycaster.intersectObjects(usdSceneRoot.children, true);
	const hit = hits.find((h) => h.object?.isMesh);
	if (hit) {
		showPickedObject(hit.object, hit);
	} else {
		showPickedObject(null);
	}
}

function deltaText(base, cur) {
	if (base == null || cur == null) return '';
	const d = cur - base;
	if (d === 0) return '±0';
	const pct = base > 0 ? Math.round((d / base) * 100) : 0;
	return `${d > 0 ? '+' : ''}${d} (${pct > 0 ? '+' : ''}${pct}%)`;
}

function setCell(id, value) {
	const el = document.getElementById(id);
	if (el) el.textContent = (value == null) ? '–' : String(value);
}

function updateStatsUI(counts, info) {
	const b = baseline || {};
	setCell('meshes-base', b.meshes);
	setCell('materials-base', b.materials);
	setCell('textures-base', b.textures);
	setCell('draws-base', b.draws);
	setCell('tris-base', b.triangles);

	setCell('meshes-cur', counts.meshes);
	setCell('materials-cur', counts.materials);
	setCell('textures-cur', counts.textures);
	setCell('draws-cur', info.draws);
	setCell('tris-cur', info.triangles);

	setCell('meshes-delta', deltaText(b.meshes, counts.meshes));
	setCell('materials-delta', deltaText(b.materials, counts.materials));
	setCell('textures-delta', deltaText(b.textures, counts.textures));
	setCell('draws-delta', deltaText(b.draws, info.draws));
	setCell('tris-delta', deltaText(b.triangles, info.triangles));
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------

function refreshGuiControllers() {
	guiControllers.forEach((c) => c.updateDisplay());
}

function buildGui() {
	gui = new GUI({ title: 'Dedup / Optimization' });

	const backendFolder = gui.addFolder('Backend');
	guiControllers.push(backendFolder.add(params, 'backend', ['next'])
		.name('Loader Backend').onChange(rebuild));
	guiControllers.push(backendFolder.add(params, 'useWorker')
		.name('Worker Conversion').onChange(rebuild));
	backendFolder.open();

	const optFolder = gui.addFolder('Tydra Optimizations');
	guiControllers.push(optFolder.add(params, 'materialDedup').name('Material Dedup').onChange(rebuild));
	guiControllers.push(optFolder.add(params, 'mergeMeshes').name('Merge Meshes').onChange(rebuild));
	guiControllers.push(optFolder.add(params, 'mergeMeshesBakeTransform').name('Bake Transform').onChange(rebuild));
	guiControllers.push(optFolder.add(params, 'flattenRenderTree').name('Flatten Render Tree').onChange(rebuild));
	optFolder.add(params, 'enableAllOptimizations').name('▶ Enable All');
	optFolder.add(params, 'resetOptimizations').name('■ Disable All');
	optFolder.open();

	const sceneFolder = gui.addFolder('Scene');
	sceneFolder.add(params, 'globalScale', 0.001, 100, 0.001).name('Global Scale').onChange(applySceneTransform);
	sceneFolder.add(params, 'honorMetersPerUnit').name('Honor metersPerUnit').onChange(applySceneTransform);
	sceneFolder.add(params, 'upAxisConversion').name('Z-up → Y-up').onChange(applySceneTransform);
	// Textures decode lazily on the JS side, so toggling only needs a re-mount
	// (no native re-conversion).
	guiControllers.push(sceneFolder.add(params, 'loadTextures').name('Load Textures').onChange(reapplyThreePostProcess));
	sceneFolder.add(params, 'cullBackfaces').name('Cull Backfaces').onChange(() => {
		applyCullBackfaces();
		setStatus(describeOptions());
		updateDebugHandle();
	});
	sceneFolder.open();

	// three.js-side post-process: operates on the built scene graph, no native
	// re-conversion. Complementary to the Tydra optimizations above.
	const jsFolder = gui.addFolder('three.js Post-process');
	guiControllers.push(jsFolder.add(params, 'jsMaterialDedup')
		.name('JS Material Dedup').onChange(reapplyThreePostProcess));
	guiControllers.push(jsFolder.add(params, 'jsBatchByMaterial')
		.name('Batch By Material').onChange(reapplyThreePostProcess));
	jsFolder.open();

	const sampleFolder = gui.addFolder('Sample Scene');
	sampleFolder.add(params, 'sampleGrid', 2, 16, 1).name('Grid N×N');
	sampleFolder.add({ regen: loadSampleScene }, 'regen').name('Regenerate Sample');
}

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

async function loadSampleScene() {
	const usda = buildSampleUSDA(Math.max(2, Math.round(params.sampleGrid)));
	const bytes = new TextEncoder().encode(usda);
	const stats = beginLoadStats(bytes.byteLength);
	stats.fetchMs = 0;
	currentIsGeneratedSample = true;
	showProgress('fetch', 10, 'Generated sample scene');
	await loadModel(bytes, `sample ${params.sampleGrid}×${params.sampleGrid} grid`, stats);
}

// Load a USD asset served over HTTP.
async function loadModelFromURL(url, stats = null) {
	setStatus('Fetching ' + url + ' …');
	const localStats = stats || beginLoadStats();
	showProgress('fetch', 5, 'Fetching USD file...');
	const fetchStart = performance.now();
	const resp = await fetch(url);
	if (!resp.ok) throw new Error(`fetch ${url}: ${resp.status}`);
	const buf = await resp.arrayBuffer();
	localStats.fetchMs = performance.now() - fetchStart;
	localStats.fileSize = buf.byteLength;
	currentIsGeneratedSample = false;
	showProgress('fetch', 15, 'USD file fetched');
	await loadModel(new Uint8Array(buf), getDisplayNameFromURI(url), localStats);
}

function setupFileInput() {
	document.getElementById('fileInput').addEventListener('change', async (event) => {
		const file = event.target.files[0];
		if (!file) return;
		const stats = beginLoadStats();
		try {
			showProgress('fetch', 5, 'Reading USD file...');
			const readStart = performance.now();
			const buf = await file.arrayBuffer();
			stats.fetchMs = performance.now() - readStart;
			stats.fileSize = buf.byteLength;
			currentIsGeneratedSample = false;
			showProgress('fetch', 15, 'USD file read');
			await loadModel(new Uint8Array(buf), file.name, stats);
		} catch (err) {
			console.error(err);
			failLoadStats(stats);
			setStatus('Error: ' + (err && err.message ? err.message : String(err)));
		} finally {
			event.target.value = '';
		}
	});

	document.getElementById('loadSampleBtn').addEventListener('click', loadSampleScene);

	// GUI toggle button + 'H' shortcut.
	const toggleBtn = document.getElementById('gui-toggle');
	const toggleGui = () => {
		if (!gui) return;
		const el = gui.domElement;
		el.style.display = (el.style.display === 'none') ? '' : 'none';
	};
	toggleBtn.addEventListener('click', toggleGui);
	window.addEventListener('keydown', (e) => {
		if (e.key === 'h' || e.key === 'H') toggleGui();
	});
}

function setupDragAndDrop() {
	document.body.addEventListener('dragover', (event) => {
		event.preventDefault();
		document.body.classList.add('drag-over');
	});
	document.body.addEventListener('dragleave', (event) => {
		if (!event.relatedTarget || !document.body.contains(event.relatedTarget)) {
			document.body.classList.remove('drag-over');
		}
	});
	document.body.addEventListener('drop', async (event) => {
		event.preventDefault();
		document.body.classList.remove('drag-over');
		const file = event.dataTransfer?.files?.[0];
		if (!file) return;
		if (!/\.(usd|usda|usdc|usdz)$/i.test(file.name)) {
			setStatus('Please drop a USD file (.usd, .usda, .usdc, .usdz)');
			return;
		}
		const stats = beginLoadStats();
		try {
			showProgress('fetch', 5, 'Reading dropped USD file...');
			const readStart = performance.now();
			const buf = await file.arrayBuffer();
			stats.fetchMs = performance.now() - readStart;
			stats.fileSize = buf.byteLength;
			currentIsGeneratedSample = false;
			showProgress('fetch', 15, 'USD file read');
			await loadModel(new Uint8Array(buf), file.name, stats);
		} catch (err) {
			console.error(err);
			failLoadStats(stats);
			setStatus('Error: ' + (err && err.message ? err.message : String(err)));
		}
	});
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

async function main() {
	initThree();
	setupCameraMovementKeys();
	buildGui();
	setupFileInput();
	setupDragAndDrop();

	// Allow ?uri= / ?url= / ?src= / ?model= / ?usd= to load an asset directly
	// (handy for testing
	// large external models without the file picker). Optional flags configure
	// the initial state (avoids extra re-conversions when scripting tests):
	//   textures=1  dedup=0/1  merge=0/1  bake=0/1  flatten=0/1  cull=0/1  js=1
	const search = new URLSearchParams(window.location.search);
	const flag = (key, cur) => {
		const v = search.get(key);
		return v == null ? cur : (v === '1' || v === 'true');
	};
	params.loadTextures = flag('textures', params.loadTextures);
	params.useWorker = flag('worker', params.useWorker);
	const backend = search.get('backend');
	if (backend === 'legacy' || backend === 'next' || backend === 'auto') {
		params.backend = 'next';
	}
	params.materialDedup = flag('dedup', params.materialDedup);
	params.mergeMeshes = flag('merge', params.mergeMeshes);
	params.mergeMeshesBakeTransform = flag('bake', params.mergeMeshesBakeTransform);
	params.flattenRenderTree = flag('flatten', params.flattenRenderTree);
	params.cullBackfaces = flag('cull', params.cullBackfaces);
	if (search.get('js') === '1') {
		params.jsMaterialDedup = true;
		params.jsBatchByMaterial = true;
	}
	refreshGuiControllers();
	const urlParam = getStartupUSDModelURI(search);
	if (urlParam) {
		try {
			await loadModelFromURL(urlParam);
			return;
		} catch (err) {
			console.error(err);
			setStatus('Error loading URL: ' + (err && err.message ? err.message : String(err)));
		}
	}
	await loadSampleScene();
}

main();
