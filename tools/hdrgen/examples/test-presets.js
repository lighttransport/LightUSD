#!/usr/bin/env node
/**
 * Quick test script for validating all presets
 */

import { EnvironmentPrefilter, HDRGenerator, HDRImage, HDRReader, ImportanceMap, Vec3 } from '../src/hdrgen.js';
import * as path from 'path';
import { fileURLToPath } from 'url';
import * as fs from 'fs';
import * as os from 'os';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const outputDir = fs.mkdtempSync(path.join(os.tmpdir(), 'lightusd-hdrgen-'));

console.log('Running HDRGen tests...\n');

let passed = 0;
let failed = 0;

function test(name, fn) {
  try {
    console.log(`Testing: ${name}`);
    fn();
    console.log(`✓ ${name} passed\n`);
    passed++;
  } catch (err) {
    console.error(`✗ ${name} failed: ${err.message}\n`);
    failed++;
  }
}

// Test 1: White Furnace Generation
test('White Furnace (256x128)', () => {
  const result = HDRGenerator.generate({
    preset: 'white-furnace',
    width: 256,
    height: 128,
    projection: 'latlong',
    format: 'hdr',
    output: path.join(outputDir, 'test_furnace.hdr'),
    presetOptions: { intensity: 1.0 }
  });

  if (!result.latLongImage) throw new Error('No image generated');
  if (result.latLongImage.width !== 256) throw new Error('Wrong width');
  if (result.latLongImage.height !== 128) throw new Error('Wrong height');

  // Check first pixel is white
  const pixel = result.latLongImage.getPixel(0, 0);
  if (Math.abs(pixel.r - 1.0) > 0.01) throw new Error('Wrong intensity');
});

// Test 2: Sun & Sky Generation
test('Sun & Sky (256x128)', () => {
  const result = HDRGenerator.generate({
    preset: 'sun-sky',
    width: 256,
    height: 128,
    projection: 'latlong',
    format: 'hdr',
    output: path.join(outputDir, 'test_sunsky.hdr'),
    presetOptions: {
      sunElevation: 45,
      sunAzimuth: 135,
      sunIntensity: 100.0
    }
  });

  if (!result.latLongImage) throw new Error('No image generated');

  // Check that sky has varying intensities (not uniform)
  const p1 = result.latLongImage.getPixel(0, 0);
  const p2 = result.latLongImage.getPixel(128, 64);
  if (p1.r === p2.r && p1.g === p2.g && p1.b === p2.b) {
    throw new Error('Sky should not be uniform');
  }
});

// Test 3: Studio Lighting Generation
test('Studio Lighting (256x128)', () => {
  const result = HDRGenerator.generate({
    preset: 'studio',
    width: 256,
    height: 128,
    projection: 'latlong',
    format: 'hdr',
    output: path.join(outputDir, 'test_studio.hdr'),
    presetOptions: {}
  });

  if (!result.latLongImage) throw new Error('No image generated');
});

test('Sunset preset', () => {
  const result = HDRGenerator.generate({
    preset: 'sunset', width: 128, height: 64, projection: 'latlong'
  });
  const image = result.latLongImage;
  let maxRed = 0;
  let maxBlue = 0;
  for (let i = 0; i < image.data.length; i += 3) {
    maxRed = Math.max(maxRed, image.data[i]);
    maxBlue = Math.max(maxBlue, image.data[i + 2]);
  }
  if (!(maxRed > maxBlue * 1.5)) throw new Error('Sunset is not warm-dominant');
});

test('Overcast preset', () => {
  const result = HDRGenerator.generate({
    preset: 'overcast', width: 64, height: 32, projection: 'latlong'
  });
  const zenith = result.latLongImage.getPixel(16, 0);
  const horizon = result.latLongImage.getPixel(16, 15);
  const ground = result.latLongImage.getPixel(16, 24);
  if (!(zenith.r > horizon.r && horizon.r > ground.r)) {
    throw new Error('Overcast zenith/horizon/ground ordering is incorrect');
  }
});

// Test 4: Cubemap Generation
test('Cubemap Generation (64x64 faces)', () => {
  const result = HDRGenerator.generate({
    preset: 'white-furnace',
    width: 64,
    height: 64,
    projection: 'cubemap',
    format: 'hdr',
    output: path.join(outputDir, 'test_cube'),
    presetOptions: { intensity: 1.0 }
  });

  if (!result.faces) throw new Error('No cubemap faces generated');
  if (result.faces.length !== 6) throw new Error('Should generate 6 faces');

  // Check each face
  for (const face of result.faces) {
    if (!face.image) throw new Error('Missing face image');
    if (face.image.width !== 64) throw new Error('Wrong face size');
  }
});

// Test 5: HDR Image Class
test('HDRImage class', () => {
  const img = new HDRImage(100, 50);
  if (img.width !== 100) throw new Error('Wrong width');
  if (img.height !== 50) throw new Error('Wrong height');
  if (img.data.length !== 100 * 50 * 3) throw new Error('Wrong data size');

  img.setPixel(10, 20, 1.5, 2.5, 3.5);
  const pixel = img.getPixel(10, 20);
  if (Math.abs(pixel.r - 1.5) > 0.001) throw new Error('setPixel/getPixel failed');
});

// Test 6: Vec3 math
test('Vec3 math utilities', () => {
  const v1 = new Vec3(1, 2, 3);
  const v2 = new Vec3(4, 5, 6);

  const sum = Vec3.add(v1, v2);
  if (sum.x !== 5 || sum.y !== 7 || sum.z !== 9) throw new Error('Vec3.add failed');

  const dot = Vec3.dot(v1, v2);
  if (dot !== 32) throw new Error('Vec3.dot failed');

  const len = new Vec3(3, 4, 0).length();
  if (Math.abs(len - 5) > 0.001) throw new Error('Vec3.length failed');

  const norm = new Vec3(0, 5, 0).normalize();
  if (Math.abs(norm.y - 1) > 0.001) throw new Error('Vec3.normalize failed');
});

// Test 7: Lat-Long to Direction Conversion
test('Lat-Long coordinate conversion', () => {
  // Test north pole (u=0.5, v=0)
  const north = HDRImage.latLongToDir(0.5, 0.0);
  if (Math.abs(north.y - 1) > 0.01) throw new Error('North pole conversion failed');

  // Test south pole (u=0.5, v=1)
  const south = HDRImage.latLongToDir(0.5, 1.0);
  if (Math.abs(south.y + 1) > 0.01) throw new Error('South pole conversion failed');

  // Test equator front (u=0.5, v=0.5)
  const front = HDRImage.latLongToDir(0.5, 0.5);
  if (Math.abs(front.y) > 0.01) throw new Error('Equator conversion failed');
});

// Test 8: Custom Intensity White Furnace
test('White Furnace with custom intensity', () => {
  const result = HDRGenerator.generate({
    preset: 'white-furnace',
    width: 64,
    height: 32,
    projection: 'latlong',
    format: 'hdr',
    output: path.join(outputDir, 'test_furnace_10x.hdr'),
    presetOptions: { intensity: 10.0 }
  });

  const pixel = result.latLongImage.getPixel(32, 16);
  if (Math.abs(pixel.r - 10.0) > 0.01) throw new Error('Wrong intensity');
});

test('Importance map normalization and sampling', () => {
  const image = new HDRImage(8, 4);
  for (let y = 0; y < image.height; y++) {
    for (let x = 0; x < image.width; x++) image.setPixel(x, y, 1, 1, 1);
  }
  const distribution = ImportanceMap.build(image);
  if (distribution.rowCdf[0] !== 0 || distribution.rowCdf[4] !== 1) {
    throw new Error('Row CDF is not normalized');
  }
  if (!(distribution.rowWeights[1] > distribution.rowWeights[0])) {
    throw new Error('Solid-angle weighting did not reduce polar rows');
  }
  const sample = ImportanceMap.sample(distribution, 0.5, 0.5);
  const expectedPdf = 1.0 / (4.0 * Math.PI);
  if (Math.abs(sample.pdf - expectedPdf) > 0.01) {
    throw new Error(`Uniform-environment PDF is incorrect: ${sample.pdf}`);
  }
});

test('Importance map selects a bright texel', () => {
  const image = new HDRImage(8, 4);
  image.setPixel(6, 2, 100, 100, 100);
  const distribution = ImportanceMap.build(image);
  const sample = ImportanceMap.sample(distribution, 0.5, 0.5);
  if (sample.x !== 6 || sample.y !== 2) {
    throw new Error(`Expected bright texel (6,2), got (${sample.x},${sample.y})`);
  }
});

test('OpenEXR scanline output', () => {
  const output = path.join(outputDir, 'test.exr');
  HDRGenerator.generate({
    preset: 'white-furnace', width: 7, height: 3,
    projection: 'latlong', format: 'exr', output,
    presetOptions: { intensity: 2.0 }
  });
  const bytes = fs.readFileSync(output);
  if (bytes.readUInt32LE(0) !== 20000630 || bytes.readUInt32LE(4) !== 2) {
    throw new Error('Invalid OpenEXR magic or version');
  }
  if (!bytes.includes(Buffer.from('channels\0chlist\0', 'ascii'))) {
    throw new Error('OpenEXR channel list is missing');
  }
  if (!bytes.includes(Buffer.from('compression\0compression\0\x01\0\0\0\x02', 'binary'))) {
    throw new Error('OpenEXR ZIPS compression attribute is missing');
  }
  if (fs.existsSync(path.join(outputDir, 'test.hdr'))) {
    throw new Error('OpenEXR output unexpectedly fell back to HDR');
  }
});

test('OpenEXR uncompressed compatibility mode', () => {
  const output = path.join(outputDir, 'test-uncompressed.exr');
  HDRGenerator.generate({
    preset: 'white-furnace', width: 5, height: 2,
    projection: 'latlong', format: 'exr', output,
    exrCompression: 'none'
  });
  const bytes = fs.readFileSync(output);
  if (!bytes.includes(Buffer.from('compression\0compression\0\x01\0\0\0\0', 'binary'))) {
    throw new Error('OpenEXR uncompressed attribute is missing');
  }
});

test('Diffuse and GGX environment prefiltering', () => {
  const source = new HDRImage(16, 8);
  for (let y = 0; y < source.height; y++) {
    for (let x = 0; x < source.width; x++) source.setPixel(x, y, 2, 2, 2);
  }
  const directory = path.join(outputDir, 'prefilter');
  const result = EnvironmentPrefilter.generate(source, {
    directory, width: 8, height: 4, levels: 3, samples: 16
  });
  const diffuse = result.diffuse.getPixel(0, 0);
  if (Math.abs(diffuse.r - 2.0 * Math.PI) > 0.001) {
    throw new Error(`Diffuse furnace integral is incorrect: ${diffuse.r}`);
  }
  if (result.specular.length !== 3 || result.specular[2].image.width !== 2) {
    throw new Error('Specular mip chain dimensions are incorrect');
  }
  for (const level of result.specular) {
    const pixel = level.image.getPixel(0, 0);
    if (Math.abs(pixel.r - 2.0) > 0.001) {
      throw new Error(`Specular furnace changed at roughness ${level.roughness}`);
    }
  }
  if (!fs.existsSync(path.join(directory, 'manifest.json')) ||
      !fs.existsSync(path.join(directory, 'diffuse.exr'))) {
    throw new Error('Prefilter outputs are missing');
  }
});

test('HDR and compressed EXR panorama input', () => {
  const exr = path.join(outputDir, 'input.exr');
  const hdr = path.join(outputDir, 'input.hdr');
  HDRGenerator.generate({
    preset: 'white-furnace', width: 12, height: 6,
    format: 'exr', output: exr, presetOptions: { intensity: 4.0 }
  });
  HDRGenerator.generate({
    preset: 'white-furnace', width: 12, height: 6,
    format: 'hdr', output: hdr, presetOptions: { intensity: 4.0 }
  });
  const exrImage = HDRReader.read(exr);
  const hdrImage = HDRReader.read(hdr);
  if (exrImage.width !== 12 || exrImage.height !== 6 ||
      Math.abs(exrImage.getPixel(3, 2).r - 4.0) > 1e-6) {
    throw new Error('Compressed EXR panorama read failed');
  }
  if (Math.abs(hdrImage.getPixel(3, 2).r - 4.0) > 0.05) {
    throw new Error('Radiance HDR panorama read failed');
  }
  const transformed = HDRGenerator.generate({
    input: exr, rotation: 90, intensityScale: 0.25
  }).latLongImage;
  if (Math.abs(transformed.getPixel(0, 0).r - 1.0) > 1e-6) {
    throw new Error('Input panorama transform pipeline failed');
  }
});

test('Time-of-day sequence generation', () => {
  const output = path.join(outputDir, 'sequence', 'sky-###.exr');
  const sequence = HDRGenerator.generateTimeSequence({
    start: 6, end: 8, step: 1, width: 16, height: 8,
    format: 'exr', output
  });
  if (sequence.frames.length !== 3 ||
      !sequence.frames[0].file.endsWith('sky-000.exr') ||
      !sequence.frames[2].file.endsWith('sky-002.exr')) {
    throw new Error('Time sequence filenames are not stable');
  }
  if (!(sequence.frames[1].sunElevation > sequence.frames[0].sunElevation)) {
    throw new Error('Time sequence solar elevation did not advance');
  }
  if (!fs.existsSync(sequence.manifest)) throw new Error('Time sequence manifest is missing');
});

// Summary
console.log('='.repeat(60));
console.log(`Test Results: ${passed} passed, ${failed} failed`);
console.log('='.repeat(60));
fs.rmSync(outputDir, { recursive: true, force: true });

if (failed > 0) {
  console.error('\n✗ Some tests failed');
  process.exit(1);
} else {
  console.log('\n✓ All tests passed!');
  process.exit(0);
}
