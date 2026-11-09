import { LuciaError, encoder } from './utils.js';

const LIMIT = 256 * 1024 * 1024;
export async function hashContent(bytes) {
  return [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))]
    .map(byte => byte.toString(16).padStart(2, '0')).join('');
}

// One atomic transaction writes blobs and switches the current manifest.
// Hashes are verified on recovery; missing/corrupt data never partly restores.
export class LuciaProjectStorage {
  constructor(adapter = null) { this.adapter = adapter; }
  async backend() {
    if (this.adapter) return this.adapter;
    if (!globalThis.indexedDB) throw new LuciaError('LUCIA_PROJECT_STORAGE', 'Local project storage is unavailable.');
    this.adapter = await new Promise((resolve, reject) => {
      const request = indexedDB.open('lucia-projects', 1);
      request.onupgradeneeded = () => request.result.createObjectStore('records');
      request.onerror = () => reject(request.error);
      request.onsuccess = () => {
        const database = request.result;
        resolve({
          get: key => new Promise((done, fail) => {
            const request = database.transaction('records').objectStore('records').get(key);
            request.onsuccess = () => done(request.result); request.onerror = () => fail(request.error);
          }),
          putMany: entries => new Promise((done, fail) => {
            const transaction = database.transaction('records', 'readwrite');
            transaction.oncomplete = () => done(); transaction.onerror = () => fail(transaction.error);
            transaction.onabort = () => fail(transaction.error || new Error('Storage transaction aborted.'));
            try {
              const store = transaction.objectStore('records'), retained = new Set(entries.map(([key]) => key));
              for (const [key, value] of entries) store.put(value, key);
              const cursor = store.openKeyCursor();
              cursor.onsuccess = () => {
                if (!cursor.result) return;
                if (!retained.has(cursor.result.key)) store.delete(cursor.result.key);
                cursor.result.continue();
              };
            }
            catch (error) { transaction.abort(); fail(error); }
          }),
        });
      };
    });
    return this.adapter;
  }
  async save(project, source) {
    const previous = this.pendingSave || Promise.resolve();
    let release;
    const gate = new Promise(resolve => { release = resolve; });
    const current = previous.then(() => gate);
    this.pendingSave = current;
    try {
      if (typeof source !== 'string' || !(project.assets instanceof Map)) throw new LuciaError('LUCIA_PROJECT_STORAGE', 'Invalid project snapshot.');
      if (source.length > LIMIT || project.assets.size > 100000) throw new LuciaError('LUCIA_PROJECT_MEMORY', 'Project snapshot exceeds its budget.');
      const sourceBytes = encoder.encode(source), snapshots = [];
      let bytes = sourceBytes.byteLength;
      // Copy before the first await: edits cannot change the stored snapshot.
      for (const [name, asset] of project.assets) {
        if (typeof name !== 'string' || !name || !(asset.bytes instanceof Uint8Array)) throw new LuciaError('LUCIA_PROJECT_STORAGE', 'Project assets require names and byte buffers.');
        bytes += asset.bytes.byteLength;
        if (bytes > LIMIT) throw new LuciaError('LUCIA_PROJECT_MEMORY', 'Local project storage exceeds the 256 MiB snapshot budget.');
        const metadata = {};
        for (const key of ['mime', 'mimeType', 'width', 'height', 'channels', 'bitDepth', 'colorSpace', 'normalY', 'normalConvention', 'wrapS', 'wrapT', 'semantic', 'sourcePath', 'kind', 'generated'])
          if (typeof asset[key] === 'string' || typeof asset[key] === 'boolean' || Number.isFinite(asset[key])) metadata[key] = asset[key];
        snapshots.push({ name, bytes: asset.bytes.slice(), metadata });
      }
      if (bytes > LIMIT) throw new LuciaError('LUCIA_PROJECT_MEMORY', 'Local project storage exceeds the 256 MiB snapshot budget.');
      const manifest = { version: 1, name: project.name, provenance: structuredClone(project.provenance || null), exportRemap: { ...project.exportRemap }, source: await hashContent(sourceBytes), assets: [] };
      const entries = [[`blob:${manifest.source}`, sourceBytes]];
      for (const asset of snapshots) {
        const hash = await hashContent(asset.bytes);
        manifest.assets.push({ name: asset.name, hash, metadata: asset.metadata });
        entries.push([`blob:${hash}`, asset.bytes]);
      }
      entries.push(['current', manifest]);
      await previous;
      await (await this.backend()).putMany(entries);
      return { bytes, assets: snapshots.length };
    } finally {
      release();
      if (this.pendingSave === current) this.pendingSave = null;
    }
  }

  async load() {
    const backend = await this.backend(), manifest = await backend.get('current');
    if (!manifest) return null;
    if (manifest.version !== 1 || !Array.isArray(manifest.assets) || manifest.assets.length > 100000)
      throw new LuciaError('LUCIA_PROJECT_STORAGE', 'Unsupported or corrupt project manifest.');
    let total = 0;
    const read = async hash => {
      if (typeof hash !== 'string' || !/^[a-f0-9]{64}$/.test(hash)) throw new LuciaError('LUCIA_PROJECT_STORAGE', 'Invalid project content identifier.');
      const bytes = await backend.get(`blob:${hash}`);
      if (!(bytes instanceof Uint8Array) || (total += bytes.byteLength) > LIMIT || await hashContent(bytes) !== hash)
        throw new LuciaError('LUCIA_PROJECT_STORAGE', 'Saved project content is missing, corrupt, or over budget.');
      return bytes.slice();
    };
    const source = new TextDecoder('utf-8', { fatal: true }).decode(await read(manifest.source)), assets = new Map();
    for (const asset of manifest.assets) {
      if (typeof asset.name !== 'string' || !asset.name || assets.has(asset.name)) throw new LuciaError('LUCIA_PROJECT_STORAGE', 'Invalid saved asset name.');
      assets.set(asset.name, { ...asset.metadata, bytes: await read(asset.hash) });
    }
    return { source, assets, provenance: structuredClone(manifest.provenance || null), name: manifest.name, exportRemap: { ...manifest.exportRemap } };
  }
}
