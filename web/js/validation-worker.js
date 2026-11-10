// Shared by web/js and web/demo. Validation is synchronous inside WASM, so
// run it in a disposable worker to keep the UI responsive and cancellable.
import nextGlue from './src/lightusd/lightusd_validator.js?url';
import nextWasm from './src/lightusd/lightusd_validator.wasm?url';
let module;
self.onmessage = async ({data}) => {
  try {
    if (data.type === 'init') {
      const factory = (await import(/* @vite-ignore */ nextGlue)).default;
      module = await factory({locateFile:path => path.endsWith('.wasm') ? nextWasm : path});
      if (typeof module.runValidation !== 'function') throw new Error('Rebuild the core validator feature');
      self.postMessage({id: data.id, result: true});
      return;
    }
    if (!module || data.type !== 'check') throw new Error('Validation worker is not ready');
    const assets = new Map((data.assets || []).map(asset => [asset.identifier, asset.bytes]));
    const metadata = {assets: [...assets].map(([name, bytes]) => ({name, size: bytes.byteLength})),
      aliases: {}, workingDirectory: '', searchPaths: [], allowParentPaths: false};
    const result = JSON.parse(module.runValidation(data.bytes, data.filename,
      JSON.stringify(data.options), true, {metadata, read:name => assets.get(name)}));
    self.postMessage({id: data.id, result});
  } catch (error) { self.postMessage({id: data.id, error: error.message}); }
};
