import { LightUSDLoader } from './src/lightusd/LightUSDLoader.js';
import { isScene } from './src/lightusd/RenderSceneUtils.js';

let loader = null;
let activeBytes = null;
let activeName = '';

async function ensureLoader() {
	if (loader) return loader;
	loader = new LightUSDLoader(null, {
		suppressNativeInfoLogs: true
	});
	// Workers do not inherit the page's location query parameters, so
	// backend=core cannot select the module implicitly here. This
	// worker is exclusively the core conversion path; load the full core
	// RenderStream explicitly instead of the legacy USDC-only shim.
	await loader.init({ backend: 'core' });
	return loader;
}

function addTransferable(list, seen, value) {
	if (!value || !value.buffer) return;
	const buffer = value.buffer;
	if (!buffer || seen.has(buffer)) return;
	seen.add(buffer);
	list.push(buffer);
}

function serializeScene(usd) {
	if (!isScene(usd)) {
		throw new Error('worker conversion only supports next render scenes');
	}
	const transfer = [];
	const seen = new Set();
	const meshes = usd.meshes || [];
	for (const mesh of meshes) {
		addTransferable(transfer, seen, mesh.points);
		addTransferable(transfer, seen, mesh.indices);
		addTransferable(transfer, seen, mesh.normals);
		addTransferable(transfer, seen, mesh.uv0);
	}
	return {
		payload: {
			__backend: 'core',
			filename: usd.filename || '',
			meshes,
			stats: usd.getStats ? usd.getStats() : (usd.stats || {}),
			sceneMetadata: usd.getSceneMetadata ? usd.getSceneMetadata() : (usd.sceneMetadata || {})
		},
		transfer
	};
}

async function convert(id, msg) {
	await ensureLoader();
	if (msg.bytes) {
		activeBytes = new Uint8Array(msg.bytes);
		activeName = msg.name || 'scene.usdz';
	}
	if (!activeBytes) {
		throw new Error('worker has no active USD bytes');
	}

	const options = { ...(msg.options || {}) };
	delete options.returnArchiveEntries;
	const progressBase = Number.isFinite(msg.progressBase) ? msg.progressBase : 0;
	const progressRange = Number.isFinite(msg.progressRange) ? msg.progressRange : 100;
	let lastProgressPostMs = -Infinity;
	const report = (info = {}) => {
		const now = performance.now();
		const force = info.cratePhase === 'complete' ||
			(Number.isFinite(info.meshCurrent) && Number.isFinite(info.meshTotal) &&
				info.meshCurrent >= info.meshTotal);
		// Stage reconstruction can synchronously emit thousands of callbacks.
		// Forwarding every one floods the main thread with postMessage events,
		// DOM text updates and progress-history allocations, roughly doubling
		// conversion time on large stages. Around 20 updates/s remains smooth.
		if (!force && now - lastProgressPostMs < 250) return;
		lastProgressPostMs = now;
		self.postMessage({
			type: 'progress',
			id,
			info: {
				...info,
				progressBase,
				progressRange
			}
		});
	};

	const usd = await new Promise((resolve, reject) => {
		loader.parse(activeBytes, activeName, resolve, reject, {
			...options,
			backend: 'core',
			// This worker serializes only mesh payloads, native stats and scene
			// metadata. Avoid materializing the full 69k-node render hierarchy,
			// point-instancer draws, lights and cameras merely to discard them.
			meshOnly: true,
			onProgress: report,
			onLightUSDDebug: report,
			onTydraProgress: report,
			progressBase,
			progressRange
		});
	});

	try {
		const { payload, transfer } = serializeScene(usd);
		self.postMessage({ type: 'result', id, payload }, transfer);
	} finally {
		if (usd && typeof usd.delete === 'function') {
			try { usd.delete(); } catch (_) {}
		}
	}
}

self.addEventListener('message', (event) => {
	const msg = event.data || {};
	if (msg.type === 'clear') {
		activeBytes = null;
		activeName = '';
		return;
	}
	if (msg.type !== 'convert') return;
	convert(msg.id, msg).catch((error) => {
		self.postMessage({
			type: 'error',
			id: msg.id,
			error: error?.message || String(error)
		});
	});
});
