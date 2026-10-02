// Shared by web/js and web/demo. Validation is synchronous inside WASM, so
// run it in a disposable worker to keep the UI responsive and cancellable.
import nextGlue from './src/lightusd/lightusd_next.js?url';
import nextWasm from './src/lightusd/lightusd_next.wasm?url';
import legacyGlue from './src/lightusd/lightusd.js?url';
import legacyWasm from './src/lightusd/lightusd.wasm?url';
let module, backend;
self.onmessage = async ({data}) => {
  try {
    if (data.type === 'init') {
      backend = data.backend;
      const useNext = backend !== 'legacy';
      const factory = (await import(/* @vite-ignore */ (useNext ? nextGlue : legacyGlue))).default;
      module = await factory({locateFile: path => path.endsWith('.wasm') ? (useNext ? nextWasm : legacyWasm) : path});
      if (useNext && typeof module.checkUSD !== 'function') throw new Error('Rebuild the next WASM module to enable lusdchecker');
      self.postMessage({id: data.id, result: true});
      return;
    }
    if (!module || data.type !== 'check') throw new Error('Validation worker is not ready');
    let result;
    if (backend === 'legacy') {
      const native = new module.LightUSDLoaderNative();
      try { result = JSON.parse(native.validateFromBinary(data.bytes, data.filename, JSON.stringify(data.options))); }
      finally { native.delete(); }
    } else {
      const store = new module.NextAssetStore();
      try {
        store.setMemoryLimitBytes(512 * 1024 * 1024);
        for (const asset of data.assets) store.registerMemoryAsset(asset.identifier, asset.bytes);
        result = module.checkUSD(data.bytes, data.filename, data.options, store);
      } finally { store.delete(); }
    }
    self.postMessage({id: data.id, result});
  } catch (error) { self.postMessage({id: data.id, error: error.message}); }
};
