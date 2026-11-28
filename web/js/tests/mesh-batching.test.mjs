// SPDX-License-Identifier: Apache-2.0
// three.js-side post-processing: content-based material dedup and BatchedMesh
// grouping must remain usable as standalone exported helpers.
import assert from 'node:assert/strict';
import * as THREE from 'three';
import {dedupMaterialsByContent, batchByMaterial} from '../src/lightusd/MeshBatching.js';

const quad = () => new THREE.BufferGeometry();
const indexedQuad = () => {
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute('position', new THREE.Float32BufferAttribute([
    0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0
  ], 3));
  geometry.setIndex([0, 1, 2, 0, 2, 3]);
  return geometry;
};

const root = new THREE.Group();
const materialA = new THREE.MeshStandardMaterial({color: 0xff0000, roughness: 0.5});
const materialB = new THREE.MeshStandardMaterial({color: 0xff0000, roughness: 0.5});
const other = new THREE.MeshStandardMaterial({color: 0x00ff00});
root.add(new THREE.Mesh(indexedQuad(), materialA));
root.add(new THREE.Mesh(indexedQuad(), materialB));
root.add(new THREE.Mesh(indexedQuad(), other));

const dedup = dedupMaterialsByContent(root);
assert.equal(dedup.totalBindings, 3);
assert.equal(dedup.uniqueMaterials, 2, 'visually identical materials collapse');
assert.equal(dedup.replacedBindings, 1, 'only the duplicate binding is replaced');
assert.equal(root.children[0].material, root.children[1].material);
assert.notEqual(root.children[0].material, root.children[2].material);
assert.equal(typeof quad().dispose, 'function', 'geometry helper stays compatible');

const batched = batchByMaterial(root);
assert.equal(batched.batchCount, 1, 'the two same-material meshes form one batch');
assert.equal(batched.batchedMeshCount, 2);
assert.equal(batched.skippedMeshCount, 1, 'the unique-material mesh stays unbatched');
assert.equal(batched.skipReasons.singletonGroup, 1);
const batch = root.children.find(obj => obj.isBatchedMesh);
assert.ok(batch, 'a BatchedMesh was added to the scene');
assert.equal(batch.userData.batchedInstances.size, 2);
assert.equal(batch.userData.isBatched, true);

// Skinned meshes and non-indexed geometry stay as regular meshes.
const skipRoot = new THREE.Group();
const mat = new THREE.MeshStandardMaterial();
const skinned = new THREE.SkinnedMesh(indexedQuad(), mat);
const nonIndexed = new THREE.Mesh(new THREE.BufferGeometry(), mat);
skipRoot.add(skinned, nonIndexed);
const skipped = batchByMaterial(skipRoot);
assert.equal(skipped.batchCount, 0);
assert.equal(skipped.skipReasons.skinned, 1);
assert.equal(skipped.skipReasons.noIndex, 1);
assert.equal(skipped.skippedMeshCount, 0, 'ineligible meshes are filtered before batching');

console.log('ok - MeshBatching content dedup and BatchedMesh grouping');
