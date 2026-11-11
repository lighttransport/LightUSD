/**
 * anim-clips.js — Animation Clip Mixing Demo
 *
 * Per-object animation controls: click a mesh to select its animated object,
 * then solo/crossfade/blend clips independently per object.
 * Supports multi-object USD scenes (armature + camera + light + mesh).
 */

import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { LightUSDLoaderUtils, TextureLoadingManager } from 'lightusd/LightUSDLoaderUtils.js';
import {
	createConfiguredLightUSDLoader,
	getAssetUriFromURL,
	getBackendFromURL,
	loadUSDSceneFromURL,
	makeStaticParseOptions,
	mountBackendSelector,
	parseUSDSceneFromArrayBuffer
} from 'lightusd/LoaderConfigUtils.js';
import {
	buildThreeNode,
	isScene,
	readSceneMeta
} from 'lightusd/RenderSceneUtils.js';
import { getUSDSceneMetadata } from 'lightusd/USDSceneMetadata.js';
import { buildSkeletonDataFromUSD } from 'lightusd/USDSkeletonData.js';
import { extractSkinnedMeshData } from 'lightusd/USDSceneSkinningData.js';
import { applyUSDSceneSkinningPipeline } from 'lightusd/USDSceneSkinningPipeline.js';
import { buildNodeIndexMap } from 'lightusd/USDAnimationConverter.js';
import { extractUSDSceneAnimations } from 'lightusd/USDSceneAnimationPipeline.js';
import {
	crossfadeActions,
	prepareClipsForBlending,
	soloAction,
	setActionWeights
} from 'lightusd/AnimClipUtils.js';
import { raycastSkinnedMeshes, expandBoxByMeshBones } from 'lightusd/SkinnedMeshUtils.js';
import { attachSceneHelpers } from 'lightusd/SceneHelpers.js';

// Keep the animation preview at a 1:1 drawing buffer. Large animated meshes
// already consume substantial GPU buffer memory; a 2x device-pixel ratio
// quadruples framebuffer storage and can make WebGL shader/program creation
// fail on otherwise renderable scenes.
const MAX_RENDER_PIXEL_RATIO = 1.0;
const LOADER_BACKEND = getBackendFromURL();

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
	const wasmHeap = loader?.native_?.HEAPU8?.buffer?.byteLength;
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

function finishLoadStats(stats) {
	stats.totalMs = performance.now() - stats.startTime;
	stats.memoryAfter = captureMemorySnapshot();
	updateLoadStatsPanel(stats);
}

function failLoadStats(stats) {
	if (!stats) return;
	stats.totalMs = performance.now() - stats.startTime;
	stats.memoryAfter = captureMemorySnapshot();
	updateLoadStatsPanel(stats, 'Failed');
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

function deleteUSDScene(sceneToDelete) {
	if (!sceneToDelete || typeof sceneToDelete.delete !== 'function') return;
	try {
		sceneToDelete.delete();
	} catch (err) {
		console.warn('[anim-clips] Failed to delete USD scene:', err);
	}
}

function startTrackedTextureLoading(manager, stats, label = 'textureQueue', sceneToDelete = null) {
	if (!manager || !Number.isFinite(manager.total) || manager.total <= 0) {
		updateTextureStats(stats, { loaded: 0, failed: 0, total: 0, complete: true, ms: 0 });
		deleteUSDScene(sceneToDelete);
		return null;
	}
	textureLoadingManager = manager;
	const start = performance.now();
	updateTextureStats(stats, {
		loaded: manager.loaded || 0,
		failed: manager.failed || 0,
		total: manager.total || 0
	});
	return manager.startLoading({
		concurrency: 16,
		yieldInterval: 16,
		onTextureLoaded: (material) => { material.needsUpdate = true; },
		onProgress: (info) => updateTextureStats(stats, info)
	}).then((status) => {
		const elapsed = performance.now() - start;
		const loaded = status.loaded || 0;
		const failed = status.failed || 0;
		const total = status.total || 0;
		if (textureLoadingManager === manager) textureLoadingManager = null;
		if (typeof manager.reset === 'function') manager.reset();
		deleteUSDScene(sceneToDelete);
		updateTextureStats(stats, { loaded, failed, total, complete: true, ms: elapsed });
		console.log(`[anim-clips] ${label}:done ${loaded}/${total} failed=${failed} ${formatDurationMs(elapsed)}`);
		return status;
	}).catch((err) => {
		console.warn(`[anim-clips] ${label} failed:`, err);
		const elapsed = performance.now() - start;
		const status = manager.getStatus ? manager.getStatus() : null;
		const loaded = status?.loaded || manager.loaded || 0;
		const failed = status?.failed || manager.failed || 0;
		const total = status?.total || manager.total || 0;
		if (textureLoadingManager === manager) textureLoadingManager = null;
		if (typeof manager.reset === 'function') manager.reset();
		deleteUSDScene(sceneToDelete);
		updateTextureStats(stats, { loaded, failed, total, complete: true, ms: elapsed });
		return status;
	});
}

// =====================================================
// Three.js Scene Setup
// =====================================================

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x1a1a2e);

const camera = new THREE.PerspectiveCamera(50, window.innerWidth / window.innerHeight, 0.01, 100);
camera.position.set(0, 2, 5);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, MAX_RENDER_PIXEL_RATIO));
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
document.body.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.target.set(0, 1, 0);
controls.update();

// Lights
const ambientLight = new THREE.AmbientLight(0x404060, 1.5);
scene.add(ambientLight);

const dirLight = new THREE.DirectionalLight(0xffffff, 2.0);
dirLight.position.set(3, 5, 3);
dirLight.castShadow = true;
dirLight.shadow.mapSize.set(1024, 1024);
dirLight.shadow.camera.near = 0.1;
dirLight.shadow.camera.far = 20;
dirLight.shadow.camera.left = -5;
dirLight.shadow.camera.right = 5;
dirLight.shadow.camera.top = 5;
dirLight.shadow.camera.bottom = -5;
dirLight.shadow.bias = -0.001;
scene.add(dirLight);

// Ground helper (plane + grid toggle together). polygonOffset pushes the
// plane behind co-planar grid lines.
const groundHelper = new THREE.Group();
groundHelper.name = 'GroundHelper';
scene.add(groundHelper);

const groundGeo = new THREE.PlaneGeometry(20, 20);
const groundMat = new THREE.MeshStandardMaterial({
	color: 0x222233, roughness: 0.9,
	polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1,
});
const ground = new THREE.Mesh(groundGeo, groundMat);
ground.rotation.x = -Math.PI / 2;
ground.receiveShadow = true;
groundHelper.add(ground);

// Grid — normal depth test so scene meshes correctly occlude it
const gridHelper = new THREE.GridHelper(10, 20, 0x444466, 0x333355);
groundHelper.add(gridHelper);

// The core backend can preview very large static/transform-animated scenes.
// Avoid allocating and rendering a second full shadow pass for those meshes;
// the legacy demo keeps its original shadows for the small skinned fixture.
if (LOADER_BACKEND === 'core') {
	renderer.shadowMap.enabled = false;
	dirLight.castShadow = false;
	ground.receiveShadow = false;
}

// =====================================================
// State
// =====================================================

let mixer = null;
const allClips = [];    // THREE.AnimationClip[]
const allActions = [];  // THREE.AnimationAction[]
let activeIndex = 0;
let blendMode = 'solo'; // 'solo' | 'blend'
let crossfadeDuration = 0.5;
let isPlaying = true;
let playbackSpeed = 1.0;
let sceneTimeCodesPerSecond = 24; // USD timeCodesPerSecond (tracks store frame numbers)
let showSkeletonVisualization = true;
let showGroundGrid = true;

const characterGroup = new THREE.Group();
characterGroup.name = 'characterGroup';
scene.add(characterGroup);

let allSceneMeshes = [];
let skeletons = new Map();
let skeletonHelpers = [];
let textureLoadingManager = null;

// Animation info from USD (metadata per clip)
let animationInfos = [];

// Per-object animation state
const objectAnimMap = new Map(); // objectId -> ObjectAnimState
let selectedObjectId = null;
let filterMode = 'all'; // 'all' | 'selected'

// Raycasting
const raycaster = new THREE.Raycaster();
const mouse = new THREE.Vector2();
let _mouseDownPos = { x: 0, y: 0 };
const frameState = {
	lastFpsUpdateMs: performance.now(),
	frameCount: 0,
	helperUpdateFrame: 0
};

// Selection highlight
let selectionHelper = null;

let lastFrameTimeMs = performance.now();

function hasActiveClipActions() {
	if (!isPlaying || allActions.length === 0) return false;
	for (const action of allActions) {
		if (action && action.enabled && !action.paused && action.getEffectiveWeight() > 0) {
			return true;
		}
	}
	return false;
}

// =====================================================
// Process USD Scene
// =====================================================

async function processUSDScene(usdScene, filename, stats = null) {
	window.renderComplete = false;
	const processStart = performance.now();
	const displayName = filename.split('/').pop();
	const currentFileEl = document.getElementById('currentFile');
	if (currentFileEl) currentFileEl.textContent = displayName;

	// Dispose previous state
	if (mixer) {
		mixer.stopAllAction();
		mixer.uncacheRoot(mixer.getRoot());
		mixer = null;
	}
	allClips.length = 0;
	allActions.length = 0;
	activeIndex = 0;
	animationInfos = [];
	objectAnimMap.clear();
	selectedObjectId = null;
	filterMode = 'all';
	removeSelectionHighlight();

	if (textureLoadingManager) {
		try {
			textureLoadingManager.abort();
			if (typeof textureLoadingManager.reset === 'function') {
				textureLoadingManager.reset();
			}
		} catch (_) {
			// Ignore stale texture queue cleanup errors.
		}
		textureLoadingManager = null;
	}

	for (const mesh of allSceneMeshes) {
		if (mesh.geometry) mesh.geometry.dispose();
		if (mesh.material) {
			if (Array.isArray(mesh.material)) mesh.material.forEach(m => m.dispose());
			else mesh.material.dispose();
		}
		if (mesh.customDepthMaterial) mesh.customDepthMaterial.dispose();
	}
	for (const [, skel] of skeletons) {
		if (skel && skel.boneTexture) skel.boneTexture.dispose();
	}
	for (const helper of skeletonHelpers) {
		scene.remove(helper);
		if (helper.geometry) helper.geometry.dispose();
		if (helper.material) {
			if (Array.isArray(helper.material)) helper.material.forEach(m => m.dispose());
			else helper.material.dispose();
		}
	}
	allSceneMeshes = [];
	skeletons.clear();
	skeletonHelpers = [];

	// Reset characterGroup
	characterGroup.position.set(0, 0, 0);
	characterGroup.quaternion.identity();
	characterGroup.scale.set(1, 1, 1);
	while (characterGroup.children.length > 0) {
		characterGroup.remove(characterGroup.children[0]);
	}

	if (isScene(usdScene)) {
		const meta = readSceneMeta(usdScene);
		const rawMeta = usdScene.getSceneMetadata ? usdScene.getSceneMetadata() : {};
		sceneTimeCodesPerSecond = rawMeta.timeCodesPerSecond || rawMeta.framesPerSecond || 24;
		const {
			hasSkinnedMeshData,
			allSkinnedMeshUSDData,
			skinnedMeshDataByName
		} = extractSkinnedMeshData(usdScene, { logger: console });
		const skeletonBuild = buildSkeletonDataFromUSD(usdScene, {
			logger: console,
			hasSkinnedMeshData
		});
		const built = buildThreeNode(usdScene, {
			skipTextures: false,
			lazyTextures: true,
			// Animations/skinning are extracted from the adapter AFTER the
			// build; the default releaseBuildData would wipe them.
			releaseBuildData: false
		});
		if (built.textureManager) {
			startTrackedTextureLoading(built.textureManager, stats, 'coreTextureQueue');
		}
		// Core animation target_node is a RenderScene node-table index; use the
		// table map from buildThreeNode, not a DFS walk of the built tree.
		const nodeIndexMap = built.nodeIndexMap || buildNodeIndexMap(built.node);
		if (String(meta.upAxis || 'Y').toUpperCase() === 'Z') {
			characterGroup.rotation.x = -Math.PI / 2;
		}
		const skinningResult = applyUSDSceneSkinningPipeline({
			threeNode: built.node,
			characterGroup,
			helperScene: scene,
			skeletonDataArray: skeletonBuild.skeletonDataArray,
			allSkinnedMeshUSDData,
			skinnedMeshDataByName,
			usdScene,
			showMesh: true,
			showSkeleton: showSkeletonVisualization,
			// Large static core scenes can exhaust WebGL resources compiling a
			// second depth program for every mesh. Animation preview does not
			// require those shadow passes.
			enableShadows: false,
			useWASMBoneTexture: false,
			logger: console
		});
		skeletons = skinningResult.skeletons;
		skeletonHelpers = skinningResult.skeletonHelpers;
		allSceneMeshes = skinningResult.allSceneMeshes;
		try {
			const animData = extractUSDSceneAnimations(usdScene, {
				boneMaps: skeletonBuild.boneMaps,
				nodeIndexMap,
				threeRoot: built.node,
				timeCodesPerSecond: sceneTimeCodesPerSecond,
				logger: console
			});
			animationInfos = animData.animationInfos || [];
			allClips.push(...animData.usdAnimations, ...animData.usdNodeAnimations);
			console.log(`[anim-clips] core backend loaded ${allClips.length} animation clip(s)`);
		} catch (err) {
			console.error('[anim-clips] core animation extraction failed:', err);
		}
		if (allClips.length > 0) {
			mixer = new THREE.AnimationMixer(characterGroup);
			mixer.timeScale = sceneTimeCodesPerSecond * playbackSpeed;
			allActions.push(...prepareClipsForBlending(mixer, allClips));
			buildObjectAnimMap();
			for (const [, state] of objectAnimMap) {
				state.activeClipIndex = 0;
				state.isPlaying = true;
				for (let i = 0; i < state.actions.length; i++) {
					state.actions[i].enabled = true;
					state.actions[i].setEffectiveWeight(i === 0 ? 1 : 0);
					if (i === 0) state.actions[i].play();
				}
			}
			activeIndex = 0;
			blendMode = 'solo';
			isPlaying = true;
		}
		renderClipPanel();
		fitCamera();
		if (typeof usdScene.releaseBuildData === 'function') {
			usdScene.releaseBuildData();
		}
		if (stats) {
			stats.processMs = performance.now() - processStart;
			finishLoadStats(stats);
		}
		window.renderComplete = true;
		return;
	}

	// Get metadata
	const {
		fileUpAxis,
		timeCodesPerSecond
	} = getUSDSceneMetadata(usdScene);

	sceneTimeCodesPerSecond = timeCodesPerSecond || 24;

	const {
		hasSkinnedMeshData,
		allSkinnedMeshUSDData,
		skinnedMeshDataByName
	} = extractSkinnedMeshData(usdScene, { logger: console });

	// Build skeleton data
	const skeletonBuild = buildSkeletonDataFromUSD(usdScene, {
		logger: console,
		hasSkinnedMeshData
	});
	const skeletonDataArray = skeletonBuild.skeletonDataArray;
	const boneMaps = skeletonBuild.boneMaps;

	// Build Three.js scene graph
	const usdRootNode = usdScene.getDefaultRootNode();
	const defaultMtl = LightUSDLoaderUtils.createDefaultMaterial();
	const buildOptions = {
		overrideMaterial: false,
		textureLoadingManager: new TextureLoadingManager()
	};
	const threeNode = await LightUSDLoaderUtils.buildThreeNode(
		usdRootNode, defaultMtl, usdScene, buildOptions
	);
	// Build node index map before skinning pipeline
	const nodeIndexMap = buildNodeIndexMap(threeNode);

	// Apply Z-up to Y-up conversion
	if (fileUpAxis.toUpperCase() === 'Z') {
		characterGroup.rotation.x = -Math.PI / 2;
	}

	// Apply skinning pipeline
	const skinningResult = applyUSDSceneSkinningPipeline({
		threeNode,
		characterGroup,
		helperScene: scene,
		skeletonDataArray,
		allSkinnedMeshUSDData,
		skinnedMeshDataByName,
		usdScene,
		showMesh: true,
		showSkeleton: true,
		useWASMBoneTexture: false,
		logger: console
	});

	skeletons = skinningResult.skeletons;
	skeletonHelpers = skinningResult.skeletonHelpers;
	allSceneMeshes = skinningResult.allSceneMeshes;

	// Attach Blender-style visual helpers for lights and cameras
	const box = new THREE.Box3().setFromObject(characterGroup);
	const maxDim = box.getSize(new THREE.Vector3()).length();
	const helperScale = Math.max(maxDim * 0.15, 0.2);
	attachSceneHelpers(threeNode, usdScene, { scale: helperScale });

	// Extract animations
	try {
		const animData = extractUSDSceneAnimations(usdScene, {
			boneMaps,
			nodeIndexMap,
			threeRoot: threeNode,
			timeCodesPerSecond,
			logger: console
		});

		animationInfos = animData.animationInfos || [];

		// Combine skeletal + node clips
		const combinedClips = [
			...animData.usdAnimations,
			...animData.usdNodeAnimations
		];

		allClips.push(...combinedClips);
		console.log(`Loaded ${allClips.length} animation clip(s)`);

		allClips.forEach((clip, i) => {
			const info = animationInfos[i];
			const srcType = info?.sourceType || 'Unknown';
			const joints = info?.numAnimatedJoints ?? 0;
			console.log(`  Clip ${i}: "${clip.name}" — ${clip.duration.toFixed(0)} frames (${(clip.duration / sceneTimeCodesPerSecond).toFixed(2)}s), ${clip.tracks.length} tracks, source: ${srcType}, joints: ${joints}`);
		});
	} catch (err) {
		console.error('Animation extraction failed:', err);
	}

	// Keep the native scene alive through animation extraction. An empty
	// texture queue completes synchronously and owns deletion of sceneToDelete,
	// so starting it before extraction used to delete the scene too early.
	if (buildOptions.textureLoadingManager &&
		buildOptions.textureLoadingManager.total > 0) {
		startTrackedTextureLoading(
			buildOptions.textureLoadingManager,
			stats,
			'textureQueue',
			usdScene
		);
	} else {
		deleteUSDScene(usdScene);
	}

	// Create mixer and prepare actions
	if (allClips.length > 0) {
		mixer = new THREE.AnimationMixer(characterGroup);
		// Track times are in USD timecodes (frame numbers), not seconds.
		// Set mixer timeScale to convert frame-time deltas to real-time.
		mixer.timeScale = sceneTimeCodesPerSecond * playbackSpeed;
		allActions.push(...prepareClipsForBlending(mixer, allClips));

		// Build per-object clip mapping
		buildObjectAnimMap();

		// Solo the first clip in each object by default
		for (const [, state] of objectAnimMap) {
			state.activeClipIndex = 0;
			state.isPlaying = true;
			// Solo the first clip within this object
			for (let i = 0; i < state.actions.length; i++) {
				state.actions[i].enabled = true;
				state.actions[i].setEffectiveWeight(i === 0 ? 1 : 0);
				if (i === 0) state.actions[i].play();
			}
		}
		activeIndex = 0;
		blendMode = 'solo';
		isPlaying = true;
	}

	// Update UI
	renderClipPanel();

	// Fit camera to scene
	fitCamera();

	if (stats) {
		stats.processMs = performance.now() - processStart;
		finishLoadStats(stats);
	}
	window.renderComplete = true;
}

// =====================================================
// Camera Fitting
// =====================================================

function fitCamera() {
	characterGroup.updateMatrixWorld(true);
	const box = new THREE.Box3().setFromObject(characterGroup);
	if (box.isEmpty()) return false;

	const center = box.getCenter(new THREE.Vector3());
	const size = box.getSize(new THREE.Vector3());
	const radius = Math.max(size.length() * 0.5, 0.01);
	const verticalFov = THREE.MathUtils.degToRad(camera.fov);
	const horizontalFov = 2 * Math.atan(Math.tan(verticalFov * 0.5) * camera.aspect);
	const limitingHalfFov = Math.max(
		Math.min(verticalFov, horizontalFov) * 0.5,
		THREE.MathUtils.degToRad(5)
	);
	const dist = radius / Math.sin(limitingHalfFov) * 1.15;
	const direction = camera.position.clone().sub(controls.target);
	if (direction.lengthSq() < 1.0e-8) direction.set(0.5, 0.35, 1);
	direction.normalize();

	controls.target.copy(center);
	camera.position.copy(center).addScaledVector(direction, dist);
	camera.near = Math.max(radius * 0.001, 0.01);
	camera.far = Math.max(dist + radius * 4, camera.near * 1000);
	camera.updateProjectionMatrix();
	controls.minDistance = Math.max(radius * 0.01, camera.near * 2);
	controls.maxDistance = camera.far * 0.5;
	controls.update();
	return true;
}

window._fitToScene = fitCamera;

// =====================================================
// Object-Clip Mapping
// =====================================================

function buildObjectAnimMap() {
	objectAnimMap.clear();

	if (allClips.length === 0 || allActions.length === 0) return;

	// 1. Build boneNameToSkelId map
	const boneNameToSkelId = new Map();
	for (const [skelId, skeleton] of skeletons) {
		for (const bone of skeleton.bones) {
			boneNameToSkelId.set(bone.name, skelId);
		}
	}

	// 2. Build skelIdToMeshes map
	const skelIdToMeshes = new Map();
	for (const mesh of allSceneMeshes) {
		if (mesh.isSkinnedMesh && mesh.skeleton) {
			// Find which skeleton this mesh is bound to
			for (const [skelId, skeleton] of skeletons) {
				if (mesh.skeleton === skeleton) {
					if (!skelIdToMeshes.has(skelId)) skelIdToMeshes.set(skelId, []);
					skelIdToMeshes.get(skelId).push(mesh);
					break;
				}
			}
		}
	}

	// 3. Build uuidToObject map by traversing characterGroup
	const uuidToObject = new Map();
	const nameToObject = new Map();
	characterGroup.traverse((obj) => {
		uuidToObject.set(obj.uuid, obj);
		if (obj.name) nameToObject.set(obj.name, obj);
	});

	// 4. For each clip, determine its owner object
	for (let clipIdx = 0; clipIdx < allClips.length; clipIdx++) {
		const clip = allClips[clipIdx];
		const action = allActions[clipIdx];
		let ownerId = null;
		let ownerType = 'node';
		let ownerMeshes = [];
		let ownerDisplayName = clip.name || 'Unnamed';

		// Parse track target names to find the owner
		for (const track of clip.tracks) {
			const dotIdx = track.name.indexOf('.');
			const targetName = dotIdx >= 0 ? track.name.substring(0, dotIdx) : track.name;

			// Check if target is a bone
			if (boneNameToSkelId.has(targetName)) {
				const skelId = boneNameToSkelId.get(targetName);
				ownerId = 'skeleton_' + skelId;
				ownerType = 'skeleton';
				ownerMeshes = skelIdToMeshes.get(skelId) || [];
				// Use first mesh name or skeleton id for display
				if (ownerMeshes.length > 0) {
					ownerDisplayName = ownerMeshes[0].name || ('Skeleton ' + skelId);
				} else {
					ownerDisplayName = 'Skeleton ' + skelId;
				}
				break;
			}

			// Check if target is a UUID (Three.js generates UUID-based track names)
			if (uuidToObject.has(targetName)) {
				const obj = uuidToObject.get(targetName);
				ownerId = 'node_' + obj.uuid;
				ownerType = 'node';
				ownerDisplayName = obj.name || 'Object';
				// Collect meshes from this object and its descendants
				ownerMeshes = [];
				if (obj.isMesh) {
					ownerMeshes.push(obj);
				} else {
					obj.traverse((child) => {
						if (child.isMesh) ownerMeshes.push(child);
					});
				}
				break;
			}

			// Check if target is a node name
			if (nameToObject.has(targetName)) {
				const obj = nameToObject.get(targetName);
				ownerId = 'node_' + obj.uuid;
				ownerType = 'node';
				ownerDisplayName = obj.name || 'Object';
				ownerMeshes = [];
				if (obj.isMesh) {
					ownerMeshes.push(obj);
				} else {
					obj.traverse((child) => {
						if (child.isMesh) ownerMeshes.push(child);
					});
				}
				break;
			}
		}

		// Fallback: unassigned clips go to a catch-all group
		if (!ownerId) {
			ownerId = '_unassigned';
			ownerType = 'node';
			ownerDisplayName = 'Other';
			ownerMeshes = [];
		}

		// Create or update object state
		if (!objectAnimMap.has(ownerId)) {
			objectAnimMap.set(ownerId, {
				objectId: ownerId,
				displayName: ownerDisplayName,
				type: ownerType,
				meshes: ownerMeshes,
				clips: [],
				clipIndices: [],
				actions: [],
				activeClipIndex: 0,
				isPlaying: true,
				blendMode: 'solo',
			});
		}

		const state = objectAnimMap.get(ownerId);
		state.clips.push(clip);
		state.clipIndices.push(clipIdx);
		state.actions.push(action);
	}

	console.log(`Built object-anim map: ${objectAnimMap.size} object(s)`);
	for (const [id, state] of objectAnimMap) {
		console.log(`  ${id}: "${state.displayName}" — ${state.clips.length} clip(s), ${state.meshes.length} mesh(es)`);
	}
}

// =====================================================
// Mouse Picking
// =====================================================

function onMouseDown(event) {
	_mouseDownPos.x = event.clientX;
	_mouseDownPos.y = event.clientY;
}

function onClick(event) {
	// Ignore clicks on UI panels
	if (event.target && event.target.closest && event.target.closest('#clip-panel, #info')) return;

	// Drag threshold — skip if user was orbiting
	const dx = event.clientX - _mouseDownPos.x;
	const dy = event.clientY - _mouseDownPos.y;
	if (dx * dx + dy * dy > 9) return;

	// Compute NDC
	mouse.x = (event.clientX / window.innerWidth) * 2 - 1;
	mouse.y = -(event.clientY / window.innerHeight) * 2 + 1;
	raycaster.setFromCamera(mouse, camera);

	const hits = raycastSkinnedMeshes(raycaster, allSceneMeshes);
	if (hits.length > 0) {
		selectObjectByMesh(hits[0].object);
	} else {
		deselectObject();
	}
}

function selectObjectByMesh(mesh) {
	for (const [objectId, state] of objectAnimMap) {
		if (state.meshes.includes(mesh)) {
			selectObject(objectId);
			return;
		}
	}
	// Mesh not found in any object group — deselect
	deselectObject();
}

function selectObject(objectId) {
	if (!objectAnimMap.has(objectId)) return;
	selectedObjectId = objectId;
	renderClipPanel();
}

function deselectObject() {
	selectedObjectId = null;
	removeSelectionHighlight();
	renderClipPanel();
}

renderer.domElement.addEventListener('mousedown', onMouseDown);
renderer.domElement.addEventListener('click', onClick);

// =====================================================
// Selection Highlight
// =====================================================

const _highlightBox = new THREE.Box3();

function updateSelectionHighlight() {
	if (!selectedObjectId || !objectAnimMap.has(selectedObjectId)) {
		removeSelectionHighlight();
		return;
	}

	const state = objectAnimMap.get(selectedObjectId);
	if (state.meshes.length === 0) {
		removeSelectionHighlight();
		return;
	}

	_highlightBox.makeEmpty();
	for (const mesh of state.meshes) {
		if (!mesh.visible) continue;
		if (mesh.isSkinnedMesh && mesh.skeleton) {
			expandBoxByMeshBones(mesh, _highlightBox);
		} else {
			_highlightBox.expandByObject(mesh);
		}
	}

	if (_highlightBox.isEmpty()) {
		removeSelectionHighlight();
		return;
	}

	// Add 10% padding
	const size = _highlightBox.getSize(new THREE.Vector3());
	const pad = size.multiplyScalar(0.1);
	_highlightBox.min.sub(pad);
	_highlightBox.max.add(pad);

	if (!selectionHelper) {
		selectionHelper = new THREE.Box3Helper(new THREE.Box3(), 0x4CAF50);
		scene.add(selectionHelper);
	}
	selectionHelper.box.copy(_highlightBox);
}

function removeSelectionHighlight() {
	if (selectionHelper) {
		scene.remove(selectionHelper);
		if (selectionHelper.geometry) selectionHelper.geometry.dispose();
		if (selectionHelper.material) selectionHelper.material.dispose();
		selectionHelper = null;
	}
}

// =====================================================
// UI Rendering
// =====================================================

function renderClipPanel() {
	const content = document.getElementById('clip-content');
	if (!content) return;

	if (allClips.length === 0) {
		content.innerHTML = '<p style="color: #888;">No clips loaded</p>';
		return;
	}

	let html = '';

	// Filter bar (only show if there are multiple objects)
	if (objectAnimMap.size > 1) {
		html += '<div class="filter-bar">';
		html += `<button class="${filterMode === 'all' ? 'active' : ''}" onclick="window._setFilter('all')">All</button>`;
		html += `<button class="${filterMode === 'selected' ? 'active' : ''}" onclick="window._setFilter('selected')">Selected Only</button>`;
		html += '</div>';
	}

	// Crossfade duration
	html += '<div class="crossfade-dur">';
	html += `Xfade: <input type="range" min="0.1" max="2.0" step="0.1" value="${crossfadeDuration}" id="crossfade-slider"> `;
	html += `<span id="crossfade-val">${crossfadeDuration.toFixed(1)}s</span>`;
	html += '</div>';

	// Render each object group
	for (const [objectId, state] of objectAnimMap) {
		// Apply filter
		if (filterMode === 'selected' && selectedObjectId && objectId !== selectedObjectId) continue;

		const isSelected = (objectId === selectedObjectId);
		html += `<div class="object-group${isSelected ? ' selected' : ''}">`;

		// Object header
		html += `<div class="object-header" onclick="window._selectObject('${objectId}')">`;
		html += `<span><span class="obj-name">${state.displayName}</span>`;
		html += `<span class="obj-type">${state.type === 'skeleton' ? 'Skel' : 'Node'}</span></span>`;
		html += `<span style="font-size:10px;color:#888;">${state.clips.length} clip${state.clips.length !== 1 ? 's' : ''}</span>`;
		html += '</div>';

		// Per-object controls
		html += '<div class="object-controls">';
		html += `<button onclick="window._togglePlayObject('${objectId}')">${state.isPlaying ? 'Pause' : 'Play'}</button>`;
		if (state.clips.length > 1) {
			html += `<button class="${state.blendMode !== 'blend' ? 'active' : ''}" onclick="window._setObjectMode('${objectId}','solo')">Solo</button>`;
			html += `<button class="${state.blendMode === 'blend' ? 'active' : ''}" onclick="window._setObjectMode('${objectId}','blend')">Blend</button>`;
		}
		html += '</div>';

		// Clips within this object
		html += '<div class="object-clips">';
		state.clips.forEach((clip, localIdx) => {
			const globalIdx = state.clipIndices[localIdx];
			const info = animationInfos[globalIdx] || {};
			const srcType = info.sourceType || '';
			const isActiveClip = (localIdx === state.activeClipIndex);

			html += `<div class="clip-item${isActiveClip ? ' active' : ''}">`;
			html += `<div class="clip-name">${clip.name || 'Unnamed'}</div>`;
			html += `<div class="clip-meta">${(clip.duration / sceneTimeCodesPerSecond).toFixed(2)}s | ${clip.tracks.length} tracks`;
			if (srcType) html += ` | ${srcType}`;
			html += '</div>';
			html += '<div class="clip-buttons">';
			html += `<button class="btn-solo" onclick="window._soloClipForObject('${objectId}',${localIdx})">Solo</button>`;
			if (state.clips.length > 1) {
				html += `<button class="btn-crossfade" onclick="window._crossfadeForObject('${objectId}',${localIdx})">Xfade</button>`;
			}
			html += '</div>';
			html += '</div>';
		});

		// Blend weights (visible in blend mode for this object)
		if (state.blendMode === 'blend') {
			state.clips.forEach((clip, localIdx) => {
				const action = state.actions[localIdx];
				const weight = action ? action.getEffectiveWeight() : 0;
				const name = (clip.name || 'Unnamed').substring(0, 12);
				html += '<div class="blend-row">';
				html += `<label title="${clip.name}">${name}</label>`;
				html += `<input type="range" min="0" max="1" step="0.05" value="${weight.toFixed(2)}" oninput="window._setObjectWeight('${objectId}',${localIdx},this.value)">`;
				html += `<span class="weight-val">${weight.toFixed(2)}</span>`;
				html += '</div>';
			});
		}

		html += '</div>'; // .object-clips
		html += '</div>'; // .object-group
	}

	// Global controls
	html += '<div class="global-controls">';
	html += `<button onclick="window._playAll()">Play All</button>`;
	html += `<button onclick="window._pauseAll()">Pause All</button>`;
	html += `<span style="font-size:10px;color:#aaa;">Speed:</span>`;
	html += `<input type="range" min="0.1" max="3.0" step="0.1" value="${playbackSpeed}" id="speed-slider" oninput="window._setSpeed(this.value)">`;
	html += `<span style="font-size:10px;color:#aaa;font-family:monospace;" id="speed-val">${playbackSpeed.toFixed(1)}x</span>`;
	html += '</div>';
	html += '<label class="vis-toggle">';
	html += `<input type="checkbox" ${showSkeletonVisualization ? 'checked' : ''} onchange="window._toggleSkeletonVisualization(this.checked)">`;
	html += '<span>Show Skeleton</span>';
	html += '</label>';
	html += '<label class="vis-toggle">';
	html += `<input type="checkbox" ${showGroundGrid ? 'checked' : ''} onchange="window._toggleGroundGrid(this.checked)">`;
	html += '<span>Show Ground Grid</span>';
	html += '</label>';

	content.innerHTML = html;

	// Attach crossfade slider listener
	const cfSlider = document.getElementById('crossfade-slider');
	if (cfSlider) {
		cfSlider.addEventListener('input', (e) => {
			crossfadeDuration = parseFloat(e.target.value);
			const valEl = document.getElementById('crossfade-val');
			if (valEl) valEl.textContent = crossfadeDuration.toFixed(1) + 's';
		});
	}
}

// =====================================================
// UI Handlers — Per-Object Controls
// =====================================================

window._setFilter = function(mode) {
	filterMode = mode;
	renderClipPanel();
};

window._selectObject = function(objectId) {
	if (selectedObjectId === objectId) {
		deselectObject();
	} else {
		selectObject(objectId);
	}
};

window._togglePlayObject = function(objectId) {
	const state = objectAnimMap.get(objectId);
	if (!state) return;
	state.isPlaying = !state.isPlaying;
	for (const action of state.actions) {
		action.paused = !state.isPlaying;
	}
	renderClipPanel();
};

window._setObjectMode = function(objectId, mode) {
	const state = objectAnimMap.get(objectId);
	if (!state) return;
	state.blendMode = mode;
	if (mode === 'solo') {
		// Solo the active clip
		for (let i = 0; i < state.actions.length; i++) {
			state.actions[i].enabled = true;
			state.actions[i].setEffectiveWeight(i === state.activeClipIndex ? 1 : 0);
			if (i === state.activeClipIndex) state.actions[i].play();
		}
	} else {
		// Blend mode — all clips at full weight (additive blend)
		for (const action of state.actions) {
			action.enabled = true;
			action.setEffectiveWeight(1.0);
			action.play();
		}
	}
	renderClipPanel();
};

window._soloClipForObject = function(objectId, localIndex) {
	const state = objectAnimMap.get(objectId);
	if (!state) return;
	state.activeClipIndex = localIndex;
	state.blendMode = 'solo';
	for (let i = 0; i < state.actions.length; i++) {
		state.actions[i].enabled = true;
		state.actions[i].setEffectiveWeight(i === localIndex ? 1 : 0);
		if (i === localIndex) state.actions[i].play();
	}
	renderClipPanel();
};

window._crossfadeForObject = function(objectId, localIndex) {
	const state = objectAnimMap.get(objectId);
	if (!state) return;
	if (localIndex === state.activeClipIndex) return;
	// crossfadeDuration is in real seconds; mixer time is in frame-time, so scale it
	crossfadeActions(state.actions[state.activeClipIndex], state.actions[localIndex], crossfadeDuration * sceneTimeCodesPerSecond);
	state.activeClipIndex = localIndex;
	state.blendMode = 'solo';
	renderClipPanel();
};

window._setObjectWeight = function(objectId, localIndex, value) {
	const state = objectAnimMap.get(objectId);
	if (!state) return;
	const w = parseFloat(value);
	const action = state.actions[localIndex];
	if (action) {
		action.enabled = true;
		action.setEffectiveWeight(w);
		if (w > 0) action.play();
	}
};

window._playAll = function() {
	for (const [, state] of objectAnimMap) {
		state.isPlaying = true;
		for (const action of state.actions) {
			action.paused = false;
		}
	}
	isPlaying = true;
	if (mixer) mixer.timeScale = sceneTimeCodesPerSecond * playbackSpeed;
	renderClipPanel();
};

window._pauseAll = function() {
	for (const [, state] of objectAnimMap) {
		state.isPlaying = false;
		for (const action of state.actions) {
			action.paused = true;
		}
	}
	isPlaying = false;
	renderClipPanel();
};

window._setSpeed = function(value) {
	playbackSpeed = parseFloat(value);
	if (mixer) {
		mixer.timeScale = sceneTimeCodesPerSecond * playbackSpeed;
	}
	const valEl = document.getElementById('speed-val');
	if (valEl) valEl.textContent = playbackSpeed.toFixed(1) + 'x';
};

window._toggleSkeletonVisualization = function(visible) {
	showSkeletonVisualization = !!visible;
	for (const helper of skeletonHelpers) {
		helper.visible = showSkeletonVisualization;
		if (helper.visible && typeof helper.update === 'function') {
			helper.update();
		}
	}
};

window._toggleGroundGrid = function(visible) {
	showGroundGrid = !!visible;
	groundHelper.visible = showGroundGrid;
};

// =====================================================
// Animation Loop
// =====================================================

function animate() {
	requestAnimationFrame(animate);
	frameState.frameCount++;
	const now = performance.now();
	if (now - frameState.lastFpsUpdateMs >= 500) {
		const fps = frameState.frameCount * 1000 / (now - frameState.lastFpsUpdateMs);
		const fpsEl = document.getElementById('fpsValue');
		if (fpsEl) fpsEl.textContent = fps.toFixed(1);
		frameState.frameCount = 0;
		frameState.lastFpsUpdateMs = now;
	}

	const currentFrameTimeMs = performance.now();
	const delta = (currentFrameTimeMs - lastFrameTimeMs) / 1000;
	lastFrameTimeMs = currentFrameTimeMs;
	const didUpdateAnimation = mixer && hasActiveClipActions();
	if (didUpdateAnimation) {
		mixer.update(delta);
	}
	frameState.helperUpdateFrame++;
	if (didUpdateAnimation || (frameState.helperUpdateFrame % 4) === 0) {
		for (const helper of skeletonHelpers) {
			if (helper.visible && typeof helper.update === 'function') {
				helper.update();
			}
		}
	}

	updateSelectionHighlight();
	controls.update();
	renderer.render(scene, camera);
}

// =====================================================
// File Loading
// =====================================================

let loader = null;

async function initLoader() {
	loader = await createConfiguredLightUSDLoader();
	console.log('LightUSD loader initialized');
}

// Backend switch reloads the page (the loader binds its WASM module at init).
mountBackendSelector(document.getElementById('file-controls'));

async function loadFromURL(url) {
	if (!loader) await initLoader();
	const stats = beginLoadStats();
	try {
		const fetchStart = performance.now();
		const response = await fetch(url);
		if (!response.ok) throw new Error(`HTTP ${response.status}`);
		const arrayBuffer = await response.arrayBuffer();
		stats.fetchMs = performance.now() - fetchStart;
		stats.fileSize = arrayBuffer.byteLength;
		const parseStart = performance.now();
		const usdScene = await parseUSDSceneFromArrayBuffer(
			loader,
			arrayBuffer,
			getDisplayNameFromURI(url),
			makeStaticParseOptions({ backend: LOADER_BACKEND }));
		stats.parseMs = performance.now() - parseStart;
		await processUSDScene(usdScene, url, stats);
	} catch (err) {
		failLoadStats(stats);
		console.error('Failed to load USD file from URL:', err);
		const currentFileEl = document.getElementById('currentFile');
		if (currentFileEl) currentFileEl.textContent = 'Error: ' + err.message;
	}
}

async function loadFromURLViaLoader(url) {
	if (!loader) await initLoader();
	const stats = beginLoadStats();
	try {
		const parseStart = performance.now();
		const usdScene = await loadUSDSceneFromURL(
			loader, url, makeStaticParseOptions({ backend: LOADER_BACKEND }));
		stats.parseMs = performance.now() - parseStart;
		await processUSDScene(usdScene, url, stats);
	} catch (err) {
		failLoadStats(stats);
		console.error('Failed to load USD file from URL:', err);
		const currentFileEl = document.getElementById('currentFile');
		if (currentFileEl) currentFileEl.textContent = 'Error: ' + err.message;
	}
}

async function loadFromArrayBuffer(buffer, filename) {
	if (!loader) await initLoader();
	const stats = beginLoadStats(buffer.byteLength);
	try {
		const parseStart = performance.now();
		const usdScene = await parseUSDSceneFromArrayBuffer(
			loader, buffer, filename, makeStaticParseOptions({ backend: LOADER_BACKEND }));
		stats.parseMs = performance.now() - parseStart;
		await processUSDScene(usdScene, filename, stats);
	} catch (err) {
		failLoadStats(stats);
		console.error('Failed to parse USD file:', err);
		const currentFileEl = document.getElementById('currentFile');
		if (currentFileEl) currentFileEl.textContent = 'Error: ' + err.message;
	}
}

// Handle file upload events
window.addEventListener('loadUSDFile', async (event) => {
	const file = event.detail.file;
	if (!file) return;

	const stats = beginLoadStats();
	const readStart = performance.now();
	const buffer = await file.arrayBuffer();
	stats.fetchMs = performance.now() - readStart;
	stats.fileSize = buffer.byteLength;
	if (!loader) await initLoader();
	try {
		const parseStart = performance.now();
		const usdScene = await parseUSDSceneFromArrayBuffer(
			loader, buffer, file.name, makeStaticParseOptions({ backend: LOADER_BACKEND }));
		stats.parseMs = performance.now() - parseStart;
		await processUSDScene(usdScene, file.name, stats);
	} catch (err) {
		failLoadStats(stats);
		console.error('Failed to parse USD file:', err);
		const currentFileEl = document.getElementById('currentFile');
		if (currentFileEl) currentFileEl.textContent = 'Error: ' + err.message;
	}
});

// Handle window resize
window.addEventListener('resize', () => {
	camera.aspect = window.innerWidth / window.innerHeight;
	camera.updateProjectionMatrix();
	renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, MAX_RENDER_PIXEL_RATIO));
	renderer.setSize(window.innerWidth, window.innerHeight);
});

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
		const currentFileEl = document.getElementById('currentFile');
		if (currentFileEl) currentFileEl.textContent = 'Please drop a USD file (.usd, .usda, .usdc, .usdz)';
		return;
	}
	window.dispatchEvent(new CustomEvent('loadUSDFile', { detail: { file } }));
});

// =====================================================
// Startup
// =====================================================

animate();

// Load default test asset
(async () => {
	const USD_URL = getStartupUSDModelURI() || './assets/multi-clip-skeleton.usda';
	//const USD_URL = './assets/anim-clips.usdc';
	await loadFromURL(USD_URL);
})();
