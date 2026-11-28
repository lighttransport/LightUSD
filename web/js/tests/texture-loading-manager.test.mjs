// SPDX-License-Identifier: Apache-2.0
// Both texture paths must share one canonical manager implementation; only
// their queue/load hooks may differ.
import assert from 'node:assert/strict';
import {TextureLoadingManager as Base} from '../src/lightusd/TextureLoadingManager.js';
import {TextureLoadingManager as SceneManager} from '../src/lightusd/RenderSceneUtils.js';
import {TextureLoadingManager as LoaderManager} from '../src/lightusd/LightUSDLoaderUtils.js';

for (const Manager of [SceneManager, LoaderManager]) {
  assert.ok(Manager.prototype instanceof Base, `${Manager.name} shares the canonical base`);
  const manager = new Manager();
  assert.ok(Array.isArray(manager.tasks));
  assert.equal(manager.queue, manager.tasks, 'one task array under both historical names');
  assert.ok(manager.taskMap instanceof Map);
  assert.ok(manager.promiseCache instanceof Map);
  for (const method of ['getStatus', 'abort', 'reset', 'cloneMaterialBindings',
    'rebindMaterial', 'replaceMaterials', 'startLoading', 'loadTexture']) {
    assert.equal(typeof manager[method], 'function', `${Manager.name}.${method}`);
  }
  // Shared status accounting: queued tasks are reflected without loading.
  assert.deepEqual(manager.getStatus(), {
    ...manager.getStatus(), total: 0, loaded: 0, failed: 0
  });
}

console.log('ok - one canonical TextureLoadingManager shared by both texture paths');
