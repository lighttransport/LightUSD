import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { TransformControls } from 'three/examples/jsm/controls/TransformControls.js';
import GUI from 'three/examples/jsm/libs/lil-gui.module.min.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import {
	createConfiguredLightUSDLoader,
	getAssetUriFromURL,
	getBackendFromURL,
	LOADER_BACKEND_CHOICES,
	makeStaticParseOptions,
	parseUSDSceneFromArrayBuffer,
	setBackendAndReload
} from 'lightusd/LoaderConfigUtils.js';
import {
	buildThreeNode,
	isScene,
	readSceneMeta
} from 'lightusd/RenderSceneUtils.js';
import { buildJointHierarchyHTML } from 'lightusd/JointHierarchyUtils.js';
import { extractSkinnedMeshData } from 'lightusd/USDSceneSkinningData.js';
import { getUSDSceneMetadata } from 'lightusd/USDSceneMetadata.js';
import { buildSkeletonDataFromUSD } from 'lightusd/USDSkeletonData.js';
import { applyUSDSceneSkinningPipeline } from 'lightusd/USDSceneSkinningPipeline.js';
// USD Skeletal Animation Helper - skeleton building with bind-transform logic
import {
	resetSkeletonToRestPose
} from 'lightusd/USDSkeletalHelper.js';
// USD Animation Converter - skeletal and node animation extraction
import { buildNodeIndexMap } from 'lightusd/USDAnimationConverter.js';
import {
	extractUSDSceneAnimations,
	computeUSDSceneTimelineDuration,
	createUSDSceneAnimationPlayback
} from 'lightusd/USDSceneAnimationPipeline.js';
// Skinned Mesh Utilities - bbox, raycasting, hierarchy helpers
import {
	computeSceneBoundingBox,
	expandBoxByMeshBones,
	expandBoxBySkeletonBones,
	raycastSkinnedMeshes
} from 'lightusd/SkinnedMeshUtils.js';
// Extended Skinning Support - supports 4, 8, 16, 32, 64+ bones per vertex
import {
	SkinningMode,
	createExtendedWeightVisualizationMaterial
} from 'lightusd/ExtendedSkinning.js';

// ===========================================
// Configuration
// ===========================================

const DEFAULT_USD_MODEL_URI = './assets/skintest-animated.usda';
const LOADER_BACKEND = getBackendFromURL();

// Scene setup
const scene = new THREE.Scene();
window._scene = scene; // Debug: expose for console access
scene.background = new THREE.Color(0x1a1a1a);

// Reusable temporaries to reduce per-frame GC pressure
const _tmpVec3 = new THREE.Vector3();
const _tmpVec3b = new THREE.Vector3();
const _tmpVec3c = new THREE.Vector3();
const _tmpBox3 = new THREE.Box3();
// =====================================================================
// CPU Skinning Debug Path
//
// Computes skinned vertex positions entirely on the CPU, bypassing the
// GPU skinning shader.  Useful for isolating shader bugs (e.g. wrong
// bone texture fetch, precision issues).
//
// When enabled, each frame:
//   1. Creates a non-skinned debug Mesh with cloned geometry
//   2. Computes: pos' = bindMatrixInverse * sum(w * boneMatrix * bindMatrix * pos)
//   3. Writes result to the debug mesh geometry
//   4. Hides the SkinnedMesh, shows the debug Mesh
// =====================================================================

let _cpuSkinEnabled = false;
let _rawMeshEnabled = false;

/** Remove CPU skinning debug meshes and restore skinned mesh visibility */
function _cleanupCpuSkin() {
	for (const mesh of allSceneMeshes) {
		if (mesh._cpuDebugMesh) {
			scene.remove(mesh._cpuDebugMesh);
			mesh._cpuDebugMesh.geometry.dispose();
			mesh._cpuDebugMesh.material.dispose();
			mesh._cpuDebugMesh = null;
			mesh._cpuOrigPos = null;
			mesh._cpuOrigNorm = null;
			mesh._cpuBoneMatrices = null;
		}
		if (mesh.isSkinnedMesh) {
			mesh.visible = animationParams.showMesh;
		}
	}
}

/** Remove raw mesh debug meshes and restore skinned mesh visibility */
function _cleanupRawMesh() {
	for (const mesh of allSceneMeshes) {
		if (mesh._rawDebugMesh) {
			scene.remove(mesh._rawDebugMesh);
			mesh._rawDebugMesh.geometry.dispose();
			mesh._rawDebugMesh.material.dispose();
			mesh._rawDebugMesh = null;
		}
		if (mesh.isSkinnedMesh) {
			mesh.visible = animationParams.showMesh;
		}
	}
}

/**
 * Hint V8 to run garbage collection.
 * Uses window.gc() if available (Chrome --expose-gc), otherwise creates a
 * short-lived large allocation to nudge the GC heuristics.
 */
function hintGC() {
	if (typeof window.gc === 'function') {
		window.gc();
	} else {
		// Allocate and immediately discard — pushes V8 past its allocation threshold.
		// 16MB is enough to nudge V8's minor GC without wasting memory on the hint itself.
		void new ArrayBuffer(16 * 1024 * 1024);
	}
}

// Camera
const camera = new THREE.PerspectiveCamera(
	75,
	window.innerWidth / window.innerHeight,
	0.1,
	1000
);
camera.position.set(5, 5, 5);
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

// Transform controls for joint manipulation
const transformControls = new TransformControls(camera, renderer.domElement);
transformControls.setMode('translate');
transformControls.setSpace('local');
transformControls.addEventListener('dragging-changed', (event) => {
	controls.enabled = !event.value; // Disable orbit controls while dragging
});
// In Three.js r150+, TransformControls may need special handling
if (transformControls.isObject3D) {
	scene.add(transformControls);
} else if (transformControls.getHelper) {
	// Some versions use getHelper() for the visual gizmo
	scene.add(transformControls.getHelper());
} else {
	console.warn('TransformControls could not be added to scene - check Three.js version compatibility');
}

// Raycaster for joint selection
const raycaster = new THREE.Raycaster();
const mouse = new THREE.Vector2();

// Lighting - brighter ambient for better visibility
const ambientLight = new THREE.AmbientLight(0x808080, 1.5);
scene.add(ambientLight);

const directionalLight = new THREE.DirectionalLight(0xffffff, 2.5);
directionalLight.position.set(5, 10, 5);
directionalLight.castShadow = true;
// Higher resolution shadow map
directionalLight.shadow.mapSize.width = 2048;
directionalLight.shadow.mapSize.height = 2048;
// Initial shadow camera frustum (will be updated based on scene)
directionalLight.shadow.camera.left = -10;
directionalLight.shadow.camera.right = 10;
directionalLight.shadow.camera.top = 10;
directionalLight.shadow.camera.bottom = -10;
directionalLight.shadow.camera.near = 0.1;
directionalLight.shadow.camera.far = 50;
directionalLight.shadow.bias = -0.001;
scene.add(directionalLight);

// Ground helper (plane + grid toggle together)
const groundHelper = new THREE.Group();
groundHelper.name = 'GroundHelper';
scene.add(groundHelper);

const groundGeometry = new THREE.PlaneGeometry(20, 20);
const groundMaterial = new THREE.MeshStandardMaterial({
	color: 0x333333,
	roughness: 0.8,
	metalness: 0.2
});
const ground = new THREE.Mesh(groundGeometry, groundMaterial);
ground.rotation.x = -Math.PI / 2;
ground.receiveShadow = true;
groundHelper.add(ground);

// Grid helper
const gridHelper = new THREE.GridHelper(20, 20, 0x666666, 0x444444);
groundHelper.add(gridHelper);

// Skeleton visualization helpers
let skeletonHelpers = []; // Array of skeleton helpers for multi-skeleton scenes

// Character root group (virtual USD scene root)
const usdSceneRoot = new THREE.Group();
usdSceneRoot.name = "/";
scene.add(usdSceneRoot);

// Character content group (holds the actual mesh)
const characterGroup = new THREE.Group();
usdSceneRoot.add(characterGroup);

// Animation state
let skinnedMesh = null;
let skeletons = new Map(); // Map from skel_id to THREE.Skeleton (multi-skeleton support)
let mixer = null;
let animationPlayback = null;
let animationAction = null;
let animationActions = []; // Array of actions when playing all animations
let usdAnimations = [];
let usdNodeAnimations = []; // Node (xformOp) animation clips — always play alongside skeletal clips
let animationEnabled = []; // Array of booleans tracking which animations are enabled for multi-play
let boneMaps = new Map(); // Map from skel_id to Map(joint_id -> THREE.Bone)

/** Sync module-level playback variables from an animationPlayback state object. */
function syncPlaybackState(state) {
	mixer = state.mixer;
	animationAction = state.animationAction;
	animationActions = state.animationActions;
}
let animationParams = null; // Assigned later; keep nullable for early startup loader init
let timelineController = null;
let timelineStartController = null;
let timelineEndController = null;
let timelineBaseStart = 0;
let timelineBaseEnd = 30;
let selectAnimationController = null;
let _lastMixerUpdateTime = 0; // For mixer update throttling
let _mixerAccumDelta = 0; // Accumulated delta for throttled updates
let _enableBoneInterpolation = true;
let _totalLoadedBones = 0;
let _shadowsAutoDisabled = false;
const BONE_INTERPOLATION_MAX_BONES = 512;
const SHADOWS_MAX_BONES = 512;

function currentMixerUpdateInterval() {
	if (_totalLoadedBones > BONE_INTERPOLATION_MAX_BONES) {
		const authoredFps = Number.isFinite(currentTimeCodesPerSecond)
			? currentTimeCodesPerSecond
			: 24;
		return 1 / Math.max(1, Math.min(24, authoredFps));
	}
	return 1 / 30;
}

function setMeshShadowFlags(mesh, visible = mesh.visible) {
	const shadowsEnabled = animationParams?.enableShadows === true;
	mesh.castShadow = shadowsEnabled && visible;
	mesh.receiveShadow = shadowsEnabled;
}

function applyShadowSettings() {
	const enabled = animationParams?.enableShadows === true;
	renderer.shadowMap.enabled = enabled;
	directionalLight.castShadow = enabled;
	ground.receiveShadow = enabled;
	for (const mesh of allSceneMeshes) {
		const visible = meshVisibility.get(mesh) !== false && animationParams.showMesh;
		setMeshShadowFlags(mesh, visible);
		if (mesh.material) {
			if (Array.isArray(mesh.material)) {
				mesh.material.forEach((mat) => { if (mat) mat.needsUpdate = true; });
			} else {
				mesh.material.needsUpdate = true;
			}
		}
	}
}


// Store the current file's timeCodesPerSecond (default 24)
let currentTimeCodesPerSecond = 24;
let currentMetersPerUnit = 1.0;

// Debug visualization
let jointSpheres = [];
let weightVisualizationMaterial = null;
let originalMaterial = null;

// Joint selection state
let selectedJoint = null;
let selectedSphere = null;

// Mesh selection state
let allSceneMeshes = []; // Track all meshes in characterGroup
let meshVisibility = new Map(); // Per-mesh visibility state: Map<THREE.Mesh, boolean>
let selectedMeshObj = null; // Currently selected mesh
let bboxHelper = null; // Bounding box visualization helper
let currentUSDSource = null;
let currentVariantSelection = null;
let availableVariantSelections = [];

// =====================================================================
// Sub-frame Bone Interpolation
//
// mixer.update() at 60fps causes ~67MB/s of transient allocations from
// Three.js interpolants, ballooning the JS heap to ~2.5GB before GC.
// Keeping the mixer at 30fps halves the allocation rate, but makes
	// animation visibly choppy (every other rendered frame shows the same
	// skeletal pose).
//
// Solution: run the mixer at 30fps and on intermediate frames lerp/slerp
// bone local transforms between the two most recent mixer snapshots.
// All buffers are pre-allocated Float32Arrays — zero per-frame allocation.
// =====================================================================

const _BONE_STRIDE = 10; // px,py,pz, qx,qy,qz,qw, sx,sy,sz
const _boneInterpData = new Map(); // Map<skel_id, {prev,curr,count,ready}>
const _slerpTmp = new Float32Array(4); // Temp for quaternion slerpFlat output

/** Ensure pre-allocated interp buffers exist for the given skeleton. */
function _ensureBoneInterpBuffers(skelId, boneCount) {
	let data = _boneInterpData.get(skelId);
	if (!data || data.count !== boneCount) {
		data = {
			prev: new Float32Array(boneCount * _BONE_STRIDE),
			curr: new Float32Array(boneCount * _BONE_STRIDE),
			count: boneCount,
			ready: false
		};
		_boneInterpData.set(skelId, data);
	}
	return data;
}

/** Snapshot current bone transforms into the interp buffer (swap prev←curr first). */
function _snapshotBones(skelId, bones) {
	const data = _ensureBoneInterpBuffers(skelId, bones.length);
	// Swap: prev ← curr
	const tmp = data.prev;
	data.prev = data.curr;
	data.curr = tmp;
	// Write current transforms
	for (let i = 0; i < bones.length; i++) {
		const off = i * _BONE_STRIDE;
		const p = bones[i].position, q = bones[i].quaternion, s = bones[i].scale;
		data.curr[off]   = p.x; data.curr[off+1] = p.y; data.curr[off+2] = p.z;
		data.curr[off+3] = q.x; data.curr[off+4] = q.y; data.curr[off+5] = q.z; data.curr[off+6] = q.w;
		data.curr[off+7] = s.x; data.curr[off+8] = s.y; data.curr[off+9] = s.z;
	}
	if (!data.ready) {
		// First snapshot: copy curr→prev so interpolation works immediately
		data.prev.set(data.curr);
		data.ready = true;
	}
}

/** Lerp/slerp bone transforms between prev and curr snapshots. Zero allocation. */
function _lerpBones(skelId, bones, t) {
	const data = _boneInterpData.get(skelId);
	if (!data || !data.ready) return;
	const prev = data.prev, curr = data.curr;
	const omt = 1 - t;
	for (let i = 0; i < bones.length; i++) {
		const off = i * _BONE_STRIDE;
		const bone = bones[i];
		// Lerp position
		bone.position.set(
			prev[off]   * omt + curr[off]   * t,
			prev[off+1] * omt + curr[off+1] * t,
			prev[off+2] * omt + curr[off+2] * t
		);
		// Slerp quaternion (zero-alloc via flat arrays)
		THREE.Quaternion.slerpFlat(_slerpTmp, 0, prev, off + 3, curr, off + 3, t);
		bone.quaternion.set(_slerpTmp[0], _slerpTmp[1], _slerpTmp[2], _slerpTmp[3]);
		// Lerp scale
		bone.scale.set(
			prev[off+7] * omt + curr[off+7] * t,
			prev[off+8] * omt + curr[off+8] * t,
			prev[off+9] * omt + curr[off+9] * t
		);
	}
}

/** Clear interpolation state (call on scene reload or rest pose reset). */
function _clearBoneInterpData() {
	_boneInterpData.clear();
}

// ===========================================
// App-specific Functions
// ===========================================

/**
 * Create weight visualization material with pseudo-color shader
 * Includes proper Three.js skinning support for SkinnedMesh
 * @returns {THREE.ShaderMaterial} Weight visualization shader material
 */
function createWeightVisualizationMaterial() {
	// Use Three.js shader chunks for skinning support
	const vertexShader = `
		#include <common>
		#include <skinning_pars_vertex>

		varying vec3 vColor;
		varying vec4 vSkinWeight;
		varying vec4 vSkinIndex;

		// Pseudo-color palette for weight visualization
		vec3 getWeightColor(float weight, float index) {
			// Use HSV color wheel based on bone index
			float hue = mod(index * 0.618033988749895, 1.0); // Golden ratio for good distribution
			float sat = 0.8;
			float val = weight;

			// HSV to RGB conversion
			float h = hue * 6.0;
			float c = val * sat;
			float x = c * (1.0 - abs(mod(h, 2.0) - 1.0));
			float m = val - c;

			vec3 rgb;
			if (h < 1.0) rgb = vec3(c, x, 0.0);
			else if (h < 2.0) rgb = vec3(x, c, 0.0);
			else if (h < 3.0) rgb = vec3(0.0, c, x);
			else if (h < 4.0) rgb = vec3(0.0, x, c);
			else if (h < 5.0) rgb = vec3(x, 0.0, c);
			else rgb = vec3(c, 0.0, x);

			return rgb + m;
		}

		void main() {
			#include <skinbase_vertex>

			vSkinWeight = skinWeight;
			vSkinIndex = skinIndex;

			// Blend colors based on weights
			vColor = vec3(0.0);
			vColor += getWeightColor(skinWeight.x, skinIndex.x) * skinWeight.x;
			vColor += getWeightColor(skinWeight.y, skinIndex.y) * skinWeight.y;
			vColor += getWeightColor(skinWeight.z, skinIndex.z) * skinWeight.z;
			vColor += getWeightColor(skinWeight.w, skinIndex.w) * skinWeight.w;

			// Apply skinning transformation
			vec3 transformed = vec3(position);
			#include <skinning_vertex>

			vec4 mvPosition = modelViewMatrix * vec4(transformed, 1.0);
			gl_Position = projectionMatrix * mvPosition;
		}
	`;

	const fragmentShader = `
		varying vec3 vColor;
		varying vec4 vSkinWeight;
		varying vec4 vSkinIndex;

		uniform int visualizationMode;

		void main() {
			if (visualizationMode == 0) {
				// Blended weight colors
				gl_FragColor = vec4(vColor, 1.0);
			} else if (visualizationMode == 1) {
				// Primary influence only
				float maxWeight = max(max(vSkinWeight.x, vSkinWeight.y), max(vSkinWeight.z, vSkinWeight.w));
				if (vSkinWeight.x == maxWeight) {
					gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
				} else if (vSkinWeight.y == maxWeight) {
					gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
				} else if (vSkinWeight.z == maxWeight) {
					gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);
				} else {
					gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0);
				}
			} else {
				// Weight intensity (grayscale based on total weight)
				float totalWeight = vSkinWeight.x + vSkinWeight.y + vSkinWeight.z + vSkinWeight.w;
				gl_FragColor = vec4(vec3(totalWeight), 1.0);
			}
		}
	`;

	// Merge Three.js skinning uniforms with our custom uniforms
	const uniforms = THREE.UniformsUtils.merge([
		THREE.UniformsLib.skinning,
		{
			visualizationMode: { value: 0 }
		}
	]);

	return new THREE.ShaderMaterial({
		vertexShader: vertexShader,
		fragmentShader: fragmentShader,
		uniforms: uniforms,
		skinning: true,  // Enable skinning for this material
		side: THREE.DoubleSide
	});
}

/**
 * Create joint visualization spheres
 * @param {Array<THREE.Bone>} bones - Array of bones
 * @returns {Array<THREE.Mesh>} Array of sphere meshes
 */
function createJointSpheres(bones) {
	const spheres = [];
	const geometry = new THREE.SphereGeometry(0.05, 16, 16);

	bones.forEach((bone, index) => {
		const material = new THREE.MeshStandardMaterial({
			color: 0xff0000,
			emissive: 0x000000,
			metalness: 0.3,
			roughness: 0.7
		});

		const sphere = new THREE.Mesh(geometry, material);
		sphere.material.color.setHSL(index / bones.length, 1.0, 0.5);
		sphere.userData.bone = bone;
		sphere.userData.boneName = bone.name;
		spheres.push(sphere);
		scene.add(sphere);
	});

	return spheres;
}

/**
 * Update joint sphere positions
 */
function updateJointSpheres() {
	jointSpheres.forEach(sphere => {
		const bone = sphere.userData.bone;
		bone.getWorldPosition(_tmpVec3);
		sphere.position.copy(_tmpVec3);
	});
}

function applySkeletonHelperOptions() {
	for (const helper of skeletonHelpers) {
		if (helper && typeof helper.setDisplayOptions === 'function') {
			helper.setDisplayOptions({
				mode: animationParams.skeletonDisplayMode,
				showTips: animationParams.showSkeletonTips,
				capTips: animationParams.capSkeletonTips,
				tipMaxLength: animationParams.skeletonTipMaxLength
			});
		}
		helper.visible = animationParams.showSkeleton;
	}
}

function applyMetersPerUnitScale({ refit = false } = {}) {
	const scale = animationParams?.ignoreMetersPerUnit ? 1.0 : currentMetersPerUnit;
	const safeScale = Number.isFinite(scale) && scale > 0 ? scale : 1.0;
	characterGroup.scale.setScalar(safeScale);
	characterGroup.updateMatrixWorld(true);
	usdSceneRoot.updateMatrixWorld(true);
	console.log(`metersPerUnit scale: ${safeScale} (${animationParams?.ignoreMetersPerUnit ? 'ignored' : 'applied'})`);

	if (refit) {
		fitCameraToScene();
	}
}

/**
 * Update shadow camera frustum based on scene bounding box
 * @param {THREE.DirectionalLight} light - Directional light with shadow
 * @param {THREE.Box3} sceneBounds - Scene bounding box
 */
function updateShadowCameraFromBounds(light, sceneBounds) {
	sceneBounds.getCenter(_tmpVec3);
	sceneBounds.getSize(_tmpVec3b);

	// Calculate the maximum extent with some padding
	const maxDim = Math.max(_tmpVec3b.x, _tmpVec3b.y, _tmpVec3b.z);
	const padding = maxDim * 0.5;
	const frustumSize = maxDim + padding;

	// Update shadow camera frustum
	light.shadow.camera.left = -frustumSize;
	light.shadow.camera.right = frustumSize;
	light.shadow.camera.top = frustumSize;
	light.shadow.camera.bottom = -frustumSize;

	// Update near/far based on light position and scene bounds
	_tmpVec3c.copy(light.position);
	const dist = _tmpVec3c.sub(_tmpVec3).length(); // lightToCenter
	light.shadow.camera.near = Math.max(0.1, dist - maxDim);
	light.shadow.camera.far = dist + maxDim * 2;

	// Move light target to scene center (_tmpVec3 was overwritten by sub, recompute)
	sceneBounds.getCenter(_tmpVec3);
	light.target.position.copy(_tmpVec3);
	light.target.updateMatrixWorld();

	// Update shadow camera
	light.shadow.camera.updateProjectionMatrix();

	console.log(`Shadow camera updated: frustum=${frustumSize.toFixed(2)}, near=${light.shadow.camera.near.toFixed(2)}, far=${light.shadow.camera.far.toFixed(2)}`);
}

/**
 * Select a joint and attach transform controls
 * @param {THREE.Bone} bone - The bone to select
 * @param {THREE.Mesh} sphere - The corresponding sphere mesh
 */
function selectJoint(bone, sphere) {
	// Deselect previous joint
	if (selectedSphere && selectedSphere !== sphere) {
		selectedSphere.material.emissive.setHex(0x000000);
		selectedSphere.scale.set(1, 1, 1);
	}

	selectedJoint = bone;
	selectedSphere = sphere;

	if (sphere) {
		// Highlight selected sphere
		sphere.material.emissive.setHex(0xffffff);
		sphere.scale.set(1.5, 1.5, 1.5);
	}

	// Attach transform controls to bone
	transformControls.attach(bone);
	transformControls.visible = animationParams.showGizmo;

	// Update UI to show selected joint name
	if (window.updateSelectedJoint) {
		window.updateSelectedJoint(bone.name);
	}
	// Also update GUI if available
	if (typeof updateSelectedJointGUI === 'function') {
		updateSelectedJointGUI(bone.name);
	}

	console.log(`Selected joint: ${bone.name}`);
}

/**
 * Deselect current joint
 */
function deselectJoint() {
	if (selectedSphere) {
		selectedSphere.material.emissive.setHex(0x000000);
		selectedSphere.scale.set(1, 1, 1);
	}

	selectedJoint = null;
	selectedSphere = null;
	transformControls.detach();

	if (window.updateSelectedJoint) {
		window.updateSelectedJoint(null);
	}
	// Also update GUI if available
	if (typeof updateSelectedJointGUI === 'function') {
		updateSelectedJointGUI(null);
	}
}

/**
 * Select a mesh and highlight it
 * @param {THREE.Mesh} mesh - The mesh to select
 */
function selectMesh(mesh) {
	// Deselect previous mesh
	deselectMesh();

	selectedMeshObj = mesh;

	// Update bbox helper
	updateBBoxHelper();

	// Update GUI selection
	const meshIndex = allSceneMeshes.indexOf(mesh);
	if (meshIndex >= 0) {
		animationParams.selectedMeshName = mesh.name || `Mesh_${meshIndex}`;
	}

	console.log(`Selected mesh: ${mesh.name}`);
}

/**
 * Deselect current mesh
 */
function deselectMesh() {
	selectedMeshObj = null;
	removeBBoxHelper();
	animationParams.selectedMeshName = 'None';
}

/**
 * Remove bounding box helper from scene
 */
function removeBBoxHelper() {
	if (bboxHelper) {
		scene.remove(bboxHelper);
		if (bboxHelper.geometry) bboxHelper.geometry.dispose();
		if (bboxHelper.material) bboxHelper.material.dispose();
		bboxHelper = null;
	}
}

/**
 * Compute the current bounding box for selected mesh or all scene meshes
 * @returns {THREE.Box3} The computed bounding box
 */
function computeCurrentBBox() {
	_tmpBox3.makeEmpty();

	if (selectedMeshObj) {
		if (selectedMeshObj.isSkinnedMesh && selectedMeshObj.skeleton) {
			expandBoxByMeshBones(selectedMeshObj, _tmpBox3);
		} else {
			_tmpBox3.expandByObject(selectedMeshObj);
		}
	} else {
		for (const mesh of allSceneMeshes) {
			if (mesh.isSkinnedMesh && mesh.skeleton) {
				expandBoxBySkeletonBones(mesh.skeleton, _tmpBox3);
			} else {
				_tmpBox3.expandByObject(mesh);
			}
		}
	}

	if (!_tmpBox3.isEmpty()) {
		_tmpBox3.getSize(_tmpVec3);
		_tmpVec3b.copy(_tmpVec3).multiplyScalar(0.15);
		_tmpBox3.min.sub(_tmpVec3b);
		_tmpBox3.max.add(_tmpVec3b);
	}

	return _tmpBox3;
}

/**
 * Update bounding box helper for the selected mesh or all scene meshes
 */
function updateBBoxHelper() {
	if (!animationParams.showBBox) {
		removeBBoxHelper();
		return;
	}

	const box = computeCurrentBBox();

	if (box.isEmpty()) {
		removeBBoxHelper();
		return;
	}

	if (!bboxHelper) {
		bboxHelper = new THREE.Box3Helper(box, 0x00ff00);
		scene.add(bboxHelper);
	} else {
		// Update existing helper's box bounds
		bboxHelper.box.copy(box);
	}
}

// Track mouse down position for drag detection
let _mouseDownPos = { x: 0, y: 0 };
window.addEventListener('mousedown', (event) => {
	_mouseDownPos.x = event.clientX;
	_mouseDownPos.y = event.clientY;
});

/**
 * Handle mouse click for joint and mesh selection via raycasting
 * @param {MouseEvent} event - Mouse event
 */
function onMouseClick(event) {
	// Ignore clicks on GUI elements
	if (event.target && event.target.closest && event.target.closest('.lil-gui')) return;

	// Ignore drags (user was orbiting, not clicking)
	const dx = event.clientX - _mouseDownPos.x;
	const dy = event.clientY - _mouseDownPos.y;
	if (dx * dx + dy * dy > 9) return; // 3px threshold

	// Calculate mouse position in normalized device coordinates
	mouse.x = (event.clientX / window.innerWidth) * 2 - 1;
	mouse.y = -(event.clientY / window.innerHeight) * 2 + 1;

	// Update raycaster
	raycaster.setFromCamera(mouse, camera);

	// Check for intersections with joint spheres first (higher priority)
	if (jointSpheres.length > 0 && animationParams.showJoints) {
		const jointIntersects = raycaster.intersectObjects(jointSpheres);
		if (jointIntersects.length > 0) {
			const sphere = jointIntersects[0].object;
			const bone = sphere.userData.bone;
			selectJoint(bone, sphere);
			return;
		}
	}

	// Check for intersections with scene meshes (using skinned positions)
	if (allSceneMeshes.length > 0) {
		const meshIntersects = raycastSkinnedMeshes(raycaster, allSceneMeshes);
		if (meshIntersects.length > 0) {
			selectMesh(meshIntersects[0].object);
			return;
		}
	}

	// Click on empty space - deselect mesh
	deselectMesh();
}

function applyCameraFit(box, paddingFactor, label) {
	// Add padding for mesh volume around bones
	box.getSize(_tmpVec3);
	_tmpVec3b.copy(_tmpVec3).multiplyScalar(0.15);
	box.min.sub(_tmpVec3b);
	box.max.add(_tmpVec3b);

	box.getCenter(_tmpVec3);   // center
	box.getSize(_tmpVec3b);    // finalSize
	const maxDim = Math.max(_tmpVec3b.x, _tmpVec3b.y, _tmpVec3b.z);

	const fov = camera.fov * (Math.PI / 180);
	const cameraDistance = (maxDim / 2) / Math.tan(fov / 2) * paddingFactor;

	_tmpVec3c.set(cameraDistance * 0.7, cameraDistance * 0.5, cameraDistance * 0.7);
	camera.position.copy(_tmpVec3).add(_tmpVec3c);
	camera.lookAt(_tmpVec3);

	controls.target.copy(_tmpVec3);

	// Adapt controls to scene scale
	controls.minDistance = maxDim * 0.05;
	controls.maxDistance = maxDim * 50;
	controls.zoomSpeed = 3.0;
	controls.panSpeed = 2.0;
	controls.update();

	camera.near = Math.max(0.01, maxDim * 0.001);
	camera.far = maxDim * 100;
	camera.updateProjectionMatrix();

	console.log(`fitCameraToScene (${label}): center=(${_tmpVec3.x.toFixed(2)}, ${_tmpVec3.y.toFixed(2)}, ${_tmpVec3.z.toFixed(2)}), size=(${_tmpVec3b.x.toFixed(2)}, ${_tmpVec3b.y.toFixed(2)}, ${_tmpVec3b.z.toFixed(2)}), distance=${cameraDistance.toFixed(2)}, maxDim=${maxDim.toFixed(2)}`);
}

function fitCameraToScene(targetObject = null, paddingFactor = 1.5) {
	const target = targetObject || usdSceneRoot;
	if (!target || target.children.length === 0) {
		console.warn('fitCameraToScene: No object to fit');
		return;
	}
	target.updateMatrixWorld(true);

	// Since bind() uses mesh.matrixWorld as bindMatrix, skinned meshes
	// render at their natural hierarchy position. expandByObject correctly
	// computes world-space bounds via matrixWorld × geometry bbox.
	const box = new THREE.Box3();
	box.expandByObject(target);

	if (box.isEmpty()) {
		console.warn('fitCameraToScene: Could not compute bounding box');
		return;
	}
	applyCameraFit(box, paddingFactor, 'current frame');
}

function fitCameraToAllFrames(targetObject = null, paddingFactor = 1.5) {
	const target = targetObject || usdSceneRoot;
	if (!target || target.children.length === 0) {
		console.warn('fitCameraToAllFrames: No object to fit');
		return;
	}
	target.updateMatrixWorld(true);

	const box = new THREE.Box3();
	const hasAnimation = mixer && usdAnimations.length > 0 && animationAction;

	if (hasAnimation && allSceneMeshes.length > 0) {
		const clip = usdAnimations[animationParams.currentAnimation] || usdAnimations[0];
		const duration = clip.duration;
		const boneCount = skinnedMesh?.skeleton?.bones?.length || 0;
		const numSamples = boneCount > 500 ? 5 : 20;
		const savedTime = animationAction.time;

		console.log(`fitCameraToAllFrames: Sampling ${numSamples} frames over ${duration.toFixed(2)}s (${boneCount} bones)`);

		for (let i = 0; i <= numSamples; i++) {
			const sampleTime = (i / numSamples) * duration;
			animationAction.time = sampleTime;
			mixer.setTime(sampleTime);
			if (skinnedMesh?.skeleton) skinnedMesh.skeleton.update();
			target.updateMatrixWorld(true);
			for (const mesh of allSceneMeshes) {
				if (!mesh.visible) continue;
				if (mesh.isSkinnedMesh && mesh.skeleton) {
					expandBoxByMeshBones(mesh, box);
				}
			}
		}

		// Restore original time
		animationAction.time = savedTime;
		mixer.setTime(savedTime);
		if (skinnedMesh?.skeleton) skinnedMesh.skeleton.update();
		target.updateMatrixWorld(true);
	} else {
		// No animation — fall back to current frame
		for (const mesh of allSceneMeshes) {
			if (!mesh.visible) continue;
			if (mesh.isSkinnedMesh && mesh.skeleton) {
				expandBoxByMeshBones(mesh, box);
			}
		}
		if (box.isEmpty()) box.expandByObject(target);
	}

	if (box.isEmpty()) {
		console.warn('fitCameraToAllFrames: Could not compute bounding box');
		return;
	}
	applyCameraFit(box, paddingFactor, 'all frames');
}

async function createConfiguredLoader() {
	const enableBoneReduction = animationParams?.enableBoneReduction === true;
	const targetBoneCount = Number.isFinite(animationParams?.targetBoneCount)
		? animationParams.targetBoneCount
		: 4;

	const loader = await createConfiguredLightUSDLoader({
		initOptions: { useZstdCompressedWasm: false, useMemory64: false },
		skinningOptions: {
			enableBoneReduction,
			targetBoneCount,
			roundBoneCount: false,
			logger: console
		}
	});
	window._lightusd_module = loader.native_;
	window._lightusd_wasm_memory = {
		get buffer() {
			return loader.native_?.HEAPU8?.buffer ?? null;
		}
	};
	return loader;
}

function getStartupUSDModelURI() {
	return getAssetUriFromURL(undefined, ['usd']) || DEFAULT_USD_MODEL_URI;
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

function normalizeVariantInfos(variantInfos) {
	const selections = [];
	if (!Array.isArray(variantInfos)) return selections;

	for (const primInfo of variantInfos) {
		const primPath = primInfo?.primPath;
		const variantSets = Array.isArray(primInfo?.variantSets) ? primInfo.variantSets : [];
		if (!primPath) continue;

		for (const variantSet of variantSets) {
			const variantSetName = variantSet?.name;
			const options = Array.isArray(variantSet?.options) ? variantSet.options : [];
			if (!variantSetName || options.length === 0) continue;

			for (const variantName of options) {
				selections.push({
					primPath,
					variantSet: variantSetName,
					variantName,
					authoredSelection: variantSet.selection || ''
				});
			}
		}
	}

	return selections;
}

function updateVariantControls(variantInfos, activeSelection = null) {
	availableVariantSelections = normalizeVariantInfos(variantInfos);

	const controlsEl = document.getElementById('variant-controls');
	const selectEl = document.getElementById('variantSelect');
	const statusEl = document.getElementById('variantStatus');
	if (!controlsEl || !selectEl || !statusEl) return;

	selectEl.innerHTML = '';
	if (availableVariantSelections.length === 0) {
		controlsEl.style.display = 'none';
		statusEl.textContent = '';
		return;
	}

	controlsEl.style.display = 'flex';
	for (const selection of availableVariantSelections) {
		const option = document.createElement('option');
		option.value = JSON.stringify({
			primPath: selection.primPath,
			variantSet: selection.variantSet,
			variantName: selection.variantName
		});
		const defaultSuffix = selection.authoredSelection === selection.variantName ? ' (authored)' : '';
		option.textContent = `${selection.primPath} ${selection.variantSet}=${selection.variantName}${defaultSuffix}`;
		selectEl.appendChild(option);
	}

	const selectionToUse = activeSelection || currentVariantSelection;
	if (selectionToUse) {
		const encoded = JSON.stringify(selectionToUse);
		if ([...selectEl.options].some(option => option.value === encoded)) {
			selectEl.value = encoded;
		}
	} else {
		const authored = availableVariantSelections.find(
			selection => selection.authoredSelection === selection.variantName
		);
		if (authored) {
			selectEl.value = JSON.stringify({
				primPath: authored.primPath,
				variantSet: authored.variantSet,
				variantName: authored.variantName
			});
		}
	}

	statusEl.textContent = `${availableVariantSelections.length} variant option${availableVariantSelections.length === 1 ? '' : 's'}`;
}

async function parseSkinAnimUSDSceneFromArrayBuffer(loader, arrayBuffer, filename, variantSelection = null) {
	let variantInfos = [];
	const usdScene = await parseUSDSceneFromArrayBuffer(loader, arrayBuffer, filename, {
		...makeStaticParseOptions({ backend: LOADER_BACKEND }),
		variantSelection,
		onVariants: (infos) => {
			variantInfos = infos;
		}
	});
	return { usdScene, variantInfos };
}

function formatDurationMs(ms) {
	if (!Number.isFinite(ms)) return 'n/a';
	if (ms < 1000) return `${ms.toFixed(0)} ms`;
	return `${(ms / 1000).toFixed(2)} s`;
}

function formatBytes(bytes) {
	if (!Number.isFinite(bytes)) return 'n/a';
	const units = ['B', 'KB', 'MB', 'GB'];
	let value = bytes;
	let unitIndex = 0;
	while (value >= 1024 && unitIndex < units.length - 1) {
		value /= 1024;
		unitIndex++;
	}
	return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

function captureMemorySnapshot() {
	const jsHeap = performance.memory?.usedJSHeapSize;
	const wasmHeap =
		window._lightusd_wasm_memory?.buffer?.byteLength ??
		window._lightusd_module?.HEAPU8?.buffer?.byteLength;
	return {
		jsHeap: Number.isFinite(jsHeap) ? jsHeap : null,
		wasmHeap: Number.isFinite(wasmHeap) ? wasmHeap : null
	};
}

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

function updateLoadStatsPanel(stats) {
	const container = document.getElementById('load-stats');
	const details = document.getElementById('loadStatsDetails');
	if (!container || !details) return;

	container.style.display = '';

	if (stats.status === 'loading') {
		details.textContent = 'Loading...';
		return;
	}

	if (stats.status === 'failed') {
		details.textContent = 'Failed';
		return;
	}

	const lines = [
		`File: ${formatBytes(stats.fileSize)}`,
		`Fetch/read: ${stats.fetchMs === null ? 'n/a' : formatDurationMs(stats.fetchMs)}`,
		`Parse/load: ${formatDurationMs(stats.parseMs)}`,
		`Process/build: ${formatDurationMs(stats.processMs)}`,
		`Total: ${formatDurationMs(stats.totalMs)}`,
		`JS heap: ${formatMemoryUse(stats.memoryBefore, stats.memoryAfter, 'jsHeap')}`,
		`WASM heap: ${formatMemoryUse(stats.memoryBefore, stats.memoryAfter, 'wasmHeap')}`
	];
	if (stats.coreTimings) {
		const timing = stats.coreTimings;
		lines.push(
			`Core archive setup: ${formatDurationMs(timing.archiveMs)}`,
			`Core native begin: ${formatDurationMs(timing.nativeBeginMs)}`,
			`Core animation copy: ${formatDurationMs(timing.animationCopyMs)}`,
			`Core entity copy: ${formatDurationMs(timing.entityCopyMs)}`,
			`Core mesh copy: ${formatDurationMs(timing.meshCopyMs)}`,
			`Core adapter total: ${formatDurationMs(timing.totalMs)}`
		);
	}

	details.textContent = lines.join('\n');
	details.style.whiteSpace = 'pre-line';
	window.__skinAnimLoadStats = stats;
}

function beginLoadStats(fileSize = null) {
	window.renderComplete = false;
	const stats = {
		status: 'loading',
		startTime: performance.now(),
		fileSize,
		fetchMs: null,
		parseMs: null,
		processMs: null,
		totalMs: null,
		memoryBefore: captureMemorySnapshot(),
		memoryAfter: null
	};
	updateLoadStatsPanel(stats);
	return stats;
}

async function parseAndProcessUSD(loader, arrayBuffer, filename, stats) {
	const parseStart = performance.now();
	const { usdScene: usd_scene, variantInfos } =
		await parseSkinAnimUSDSceneFromArrayBuffer(loader, arrayBuffer, filename, currentVariantSelection);
	stats.parseMs = performance.now() - parseStart;
	stats.coreTimings = isScene(usd_scene)
		? (usd_scene.getStats?.().timings || null)
		: null;
	updateVariantControls(variantInfos, currentVariantSelection);

	console.log('USD scene loaded:', usd_scene);

	const processStart = performance.now();
	await processUSDScene(usd_scene, filename);
	stats.processMs = performance.now() - processStart;
	stats.totalMs = performance.now() - stats.startTime;
	stats.memoryAfter = captureMemorySnapshot();
	stats.status = 'done';
	updateLoadStatsPanel(stats);
	window.renderComplete = true;
}

/**
 * Generate joint hierarchy text with clickable elements
 * @param {Array<THREE.Bone>} bones - Array of bones
 * @returns {string} HTML formatted hierarchy
 */
function generateJointHierarchy(bones) {
	return buildJointHierarchyHTML(bones, {
		wrap: true,
		wrapperStyle: 'font-family: monospace; font-size: 12px; line-height: 1.4;',
		itemClassName: 'joint-item',
		hoverBackground: 'rgba(255,255,255,0.1)',
		rootColor: '#ff6b6b',
		childColor: '#4ecdc4'
	});
}

/**
 * Handle joint selection from hierarchy UI
 * @param {string} boneName - Name of the bone to select
 */
function selectJointByName(boneName) {
	// Find the bone and sphere with matching name
	const sphere = jointSpheres.find(s => s.userData.boneName === boneName);
	if (sphere) {
		const bone = sphere.userData.bone;
		selectJoint(bone, sphere);
	}
}

/**
 * Load USD model from a URL path
 */
async function loadUSDModel() {
	const loader = await createConfiguredLoader();

	// Default USD file to load
	const usd_filename = DEFAULT_USD_MODEL_URI;
	const stats = beginLoadStats();

	console.log(`Loading USD file: ${usd_filename}`);

	const fetchStart = performance.now();
	const response = await fetch(usd_filename);
	if (!response.ok) {
		stats.status = 'failed';
		stats.totalMs = performance.now() - stats.startTime;
		stats.memoryAfter = captureMemorySnapshot();
		updateLoadStatsPanel(stats);
		throw new Error(`Failed to fetch: ${response.statusText}`);
	}
	const arrayBuffer = await response.arrayBuffer();
	stats.fetchMs = performance.now() - fetchStart;
	stats.fileSize = arrayBuffer.byteLength;
	currentUSDSource = {
		type: 'url',
		url: usd_filename,
		filename: usd_filename,
		arrayBuffer: arrayBuffer.slice(0)
	};

	try {
		await parseAndProcessUSD(loader, arrayBuffer, usd_filename, stats);
	} catch (error) {
		stats.status = 'failed';
		stats.totalMs = performance.now() - stats.startTime;
		stats.memoryAfter = captureMemorySnapshot();
		updateLoadStatsPanel(stats);
		throw error;
	}
}

async function loadUSDModelFromURI(uri) {
	const loader = await createConfiguredLoader();
	const stats = beginLoadStats();
	const displayName = getDisplayNameFromURI(uri);

	console.log(`Loading USD file from URI: ${uri}`);

	const fetchStart = performance.now();
	const response = await fetch(uri);
	if (!response.ok) {
		stats.status = 'failed';
		stats.totalMs = performance.now() - stats.startTime;
		stats.memoryAfter = captureMemorySnapshot();
		updateLoadStatsPanel(stats);
		throw new Error(`Failed to fetch ${uri}: ${response.statusText}`);
	}
	const arrayBuffer = await response.arrayBuffer();
	stats.fetchMs = performance.now() - fetchStart;
	stats.fileSize = arrayBuffer.byteLength;
	currentUSDSource = {
		type: 'url',
		url: uri,
		filename: displayName,
		arrayBuffer: arrayBuffer.slice(0)
	};

	try {
		await parseAndProcessUSD(loader, arrayBuffer, displayName, stats);
	} catch (error) {
		stats.status = 'failed';
		stats.totalMs = performance.now() - stats.startTime;
		stats.memoryAfter = captureMemorySnapshot();
		updateLoadStatsPanel(stats);
		throw error;
	}
}

/**
 * Load USD file and extract skeletal mesh and animations
 * @param {ArrayBuffer} arrayBuffer - USD file data
 * @param {string} filename - File name
 */
async function loadUSDFromArrayBuffer(arrayBuffer, filename) {
	const loader = await createConfiguredLoader();
	const stats = beginLoadStats(arrayBuffer.byteLength);
	stats.fetchMs = null;

	console.log(`Loading USD from file: ${filename} (${(arrayBuffer.byteLength / 1024).toFixed(2)} KB)`);

	currentUSDSource = {
		type: 'file',
		filename,
		arrayBuffer: arrayBuffer.slice(0)
	};

	try {
		await parseAndProcessUSD(loader, arrayBuffer, filename, stats);
	} catch (error) {
		stats.status = 'failed';
		stats.totalMs = performance.now() - stats.startTime;
		stats.memoryAfter = captureMemorySnapshot();
		updateLoadStatsPanel(stats);
		throw error;
	}
}

async function reloadCurrentUSDWithVariant(variantSelection) {
	if (!currentUSDSource?.arrayBuffer) {
		console.warn('No USD source is loaded; cannot reload variant.');
		return;
	}

	currentVariantSelection = variantSelection;
	await loadUSDFromArrayBuffer(currentUSDSource.arrayBuffer.slice(0), currentUSDSource.filename);
}

/**
 * Process loaded USD scene and extract skeletal mesh and animations
 * @param {Object} usd_scene - Loaded USD scene
 * @param {string} filename - File name
 */
async function processUSDScene(usd_scene, filename) {
	// Update current file display in UI
	const currentFileElement = document.getElementById('currentFile');
	if (currentFileElement) {
		// Extract just the filename from the path
		const displayName = filename.split('/').pop();
		currentFileElement.textContent = displayName;
	}

	// Dispose old animation mixer/playback before clearing references
	if (animationPlayback) {
		animationPlayback.dispose();
		animationPlayback = null;
		mixer = null;
	} else if (mixer) {
		mixer.stopAllAction();
		mixer.uncacheRoot(mixer.getRoot());
		mixer = null;
	}
	animationAction = null;
	animationActions = [];

	// Dispose all tracked meshes (geometries, materials, textures)
	for (const mesh of allSceneMeshes) {
		if (mesh.geometry) mesh.geometry.dispose();
		if (mesh.material) {
			if (Array.isArray(mesh.material)) {
				mesh.material.forEach(m => m.dispose());
			} else {
				mesh.material.dispose();
			}
		}
		if (mesh.customDepthMaterial) mesh.customDepthMaterial.dispose();
	}

	// Dispose skeleton bone textures (all skeletons)
	for (const [skelId, skel] of skeletons) {
		if (skel && skel.boneTexture) {
			skel.boneTexture.dispose();
		}
	}
	skinnedMesh = null;
	skeletons.clear();
	_clearBoneInterpData();

	// Dispose skeleton helpers
	for (const helper of skeletonHelpers) {
		scene.remove(helper);
		if (helper.geometry) helper.geometry.dispose();
		if (helper.material) {
			if (Array.isArray(helper.material)) {
				helper.material.forEach(m => m.dispose());
			} else {
				helper.material.dispose();
			}
		}
	}
	skeletonHelpers = [];

	// Clear all tracked meshes
	allSceneMeshes = [];
	meshVisibility.clear();
	deselectMesh();

	// Dispose and clear joint spheres
	jointSpheres.forEach(sphere => {
		if (sphere.geometry) sphere.geometry.dispose();
		if (sphere.material) sphere.material.dispose();
		scene.remove(sphere);
	});
	jointSpheres = [];

	// Deselect any selected joint
	deselectJoint();

	// Reset materials
	originalMaterial = null;
	if (weightVisualizationMaterial) {
		weightVisualizationMaterial.dispose();
		weightVisualizationMaterial = null;
	}

	// Reset animations and caches
	usdAnimations = [];
	usdNodeAnimations = [];
	animationEnabled = [];
	boneMaps.clear();
	animationParams.hasUSDAnimations = false;
	animationParams.usdAnimationCount = 0;
	animationParams.playAllAnimations = false;
	// Store loader globally for debugging
	window.usd_scene = usd_scene;

	// Reset characterGroup transforms to prevent stale state from previous loads
	characterGroup.position.set(0, 0, 0);
	characterGroup.quaternion.identity();
	characterGroup.scale.set(1, 1, 1);

	// Clear previous threeNode children from characterGroup
	while (characterGroup.children.length > 0) {
		characterGroup.remove(characterGroup.children[0]);
	}

	let coreBuiltNode = null;
	let coreNodeIndexMap = null;
	let coreTextureManager = null;
	let coreTextureLoadPromise = null;
	if (isScene(usd_scene)) {
		const built = buildThreeNode(usd_scene, {
			skipTextures: false,
			lazyTextures: true,
			releaseBuildData: false
		});
		coreBuiltNode = built.node;
		coreNodeIndexMap = built.nodeIndexMap || null;
		coreTextureManager = built.textureManager || null;
		if (coreTextureManager) {
			coreTextureLoadPromise = coreTextureManager.startLoading({
				concurrency: LightUSDLoaderUtils.defaultTextureConcurrency(),
				yieldInterval: 16,
				onTextureLoaded: (material) => { material.needsUpdate = true; }
			}).catch((err) => {
				console.warn('[skin-anim] core texture loading failed:', err);
			});
		}
	}

	// Get normalized scene metadata from library helper.
	const {
		sceneMetadata,
		fileUpAxis,
		timeCodesPerSecond,
		startTimeCode,
		endTimeCode
	} = getUSDSceneMetadata(usd_scene);

	// Update global timeCodesPerSecond and speed parameter
	currentTimeCodesPerSecond = timeCodesPerSecond;
	animationParams.speed = timeCodesPerSecond;
	animationParams.timelineStart = startTimeCode;
	animationParams.timelineEnd = endTimeCode;
	timelineBaseStart = Number.isFinite(Number(startTimeCode)) ? Number(startTimeCode) : 0;
	timelineBaseEnd = Number.isFinite(Number(endTimeCode)) ? Number(endTimeCode) : timelineBaseStart + 1;
	animationParams.time = Math.min(
		Math.max(animationParams.time, startTimeCode),
		endTimeCode
	);
	if (window.updateTimelineRange) {
		window.updateTimelineRange(startTimeCode, endTimeCode);
	}

	console.log('=== USD Scene Metadata ===');
	console.log(`upAxis (from metadata): "${fileUpAxis}"`);
	console.log(`metersPerUnit: ${sceneMetadata.metersPerUnit || 1.0}`);
	console.log(`timeCodesPerSecond: ${timeCodesPerSecond}`);
	console.log(`startTimeCode: ${startTimeCode}, endTimeCode: ${endTimeCode}`);
	currentMetersPerUnit = Number.isFinite(Number(sceneMetadata.metersPerUnit))
		? Number(sceneMetadata.metersPerUnit)
		: 1.0;
	applyMetersPerUnitScale();

	const {
		hasSkinnedMeshData,
		firstGeomBindTransform,
		allSkinnedMeshUSDData,
		skinnedMeshDataByName
	} = extractSkinnedMeshData(usd_scene, { logger: console });

	// Update geomBindTransform display in UI
	if (window.updateGeomBindTransform) {
		if (firstGeomBindTransform) {
			window.updateGeomBindTransform(firstGeomBindTransform.elements);
		} else if (hasSkinnedMeshData) {
			window.updateGeomBindTransform(null); // Show "Identity" message
		}
	}

	// Hide geomBindTransform display if no skinned mesh data
	if (!hasSkinnedMeshData && window.updateGeomBindTransform) {
		window.updateGeomBindTransform(undefined); // Hide the section
	}

	console.log(`upAxis: "${fileUpAxis}"`);
	console.log('========================');

	// Build skeleton and bone map data from shared library module.
	const skeletonBuild = buildSkeletonDataFromUSD(usd_scene, {
		logger: console,
		hasSkinnedMeshData,
		onSkeletonInfo: window.updateSkeletonInfo || null
	});
	const skeletonDataArray = skeletonBuild.skeletonDataArray;
	let bones = skeletonBuild.firstBones;
	boneMaps = skeletonBuild.boneMaps;
	_totalLoadedBones = skeletonDataArray.reduce((sum, skel) => {
		const count = skel?.bones?.length ?? skel?.joints?.length ?? 0;
		return sum + count;
	}, 0);
	_enableBoneInterpolation = _totalLoadedBones <= BONE_INTERPOLATION_MAX_BONES;
	if (!_enableBoneInterpolation) {
		_clearBoneInterpData();
		console.log(
			`Sub-frame bone interpolation disabled for large rig (${_totalLoadedBones} bones > ${BONE_INTERPOLATION_MAX_BONES})`
		);
	}

	// Create default material
	const defaultMtl = LightUSDLoaderUtils.createDefaultMaterial();

	const options = {
		overrideMaterial: false,
		envMap: null,
		envMapIntensity: 1.0,
		sourceFileName: filename,
	};

	// Monitor WASM heap for growth during scene building.
	// If memory.grow() is called, ALL typed_memory_view ArrayBuffers are detached.
	const wasmHeapBefore = typeof WebAssembly !== 'undefined' && WebAssembly.Memory ?
		(window._lightusd_wasm_memory ? window._lightusd_wasm_memory.buffer.byteLength : null) : null;

	// Build Three.js node from USD. Core scenes are already materialized from
	// RenderScene data; legacy scenes are built from the USD node tree.
	let threeNode = coreBuiltNode;
	if (!threeNode) {
		const usdRootNode = usd_scene.getDefaultRootNode();
		threeNode = await LightUSDLoaderUtils.buildThreeNode(usdRootNode, defaultMtl, usd_scene, options);
	}

	if (wasmHeapBefore !== null && window._lightusd_wasm_memory) {
		const wasmHeapAfter = window._lightusd_wasm_memory.buffer.byteLength;
		if (wasmHeapAfter !== wasmHeapBefore) {
			console.warn(`[WASM HEAP GREW] ${wasmHeapBefore} -> ${wasmHeapAfter} bytes during buildThreeNode. Retained raw heap views would detach; getMeshCopy/getMeshPtr copies are expected to remain safe.`);
		}
	}

	// Build node index map BEFORE bones are added to the hierarchy.
	// Adding bones changes the DFS traversal order, which would break
	// the mapping from USD node indices to Three.js objects.
	// Core scenes use the RenderScene node-table map from buildThreeNode:
	// animation target_node is a table index, not a DFS index.
	const nodeIndexMap = coreNodeIndexMap || buildNodeIndexMap(threeNode);

	// Set Z-up to Y-up conversion based on file metadata.
	animationParams.convertZUp = (fileUpAxis.toUpperCase() === 'Z');
	animationParams.toggleZUp();
	console.log(`Z-up → Y-up conversion: ${animationParams.convertZUp ? 'ON' : 'OFF'} (upAxis="${fileUpAxis}")`);

	// Build skeletons, bind meshes, and create optional skeleton helpers.
	const skinningResult = applyUSDSceneSkinningPipeline({
		threeNode,
		characterGroup,
		helperScene: scene,
		skeletonDataArray,
		allSkinnedMeshUSDData,
		skinnedMeshDataByName,
		usdScene: usd_scene,
		showMesh: animationParams.showMesh,
		showSkeleton: animationParams.showSkeleton,
		skeletonDisplayMode: animationParams.skeletonDisplayMode,
		showSkeletonTips: animationParams.showSkeletonTips,
		capSkeletonTips: animationParams.capSkeletonTips,
		skeletonTipMaxLength: animationParams.skeletonTipMaxLength,
		useWASMBoneTexture: animationParams.useWASMBoneTexture,
		logger: console
	});

	skeletons = skinningResult.skeletons;
	skeletonHelpers = skinningResult.skeletonHelpers;
	allSceneMeshes = skinningResult.allSceneMeshes;
	meshVisibility = skinningResult.meshVisibility;
	if (_totalLoadedBones > SHADOWS_MAX_BONES && animationParams.enableShadows) {
		animationParams.enableShadows = false;
		_shadowsAutoDisabled = true;
		console.log(`Shadows disabled for large rig (${_totalLoadedBones} bones > ${SHADOWS_MAX_BONES}); re-enable in GUI if needed`);
	} else if (_totalLoadedBones <= SHADOWS_MAX_BONES && _shadowsAutoDisabled) {
		animationParams.enableShadows = true;
		_shadowsAutoDisabled = false;
	}
	applyShadowSettings();

	skinnedMesh = skinningResult.primaryMesh || null;
	originalMaterial = skinnedMesh ? skinnedMesh.material : null;

	if (skinningResult.hasSkeletonHelpers) {
		if (window.updateJointHierarchy) {
			window.updateJointHierarchy(generateJointHierarchy(bones));
		}
		if (typeof updateJointHierarchyGUI === 'function') {
			updateJointHierarchyGUI(bones);
		}
	} else {
		if (window.updateJointHierarchy) {
			window.updateJointHierarchy(generateJointHierarchy([]));
		}
		if (typeof updateJointHierarchyGUI === 'function') {
			updateJointHierarchyGUI([]);
		}
	}

	// Extract animations and initialize playback wiring.
	try {
		const animationData = extractUSDSceneAnimations(usd_scene, {
			boneMaps,
			nodeIndexMap,
			threeRoot: threeNode,
			timeCodesPerSecond,
			logger: console
		});

		usdAnimations = animationData.usdAnimations;
		usdNodeAnimations = animationData.usdNodeAnimations;
		animationEnabled = animationData.animationEnabled;

		animationParams.hasUSDAnimations = animationData.hasAnyAnimation;
		animationParams.usdAnimationCount = usdAnimations.length;
		animationParams.currentAnimation = 0;
		const hasMultipleAnimations =
			(animationData.animationInfos?.length || 0) > 1 ||
			(usdAnimations.length + usdNodeAnimations.length) > 1;
		animationParams.playAllAnimations = hasMultipleAnimations;
		updateSelectAnimationControllerState();

		if (animationData.disabledCount > 0) {
			console.log(`Auto-disabled ${animationData.disabledCount} animation(s) with 0 duration (rest pose only)`);
		}

		if (window.updateAnimationList) {
			if (animationData.hasAnyAnimation) {
				window.updateAnimationList(usdAnimations, animationData.animationInfos, timeCodesPerSecond);
			} else {
				window.updateAnimationList([], []);
			}
		}
		if (typeof updateAnimationCheckboxes === 'function') {
			updateAnimationCheckboxes();
		}

		usdAnimations.forEach((clip, index) => {
			const info = animationData.animationInfos[index];
			let typeStr = '';
			if (info && info.has_skeletal_animation) {
				typeStr = ' [skeletal]';
			}
			console.log(`Animation ${index}: ${clip.name}, duration: ${clip.duration} frames, tracks: ${clip.tracks.length}${typeStr}`);
		});

		if (animationData.hasAnyAnimation) {
			animationPlayback = createUSDSceneAnimationPlayback(characterGroup, {
				usdAnimations,
				usdNodeAnimations,
				speed: animationParams.speed,
				logger: console
			});

			syncPlaybackState(animationPlayback.getState());

			if (animationParams.playAllAnimations) {
				playAllAnimations();
			} else if (usdAnimations.length > 0) {
				playAnimation(0);
			} else {
				playNodeAnimations();
			}

			const maxDuration = computeUSDSceneTimelineDuration(
				endTimeCode,
				usdAnimations,
				usdNodeAnimations
			);
			if (window.updateTimelineRange && maxDuration > 0 && isFinite(maxDuration)) {
				console.log(`Setting timeline range to [${startTimeCode}, ${maxDuration}] frames (from animations and endTimeCode ${endTimeCode})`);
				window.updateTimelineRange(startTimeCode, maxDuration);
			}
		} else {
			animationPlayback = null;
			mixer = null;
			animationAction = null;
			animationActions = [];
			animationParams.playAllAnimations = false;
			updateSelectAnimationControllerState();
			console.log('No skeletal animations found in USD file (loading as static mesh)');
		}
	} catch (error) {
		console.error('Error extracting skeletal animations:', error);
		console.log('Continuing without animations...');
		animationPlayback = null;
		mixer = null;
		animationAction = null;
		animationActions = [];
		animationEnabled = [];
		animationParams.hasUSDAnimations = false;
		animationParams.usdAnimationCount = 0;
		animationParams.currentAnimation = 0;
		animationParams.playAllAnimations = false;
		updateSelectAnimationControllerState();
		if (window.updateAnimationList) {
			window.updateAnimationList([], []);
		}
		if (typeof updateAnimationCheckboxes === 'function') {
			updateAnimationCheckboxes();
		}
	}

	// Geometry, skeleton, metadata, and animation data are now JS-owned. Drop
	// the next adapter's large build-time arrays while retaining archiveEntries
	// for the lazy texture loader. delete() still runs after textures settle.
	if (isScene(usd_scene) && typeof usd_scene.releaseBuildData === 'function') {
		usd_scene.releaseBuildData();
	}

	// Update mesh list GUI
	updateMeshListGUI();

	// Update shadow camera frustum based on scene bounds
	usdSceneRoot.updateMatrixWorld(true);
	const sceneBounds = computeSceneBoundingBox(usdSceneRoot);
	updateShadowCameraFromBounds(directionalLight, sceneBounds);

	// Fit camera to scene after loading
	fitCameraToScene();

	// Release WASM scene object — all USD data must be copied into JS-owned buffers
	// BEFORE this point. The C++ destructor frees render_scene_ vectors, invalidating
	// all typed_memory_view references (mesh.points, sampler.times/values, etc.).
	// Keeping it alive retains the entire parsed USD scene in WASM heap memory.
	// Exception: next-backend lazy textures read archive bytes from the adapter
	// asynchronously; adapter.delete() clears that archive map, so it must wait
	// for texture loading to settle.
	const releaseUSDScene = () => {
		if (usd_scene && typeof usd_scene.delete === 'function') {
			usd_scene.delete();
		}
		if (window.usd_scene === usd_scene) {
			window.usd_scene = null;
		}

		// Hint GC after scene loading — processUSDScene creates many transient objects
		// (WASM data copies, temporary matrices, skeleton building intermediaries) that
		// should be collected before the animation loop starts allocating.
		hintGC();
	};
	if (coreTextureLoadPromise) {
		coreTextureLoadPromise.then(releaseUSDScene);
	} else {
		releaseUSDScene();
	}
}

/**
 * Play node (xformOp) animations alongside skeletal animations.
 * Node animations drive scene graph transforms (SkelRoot, Xform ancestors)
 * and must always play when any animation is active. With AttachedBindMode,
 * animated ancestors are handled correctly by the skinning equation.
 */
function playNodeAnimations() {
	if (!animationPlayback) return;
	syncPlaybackState(animationPlayback.playNodeAnimations());
}

/**
 * Play animation by index
 * @param {number} index - Animation index
 */
function playAnimation(index) {
	if (!animationPlayback || index < 0 || index >= usdAnimations.length) {
		return;
	}

	const result = animationPlayback.playAnimation(index);
	syncPlaybackState(result);

	animationParams.currentAnimation = index;
	const clip = result.clip || usdAnimations[index];
	console.log(`Playing animation: ${clip.name}${usdNodeAnimations.length > 0 ? ` (+${usdNodeAnimations.length} node clip(s))` : ''}`);
}

/**
 * Play all enabled animations simultaneously (like Blender)
 */
function playAllAnimations() {
	if (!animationPlayback || (usdAnimations.length === 0 && usdNodeAnimations.length === 0)) {
		return;
	}

	const result = animationPlayback.playAllAnimations(animationEnabled);
	syncPlaybackState(result);
	const start = Number.isFinite(animationParams.timelineStart) ? animationParams.timelineStart : 0;
	syncPlaybackState(animationPlayback.setTime(start, true));
	animationParams.time = start;

	const enabledCount = result.enabledCount || 0;
	const skippedCount = result.skippedCount || 0;
	console.log(`Playing ${enabledCount} enabled animations (${skippedCount} skipped: 0 duration, ${usdAnimations.length} skeletal + ${usdNodeAnimations.length} node total)`);
}

/**
 * Stop all active animations
 */
function stopAllAnimations() {
	if (!animationPlayback) {
		animationAction = null;
		animationActions = [];
		return;
	}
	syncPlaybackState(animationPlayback.stopAllAnimations());
}

// Listen for file upload events
window.addEventListener('loadUSDFile', async (event) => {
	const file = event.detail.file;
	if (!file) return;

	try {
		currentVariantSelection = null;
		const arrayBuffer = await file.arrayBuffer();
		await loadUSDFromArrayBuffer(arrayBuffer, file.name);
		console.log('USD file loaded successfully:', file.name);
	} catch (error) {
		console.error('Failed to load USD file:', error);
	}
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

const reloadVariantButton = document.getElementById('reloadVariantButton');
if (reloadVariantButton) {
	reloadVariantButton.addEventListener('click', async () => {
		const selectEl = document.getElementById('variantSelect');
		if (!selectEl?.value) return;

		try {
			const selection = JSON.parse(selectEl.value);
			await reloadCurrentUSDWithVariant(selection);
			console.log(
				`Reloaded variant: ${selection.primPath} ${selection.variantSet}=${selection.variantName}`
			);
		} catch (error) {
			console.error('Failed to reload selected variant:', error);
		}
	});
}

const variantSelect = document.getElementById('variantSelect');
if (variantSelect) {
	variantSelect.addEventListener('change', () => {
		if (!variantSelect.value) {
			currentVariantSelection = null;
			return;
		}
		currentVariantSelection = JSON.parse(variantSelect.value);
	});
}

// Load USD model on startup. `?uri=/models/foo.usdz` overrides the built-in default.
loadUSDModelFromURI(getStartupUSDModelURI()).catch((error) => {
	console.error('Failed to load startup USD file:', error);
	console.error('Error details:', error.message || error);
	console.log('Please upload a USD file (with or without skeletal animation).');
	// Update UI to show error
	const currentFileElement = document.getElementById('currentFile');
	if (currentFileElement) {
		currentFileElement.textContent = 'Failed to load (see console)';
		currentFileElement.style.color = '#ff6b6b';
	}
});

// Listen for mouse clicks for joint selection
window.addEventListener('click', onMouseClick);

// Keyboard shortcuts for transform modes
window.addEventListener('keydown', (event) => {
	// Don't trigger if user is typing in an input field
	if (event.target.tagName === 'INPUT' || event.target.tagName === 'TEXTAREA') {
		return;
	}

	switch(event.key.toLowerCase()) {
		case 'g': // Translate/Grab
			animationParams.transformMode = 'translate';
			animationParams.setTransformMode();
			console.log('Transform mode: Translate');
			break;
		case 'r': // Rotate
			animationParams.transformMode = 'rotate';
			animationParams.setTransformMode();
			console.log('Transform mode: Rotate');
			break;
		case 's': // Scale
			if (!event.ctrlKey && !event.metaKey) { // Avoid conflicts with Save
				animationParams.transformMode = 'scale';
				animationParams.setTransformMode();
				console.log('Transform mode: Scale');
			}
			break;
		case 'escape': // Deselect
			deselectJoint();
			console.log('Joint deselected');
			break;
		case 'x': // Toggle local/world space
			animationParams.transformSpace = animationParams.transformSpace === 'local' ? 'world' : 'local';
			animationParams.setTransformSpace();
			console.log(`Transform space: ${animationParams.transformSpace}`);
			break;
		case ' ': // Space = Play/Pause
			event.preventDefault();
			animationParams.playPause();
			console.log(`Animation ${animationParams.isPlaying ? 'playing' : 'paused'}`);
			break;
		case 'h': // Hide/Show GUI
			animationParams.toggleGUI();
			break;
	}
});

// Expose selectJointByName for UI hierarchy clicks
window.selectJointByName = selectJointByName;

function applyTimelineRange(start, end, options = {}) {
	const updateBaseRange = options.updateBaseRange === true;
	const clampCurrentTime = options.clampCurrentTime !== false;
	let timelineStart = Number(start);
	let timelineEnd = Number(end);

	if (!isFinite(timelineStart)) timelineStart = 0;
	if (!isFinite(timelineEnd)) timelineEnd = timelineStart + 1;
	if (timelineEnd <= timelineStart) timelineEnd = timelineStart + 1;
	if (updateBaseRange) {
		timelineBaseStart = timelineStart;
		timelineBaseEnd = timelineEnd;
	}

	if (animationParams) {
		animationParams.timelineStart = timelineStart;
		animationParams.timelineEnd = timelineEnd;
	}

	if (timelineController) {
		timelineController._min = timelineStart;
		timelineController._max = timelineEnd;
		if (timelineController.$slider) {
			timelineController.$slider.min = timelineStart;
			timelineController.$slider.max = timelineEnd;
		}
		if (timelineController.$input) {
			timelineController.$input.min = timelineStart;
			timelineController.$input.max = timelineEnd;
		}
	}

	if (animationParams && clampCurrentTime) {
		const clampedTime = Math.min(Math.max(animationParams.time, timelineStart), timelineEnd);
		if (clampedTime !== animationParams.time) {
			animationParams.time = clampedTime;
			if (animationPlayback) {
				syncPlaybackState(animationPlayback.setTime(
					animationParams.time,
					!animationParams.isPlaying
				));
			}
		}
	}

	if (timelineController) timelineController.updateDisplay();
	if (timelineStartController) timelineStartController.updateDisplay();
	if (timelineEndController) timelineEndController.updateDisplay();

	return { timelineStart, timelineEnd };
}

// Animation parameters
animationParams = {
	isPlaying: true,
	playPause: function() {
		this.isPlaying = !this.isPlaying;
		if (animationPlayback) {
			// If resuming but no active actions (e.g. after Reset to Rest Pose),
			// re-create the animation actions instead of just unpausing.
			if (this.isPlaying && !animationAction && animationActions.length === 0 &&
				(usdAnimations.length > 0 || usdNodeAnimations.length > 0)) {
				if (this.playAllAnimations) {
					playAllAnimations();
				} else if (usdAnimations.length > 0) {
					playAnimation(this.currentAnimation);
				} else {
					playNodeAnimations();
				}
			} else {
				syncPlaybackState(animationPlayback.setPaused(!this.isPlaying));
			}
		}
		if (this.isPlaying && this.playAllAnimations && window.updateTimelineRange) {
			const maxDuration = computeUSDSceneTimelineDuration(
				timelineBaseEnd,
				usdAnimations,
				usdNodeAnimations
			);
			window.updateTimelineRange(timelineBaseStart, maxDuration);
		}
		if (!this.isPlaying) {
			_mixerAccumDelta = 0;
			hintGC();
		}
	},
	reset: function() {
		if (animationPlayback) {
			// If no active actions (e.g. after Reset to Rest Pose),
			// re-create the animation actions instead of just resetting.
			if (!animationAction && animationActions.length === 0 &&
				(usdAnimations.length > 0 || usdNodeAnimations.length > 0)) {
				if (this.playAllAnimations) {
					playAllAnimations();
				} else if (usdAnimations.length > 0) {
					playAnimation(this.currentAnimation);
				} else {
					playNodeAnimations();
				}
				this.isPlaying = true;
			} else {
				syncPlaybackState(animationPlayback.reset());
			}
		}
	},
	resetToRestPose: function() {
		// Stop all animations
		stopAllAnimations();
		this.isPlaying = false;

		// Reset all skeletons to rest pose
		for (const [skelId, skel] of skeletons) {
			resetSkeletonToRestPose(skel);
		}
		_mixerAccumDelta = 0;
		_clearBoneInterpData();
		hintGC();
	},
	speed: 24,  // Default to timeCodesPerSecond (updated on file load)
	time: 0,
	timelineStart: 0,
	timelineEnd: 30,

	// Skeletal animation properties
	hasUSDAnimations: false,
	usdAnimationCount: 0,
	currentAnimation: 0,
	selectAnimation: function() {
		if (this.hasUSDAnimations && this.currentAnimation < usdAnimations.length) {
			playAnimation(this.currentAnimation);
		}
	},
	playAllAnimations: false,
	togglePlayAllAnimations: function() {
		const baseStart = Number.isFinite(timelineBaseStart) ? timelineBaseStart : 0;
		const baseEnd = Number.isFinite(timelineBaseEnd) ? timelineBaseEnd : baseStart + 1;

		if (this.playAllAnimations) {
			// Switch to play all mode
			playAllAnimations();
			// Keep USD stage timeline end as baseline, then extend if clips run longer.
			const maxDuration = computeUSDSceneTimelineDuration(
				baseEnd,
				usdAnimations,
				usdNodeAnimations
			);
			if (window.updateTimelineRange && maxDuration > 0) {
				window.updateTimelineRange(baseStart, maxDuration);
			}
		} else {
			// Switch back to single animation mode
			if (usdAnimations.length > 0) {
				playAnimation(this.currentAnimation);
			} else {
				playNodeAnimations();
			}
			// Preserve stage timeline range when a single clip is shorter.
			const currentDuration = usdAnimations[this.currentAnimation]?.duration;
			const singleModeEnd = Math.max(baseEnd, isFinite(currentDuration) ? currentDuration : 0);
			if (singleModeEnd > 0 && window.updateTimelineRange) {
				window.updateTimelineRange(baseStart, singleModeEnd);
			}
		}
		updateSelectAnimationControllerState();
	},

	// Visualization
	showMesh: true,
	toggleMesh: function() {
		// Toggle visibility for all tracked meshes, respecting per-mesh state
		for (const mesh of allSceneMeshes) {
			const perMesh = meshVisibility.get(mesh) !== false;
			mesh.visible = perMesh && this.showMesh;
			setMeshShadowFlags(mesh, mesh.visible);
		}
	},

	showSkeleton: false,
	toggleSkeleton: function() {
		applySkeletonHelperOptions();
	},
	showGroundGrid: true,
	toggleGroundGrid: function() {
		groundHelper.visible = this.showGroundGrid;
	},
	skeletonDisplayMode: 'full',
	updateSkeletonDisplay: function() {
		applySkeletonHelperOptions();
	},
	showSkeletonTips: true,
	toggleSkeletonTips: function() {
		applySkeletonHelperOptions();
	},
	capSkeletonTips: false,
	toggleSkeletonTipCap: function() {
		applySkeletonHelperOptions();
	},
	skeletonTipMaxLength: 120,
	updateSkeletonTipMaxLength: function() {
		applySkeletonHelperOptions();
	},

	// Camera controls
	fitToScene: function() {
		fitCameraToScene();
	},
	fitToAllFrames: function() {
		fitCameraToAllFrames();
	},

	// Debug visualization
	showJoints: false,
	toggleJoints: function() {
		if (this.showJoints) {
			// Lazy-create joint spheres on first toggle
			const firstSkeleton = skeletons.get(0);
			if (jointSpheres.length === 0 && firstSkeleton && firstSkeleton.bones.length > 0) {
				jointSpheres = createJointSpheres(firstSkeleton.bones);
			}
			jointSpheres.forEach(sphere => {
				sphere.visible = true;
				if (!sphere.parent) scene.add(sphere);
			});
		} else {
			// Remove from scene to avoid bloating scene graph traversal
			jointSpheres.forEach(sphere => scene.remove(sphere));
		}
	},

	// CPU skinning debug mode
	cpuSkinning: false,
	toggleCPUSkinning: function() {
		_cpuSkinEnabled = this.cpuSkinning;
		if (_cpuSkinEnabled) {
			// Disable raw mesh if enabling CPU skinning
			this.rawMesh = false;
			_rawMeshEnabled = false;
			_cleanupRawMesh();
		}
		if (!_cpuSkinEnabled) {
			_cleanupCpuSkin();
			console.log('CPU skinning disabled, GPU skinning restored');
		} else {
			console.log('CPU skinning enabled — bypassing GPU shader');
		}
	},

	// Raw mesh (no skinning) debug mode
	rawMesh: false,
	toggleRawMesh: function() {
		_rawMeshEnabled = this.rawMesh;
		if (_rawMeshEnabled) {
			// Disable CPU skinning if enabling raw mesh
			this.cpuSkinning = false;
			_cpuSkinEnabled = false;
			_cleanupCpuSkin();
		}
		if (!_rawMeshEnabled) {
			_cleanupRawMesh();
			console.log('Raw mesh disabled, GPU skinning restored');
		} else {
			console.log('Raw mesh enabled — showing original geometry (no skinning)');
		}
	},

	showWeights: false,
	weightVisualizationMode: 0, // 0: blended, 1: primary only, 2: intensity
	toggleWeights: function() {
		if (!skinnedMesh || !originalMaterial) return;

		if (this.showWeights) {
			// Check if mesh has extended skinning and use appropriate visualization
			const skinningConfig = skinnedMesh.geometry?.userData?.extendedSkinning;
			if (skinningConfig && skinningConfig.mode > SkinningMode.STANDARD) {
				// Use extended weight visualization for 8+ bone meshes
				if (!weightVisualizationMaterial || !weightVisualizationMaterial.userData?.isExtended) {
					weightVisualizationMaterial = createExtendedWeightVisualizationMaterial({
						maxInfluences: skinningConfig.maxInfluences
					});
					weightVisualizationMaterial.userData = { isExtended: true };
				}
			} else {
				// Use standard weight visualization for 4-bone meshes
				if (!weightVisualizationMaterial || weightVisualizationMaterial.userData?.isExtended) {
					weightVisualizationMaterial = createWeightVisualizationMaterial();
					weightVisualizationMaterial.userData = { isExtended: false };
				}
			}
			weightVisualizationMaterial.uniforms.visualizationMode.value = this.weightVisualizationMode;
			skinnedMesh.material = weightVisualizationMaterial;
		} else {
			skinnedMesh.material = originalMaterial;
		}
	},

	updateWeightMode: function() {
		if (this.showWeights && weightVisualizationMaterial) {
			weightVisualizationMaterial.uniforms.visualizationMode.value = this.weightVisualizationMode;
		}
	},

	// Gizmo controls
	showGizmo: true,
	transformMode: 'translate',
	transformSpace: 'local',
	toggleGizmo: function() {
		transformControls.visible = this.showGizmo && selectedJoint !== null;
	},
	setTransformMode: function() {
		transformControls.setMode(this.transformMode);
	},
	setTransformSpace: function() {
		transformControls.setSpace(this.transformSpace);
	},
	deselectJoint: function() {
		deselectJoint();
	},

	// Bone reduction settings (applied on next file load)
	enableBoneReduction: false,
	targetBoneCount: 4,

	// WASM bone texture for GPU skinning (applied on next file load)
	// When enabled, generates bone texture in WASM for efficient GPU skinning
	// with high bone counts (16+ bones per vertex)
	useWASMBoneTexture: false,

	// Z-up to Y-up conversion
	convertZUp: false,
	toggleZUp: function() {
		if (this.convertZUp) {
			characterGroup.rotation.x = -Math.PI / 2;
		} else {
			characterGroup.rotation.x = 0;
		}
	},

	ignoreMetersPerUnit: false,
	toggleMetersPerUnit: function() {
		applyMetersPerUnitScale({ refit: true });
	},

	// Extended skinning toggle (texture-based vs 4-bone fallback)
	useExtendedSkinning: true,
	toggleExtendedSkinning: function() {
		const enabled = this.useExtendedSkinning ? 1.0 : 0.0;
		for (const mesh of allSceneMeshes) {
			if (mesh._useTexSkinUniform) {
				mesh._useTexSkinUniform.value = enabled;
			}
		}
		console.log(`Extended skinning: ${this.useExtendedSkinning ? 'texture-based' : '4-bone fallback'}`);
	},

	// Shadows
	enableShadows: true,
	toggleShadows: function() {
		_shadowsAutoDisabled = false;
		applyShadowSettings();
		console.log(`Shadows ${this.enableShadows ? 'enabled' : 'disabled'}`);
	},

	// Mesh selection and bounding box
	selectedMeshName: 'None',
	showBBox: false,
	toggleBBox: function() {
		updateBBoxHelper();
	},
	selectMeshByIndex: function(index) {
		if (index < 0 || index >= allSceneMeshes.length) {
			deselectMesh();
		} else {
			selectMesh(allSceneMeshes[index]);
		}
	},
	toggleMeshVisibility: function(index) {
		if (index < 0 || index >= allSceneMeshes.length) return;
		const mesh = allSceneMeshes[index];
		const current = meshVisibility.get(mesh) !== false;
		const newState = !current;
		meshVisibility.set(mesh, newState);
		mesh.visible = newState && this.showMesh;
		setMeshShadowFlags(mesh, mesh.visible);
		updateMeshListGUI();
	},
	deselectMesh: function() {
		deselectMesh();
	},

	guiVisible: true,
	toggleGUI: function() {
		this.guiVisible = !this.guiVisible;
		const infoEl = document.getElementById('info');
		if (this.guiVisible) {
			gui.domElement.style.display = '';
			if (infoEl) infoEl.style.display = '';
		} else {
			gui.domElement.style.display = 'none';
			if (infoEl) infoEl.style.display = 'none';
		}
	},

};

// GUI setup
const gui = new GUI();
gui.title('Skeletal Animation Controls');
// Backend switch reloads the page: the loader binds its WASM module at init.
gui.add({ backend: LOADER_BACKEND }, 'backend', LOADER_BACKEND_CHOICES)
	.name('Loader Backend').onChange(setBackendAndReload);

// Wire up the GUI toggle button
document.getElementById('gui-toggle')?.addEventListener('click', () => {
	animationParams.toggleGUI();
});

// Playback controls
const playbackFolder = gui.addFolder('Playback');
playbackFolder.add(animationParams, 'playPause').name('Play / Pause');
playbackFolder.add(animationParams, 'reset').name('Reset Animation');
playbackFolder.add(animationParams, 'resetToRestPose').name('Reset to Rest Pose');
playbackFolder.add(animationParams, 'speed', 1, 120, 1).name('Speed').onChange(() => {
	if (animationPlayback) {
		syncPlaybackState(animationPlayback.setSpeed(animationParams.speed));
	}
});
timelineController = playbackFolder.add(animationParams, 'time', 0, 30, 0.01)
	.name('Timeline').onChange(() => {
		if (animationPlayback) {
			syncPlaybackState(animationPlayback.setTime(
				animationParams.time,
				!animationParams.isPlaying
			));
		}
	});
timelineStartController = playbackFolder.add(animationParams, 'timelineStart', -100000, 100000, 1)
	.name('Timeline Start')
		.onFinishChange((value) => {
		applyTimelineRange(value, animationParams.timelineEnd, { updateBaseRange: true });
	});
timelineEndController = playbackFolder.add(animationParams, 'timelineEnd', -100000, 100000, 1)
	.name('Timeline End')
		.onFinishChange((value) => {
		applyTimelineRange(animationParams.timelineStart, value, { updateBaseRange: true });
	});
applyTimelineRange(animationParams.timelineStart, animationParams.timelineEnd, {
	clampCurrentTime: false,
	updateBaseRange: true
});
playbackFolder.open();

// Function to update timeline range when animation loads
window.updateTimelineRange = function(startOrEnd, maybeEnd) {
	const start = maybeEnd === undefined
		? (Number.isFinite(timelineBaseStart) ? timelineBaseStart : 0)
		: startOrEnd;
	const end = maybeEnd === undefined ? startOrEnd : maybeEnd;
	console.log(`updateTimelineRange called with start=${start}, end=${end}`);
	const range = applyTimelineRange(start, end);
	console.log(`Timeline range updated to: [${range.timelineStart}, ${range.timelineEnd}]`);
};

// Animation controls
const animationFolder = gui.addFolder('Skeletal Animations');
animationFolder.add(animationParams, 'hasUSDAnimations').name('Has Animations').listen().disable();
animationFolder.add(animationParams, 'usdAnimationCount').name('Animation Count').listen().disable();
animationFolder.add(animationParams, 'playAllAnimations')
	.name('Play All Animations')
		.onChange(() => animationParams.togglePlayAllAnimations());

// Container for individual animation checkboxes (dynamically populated)
let animationCheckboxControllers = [];

selectAnimationController = animationFolder.add(animationParams, 'currentAnimation', 0, 10, 1)
	.name('Select Animation')
		.onChange(() => animationParams.selectAnimation());
animationFolder.open();
updateSelectAnimationControllerState();

function updateSelectAnimationControllerState() {
	if (!selectAnimationController) return;
	const shouldDisable =
		animationParams.playAllAnimations ||
		!animationParams.hasUSDAnimations ||
		usdAnimations.length === 0;
	if (shouldDisable) {
		if (typeof selectAnimationController.disable === 'function') {
			selectAnimationController.disable();
		}
	} else {
		if (typeof selectAnimationController.enable === 'function') {
			selectAnimationController.enable();
		}
	}
}

// Function to update individual animation checkboxes
function updateAnimationCheckboxes() {
	// Remove existing checkboxes
	for (const controller of animationCheckboxControllers) {
		controller.destroy();
	}
	animationCheckboxControllers = [];

	// Add checkbox for each animation
	for (let i = 0; i < usdAnimations.length; i++) {
		const clip = usdAnimations[i];
		const obj = {
			enabled: animationEnabled[i]
		};
		const controller = animationFolder.add(obj, 'enabled')
			.name(`  ☐ ${i}: ${clip.name} (${Math.round(clip.duration)} frames)`)
						.onChange((value) => {
				animationEnabled[i] = value;
				// If in play all mode, restart to apply changes
				if (animationParams.playAllAnimations) {
					playAllAnimations();
				}
			});

		// Update checkbox state when animationEnabled changes
		Object.defineProperty(obj, 'enabled', {
			get: () => animationEnabled[i],
			set: (v) => { animationEnabled[i] = v; }
		});

		animationCheckboxControllers.push(controller);
	}
	updateSelectAnimationControllerState();
}

// Visualization controls
const visualFolder = gui.addFolder('Visualization');
visualFolder.add(animationParams, 'showMesh')
	.name('Show Mesh')
	.onChange(() => animationParams.toggleMesh());
visualFolder.add(animationParams, 'showSkeleton')
	.name('Show Skeleton')
	.onChange(() => animationParams.toggleSkeleton());
visualFolder.add(animationParams, 'showGroundGrid')
	.name('Show Ground Grid')
	.onChange(() => animationParams.toggleGroundGrid());
visualFolder.add(animationParams, 'skeletonDisplayMode', {
	Full: 'full',
	'Deforming Only': 'deforming'
})
	.name('Skeleton Mode')
	.onChange(() => animationParams.updateSkeletonDisplay());
visualFolder.add(animationParams, 'showSkeletonTips')
	.name('Show Tip Bones')
	.onChange(() => animationParams.toggleSkeletonTips());
visualFolder.add(animationParams, 'capSkeletonTips')
	.name('Cap Tip Bones')
	.onChange(() => animationParams.toggleSkeletonTipCap());
visualFolder.add(animationParams, 'skeletonTipMaxLength', 1, 1000, 1)
	.name('Tip Max Length')
	.onChange(() => animationParams.updateSkeletonTipMaxLength());
visualFolder.add(animationParams, 'enableShadows')
	.name('Enable Shadows')
	.onChange(() => animationParams.toggleShadows());
visualFolder.add(animationParams, 'useExtendedSkinning')
	.name('Extended Skinning')
	.onChange(() => animationParams.toggleExtendedSkinning());
visualFolder.add(animationParams, 'convertZUp')
	.name('Z-up → Y-up')
	.onChange(() => animationParams.toggleZUp())
	.listen();
visualFolder.add(animationParams, 'ignoreMetersPerUnit')
	.name('Ignore metersPerUnit')
	.onChange(() => animationParams.toggleMetersPerUnit());
visualFolder.add(animationParams, 'fitToScene').name('Fit (Current Frame)');
visualFolder.add(animationParams, 'fitToAllFrames').name('Fit (All Frames)');
visualFolder.open();

// Mesh Selection controls
const meshFolder = gui.addFolder('Mesh Selection');
meshFolder.add(animationParams, 'selectedMeshName').name('Selected').listen().disable();
meshFolder.add(animationParams, 'showBBox')
	.name('Show BBox')
	.onChange(() => animationParams.toggleBBox());
meshFolder.add(animationParams, 'deselectMesh').name('Deselect Mesh');

// Custom container for mesh list (populated when model loads)
const meshListContainer = document.createElement('div');
meshListContainer.id = 'gui-mesh-list';
meshListContainer.style.cssText = `
	max-height: 150px;
	overflow-y: auto;
	padding: 4px 8px;
	font-family: monospace;
	font-size: 11px;
	line-height: 1.6;
	background: rgba(0, 0, 0, 0.2);
	border-radius: 3px;
	margin: 4px;
`;
meshListContainer.innerHTML = '<span style="color: #888;">No meshes loaded</span>';
try {
	if (meshFolder.$children) {
		meshFolder.$children.appendChild(meshListContainer);
	} else if (meshFolder.domElement) {
		const childrenContainer = meshFolder.domElement.querySelector('.children');
		if (childrenContainer) {
			childrenContainer.appendChild(meshListContainer);
		} else {
			meshFolder.domElement.appendChild(meshListContainer);
		}
	}
} catch (e) {
	console.warn('Could not append mesh list container to GUI:', e);
}
meshFolder.open();

/**
 * Update mesh list in GUI when model is loaded
 */
// Escape a string for safe interpolation into HTML. Mesh names come from
// untrusted USD files and must never reach innerHTML unescaped.
function escapeHtml(value) {
	return String(value)
		.replace(/&/g, '&amp;')
		.replace(/</g, '&lt;')
		.replace(/>/g, '&gt;')
		.replace(/"/g, '&quot;')
		.replace(/'/g, '&#39;');
}

function updateMeshListGUI() {
	if (!meshListContainer) return;

	if (allSceneMeshes.length === 0) {
		meshListContainer.innerHTML = '<span style="color: #888;">No meshes loaded</span>';
		return;
	}

	let html = '';
	allSceneMeshes.forEach((mesh, index) => {
		const name = mesh.name || `Mesh_${index}`;
		const isSkinned = mesh.isSkinnedMesh ? ' [skinned]' : '';
		const vertCount = mesh.geometry?.attributes?.position?.count || 0;
		const isVisible = meshVisibility.get(mesh) !== false;
		html += `<div style="display: flex; align-items: center; padding: 2px; border-radius: 3px;"
		              class="gui-mesh-item"
		              data-mesh-index="${index}"
		              onmouseover="this.style.backgroundColor='rgba(255,255,255,0.15)'"
		              onmouseout="this.style.backgroundColor='transparent'">` +
			`<span class="gui-mesh-eye" data-mesh-index="${index}"
			       style="cursor: pointer; margin-right: 6px; font-size: 14px; user-select: none; opacity: ${isVisible ? '1' : '0.35'};"
			       title="${isVisible ? 'Hide mesh' : 'Show mesh'}">${isVisible ? '\u{1F441}' : '\u{1F6AB}'}</span>` +
			`<span class="gui-mesh-name" data-mesh-index="${index}"
			       style="color: ${isVisible ? '#6bbfff' : '#666'}; cursor: pointer; flex: 1;">${escapeHtml(name)}${isSkinned} (${vertCount}v)</span></div>`;
	});

	meshListContainer.innerHTML = html;

	// Add click handlers for mesh name (selection)
	const meshNames = meshListContainer.querySelectorAll('.gui-mesh-name');
	meshNames.forEach(item => {
		item.addEventListener('click', function() {
			const meshIndex = parseInt(this.getAttribute('data-mesh-index'));
			animationParams.selectMeshByIndex(meshIndex);
		});
	});

	// Add click handlers for eye icon (visibility toggle)
	const eyeIcons = meshListContainer.querySelectorAll('.gui-mesh-eye');
	eyeIcons.forEach(item => {
		item.addEventListener('click', function(e) {
			e.stopPropagation();
			const meshIndex = parseInt(this.getAttribute('data-mesh-index'));
			animationParams.toggleMeshVisibility(meshIndex);
		});
	});
}

// Debug visualization controls
const debugFolder = gui.addFolder('Debug Visualization');
debugFolder.add(animationParams, 'showJoints')
	.name('Show Joints')
	.onChange(() => animationParams.toggleJoints());
debugFolder.add(animationParams, 'showWeights')
	.name('Show Weights')
	.onChange(() => animationParams.toggleWeights());
debugFolder.add(animationParams, 'weightVisualizationMode', {
	'Blended Colors': 0,
	'Primary Influence': 1,
	'Weight Intensity': 2
})
	.name('Weight Mode')
	.onChange(() => animationParams.updateWeightMode());
debugFolder.add(animationParams, 'cpuSkinning')
	.name('CPU Skinning')
		.onChange(() => animationParams.toggleCPUSkinning());
debugFolder.add(animationParams, 'rawMesh')
	.name('Raw Mesh (No Skin)')
		.onChange(() => animationParams.toggleRawMesh());
debugFolder.open();

// Gizmo controls
const gizmoFolder = gui.addFolder('Joint Manipulation');
gizmoFolder.add(animationParams, 'showGizmo')
	.name('Show Gizmo')
	.onChange(() => animationParams.toggleGizmo());
gizmoFolder.add(animationParams, 'transformMode', {
	'Translate': 'translate',
	'Rotate': 'rotate',
	'Scale': 'scale'
})
	.name('Transform Mode')
	.onChange(() => animationParams.setTransformMode());
gizmoFolder.add(animationParams, 'transformSpace', {
	'Local': 'local',
	'World': 'world'
})
	.name('Space')
	.onChange(() => animationParams.setTransformSpace());
gizmoFolder.add(animationParams, 'deselectJoint')
	.name('Deselect Joint');
gizmoFolder.open();

// Bone reduction settings
const boneReductionFolder = gui.addFolder('Bone Settings (Core Load)');
boneReductionFolder.add(animationParams, 'enableBoneReduction')
	.name('Enable Reduction')
	.onChange(() => {
		console.log(`Bone reduction ${animationParams.enableBoneReduction ? 'enabled' : 'disabled'} (applies on next file load)`);
	});
boneReductionFolder.add(animationParams, 'targetBoneCount', 1, 8, 1)
	.name('Target Bone Count')
	.onChange(() => {
		console.log(`Target bone count set to ${animationParams.targetBoneCount} (applies on next file load)`);
	});
boneReductionFolder.add(animationParams, 'useWASMBoneTexture')
	.name('WASM Bone Texture')
	.onChange(() => {
		console.log(`WASM bone texture ${animationParams.useWASMBoneTexture ? 'enabled' : 'disabled'} (applies on next file load)`);
	});
boneReductionFolder.close(); // Closed by default as advanced feature

// Joint Hierarchy folder (with custom HTML content)
const jointHierarchyFolder = gui.addFolder('Joint Hierarchy');

// Selected joint indicator (add this first so we can append custom content after)
const selectedJointInfo = {
	selected: 'None'
};
jointHierarchyFolder.add(selectedJointInfo, 'selected').name('Selected').listen().disable();

// Create custom container for joint hierarchy
const jointHierarchyContainer = document.createElement('div');
jointHierarchyContainer.id = 'gui-joint-hierarchy';
jointHierarchyContainer.style.cssText = `
	max-height: 200px;
	overflow-y: auto;
	padding: 4px 8px;
	font-family: monospace;
	font-size: 11px;
	line-height: 1.4;
	background: rgba(0, 0, 0, 0.2);
	border-radius: 3px;
	margin: 4px;
`;
jointHierarchyContainer.innerHTML = '<span style="color: #888;">No skeleton loaded</span>';

// Safely append custom container to folder
try {
	// Try $children first (lil-gui internal)
	if (jointHierarchyFolder.$children) {
		jointHierarchyFolder.$children.appendChild(jointHierarchyContainer);
	} else if (jointHierarchyFolder.domElement) {
		// Fallback: find children container in domElement
		const childrenContainer = jointHierarchyFolder.domElement.querySelector('.children');
		if (childrenContainer) {
			childrenContainer.appendChild(jointHierarchyContainer);
		} else {
			jointHierarchyFolder.domElement.appendChild(jointHierarchyContainer);
		}
	}
} catch (e) {
	console.warn('Could not append joint hierarchy container to GUI:', e);
}

jointHierarchyFolder.open();

// Function to update joint hierarchy in GUI
function updateJointHierarchyGUI(bones) {
	if (!jointHierarchyContainer) return;

	if (bones.length === 0) {
		jointHierarchyContainer.innerHTML = '<span style="color: #888;">No skeleton loaded</span>';
		return;
	}

	jointHierarchyContainer.innerHTML = buildJointHierarchyHTML(bones, {
		wrap: false,
		itemClassName: 'gui-joint-item',
		hoverBackground: 'rgba(255,255,255,0.15)',
		rootColor: '#ff6b6b',
		childColor: '#4ecdc4'
	});

	// Add click handlers
	const jointItems = jointHierarchyContainer.querySelectorAll('.gui-joint-item');
	jointItems.forEach(item => {
		item.addEventListener('click', function() {
			const boneName = this.getAttribute('data-bone-name');
			selectJointByName(boneName);
		});
	});
}

// Function to update selected joint display in GUI
function updateSelectedJointGUI(jointName) {
	selectedJointInfo.selected = jointName || 'None';
}

// Info folder
const infoFolder = gui.addFolder('Info');
const info = {
	fps: 0,
	objects: scene.children.length || 0,
	heapMB: 0
};
infoFolder.add(info, 'fps').name('FPS').disable();
infoFolder.add(info, 'objects').name('Objects').disable();
if (performance.memory) {
	infoFolder.add(info, 'heapMB').name('Heap (MB)').disable();
}
infoFolder.open();

// Window resize handler
window.addEventListener('resize', onWindowResize, false);

function onWindowResize() {
	camera.aspect = window.innerWidth / window.innerHeight;
	camera.updateProjectionMatrix();
	renderer.setSize(window.innerWidth, window.innerHeight);
}

// FPS calculation
let lastTime = performance.now();
let frames = 0;
let fpsUpdateTime = 0;
let _lastGCHintTime = 0; // For periodic GC hints during playback

// Animation loop
function animate() {
	requestAnimationFrame(animate);

	const currentTime = performance.now();
	const deltaTime = (currentTime - lastTime) / 1000; // Convert to seconds
	lastTime = currentTime;

	// Update FPS
	frames++;
	fpsUpdateTime += deltaTime;
	if (fpsUpdateTime >= 0.5) {
		const calculatedFps = Math.round(frames / fpsUpdateTime);
		info.fps = isFinite(calculatedFps) ? calculatedFps : 0;
		if (performance.memory) {
			const calculatedHeap = Math.round(performance.memory.usedJSHeapSize / (1024 * 1024));
			info.heapMB = isFinite(calculatedHeap) ? calculatedHeap : 0;
		}
		info.objects = scene.children.length || 0;
		frames = 0;
		fpsUpdateTime = 0;

		// Batch-update all lil-gui controllers at low rate (replaces per-frame
		// .listen() polling which triggered expensive DOM reflows every frame).
		// Skip entirely when GUI is hidden to avoid unnecessary DOM work.
		if (animationParams.guiVisible) {
			for (const c of gui.controllersRecursive()) {
				c.updateDisplay();
			}
		}
	}

	// Animation update: timeline advances every frame by real time (smooth
	// timeline display), mixer evaluates at throttled rate to limit Three.js
	// interpolant allocations, bone interpolation fills intermediate frames.
	if (mixer && animationParams.isPlaying) {
		const mixerInterval = currentMixerUpdateInterval();
		_mixerAccumDelta += deltaTime;

		// Advance timeline by real time every frame
		const start = Number.isFinite(animationParams.timelineStart) ? animationParams.timelineStart : 0;
		const end = Number.isFinite(animationParams.timelineEnd) ? animationParams.timelineEnd : (start + 1);
		const span = Math.max(end - start, 1e-6);
		const prevTimeValue = animationParams.time;
		const advancedTime = animationParams.time + (deltaTime * animationParams.speed);
		animationParams.time = start + (((advancedTime - start) % span) + span) % span;

		// Detect timeline loop-wrap (time jumped backwards by more than half the span).
		// Clear bone interpolation data to prevent lerping between end-of-loop and
		// start-of-loop poses, and force an immediate mixer tick so the correct
		// post-wrap pose is displayed without a glitch frame.
		if (animationParams.time < prevTimeValue - span * 0.5) {
			_clearBoneInterpData();
			_mixerAccumDelta = mixerInterval; // force mixer tick below
		}

		if (_mixerAccumDelta >= mixerInterval) {
			_mixerAccumDelta -= mixerInterval;
			// Clamp remainder to avoid spiral-of-death if frame rate drops
			if (_mixerAccumDelta > mixerInterval) _mixerAccumDelta = mixerInterval;

			// Evaluate pose at current timeline time
			if (animationPlayback) {
				syncPlaybackState(animationPlayback.setTime(animationParams.time, true));
			}

			// Snapshot bone transforms after mixer evaluation only when
			// interpolation is enabled. Very large rigs spend more time
			// interpolating every rendered frame than they save in smoothness.
			if (_enableBoneInterpolation) {
				for (const [skelId, skel] of skeletons) {
					_snapshotBones(skelId, skel.bones);
				}
			}
		} else if (_enableBoneInterpolation && skeletons.size > 0) {
			// Intermediate frame: lerp/slerp between prev and curr snapshots
			const alpha = _mixerAccumDelta / mixerInterval;
			for (const [skelId, skel] of skeletons) {
				_lerpBones(skelId, skel.bones, alpha);
			}
		}
	}

	// Update skeleton helpers (all of them)
	for (const helper of skeletonHelpers) {
		if (helper && helper.visible && typeof helper.update === 'function') {
			helper.update();
		}
	}

	// Update joint spheres
	if (animationParams.showJoints && jointSpheres.length > 0) {
		updateJointSpheres();
	}

	// Update transform controls position if a joint is selected
	if (selectedJoint && transformControls && transformControls.visible) {
		if (typeof transformControls.updateMatrixWorld === 'function') {
			transformControls.updateMatrixWorld();
		}
	}

	// Update bounding box helper (needs to track skinning deformation)
	if (animationParams.showBBox && bboxHelper) {
		updateBBoxHelper();
	}

	// CPU skinning debug path: compute positions on CPU, render via a
	// separate non-skinned debug mesh (avoids fighting Three.js GPU pipeline)
	if (_cpuSkinEnabled && skeletons.size > 0) {
		scene.updateMatrixWorld(true);
		skeletons.get(0)?.update();

		for (const mesh of allSceneMeshes) {
			if (!mesh.isSkinnedMesh || !mesh.skeleton) continue;

			// Create debug mesh lazily
			if (!mesh._cpuDebugMesh) {
				const debugGeo = mesh.geometry.clone();
				const debugMat = mesh.material.clone();
				const debugMesh = new THREE.Mesh(debugGeo, debugMat);
				debugMesh.name = mesh.name + '_cpuSkin';
				setMeshShadowFlags(debugMesh, animationParams.showMesh);
				// Place at mesh's world transform
				debugMesh.matrixAutoUpdate = false;
				mesh._cpuDebugMesh = debugMesh;
				// Snapshot original positions from the source geometry
				mesh._cpuOrigPos = new Float32Array(mesh.geometry.attributes.position.array);
				if (mesh.geometry.attributes.normal) {
					mesh._cpuOrigNorm = new Float32Array(mesh.geometry.attributes.normal.array);
				}
				scene.add(debugMesh);
			}

			// Sync world transform
			mesh._cpuDebugMesh.matrix.copy(mesh.matrixWorld);
			mesh._cpuDebugMesh.matrixWorld.copy(mesh.matrixWorld);
			mesh._cpuDebugMesh.visible = animationParams.showMesh;
			mesh.visible = false; // hide GPU-skinned mesh

			// Compute CPU skinning into debug mesh geometry
			const srcGeo = mesh.geometry;
			const dstGeo = mesh._cpuDebugMesh.geometry;
			const srcPos = mesh._cpuOrigPos;
			const dstPosAttr = dstGeo.attributes.position;
			const dstNormAttr = dstGeo.attributes.normal;
			const skinIdx = srcGeo.attributes.skinIndex;
			const skinWt  = srcGeo.attributes.skinWeight;
			const skel = mesh.skeleton;

			const bindMat = mesh.bindMatrix;
			const bindMatInv = mesh.bindMatrixInverse;

			// Pre-compute boneMatrix[b] = bone.matrixWorld * boneInverses[b]
			if (!mesh._cpuBoneMatrices) {
				mesh._cpuBoneMatrices = [];
				for (let b = 0; b < skel.bones.length; b++) {
					mesh._cpuBoneMatrices.push(new THREE.Matrix4());
				}
			}
			for (let b = 0; b < skel.bones.length; b++) {
				mesh._cpuBoneMatrices[b].multiplyMatrices(skel.bones[b].matrixWorld, skel.boneInverses[b]);
			}

			const vSrc = new THREE.Vector4();
			const vTmp = new THREE.Vector3();
			const vOut = new THREE.Vector3();

			for (let i = 0; i < dstPosAttr.count; i++) {
				// bindMatrix * original_pos
				vSrc.set(srcPos[i*3], srcPos[i*3+1], srcPos[i*3+2], 1).applyMatrix4(bindMat);

				vOut.set(0, 0, 0);
				for (let j = 0; j < skinIdx.itemSize; j++) {
					const bi = skinIdx.getComponent(i, j);
					const wt = skinWt.getComponent(i, j);
					if (wt === 0) continue;
					if (bi >= 0 && bi < skel.bones.length) {
						vTmp.set(vSrc.x, vSrc.y, vSrc.z).applyMatrix4(mesh._cpuBoneMatrices[bi]);
						vOut.addScaledVector(vTmp, wt);
					}
				}

				// bindMatrixInverse * accumulated
				vOut.applyMatrix4(bindMatInv);

				dstPosAttr.setXYZ(i, vOut.x, vOut.y, vOut.z);
			}

			dstPosAttr.needsUpdate = true;
			dstGeo.computeVertexNormals(); // recompute normals from new positions
			dstGeo.computeBoundingSphere();
		}
	}

	// Raw mesh debug path: show original geometry with mesh's world transform,
	// no skinning applied at all.  Useful to verify mesh data is correct.
	if (_rawMeshEnabled) {
		scene.updateMatrixWorld(true);

		for (const mesh of allSceneMeshes) {
			if (!mesh.isSkinnedMesh) continue;

			// Create raw debug mesh lazily
			if (!mesh._rawDebugMesh) {
				const debugGeo = mesh.geometry.clone();
				const debugMat = mesh.material.clone();
				const debugMesh = new THREE.Mesh(debugGeo, debugMat);
				debugMesh.name = mesh.name + '_rawMesh';
				setMeshShadowFlags(debugMesh, animationParams.showMesh);
				debugMesh.matrixAutoUpdate = false;
				mesh._rawDebugMesh = debugMesh;
				scene.add(debugMesh);
				console.log(`Raw mesh created: ${debugMesh.name}, ${debugGeo.attributes.position.count} verts`);
			}

			// Position at mesh's world transform (includes hierarchy: Z_UP, Armature, etc.)
			mesh._rawDebugMesh.matrix.copy(mesh.matrixWorld);
			mesh._rawDebugMesh.matrixWorld.copy(mesh.matrixWorld);
			mesh._rawDebugMesh.visible = animationParams.showMesh;
			mesh.visible = false; // hide GPU-skinned mesh
		}
	}

	// Update controls
	controls.update();

	// Update object count
	info.objects = scene.children.length;

	// Render
	renderer.render(scene, camera);

	// Periodic GC hint during animation playback.
	// mixer.update() generates transient allocations each frame; without periodic GC,
	// V8 accumulates garbage and eventually triggers a long pause or heap growth.
	if (animationParams.isPlaying && skeletons.size > 0) {
		if (currentTime - _lastGCHintTime > 10000) { // every 10 seconds
			_lastGCHintTime = currentTime;
			hintGC();
		}
	}
}

// Start animation loop
animate();
