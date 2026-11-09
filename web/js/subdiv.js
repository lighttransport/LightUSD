import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import GUI from 'three/examples/jsm/libs/lil-gui.module.min.js';

import {
	getBackendFromURL,
	LOADER_BACKEND_CHOICES,
	setBackendAndReload
} from './src/lightusd/LoaderConfigUtils.js';

// WASM module selection: backend=next / wasm=next load the next-only module
// (both export SubdivStreamer); default stays the legacy module.
async function importLightUSDModule() {
	const params = new URLSearchParams(window.location.search);
	const useNext = params.get('backend') === 'next' || params.get('wasm') === 'next';
	const glue = useNext ? './src/lightusd/lightusd_next.js' : './src/lightusd/lightusd.js';
	const module = await import(/* @vite-ignore */ new URL(glue, import.meta.url).href);
	return module.default;
}

// ===========================================================================
// Scene Setup
// ===========================================================================

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x1a1a2e);

const camera = new THREE.PerspectiveCamera(
	60, window.innerWidth / window.innerHeight, 0.1, 1000);
camera.position.set(3, 3, 5);
camera.lookAt(0, 0, 0);

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFShadowMap;
document.body.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
controls.dampingFactor = 0.05;
controls.minDistance = 2;
controls.maxDistance = 20;

scene.add(new THREE.AmbientLight(0x808080, 0.7));
const directionalLight1 = new THREE.DirectionalLight(0xffffff, 0.7);
directionalLight1.position.set(5, 10, 7);
directionalLight1.castShadow = true;
scene.add(directionalLight1);
const directionalLight2 = new THREE.DirectionalLight(0x6bb6ff, 0.3);
directionalLight2.position.set(-5, 5, -5);
scene.add(directionalLight2);

scene.add(new THREE.GridHelper(10, 10, 0x444444, 0x222222));
scene.add(new THREE.AxesHelper(2));

// ===========================================================================
// Control meshes (typed arrays handed to the wasm subdivider)
//
// UV meshes additionally carry a stride-2 faceVarying UV channel:
//   uvValues  (Float32Array)  - the UV value table
//   uvIndices (Uint32Array)   - per face-corner index into uvValues
// streamed through tinysubdiv's linear faceVarying ("all" mode).
// ===========================================================================

function makeControlMesh(vertices, faces) {
	const points = new Float32Array(vertices.length * 3);
	for (let i = 0; i < vertices.length; i++) {
		points[i * 3] = vertices[i][0];
		points[i * 3 + 1] = vertices[i][1];
		points[i * 3 + 2] = vertices[i][2];
	}
	const fvc = new Uint32Array(faces.length);
	let n = 0;
	for (const f of faces) n += f.length;
	const fvi = new Uint32Array(n);
	let k = 0;
	for (let i = 0; i < faces.length; i++) {
		fvc[i] = faces[i].length;
		for (const v of faces[i]) fvi[k++] = v;
	}
	return { points, fvc, fvi, uvValues: null, uvIndices: null };
}

function cubeControlMesh() {
	return makeControlMesh(
		[[-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1],
		 [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1]],
		[[0, 1, 2, 3], [4, 7, 6, 5], [0, 4, 5, 1],
		 [2, 6, 7, 3], [0, 3, 7, 4], [1, 5, 6, 2]]);
}

function icosahedronControlMesh() {
	const t = (1 + Math.sqrt(5)) / 2;
	const s = 1 / Math.sqrt(1 + t * t);
	const v = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0],
		[0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t],
		[t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]]
		.map(p => p.map(x => x * s));
	const f = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11],
		[1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
		[3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9],
		[4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]];
	return makeControlMesh(v, f);
}

function tetrahedronControlMesh() {
	return makeControlMesh(
		[[1, 1, 1], [-1, -1, 1], [-1, 1, -1], [1, -1, -1]],
		[[0, 1, 2], [0, 3, 1], [0, 2, 3], [1, 3, 2]]);
}

function gridControlMesh(n) {
	const verts = [];
	for (let y = 0; y <= n; y++) {
		for (let x = 0; x <= n; x++) {
			const u = (x / n) * 2 - 1;
			const v = (y / n) * 2 - 1;
			verts.push([u, v, 0.3 * Math.sin(u * 3) * Math.cos(v * 3)]);
		}
	}
	const w = n + 1;
	const faces = [];
	for (let y = 0; y < n; y++) {
		for (let x = 0; x < n; x++) {
			faces.push([y * w + x, y * w + x + 1, (y + 1) * w + x + 1, (y + 1) * w + x]);
		}
	}
	return makeControlMesh(verts, faces);
}

// Wavy NxN grid with a single continuous [0,1]^2 UV chart (no seams) -- linear
// faceVarying interpolation reproduces it exactly.
function uvGridControlMesh(n) {
	const verts = [];
	const uv = [];
	for (let y = 0; y <= n; y++) {
		for (let x = 0; x <= n; x++) {
			const u = (x / n) * 2 - 1;
			const v = (y / n) * 2 - 1;
			verts.push([u, v, 0.35 * Math.sin(u * 3) * Math.cos(v * 3)]);
			uv.push(x / n, y / n);
		}
	}
	const w = n + 1;
	const faces = [];
	for (let y = 0; y < n; y++) {
		for (let x = 0; x < n; x++) {
			faces.push([y * w + x, y * w + x + 1, (y + 1) * w + x + 1, (y + 1) * w + x]);
		}
	}
	const m = makeControlMesh(verts, faces);
	m.uvValues = new Float32Array(uv);  // per vertex
	m.uvIndices = m.fvi;                // per-corner UV == vertex UV (continuous)
	return m;
}

// Cube where every face maps to the full [0,1]^2 chart -> visible texture
// seams at the face edges (linear/"all" UVs cannot blend across them).
function uvCubeControlMesh() {
	const m = cubeControlMesh();  // 6 quads, 24 corners
	m.uvValues = new Float32Array([0, 0, 1, 0, 1, 1, 0, 1]);
	const idx = new Uint32Array(m.fvi.length);
	for (let f = 0; f < m.fvc.length; f++) {
		for (let k = 0; k < 4; k++) idx[f * 4 + k] = k;
	}
	m.uvIndices = idx;
	return m;
}

// ===========================================================================
// WASM streaming subdivision
// ===========================================================================

let Module = null;
let streamer = null;

const SCHEME_ID = { catmullclark: 0, loop: 1, bilinear: 2 };
const BOUNDARY_ID = { edgeAndCorner: 0, edgeOnly: 1, none: 2 };

function concatTyped(chunks, total, Ctor) {
	const out = new Ctor(total);
	let off = 0;
	for (const c of chunks) { out.set(c, off); off += c.length; }
	return out;
}

// Runs the wasm streaming refinement. Without UVs: indexed (deduped) geometry.
// With UVs: faceVarying UVs are per-corner, so the batches are expanded into a
// non-indexed mesh (each triangle corner is a standalone vertex with its UV).
function refine(cm, opts) {
	const textured = !!opts.uvValues;

	const posChunks = [], nrmChunks = [], idxChunks = [];
	let vertOffset = 0, totalVerts = 0;
	const expPos = [], expNrm = [], expUv = [];
	let totalCorners = 0;
	let totalIdx = 0, batches = 0;

	const onBatch = (pos, nrm, idx, fsrc, uv, nverts, nfaces, bidx) => {
		batches++;
		if (textured && uv) {
			const ni = idx.length;
			const p = new Float32Array(ni * 3);
			const nn = nrm ? new Float32Array(ni * 3) : null;
			const u = new Float32Array(ni * 2);
			for (let i = 0; i < ni; i++) {
				const vi = idx[i];
				p[i * 3] = pos[vi * 3]; p[i * 3 + 1] = pos[vi * 3 + 1]; p[i * 3 + 2] = pos[vi * 3 + 2];
				if (nn) { nn[i * 3] = nrm[vi * 3]; nn[i * 3 + 1] = nrm[vi * 3 + 1]; nn[i * 3 + 2] = nrm[vi * 3 + 2]; }
				u[i * 2] = uv[i * 2]; u[i * 2 + 1] = uv[i * 2 + 1];
			}
			expPos.push(p); if (nn) expNrm.push(nn); expUv.push(u);
			totalCorners += ni;
			totalIdx += ni;
		} else {
			posChunks.push(pos.slice());
			if (nrm) nrmChunks.push(nrm.slice());
			const off = new Uint32Array(idx.length);
			for (let i = 0; i < idx.length; i++) off[i] = idx[i] + vertOffset;
			idxChunks.push(off);
			vertOffset += nverts; totalVerts += nverts; totalIdx += idx.length;
		}
	};

	const t0 = performance.now();
	const err = streamer.refineStream(
		cm.points, cm.fvc, cm.fvi,
		textured ? opts.uvValues : null,
		textured ? (opts.uvIndices || null) : null,
		opts.uvInterp || 0,
		opts.scheme, opts.boundary, opts.level, opts.batchFaces,
		opts.blockFaces || 0, opts.haloRings || 0,
		opts.wantNormals, onBatch);
	const t1 = performance.now();
	if (err) { throw new Error(err); }

	const common = { batches, timeMs: t1 - t0, heapBytes: streamer.heapBytes(), numTris: totalIdx / 3 };
	if (textured) {
		return {
			...common, textured: true, numVerts: totalCorners,
			positions: concatTyped(expPos, totalCorners * 3, Float32Array),
			normals: opts.wantNormals ? concatTyped(expNrm, totalCorners * 3, Float32Array) : null,
			uvs: concatTyped(expUv, totalCorners * 2, Float32Array),
		};
	}
	return {
		...common, textured: false, numVerts: totalVerts,
		positions: concatTyped(posChunks, totalVerts * 3, Float32Array),
		normals: opts.wantNormals ? concatTyped(nrmChunks, totalVerts * 3, Float32Array) : null,
		indices: concatTyped(idxChunks, totalIdx, Uint32Array),
	};
}

// ===========================================================================
// Procedural UV-checker texture (CanvasTexture, no asset files)
// ===========================================================================

let textureCache = { size: 0, tex: null };
function getTexture() {
	if (textureCache.tex && textureCache.size === state.textureSize) return textureCache.tex;
	if (textureCache.tex) textureCache.tex.dispose();
	const size = state.textureSize;
	const cv = document.createElement('canvas');
	cv.width = cv.height = size;
	const c = cv.getContext('2d');
	const cells = 8, cell = size / cells;
	for (let j = 0; j < cells; j++) {
		for (let i = 0; i < cells; i++) {
			c.fillStyle = ((i + j) & 1) ? '#6bb6ff' : '#16243d';
			c.fillRect(i * cell, j * cell, cell, cell);
		}
	}
	c.strokeStyle = 'rgba(255,255,255,0.55)';
	c.lineWidth = Math.max(1, size / 512);
	for (let i = 0; i <= cells; i++) {
		c.beginPath(); c.moveTo(i * cell, 0); c.lineTo(i * cell, size); c.stroke();
		c.beginPath(); c.moveTo(0, i * cell); c.lineTo(size, i * cell); c.stroke();
	}
	// UV origin marker (red dot at (0,0)).
	c.fillStyle = '#ff5a5a';
	c.beginPath(); c.arc(0, 0, size / 24, 0, Math.PI * 2); c.fill();
	const tex = new THREE.CanvasTexture(cv);
	tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
	tex.colorSpace = THREE.SRGBColorSpace;
	const maxA = renderer.capabilities.getMaxAnisotropy ? renderer.capabilities.getMaxAnisotropy() : 1;
	tex.anisotropy = maxA;
	tex.needsUpdate = true;
	textureCache = { size, tex };
	return tex;
}

// ===========================================================================
// THREE.js geometry
// ===========================================================================

function buildMesh(refined, wireframe) {
	const g = new THREE.BufferGeometry();
	g.setAttribute('position', new THREE.BufferAttribute(refined.positions, 3));
	if (refined.textured) {
		g.setAttribute('uv', new THREE.BufferAttribute(refined.uvs, 2));
	} else {
		g.setIndex(new THREE.BufferAttribute(refined.indices, 1));
	}
	if (refined.normals) {
		g.setAttribute('normal', new THREE.BufferAttribute(refined.normals, 3));
	} else {
		g.computeVertexNormals();
	}

	let material;
	if (wireframe) {
		material = new THREE.MeshBasicMaterial({ color: 0x00ff00, wireframe: true });
	} else if (refined.textured) {
		material = new THREE.MeshStandardMaterial({
			map: getTexture(), roughness: 0.6, metalness: 0.05, side: THREE.DoubleSide });
	} else {
		material = new THREE.MeshStandardMaterial({
			color: 0x6bb6ff, roughness: 0.5, metalness: 0.2 });
	}
	const mesh = new THREE.Mesh(g, material);
	mesh.castShadow = true;
	mesh.receiveShadow = true;
	return mesh;
}

// ===========================================================================
// State + update
// ===========================================================================

let currentMesh = null;
let wireframeMesh = null;

const state = {
	subdivisionLevel: 2,
	scheme: 'catmullclark',
	boundary: 'edgeAndCorner',
	baseMesh: 'uv_grid',
	textured: true,
	textureSize: 2048,
	uvInterp: 'smooth',  // 'linear' (bilinear) or 'smooth' (seam-split, USD default)
	wantNormals: true,
	streaming: true,
	batchFaces: 2048,
	blockFaces: 0,  // 0 = whole-mesh streaming; >0 = block mode (bounds working set)
	showWireframe: false,
};

const VRAM_BUDGET = 2 * 1024 * 1024 * 1024;  // 2 GB reference (shared GPU)

function getControlMesh() {
	if (state.scheme === 'loop') {
		// Loop needs all-triangle input; UV meshes are quads.
		return state.baseMesh === 'tetrahedron'
			? tetrahedronControlMesh() : icosahedronControlMesh();
	}
	switch (state.baseMesh) {
		case 'uv_grid': return uvGridControlMesh(16);
		case 'uv_cube': return uvCubeControlMesh();
		case 'grid': return gridControlMesh(16);
		case 'tetrahedron': return tetrahedronControlMesh();
		case 'icosahedron': return icosahedronControlMesh();
		default: return cubeControlMesh();
	}
}

function disposeMesh(m) {
	if (!m) return;
	scene.remove(m);
	m.geometry.dispose();
	m.material.dispose();
}

function updateMesh() {
	if (!streamer) return;
	disposeMesh(currentMesh);
	disposeMesh(wireframeMesh);
	currentMesh = null;
	wireframeMesh = null;

	const cm = getControlMesh();
	const wantTexture = state.textured && !!cm.uvValues && state.subdivisionLevel >= 1;
	const opts = {
		scheme: SCHEME_ID[state.scheme],
		boundary: BOUNDARY_ID[state.boundary],
		level: state.subdivisionLevel,
		batchFaces: state.streaming ? state.batchFaces : 1 << 30,
		blockFaces: state.streaming ? state.blockFaces : 0,
		haloRings: 0,  // 0 => library's level-independent default (2 rings)
		wantNormals: state.wantNormals,
		uvValues: wantTexture ? cm.uvValues : null,
		uvIndices: wantTexture ? cm.uvIndices : null,
		uvInterp: state.uvInterp === 'smooth' ? 1 : 0,
	};

	let refined;
	try {
		refined = refine(cm, opts);
	} catch (e) {
		console.error('subdivision failed:', e.message);
		setStatus('Error: ' + e.message);
		return;
	}

	currentMesh = buildMesh(refined, false);
	scene.add(currentMesh);
	if (state.showWireframe) {
		wireframeMesh = buildMesh(refined, true);
		scene.add(wireframeMesh);
	}

	updateStats(refined);
}

function setText(id, text, warn) {
	const el = document.getElementById(id);
	if (el) {
		el.textContent = text;
		el.style.color = warn ? '#ff7a7a' : '';
	}
}

function setStatus(text) { setText('status', text); }

function estVramBytes(refined) {
	// GPU-resident vertex buffers (+ index buffer when indexed) + texture w/mips.
	const geom = refined.textured
		? refined.numTris * 3 * (12 + 12 + 8)              // non-indexed pos+nrm+uv
		: refined.numVerts * (12 + 12) + refined.numTris * 3 * 4;  // indexed pos+nrm + idx
	const tex = refined.textured
		? state.textureSize * state.textureSize * 4 * 1.34  // RGBA + mipmaps
		: 0;
	return geom + tex;
}

function fmtBytes(b) {
	return b >= 1024 * 1024 * 1024
		? (b / (1024 * 1024 * 1024)).toFixed(2) + ' GB'
		: (b / (1024 * 1024)).toFixed(1) + ' MB';
}

function updateStats(refined) {
	setText('vertCount', refined.numVerts.toLocaleString());
	setText('faceCount', (refined.numTris / 2 | 0).toLocaleString());
	setText('triCount', refined.numTris.toLocaleString());
	setText('levelDisplay', String(state.subdivisionLevel));
	setText('batchCount', refined.batches.toLocaleString());
	setText('heapMB', fmtBytes(refined.heapBytes));
	setText('timeMs', refined.timeMs.toFixed(1) + ' ms');
	const vram = estVramBytes(refined);
	setText('estVram', fmtBytes(vram) + ' / 2.00 GB', vram > VRAM_BUDGET);
	setStatus((refined.textured ? `textured (${state.uvInterp} UV) · ` : '') +
		(state.streaming ? `streaming (${state.batchFaces} f/batch)` : 'bulk (single batch)') +
		(state.streaming && state.blockFaces > 0
			? ` · block ${state.blockFaces} f (bounded working set)` : ''));
}

// ===========================================================================
// GUI
// ===========================================================================

const gui = new GUI();
// Backend switch reloads the page: the WASM module is chosen at startup.
gui.add({ backend: getBackendFromURL() }, 'backend', LOADER_BACKEND_CHOICES)
	.name('WASM Backend').onChange(setBackendAndReload);
gui.add(state, 'subdivisionLevel', 0, 5, 1).name('Subdivision Level').onChange(updateMesh);
gui.add(state, 'scheme', ['catmullclark', 'loop', 'bilinear']).name('Scheme').onChange(updateMesh);
gui.add(state, 'boundary', ['edgeAndCorner', 'edgeOnly', 'none']).name('Boundary').onChange(updateMesh);
gui.add(state, 'baseMesh', ['uv_grid', 'uv_cube', 'cube', 'grid', 'tetrahedron', 'icosahedron']).name('Base Mesh').onChange(updateMesh);
gui.add(state, 'showWireframe').name('Show Wireframe').onChange(updateMesh);

const texFolder = gui.addFolder('Texture (faceVarying UVs)');
texFolder.add(state, 'textured').name('Textured').onChange(updateMesh);
texFolder.add(state, 'textureSize', [512, 1024, 2048, 4096]).name('Texture size').onChange(updateMesh);
// faceVarying UV interpolation: linear (bilinear per corner) vs smooth
// seam-split (UVs follow the limit surface, less distortion on curved regions).
// Both stream; smooth uses the per-channel split-mesh path.
texFolder.add(state, 'uvInterp', ['linear', 'smooth']).name('UV interpolation').onChange(updateMesh);
texFolder.open();

const memFolder = gui.addFolder('Memory (wasm streaming)');
memFolder.add(state, 'wantNormals').name('Analytic Normals').onChange(updateMesh);
memFolder.add(state, 'streaming').name('Streaming').onChange(updateMesh);
memFolder.add(state, 'batchFaces', [256, 1024, 2048, 8192, 65536]).name('Faces / batch').onChange(updateMesh);
// Block mode bounds the WORKING set (not just the output): refine in blocks of
// N base faces + halo, so peak heap is independent of mesh size and level.
// 0 = off (whole-mesh streaming). Works with textured (faceVarying) meshes too.
memFolder.add(state, 'blockFaces', [0, 16, 64, 256]).name('Block faces (0=off)').onChange(updateMesh);
memFolder.open();

// ===========================================================================
// Animation / resize / init
// ===========================================================================

// FPS display (updated every ~500 ms to keep UI overhead low)
let fpsFrameCount = 0;
let fpsLastUpdateMs = performance.now();

function animate() {
	requestAnimationFrame(animate);
	controls.update();
	renderer.render(scene, camera);

	fpsFrameCount++;
	const now = performance.now();
	if (now - fpsLastUpdateMs >= 500) {
		const fps = fpsFrameCount * 1000 / (now - fpsLastUpdateMs);
		setText('fps', fps.toFixed(1));
		fpsFrameCount = 0;
		fpsLastUpdateMs = now;
	}
}

window.addEventListener('resize', () => {
	camera.aspect = window.innerWidth / window.innerHeight;
	camera.updateProjectionMatrix();
	renderer.setSize(window.innerWidth, window.innerHeight);
});

async function main() {
	setStatus('loading wasm…');
	const initLightUSD = await importLightUSDModule();
	const core = await initLightUSD();
	Module = core.loadFeature ? await core.loadFeature('geometry') : core;
	streamer = new Module.SubdivStreamer();
	setStatus('ready');
	updateMesh();
	animate();
	console.log('LightUSD streaming subdivision demo loaded');
}

main().catch((e) => {
	console.error(e);
	setStatus('failed to load wasm: ' + e.message);
});
