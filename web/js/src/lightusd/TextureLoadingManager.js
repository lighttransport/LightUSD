// SPDX-License-Identifier: Apache-2.0
//
// One TextureLoadingManager implementation shared by both texture paths:
//   - the three.js RenderScene builder (archive adapter -> decoded texture)
//   - LightUSDLoaderUtils (native texture ID -> decoded texture)
//
// The queue/dedup/concurrency/lifecycle logic lives here. A small driver
// supplies the path-specific decode, binding and reporting hooks, so the two
// call sites keep their exact public behavior while sharing one manager.

const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

export class TextureLoadingManager {
  constructor(driver) {
    this.driver = driver || {};
    this.tasks = [];
    // Loader-utils historically calls this list `queue`; keep one array with
    // both names so external readers see identical state.
    this.queue = this.tasks;
    this.taskMap = new Map();
    this.promiseCache = new Map();
    this.total = 0;
    this.loaded = 0;
    this.failed = 0;
    this.warnedFailures = new Set();
    this.aborted = false;
    this.isLoading = false;
    this.pendingReleaseAdapters = new Set();
    this.textureSignatureCache = new Map();
    this.driver.init?.(this);
  }

  // Mesh-local material clones and implementation switches must receive the
  // same pending texture results as the material that queued them.
  cloneMaterialBindings(source, target) {
    for (const task of this.tasks) {
      for (const binding of [...task.bindings]) {
        if (binding.material === source && !task.bindings.some(
          item => item.material === target && item.mapProperty === binding.mapProperty)) {
          task.bindings.push({material: target, mapProperty: binding.mapProperty});
        }
      }
    }
  }

  replaceMaterials(replacements) {
    for (const task of this.tasks) {
      for (const binding of task.bindings) {
        binding.material = replacements.get(binding.material) || binding.material;
      }
    }
  }

  // Redirect queued texture bindings when a pipeline replaces a material
  // (for example to install extended skinning shaders).
  rebindMaterial(previousMaterial, replacementMaterial) {
    if (!previousMaterial || !replacementMaterial || previousMaterial === replacementMaterial) return;
    for (const task of this.tasks) {
      for (const binding of task.bindings) {
        if (binding.material === previousMaterial) binding.material = replacementMaterial;
      }
    }
  }

  releaseAdapters(adapters) {
    if (!adapters || adapters.size === 0) return;
    if (this.isLoading) {
      for (const adapter of adapters) this.pendingReleaseAdapters.add(adapter);
      return;
    }
    for (const adapter of adapters) adapter.releaseArchiveTextureBytes();
  }

  getStatus() {
    return this.driver.status
      ? this.driver.status(this)
      : {
          loaded: this.loaded, failed: this.failed, total: this.total,
          pending: Math.max(0, this.total - this.loaded - this.failed),
          percentage: this.total > 0 ? (this.loaded + this.failed) / this.total * 100 : 100,
          isComplete: this.total === 0 || this.loaded + this.failed >= this.total
        };
  }

  abort() { this.aborted = true; }

  reset() {
    const adapters = new Set();
    for (const task of this.tasks) {
      if (task.adapter && typeof task.adapter.releaseArchiveTextureBytes === 'function') {
        adapters.add(task.adapter);
      }
    }
    this.tasks.length = 0;
    this.taskMap.clear();
    this.promiseCache.clear();
    this.textureSignatureCache.clear();
    this.total = 0;
    this.loaded = 0;
    this.failed = 0;
    this.warnedFailures.clear();
    this.aborted = !!this.driver.resetAborts;
    // Keep `isLoading` and `pendingReleaseAdapters` untouched: a reset during
    // an in-flight load must defer archive release until that load settles.
    this.driver.reset?.(this);
    this.releaseAdapters(adapters);
  }

  // Overridable by callers/tests; the default dispatches to the driver.
  loadTexture(task, options) {
    return this.driver.load(this, task, options);
  }

  async startLoading(options = {}) {
    const driver = this.driver;
    const {
      onProgress = null, onTextureLoaded = null, loadTexture = null,
      concurrency = driver.defaultConcurrency ? driver.defaultConcurrency() : 1,
      yieldInterval = 16
    } = options;
    if (this.isLoading) {
      if (driver.alreadyLoadingThrows) throw new Error('texture loading is already running');
      console.warn('TextureLoadingManager: Already loading');
      return this.getStatus();
    }
    this.isLoading = true;
    this.aborted = false;
    let lastYieldTime = now();
    if (onProgress && driver.initialProgress) onProgress(driver.initialProgress(this));
    // No await when the driver has no initial yield: callers may rely on the
    // first loadTexture call happening synchronously during startLoading().
    if (driver.initialYield) await driver.initialYield();

    let nextIndex = 0;
    const worker = async () => {
      while (!this.aborted && nextIndex < this.tasks.length) {
        const task = this.tasks[nextIndex++];
        task.status = 'loading';
        try {
          const texture = await this.loadTexture(task, {onTextureLoaded, loadTexture});
          if (texture && !this.aborted) {
            for (const binding of task.bindings) {
              driver.apply(this, binding, task, texture, {onTextureLoaded});
            }
            task.status = 'loaded';
            this.loaded++;
          } else if (driver.countNullAsLoaded) {
            task.status = 'loaded';
            this.loaded++;
          }
        } catch (error) {
          task.status = 'failed';
          this.failed++;
          driver.onError?.(this, task, error);
        }
        if (onProgress && !this.aborted && driver.taskProgress) {
          onProgress(driver.taskProgress(this, task));
        }
        const current = now();
        if (yieldInterval > 0 && current - lastYieldTime >= yieldInterval) {
          lastYieldTime = current;
          await driver.yield?.();
        }
      }
    };
    try {
      await Promise.all(Array.from({length: Math.max(1, concurrency)}, worker));
    } finally {
      this.isLoading = false;
      driver.finished?.(this);
    }
    if (onProgress && driver.finalProgress) onProgress(driver.finalProgress(this));
    return this.getStatus();
  }
}
