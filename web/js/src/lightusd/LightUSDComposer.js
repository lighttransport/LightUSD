import { FetchAssetResolver } from "./LightUSDLoader.js";

class LightUSDComposer {

    constructor() {

        this.usdLayer_ = null; // This will hold the USD layer after loading.
        this.assetMap_ = new Map();
        this.usdLoader_ = null; // LightUSDLoaderNative instance
        this.assetResolver_ = new FetchAssetResolver(); // 'fetch' Asset resolver

        this.assetSearchPaths_ = ["./"];
        this.baseWorkingPath_ = "./";

    }

    static hasSublayer(usd_layer) {
        if (!usd_layer || !usd_layer.hasSublayer) {
            console.warn("LightUSDComposer: Invalid USD layer or hasSublayer not available.");
            return false;
        }
        return usd_layer.hasSublayer();
    }

    static extractSublayerAssetPaths(usd_layer) {
        if (!usd_layer || !usd_layer.extractSublayerAssetPaths) {
            console.warn("LightUSDComposer: Invalid USD layer or extractSublayerAssetPaths not available.");
            return [];
        }
        return usd_layer.extractSublayerAssetPaths();
    }

    static hasReferences(usd_layer) {
        if (!usd_layer || !usd_layer.hasReferences) {
            console.warn("LightUSDComposer: Invalid USD layer or hasReferences not available.");
            return false;
        }
        return usd_layer.hasReferences();
    }

    static extractReferencesAssetPaths(usd_layer) {
        if (!usd_layer || !usd_layer.extractReferencesAssetPaths) {
            console.warn("LightUSDComposer: Invalid USD layer or extractReferencesAssetPaths not available.");
            return [];
        }
        return usd_layer.extractReferencesAssetPaths();
    }

    static hasPayload(usd_layer) {
        if (!usd_layer || !usd_layer.hasPayload) {
            console.warn("LightUSDComposer: Invalid USD layer or hasPayload not available.");
            return false;
        }
        return usd_layer.hasPayload();
    }

    static extractPayloadAssetPaths(usd_layer) {
        if (!usd_layer || !usd_layer.extractPayloadAssetPaths) {
            console.warn("LightUSDComposer: Invalid USD layer or extractPayloadAssetPaths not available.");
            return [];
        }
        return usd_layer.extractPayloadAssetPaths();
    }

    static composeSublayer(usd_layer) {
        if (!usd_layer || !usd_layer.composeSublayer) {
            console.warn("LightUSDComposer: Invalid USD layer or composeSublayer not available.");
            return [];
        }
        return usd_layer.composeSublayer();
    }

    static composeReferences(usd_layer) {
        if (!usd_layer || !usd_layer.composeReferences) {
            console.warn("LightUSDComposer: Invalid USD layer or composeReferences not available.");
            return [];
        }
        return usd_layer.composeReferences();
    }

    static composePayload(usd_layer) {
        if (!usd_layer || !usd_layer.composePayload) {
            console.warn("LightUSDComposer: Invalid USD layer or composePayload not available.");
            return [];
        }
        return usd_layer.composePayload();
    }

    static hasInherits(usd_layer) {
        if (!usd_layer || !usd_layer.hasInherits) {
            console.warn("LightUSDComposer: Invalid USD layer or hasInherits not available.");
            return false;
        }
        return usd_layer.hasInherits();
    }

    static composeInherits(usd_layer) {
        if (!usd_layer || !usd_layer.composeInherits) {
            console.warn("LightUSDComposer: Invalid USD layer or composeInherits not available.");
            return [];
        }
        return usd_layer.composeInherits();
    }

    static hasVariants(usd_layer) {
        if (!usd_layer || !usd_layer.hasVariants) {
            console.warn("LightUSDComposer: Invalid USD layer or hasVariants not available.");
            return false;
        }
        return usd_layer.hasVariants();
    }

    static composeVariants(usd_layer) {
        if (!usd_layer || !usd_layer.composeVariants) {
            console.warn("LightUSDComposer: Invalid USD layer or composeVariants not available.");
            return [];
        }
        return usd_layer.composeVariants();
    }

    clearAssetMap() {
        this.assetMap_.clear();
    }

    getAssetMap() {
        return this.assetMap_;
    }

    setLayer(usdLayer) {
        this.usdLayer_ = usdLayer;
    }

    getLayer() {
        if (!this.usdLayer_) {
            throw new Error("LightUSDComposer: Layer is not set. Call setLayer() first.");
        }
        return this.usdLayer_;
    }

    setUSDLoader(usd_loader) {
        this.usdLoader_ = usd_loader;
    }

    // Override the asset resolver used to fetch external assets (sublayers,
    // references, payloads). The resolver must expose
    // `resolveAsync(assetPath) -> Promise<[assetPath, ArrayBuffer]>`, where the
    // returned key is the asset path AS WRITTEN in the USD (so setAsset() stores
    // it under the name the native composer/converter looks it up by).
    // Defaults to a same-origin FetchAssetResolver; inject e.g. an
    // HttpAssetResolver to rewrite relative paths onto a remote host.
    setAssetResolver(resolver) {
        if (!resolver || typeof resolver.resolveAsync !== "function") {
            throw new Error("LightUSDComposer: asset resolver must implement resolveAsync().");
        }
        this.assetResolver_ = resolver;
    }

    getAssetResolver() {
        return this.assetResolver_;
    }

    getUSDLoader() {
        if (!this.usdloader_) {
            throw new Error("LightUSDComposer: USD loader is not set. Call setUSDLoader() first.");
        }
        return this.usdLoader_;
    }

    setAssetSearchPaths(paths) {
        this.assetSearchPaths_ = paths;
    }

    getAssetSearchPaths() {
        return this.assetSearchPaths_;
    }

    setBaseWorkingPath(path) {
        this.baseWorkingPath_ = path;
    }

    getBaseWorkingPath() {
        return this.baseWorkingPath_;
    }

    // Parse already-fetched bytes into a native USD Layer (used to recurse into
    // a sublayer's own sublayers without a second network fetch). Returns the
    // native LightUSDLoaderNative instance, or null on failure.
    _loadLayerFromBytes(binary, filename) {
        const native = this.usdLoader_ && this.usdLoader_.native_;
        if (!native) {
            throw new Error("LightUSDComposer: USD loader native module is not initialized.");
        }
        const u8 = binary instanceof Uint8Array ? binary : new Uint8Array(binary);
        const layer = new native.LightUSDLoaderNative();
        this.usdLoader_._applySkinningLoadOptions?.(layer);
        if (!layer.loadAsLayerFromBinary(u8, filename)) {
            console.warn(`LightUSDComposer: failed to load sublayer '${filename}': ${layer.error?.()}`);
            if (typeof layer.delete === "function") layer.delete();
            return null;
        }
        return layer;
    }

    _assetAliases(uri) {
        if (typeof uri !== "string") return [uri];
        const fileUri = uri.replace(/<[^>]*>\s*$/, "");
        const out = new Set([uri, fileUri]);
        uri = fileUri;
        if (uri.startsWith("./")) out.add(uri.slice(2));
        else if (!uri.startsWith("/") && !/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(uri)) out.add("./" + uri);
        try {
            const u = new URL(uri, "http://lightusd.invalid/");
            const p = decodeURIComponent(u.pathname).replace(/^\/+/, "");
            if (p) {
                out.add(p);
                out.add("./" + p);
            }
        } catch (_) {
            // Not URL-like; the simple path aliases above are enough.
        }
        return [...out];
    }

    _setAsset(uri, binary) {
        for (const key of this._assetAliases(uri)) {
            this.assetMap_.set(key, binary);
            if (this.usdLayer_ && typeof this.usdLayer_.setAsset === "function") {
                this.usdLayer_.setAsset(key, binary);
            }
        }
    }

    // Recursively resolve sublayer assets.
    async resolveSublayerAssets(depth, usdLayer, parentAssetPath = null) {

        if (depth > 16) {
            console.warn("LightUSDComposer: Maximum recursion depth reached while resolving sublayer assets.");
            return;
        }
        const sublayerAssetPaths = LightUSDComposer.extractSublayerAssetPaths(usdLayer);
        //console.log("extractSublayer", sublayerAssetPaths);

        await Promise.all(sublayerAssetPaths.map(async (sublayerPath) => {
            const [uri, binary, resolvedPath] = await this.assetResolver_.resolveAsync(
                sublayerPath, { parentAssetPath });
            //console.log("sublayerPath:", sublayerPath, "binary:", binary.byteLength, "bytes");

            // Recurse into the sublayer to discover its own (possibly remote)
            // sublayers. Parse from the bytes we just fetched rather than a second
            // raw fetch of `sublayerPath` — the latter cannot honor an asset
            // resolver's path rewriting (e.g. relative paths onto a remote host).
            const sublayer = this._loadLayerFromBytes(binary, sublayerPath);
            if (sublayer) {
                try {
                    await this.resolveSublayerAssets(depth + 1, sublayer, resolvedPath || uri);
                } finally {
                    if (typeof sublayer.delete === "function") sublayer.delete();
                }
            }

            this._setAsset(uri, binary);
        }));
    }

    async progressiveComposition(options = {}) {

        if (!this.usdLayer_) {
            throw new Error("LightUSDComposer: setLayer() is not called.");
        }

        if (!this.usdLoader_) {
            throw new Error("LightUSDComposer: setUSDLoader() is not called.");
        }

        this.usdLayer_.clearAssetSearchPaths();
        for (const path of this.assetSearchPaths_) {
            this.usdLayer_.addAssetSearchPath(path);
        }

        this.usdLayer_.setBaseWorkingPath(this.baseWorkingPath_);

        const composePayload = options.composePayload !== false;

        // LIVRPS
        // [x] local(subLayer)
        // [x] inherits
        // [x] variants
        // [x] references
        // [x ] payload
        // [ ] specializes

        // Resolving subLayer is recursive.
        await this.resolveSublayerAssets(/* depth */0, this.usdLayer_);

        for (const [uri, binary] of this.assetMap_.entries()) {
            //console.log("setAsset:", uri, "binary:", binary.byteLength, "bytes");
            this.usdLayer_.setAsset(uri, binary);
        }

        if (!this.usdLayer_.composeSublayers()) {
            throw new Error("Failed to compose sublayers:", this.usdLayer_.error());
        }


        // others are iterative.
        const kMaxIter = 16;

        for (let i = 0; i < kMaxIter; i++) {

            // In each composition operation, usd_layer may be modified(merged with sublayers, etc).
            // And we iterate until no more composition is needed.

            //console.log("iter", i);
            //console.log("hasReferences:", LightUSDComposer.hasReferences(this.usdLayer_));
            //console.log("hasPayload:", LightUSDComposer.hasPayload(this.usdLayer_));
            //console.log("hasInherits:", LightUSDComposer.hasInherits(this.usdLayer_));
            //console.log("hasVariants:", LightUSDComposer.hasVariants(this.usdLayer_));

            if (!LightUSDComposer.hasReferences(this.usdLayer_) &&
                (!composePayload || !LightUSDComposer.hasPayload(this.usdLayer_)) &&
                !LightUSDComposer.hasInherits(this.usdLayer_) &&
                !LightUSDComposer.hasVariants(this.usdLayer_)) {
                break;
            }

            // Inherits and variants does not involve asset loading.
            if (LightUSDComposer.hasInherits(this.usdLayer_)) {
                if (!this.usdLayer_.composeInherits()) {
                    throw new Error("Failed to compose inherits:", this.usdLayer_.error());
                }
            }

            if (LightUSDComposer.hasVariants(this.usdLayer_)) {
                if (!this.usdLayer_.composeVariants()) {
                    throw new Error("Failed to compose variants:", this.usdLayer_.error());
                }
            }

            if (LightUSDComposer.hasReferences(this.usdLayer_)) {
                const referencesAssetPaths = LightUSDComposer.extractReferencesAssetPaths(this.usdLayer_);

                await Promise.all(referencesAssetPaths.map(async (assetPath) => {
                    const [uri, binary, resolvedPath] = await this.assetResolver_.resolveAsync(assetPath);
                    //console.log("referencesPath:", assetPath, "binary:", binary.byteLength, "bytes");

                    const refLayer = this._loadLayerFromBytes(binary, uri);
                    if (refLayer) {
                        try {
                            await this.resolveSublayerAssets(0, refLayer, resolvedPath || uri);
                        } finally {
                            if (typeof refLayer.delete === "function") refLayer.delete();
                        }
                    }

                    this._setAsset(uri, binary);
                }));

                //console.log("do composeReferences");
                if (!this.usdLayer_.composeReferences()) {
                    throw new Error("Failed to compose references:", this.usdLayer_.error());
                }
            }

            if (composePayload && LightUSDComposer.hasPayload(this.usdLayer_)) {
                const payloadAssetPaths = LightUSDComposer.extractPayloadAssetPaths(this.usdLayer_);

                await Promise.all(payloadAssetPaths.map(async (assetPath) => {
                    const [uri, binary, resolvedPath] = await this.assetResolver_.resolveAsync(assetPath);
                    //console.log("payloadAssetPath:", assetPath, "binary:", binary.byteLength, "bytes");

                    const payloadLayer = this._loadLayerFromBytes(binary, uri);
                    if (payloadLayer) {
                        try {
                            await this.resolveSublayerAssets(0, payloadLayer, resolvedPath || uri);
                        } finally {
                            if (typeof payloadLayer.delete === "function") payloadLayer.delete();
                        }
                    }

                    this._setAsset(uri, binary);
                }));

                if (!this.usdLayer_.composePayload()) {
                    throw new Error("Failed to compose payload: " + this.usdLayer_.error());
                }
            }
        }
    }



}



export { LightUSDComposer };
