#!/usr/bin/env node
// SPDX-License-Identifier: Apache 2.0
// Exercise core adapter progress and completion from Node.js.

import fs from 'node:fs';
import assert from 'node:assert/strict';
import {LightUSDLoader} from '../src/lightusd/LightUSDLoader.js';
import createLightUSD from '../src/lightusd/lightusd.js';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// ============================================================================
// Synthetic USD Generation
// ============================================================================

/**
 * Generate a simple USDA file with multiple meshes
 */
function generateSyntheticUSDA(meshCount = 3) {
  let usda = `#usda 1.0
(
    defaultPrim = "Root"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "Root"
{
`;

  for (let i = 0; i < meshCount; i++) {
    const y = i * 2.5;
    usda += `
    def Mesh "Mesh_${String(i).padStart(3, '0')}"
    {
        float3[] extent = [(-1, -1, -1), (1, 1, 1)]
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
        point3f[] points = [
            (-1, ${y - 1}, 1), (1, ${y - 1}, 1), (-1, ${y - 1}, -1), (1, ${y - 1}, -1),
            (-1, ${y + 1}, -1), (1, ${y + 1}, -1), (-1, ${y + 1}, 1), (1, ${y + 1}, 1)
        ]
        uniform token subdivisionScheme = "none"
    }
`;
  }

  usda += `}
`;

  return usda;
}

/**
 * Generate USDA with mixed geometry types (mesh, cube, sphere)
 */
function generateMixedGeometryUSDA() {
  return `#usda 1.0
(
    defaultPrim = "Root"
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "Root"
{
    def Mesh "CustomMesh"
    {
        float3[] extent = [(-1, -1, -1), (1, 1, 1)]
        int[] faceVertexCounts = [3, 3, 3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2]
        point3f[] points = [(0, 1, 0), (-1, -1, 1), (1, -1, 1), (0, -1, -1)]
        uniform token subdivisionScheme = "none"
    }

    def Cube "SimpleCube"
    {
        double size = 2
        double3 xformOp:translate = (3, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Sphere "SimpleSphere"
    {
        double radius = 1
        double3 xformOp:translate = (-3, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
`;
}

// ============================================================================
// Test Runner
// ============================================================================

const nativePromise = createLightUSD();

async function runTest(testName, usdData, filename = 'test.usda', expectedMeshes = null) {
  console.log(`\nTest: ${testName}`);
  const loader = new LightUSDLoader();
  loader.native_ = await nativePromise;
  const bytes = typeof usdData === 'string' ? new TextEncoder().encode(usdData) : usdData;
  const events = [];
  let scene;
  try {
    scene = await loader.parseWithProgress(bytes, filename, {onProgress: info => {
      assert.ok(Number.isFinite(info.percentage));
      events.push(info);
      console.log(`  [Progress] ${Math.round(info.percentage)}% | ${info.stage}`);
    }});
    assert.ok(scene.numMeshes() > 0);
    if (expectedMeshes !== null) assert.equal(scene.numMeshes(), expectedMeshes);
    assert.ok(events.some(info => info.stage === 'native-load'));
    assert.ok(events.some(info => info.stage === 'mesh-copy'));
    assert.equal(events.at(-1)?.stage, 'complete');
    assert.equal(events.at(-1)?.percentage, 100);
    console.log(`  SUCCESS: ${scene.numMeshes()} meshes, ${events.length} progress events`);
    return true;
  } catch (error) {
    console.error(`  FAILED: ${error.message}`);
    return false;
  } finally {
    scene?.delete();
  }
}

async function runFileTest(filePath) {
  if (!fs.existsSync(filePath)) return null;
  return runTest(filePath, fs.readFileSync(filePath), path.basename(filePath));
}

// ============================================================================
// Main
// ============================================================================

async function main() {
  console.log('╔════════════════════════════════════════════════════════════╗');
  console.log('║     Core Progress Callback Test (Node.js CLI)             ║');
  console.log('╚════════════════════════════════════════════════════════════╝');

  const results = {
    passed: 0,
    failed: 0,
    skipped: 0
  };

  // Test 1: Simple synthetic USD with 3 meshes
  const result1 = await runTest(
    'Synthetic USDA - 3 Meshes',
    generateSyntheticUSDA(3),
    'synthetic-3mesh.usda', 3
  );
  if (result1) results.passed++; else results.failed++;

  // Test 2: Synthetic USD with 10 meshes
  const result2 = await runTest(
    'Synthetic USDA - 10 Meshes',
    generateSyntheticUSDA(10),
    'synthetic-10mesh.usda', 10
  );
  if (result2) results.passed++; else results.failed++;

  // Test 3: Mixed geometry types
  const result3 = await runTest(
    'Mixed Geometry (Mesh + Cube + Sphere)',
    generateMixedGeometryUSDA(),
    'mixed-geometry.usda', 3
  );
  if (result3) results.passed++; else results.failed++;

  // Test 4: Load from file (if available)
  const testFiles = [
    path.join(__dirname, 'assets/multi-mesh-test.usda'),
    path.join(__dirname, 'assets/suzanne.usdc'),
    path.join(__dirname, 'assets/mtlx-normalmap-sphere.usdz')
  ];

  for (const filePath of testFiles) {
    const result = await runFileTest(filePath);
    if (result === true) results.passed++;
    else if (result === false) results.failed++;
    else results.skipped++;
  }

  // Summary
  console.log('\n' + '═'.repeat(60));
  console.log('SUMMARY');
  console.log('═'.repeat(60));
  console.log(`  ✅ Passed:  ${results.passed}`);
  console.log(`  ❌ Failed:  ${results.failed}`);
  console.log(`  ⚠️  Skipped: ${results.skipped}`);
  console.log('═'.repeat(60));

  if (results.failed > 0) {
    process.exit(1);
  }
}

main().catch(err => {
  console.error('\n💥 Unhandled error:', err);
  process.exit(1);
});
