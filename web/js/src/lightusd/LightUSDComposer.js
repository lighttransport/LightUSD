import { FetchAssetResolver } from './LightUSDLoader.js';

const contexts = new WeakMap();
const bytes = value => value instanceof Uint8Array ? value : new Uint8Array(value);
const arcFields = {references: 2, payloads: 4, inherits: 8, specializes: 16, variantSets: 32};

// Public composition helper over the core LayerDocument/NextFlattenSession API.
// Documents remain caller-owned; temporary sessions are always released.
class LightUSDComposer {
    constructor() {
        this.usdLayer_ = null;
        this.usdLoader_ = null;
        this.assetMap_ = new Map();
        this.assetResolver_ = new FetchAssetResolver();
        this.assetSearchPaths_ = ['./'];
        this.baseWorkingPath_ = './';
        this.filename_ = 'root.usda';
    }
    static _json(layer) {
        if (!layer?.exportJSON) throw new TypeError('LightUSDComposer: expected a core LayerDocument');
        const result = layer.exportJSON();
        if (!result.success) throw new Error(result.error);
        return JSON.parse(result.text);
    }
    static _arcs(layer, field) {
        const json = this._json(layer);
        if (field === 'subLayers') return json.metas?.subLayers || [];
        const found = [];
        const pending = Object.values(json.primSpecs || {});
        while (pending.length) {
            const prim = pending.pop();
            for (const operation of prim[field] || []) {
                // Deleted/ordered items do not introduce a dependency.
                if (!['delete', 'order'].includes(operation.op)) found.push(...operation.items);
            }
            pending.push(...Object.values(prim.children || {}));
        }
        return found;
    }
    static _paths(layer, field) {
        return [...new Set(this._arcs(layer, field).map(item => item.assetPath).filter(Boolean))];
    }
    static hasSublayer(layer) { return this._arcs(layer, 'subLayers').length > 0; }
    static extractSublayerAssetPaths(layer) { return this._paths(layer, 'subLayers'); }
    static hasReferences(layer) { return this._arcs(layer, 'references').length > 0; }
    static extractReferencesAssetPaths(layer) { return this._paths(layer, 'references'); }
    static hasPayload(layer) { return this._arcs(layer, 'payloads').length > 0; }
    static extractPayloadAssetPaths(layer) { return this._paths(layer, 'payloads'); }
    static hasInherits(layer) { return this._arcs(layer, 'inherits').length > 0; }
    static hasVariants(layer) { return this._arcs(layer, 'variantSets').length > 0; }
    static _compose(layer, mask) {
        const owner = contexts.get(layer);
        if (!owner) throw new Error('LightUSDComposer: associate the document with setLayer() and setUSDLoader() first');
        const session = owner._session(mask);
        try {
            for (let i = 0; i < 4096; ++i) {
                const step = session.step(null);
                if (!step.success) throw new Error(step.error);
                if (step.status === 'done') { owner._accept(step.data); return true; }
                if (step.status !== 'need-layer') continue;
                const data = owner.assetMap_.get(step.key);
                if (!data) throw new Error(`LightUSDComposer: missing preloaded layer '${step.key}'`);
                const provided = session.provideLayer(step.key, bytes(data));
                if (!provided.success) throw new Error(provided.error);
            }
            throw new Error('LightUSDComposer: dependency limit exceeded');
        } finally { session.delete(); }
    }
    static composeSublayer(layer) { return this._compose(layer, 1); }
    static composeSublayers(layer) { return this.composeSublayer(layer); }
    static composeReferences(layer) { return this._compose(layer, arcFields.references); }
    static composePayload(layer) { return this._compose(layer, arcFields.payloads); }
    static composeInherits(layer) { return this._compose(layer, arcFields.inherits); }
    static composeVariants(layer) { return this._compose(layer, arcFields.variantSets); }
    clearAssetMap() { this.assetMap_.clear(); }
    getAssetMap() { return this.assetMap_; }
    setLayer(layer, filename = 'root.usda') {
        if (!layer?.exportUSDC || !layer?.load) throw new TypeError('LightUSDComposer: expected a core LayerDocument');
        if (this.usdLayer_ && contexts.get(this.usdLayer_) === this) contexts.delete(this.usdLayer_);
        this.usdLayer_ = layer;
        this.filename_ = filename;
        contexts.set(layer, this);
    }
    getLayer() {
        if (!this.usdLayer_) throw new Error('LightUSDComposer: call setLayer() first');
        return this.usdLayer_;
    }
    setUSDLoader(loader) { this.usdLoader_ = loader; }
    getUSDLoader() {
        if (!this.usdLoader_) throw new Error('LightUSDComposer: call setUSDLoader() first');
        return this.usdLoader_;
    }
    setAssetResolver(resolver) {
        if (typeof resolver?.resolveAsync !== 'function') throw new TypeError('LightUSDComposer: resolver must implement resolveAsync()');
        this.assetResolver_ = resolver;
    }
    getAssetResolver() { return this.assetResolver_; }
    setAssetSearchPaths(paths) { this.assetSearchPaths_ = [...paths]; }
    getAssetSearchPaths() { return this.assetSearchPaths_; }
    setBaseWorkingPath(path) { this.baseWorkingPath_ = path; }
    getBaseWorkingPath() { return this.baseWorkingPath_; }
    _session(mask, layer = this.getLayer()) {
        const native = this.getUSDLoader().native_;
        if (!native?.NextFlattenSession) throw new Error('LightUSDComposer: initialize the core USD loader first');
        const exported = layer.exportUSDC();
        if (!exported.success) throw new Error(exported.error);
        const session = new native.NextFlattenSession();
        try {
            const name = this.baseWorkingPath_.replace(/\/?$/, '/') + this.filename_;
            const begun = session.begin(exported.data, name, true);
            if (!begun.success) throw new Error(begun.error);
            session.setCompositionMask(mask);
            session.setRebaseAssetPaths(true);
            return session;
        } catch (error) { session.delete(); throw error; }
    }
    _accept(data) {
        const loaded = this.getLayer().load(data);
        if (!loaded.success) throw new Error(loaded.error);
    }
    async progressiveComposition(options = {}) {
        const session = this._session(options.composePayload === false ? 59 : 63);
        const requested = new Set();
        try {
            for (let i = 0; i < 4096; ++i) {
                const step = session.step(null);
                if (!step.success) throw new Error(step.error);
                if (step.status === 'done') { this._accept(step.data); return this.getLayer(); }
                if (step.status !== 'need-layer') continue;
                if (requested.has(step.key)) throw new Error(`LightUSDComposer: unresolved layer '${step.key}'`);
                requested.add(step.key);
                let data = this.assetMap_.get(step.key);
                if (!data) {
                    let failure;
                    for (const prefix of ['', ...this.assetSearchPaths_]) {
                        try {
                            const path = prefix ? prefix.replace(/\/?$/, '/') + step.key : step.key;
                            const [uri, binary] = await this.assetResolver_.resolveAsync(path);
                            data = bytes(binary);
                            this.assetMap_.set(uri, data);
                            break;
                        } catch (error) { failure = error; }
                    }
                    if (!data) throw failure || new Error(`Missing layer '${step.key}'`);
                    this.assetMap_.set(step.key, data);
                }
                const provided = session.provideLayer(step.key, bytes(data));
                if (!provided.success) throw new Error(provided.error);
            }
            throw new Error('LightUSDComposer: dependency limit exceeded');
        } finally { session.delete(); }
    }
    // Retain the old prefetch entry point, but let the core resolver discover
    // nested dependencies and their authoring-layer anchors.
    async resolveSublayerAssets(_depth, layer = this.getLayer()) {
        const session = this._session(1, layer);
        try {
            for (let i = 0; i < 4096; ++i) {
                const step = session.step(null);
                if (!step.success) throw new Error(step.error);
                if (step.status === 'done') return;
                if (step.status !== 'need-layer') continue;
                let data = this.assetMap_.get(step.key);
                if (!data) {
                    const [, binary] = await this.assetResolver_.resolveAsync(step.key);
                    data = bytes(binary);
                    this.assetMap_.set(step.key, data);
                }
                const provided = session.provideLayer(step.key, bytes(data));
                if (!provided.success) throw new Error(provided.error);
            }
            throw new Error('LightUSDComposer: dependency limit exceeded');
        } finally { session.delete(); }
    }
}
export { LightUSDComposer };
