import { buildIndexedEdgeUses, forEachIndexedTriangle, hasInvalidValue, INDEXED_MESH_MAX_INDICES, normalizeIndexedMesh } from './indexed-mesh.js';
import { linearColorTransformMatrix } from '../../src/lightusd/ColorSpaceMath.js';

const clamp = (value) => Math.max(0, Math.min(255, Math.round(value * 255)));
const isIterableBuffer = (value) => value != null && Number.isSafeInteger(value.length) && value.length >= 0 && typeof value[Symbol.iterator] === 'function';
const srgbToLinear = (value) => value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
const linearToSrgb = (value) => value <= 0.0031308 ? value * 12.92 : 1.055 * (Math.max(0, value) ** (1 / 2.4)) - 0.055;
const invert3 = (m) => {
  const d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
  return [(m[4] * m[8] - m[5] * m[7]) / d, (m[2] * m[7] - m[1] * m[8]) / d, (m[1] * m[5] - m[2] * m[4]) / d, (m[5] * m[6] - m[3] * m[8]) / d, (m[0] * m[8] - m[2] * m[6]) / d, (m[2] * m[3] - m[0] * m[5]) / d, (m[3] * m[7] - m[4] * m[6]) / d, (m[1] * m[6] - m[0] * m[7]) / d, (m[0] * m[4] - m[1] * m[3]) / d];
};
const multiply3 = (a, b) => Array.from({ length: 9 }, (_, i) => { const row = Math.floor(i / 3), column = i % 3; return a[row * 3] * b[column] + a[row * 3 + 1] * b[column + 3] + a[row * 3 + 2] * b[column + 6]; });
const multiply3v = (m, v) => [m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]];
const chromaticitiesToXyz = (primaries, white = [.3127, .3290]) => {
  const [r, g, b] = primaries, basis = [r[0], g[0], b[0], r[1], g[1], b[1], 1 - r[0] - r[1], 1 - g[0] - g[1], 1 - b[0] - b[1]], scale = multiply3v(invert3(basis), [white[0] / white[1], 1, (1 - white[0] - white[1]) / white[1]]);
  return basis.map((value, index) => value * scale[index % 3]);
};
const srgbToXyz = chromaticitiesToXyz([[.64, .33], [.30, .60], [.15, .06]]);
// The primary matrices come from LightUSD's canonical color-space utility.
const COLOR_MATRICES = {
  'display-p3->linear-srgb': linearColorTransformMatrix('lin_p3d65_scene', 'lin_rec709_scene'),
  'linear-srgb->display-p3': linearColorTransformMatrix('lin_rec709_scene', 'lin_p3d65_scene'),
  'acescg->linear-srgb': linearColorTransformMatrix('lin_ap1_scene', 'lin_rec709_scene'),
  'linear-srgb->acescg': linearColorTransformMatrix('lin_rec709_scene', 'lin_ap1_scene'),
  'aces2065-1->linear-srgb': linearColorTransformMatrix('lin_ap0_scene', 'lin_rec709_scene'),
  'linear-srgb->aces2065-1': linearColorTransformMatrix('lin_rec709_scene', 'lin_ap0_scene'),
  'rec2020->linear-srgb': linearColorTransformMatrix('lin_rec2020_scene', 'lin_rec709_scene'),
  'linear-srgb->rec2020': linearColorTransformMatrix('lin_rec709_scene', 'lin_rec2020_scene'),
  'adobergb->linear-srgb': linearColorTransformMatrix('lin_adobergb_scene', 'lin_rec709_scene'),
  'linear-srgb->adobergb': linearColorTransformMatrix('lin_rec709_scene', 'lin_adobergb_scene'),
};
const applyColorMatrix = (rgb, matrix, clampNonNegative = true) => { const result = [matrix[0] * rgb[0] + matrix[1] * rgb[1] + matrix[2] * rgb[2], matrix[3] * rgb[0] + matrix[4] * rgb[1] + matrix[5] * rgb[2], matrix[6] * rgb[0] + matrix[7] * rgb[1] + matrix[8] * rgb[2]]; return clampNonNegative ? result.map((value) => Math.max(0, value)) : result; };
const rec2020ToLinear = (value) => value <= 0 ? 0 : value < 0.0812428582986353 ? value / 4.5 : ((value + 0.09929682680944) / 1.09929682680944) ** (1 / .45);
const linearToRec2020 = (value) => value <= 0 ? 0 : value < .018054 ? 4.5 * value : 1.09929682680944 * value ** .45 - .09929682680944;

const ascii4 = (view, offset) => String.fromCharCode(view.getUint8(offset), view.getUint8(offset + 1), view.getUint8(offset + 2), view.getUint8(offset + 3));
const s15Fixed16 = (view, offset) => view.getInt32(offset, false) / 65536;
const ICC_D50_TO_D65 = [.9554734, -.0230985, .0632593, -.0283697, 1.0099955, .0210414, .012314, -.0205077, 1.3303659];

// Decode the portable ICC matrix-shaper subset used by ordinary RGB images.
// LUT-based, CMYK, Lab, and malformed profiles deliberately fail closed: the
// browser/native image decoders remain responsible for those profiles.
export function decodeEmbeddedICCProfile(profile) {
  const bytes = profile instanceof Uint8Array ? profile : profile instanceof ArrayBuffer ? new Uint8Array(profile) : null;
  if (!bytes || bytes.length < 132) throw new Error('Embedded ICC profile is truncated.');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), declaredSize = view.getUint32(0, false);
  if (declaredSize < 132 || declaredSize > bytes.length || ascii4(view, 36) !== 'acsp' || ascii4(view, 16) !== 'RGB ') throw new Error('Embedded ICC profile is not a supported RGB profile.');
  const tagCount = view.getUint32(128, false);
  if (tagCount > 64 || 132 + tagCount * 12 > declaredSize) throw new Error('Embedded ICC profile has an invalid tag table.');
  const tags = new Map();
  for (let i = 0; i < tagCount; i++) {
    const offset = 132 + i * 12, signature = ascii4(view, offset), tagOffset = view.getUint32(offset + 4, false), tagSize = view.getUint32(offset + 8, false);
    if (tagOffset < 132 || tagSize < 12 || tagOffset + tagSize > declaredSize) throw new Error('Embedded ICC profile contains an out-of-range tag.');
    tags.set(signature, { offset: tagOffset, size: tagSize });
  }
  const xyzTags = ['rXYZ', 'gXYZ', 'bXYZ'].map((name) => tags.get(name)), trcTags = ['rTRC', 'gTRC', 'bTRC'].map((name) => tags.get(name));
  if (xyzTags.some((tag) => !tag) || trcTags.some((tag) => !tag)) throw new Error('Embedded ICC profile lacks a complete RGB matrix-shaper tag set.');
  const matrixToXyzD50 = xyzTags.map((tag) => {
    if (ascii4(view, tag.offset) !== 'XYZ ' || tag.size < 20) throw new Error('Embedded ICC XYZ tag is malformed.');
    return [s15Fixed16(view, tag.offset + 8), s15Fixed16(view, tag.offset + 12), s15Fixed16(view, tag.offset + 16)];
  });
  const gammaValues = trcTags.map((tag) => {
    const type = ascii4(view, tag.offset);
    if (type === 'curv') {
      if (tag.size < 14) throw new Error('Embedded ICC curve tag is malformed.');
      const count = view.getUint32(tag.offset + 8, false);
      if (count === 0) return 1;
      if (count !== 1) throw new Error('Embedded ICC LUT curves are unsupported.');
      return view.getUint16(tag.offset + 12, false) / 256;
    }
    if (type === 'para' && tag.size >= 16 && view.getUint16(tag.offset + 8, false) === 0) return s15Fixed16(view, tag.offset + 12);
    throw new Error('Embedded ICC tone curves are unsupported.');
  });
  if (!gammaValues.every((value) => Number.isFinite(value) && value > 0)) throw new Error('Embedded ICC channel curves are invalid.');
  const profileMatrix = [matrixToXyzD50[0][0], matrixToXyzD50[1][0], matrixToXyzD50[2][0], matrixToXyzD50[0][1], matrixToXyzD50[1][1], matrixToXyzD50[2][1], matrixToXyzD50[0][2], matrixToXyzD50[1][2], matrixToXyzD50[2][2]], toLinearSRGB = multiply3(invert3(srgbToXyz), ICC_D50_TO_D65), matrix = multiply3(toLinearSRGB, profileMatrix), gamma = gammaValues.every((value) => Math.abs(value - gammaValues[0]) <= 1e-4) ? gammaValues[0] : gammaValues;
  if (!matrix.every((value) => Number.isFinite(value))) throw new Error('Embedded ICC profile produced an invalid RGB transform.');
  return { matrix, gamma, bias: 0, canonical: 'icc-matrix-shaper', colorRole: 'color', profileClass: ascii4(view, 12), colorSpace: 'RGB' };
}

// Encode the same deliberately small ICC subset. This is suitable for Lucia
// generated RGB assets; profiles requiring LUTs or non-uniform channel curves
// remain outside the bake contract.
export function encodeEmbeddedICCProfile(transform = {}) {
  const matrix = transform.matrix ?? transform.sourceToDisplayLinear, rawGamma = transform.gamma ?? transform.sourceGamma ?? 1, gamma = isIterableBuffer(rawGamma) ? Array.from(rawGamma, Number) : Number(rawGamma);
  if (!isIterableBuffer(matrix) || matrix.length !== 9 || hasInvalidValue(matrix, (value) => !Number.isFinite(Number(value))) || !(typeof gamma === 'number' && Number.isFinite(gamma) && gamma > 0 && gamma <= 255 || Array.isArray(gamma) && gamma.length === 3 && gamma.every((value) => Number.isFinite(value) && value > 0 && value <= 255))) throw new Error('ICC encoding requires a finite 3x3 RGB matrix and gamma in the supported range.');
  const profileMatrix = multiply3(invert3(ICC_D50_TO_D65), multiply3(srgbToXyz, Array.from(matrix, Number)));
  if (!profileMatrix.every((value) => Number.isFinite(value) && value >= -32768 && value < 32768)) throw new Error('ICC encoding received a singular or out-of-range RGB matrix.');
  const profile = new Uint8Array(312), view = new DataView(profile.buffer), write = (offset, value) => [...value].forEach((character, index) => view.setUint8(offset + index, character.charCodeAt(0))), writeFixed = (offset, value) => view.setInt32(offset, Math.round(value * 65536), false);
  view.setUint32(0, profile.length, false); view.setUint32(8, 0x04300000, false); write(12, 'mntr'); write(16, 'RGB '); write(36, 'acsp'); view.setUint32(128, 6, false);
  const xyzOffsets = [204, 224, 244];
  ['rXYZ', 'gXYZ', 'bXYZ'].forEach((tag, index) => { const table = 132 + index * 12, offset = xyzOffsets[index]; write(table, tag); view.setUint32(table + 4, offset, false); view.setUint32(table + 8, 20, false); write(offset, 'XYZ '); writeFixed(offset + 8, profileMatrix[index]); writeFixed(offset + 12, profileMatrix[index + 3]); writeFixed(offset + 16, profileMatrix[index + 6]); });
  const encodedGamma = Array.isArray(gamma) ? gamma.map((value) => Math.round(value * 256)) : [Math.round(gamma * 256), Math.round(gamma * 256), Math.round(gamma * 256)], trcOffsets = [264, 280, 296];
  ['rTRC', 'gTRC', 'bTRC'].forEach((tag, index) => { const table = 132 + (index + 3) * 12, offset = trcOffsets[index]; write(table, tag); view.setUint32(table + 4, offset, false); view.setUint32(table + 8, 14, false); write(offset, 'curv'); view.setUint32(offset + 8, 1, false); view.setUint16(offset + 12, encodedGamma[index], false); });
  return profile;
}

// Bake images are encoded as sRGB for color channels and linear/raw for data
// channels. Keep this small path in the shared bake utility so worker/native
// callers use the same transfer curve without depending on Canvas color space.
export function normalizeBakeColorSpace(value = 'srgb') {
  const token = String(value || 'srgb').toLowerCase().replace(/[ _-]+/g, '');
  if (token === 'raw' || token === 'data' || token === 'identity') return 'raw';
  if (token.includes('displayp3') || token === 'p3d65' || token === 'p3') return token.includes('linear') || token.startsWith('lin') ? 'linear-display-p3' : 'display-p3';
  if (token.includes('acescg') || token === 'linap1' || token === 'linap1scene') return 'acescg';
  if (token.includes('aces20651') || token === 'linap0scene') return 'aces2065-1';
  if (token.includes('adobergb') || token === 'adobe1998') return token.includes('linear') || token.startsWith('lin') ? 'linear-adobergb' : 'adobergb';
  if (token.includes('rec2020') || token === 'bt2020') return token.includes('linear') || token.startsWith('lin') ? 'linear-rec2020' : 'rec2020';
  if (token.includes('srgb')) return token.includes('linear') || token.startsWith('lin') ? 'linear-srgb' : 'srgb';
  if (token.includes('linear') || token.startsWith('lin')) return 'linear-srgb';
  throw new Error(`Unsupported bake color space: ${value}`);
}

export function normalizeBakeColorTransform(transform) {
  if (!transform || typeof transform !== 'object') return null;
  let embedded = null;
  if (transform.iccProfile != null) {
    try { embedded = decodeEmbeddedICCProfile(transform.iccProfile); } catch { return null; }
  }
  const matrix = transform.matrix ?? transform.sourceToDisplayLinear ?? embedded?.matrix;
  if (!isIterableBuffer(matrix) || matrix.length !== 9 || hasInvalidValue(matrix, (value) => !Number.isFinite(Number(value)))) return null;
  const rawGamma = transform.gamma ?? transform.sourceGamma ?? embedded?.gamma ?? 1, gamma = isIterableBuffer(rawGamma) ? Array.from(rawGamma, Number) : Number(rawGamma), bias = Number(transform.linearBias ?? transform.sourceLinearBias ?? embedded?.bias ?? 0);
  if (!(typeof gamma === 'number' && Number.isFinite(gamma) && gamma > 0 || Array.isArray(gamma) && gamma.length === 3 && gamma.every((value) => Number.isFinite(value) && value > 0)) || !Number.isFinite(bias) || bias < 0) return null;
  return { matrix: Array.from(matrix, Number), gamma, bias, canonical: String(transform.canonical || embedded?.canonical || ''), colorRole: String(transform.colorRole || embedded?.colorRole || (transform.sourceColorIsData ? 'data' : '')) };
}

const resizeColorTransfer = (colorSpace) => {
  const normalized = normalizeBakeColorSpace(colorSpace);
  if (normalized === 'srgb' || normalized === 'display-p3') return { toLinear: srgbToLinear, fromLinear: linearToSrgb };
  if (normalized === 'rec2020') return { toLinear: rec2020ToLinear, fromLinear: linearToRec2020 };
  if (normalized === 'adobergb') return { toLinear: (value) => value ** 2.2, fromLinear: (value) => value ** (1 / 2.2) };
  return null;
};

export function convertBakeColor(rgb, sourceColorSpace = 'srgb', destinationColorSpace = 'srgb', sourceTransform = null) {
  if (!isIterableBuffer(rgb) || rgb.length !== 3 || hasInvalidValue(rgb, (value) => !Number.isFinite(value))) throw new Error('Bake colors must contain exactly three finite RGB components.');
  const source = normalizeBakeColorSpace(sourceColorSpace), destination = normalizeBakeColorSpace(destinationColorSpace);
  const resolved = normalizeBakeColorTransform(sourceTransform);
  if (!resolved && (source === destination || source === 'raw' || source === 'data' || destination === 'raw' || destination === 'data')) return rgb.map((value) => Number(value));
  if (source === 'raw' || source === 'data' || destination === 'raw' || destination === 'data') return rgb.map((value) => Number(value));
  if (resolved && (destination === 'srgb' || destination === 'linear-srgb')) {
    let sourceLinear = rgb.map((value) => Math.max(0, Math.min(1, Number(value))));
    const gamma = Array.isArray(resolved.gamma) ? resolved.gamma : [resolved.gamma, resolved.gamma, resolved.gamma];
    if (gamma.some((value) => value !== 1)) {
      if (resolved.bias > 0) sourceLinear = sourceLinear.map((value, index) => { const exponent = gamma[index], threshold = resolved.bias / (exponent - 1), phi = (resolved.bias / Math.exp(Math.log(exponent * resolved.bias / (exponent + exponent * resolved.bias - 1 - resolved.bias)) * exponent)) / (exponent - 1); return value < threshold ? value / phi : ((value + resolved.bias) / (1 + resolved.bias)) ** exponent; });
      else sourceLinear = sourceLinear.map((value, index) => value ** gamma[index]);
    } else if (resolved.colorRole === 'color' && /^srgb/i.test(resolved.canonical)) sourceLinear = sourceLinear.map(srgbToLinear);
    const linearSRGB = applyColorMatrix(sourceLinear, resolved.matrix, false);
    return destination === 'linear-srgb' ? linearSRGB : linearSRGB.map((value) => linearToSrgb(Math.max(0, value)));
  }
  const sourceLinear = source === 'srgb' || source === 'display-p3' ? rgb.map((value) => srgbToLinear(Math.max(0, Math.min(1, Number(value))))) : source === 'rec2020' ? rgb.map((value) => rec2020ToLinear(Math.max(0, Math.min(1, Number(value))))) : source === 'adobergb' ? rgb.map((value) => Math.max(0, Math.min(1, Number(value))) ** 2.2) : rgb.map(Number);
  const linearSRGB = source === 'linear-srgb' ? sourceLinear : source === 'display-p3' || source === 'linear-display-p3' ? applyColorMatrix(sourceLinear, COLOR_MATRICES['display-p3->linear-srgb']) : source === 'rec2020' || source === 'linear-rec2020' ? applyColorMatrix(sourceLinear, COLOR_MATRICES['rec2020->linear-srgb']) : source === 'adobergb' || source === 'linear-adobergb' ? applyColorMatrix(sourceLinear, COLOR_MATRICES['adobergb->linear-srgb']) : source === 'acescg' ? applyColorMatrix(sourceLinear, COLOR_MATRICES['acescg->linear-srgb']) : source === 'aces2065-1' ? applyColorMatrix(sourceLinear, COLOR_MATRICES['aces2065-1->linear-srgb']) : null;
  if (!linearSRGB) throw new Error(`Unsupported bake color-space conversion: ${sourceColorSpace} -> ${destinationColorSpace}`);
  if (destination === 'linear-srgb') return linearSRGB;
  const destinationLinear = destination === 'display-p3' || destination === 'linear-display-p3' ? applyColorMatrix(linearSRGB, COLOR_MATRICES['linear-srgb->display-p3']) : destination === 'rec2020' || destination === 'linear-rec2020' ? applyColorMatrix(linearSRGB, COLOR_MATRICES['linear-srgb->rec2020'], false) : destination === 'adobergb' || destination === 'linear-adobergb' ? applyColorMatrix(linearSRGB, COLOR_MATRICES['linear-srgb->adobergb'], false) : destination === 'acescg' ? applyColorMatrix(linearSRGB, COLOR_MATRICES['linear-srgb->acescg']) : destination === 'aces2065-1' ? applyColorMatrix(linearSRGB, COLOR_MATRICES['linear-srgb->aces2065-1'], false) : destination === 'srgb' ? linearSRGB : null;
  if (!destinationLinear) throw new Error(`Unsupported bake color-space conversion: ${sourceColorSpace} -> ${destinationColorSpace}`);
  return destination === 'srgb' || destination === 'display-p3' ? destinationLinear.map((value) => linearToSrgb(Math.max(0, value))) : destination === 'rec2020' ? destinationLinear.map((value) => linearToRec2020(Math.max(0, value))) : destination === 'adobergb' ? destinationLinear.map((value) => Math.max(0, value) ** (1 / 2.2)) : destinationLinear;
}

// Keep worker-side raster, ray, and readback buffers below the browser's
// conservative Lucia memory budget. This is an estimate, not an allocator;
// callers should fail before transferring large buffers to a worker.
export const BAKE_MEMORY_LIMIT_BYTES = 256 * 1024 * 1024;
export function estimateBakeWorkingBytes({ resolution, vertexCount = 0, indexCount = 0, sourceTextureBytes = 0, occlusion = false, samples = 1 } = {}) {
  const size = Math.max(1, Math.floor(resolution || 1)), pixels = size * size, rays = Math.max(1, Math.min(16, Math.floor(samples || 1)));
  const rasterBytes = pixels * (4 + 1 + 4); // RGBA output, coverage, island/temporary data.
  const meshBytes = Math.max(0, Math.floor(vertexCount)) * (3 + 2 + (occlusion ? 3 : 0)) * 4 + Math.max(0, Math.floor(indexCount)) * 4;
  const rayBytes = occlusion ? pixels * (3 * 4 + 3 * 4) + pixels * rays * (3 * 4 + 3 * 4 + 4 + 1) : 0;
  return rasterBytes + meshBytes + rayBytes + Math.max(0, Number(sourceTextureBytes) || 0);
}

export function estimateProjectionWorkingBytes({ resolution, targetVertexCount = 0, targetIndexCount = 0, sourceVertexCount = 0, sourceIndexCount = 0, sourceTextureBytes = 0 } = {}) {
  const size = Math.max(1, Math.floor(Number(resolution) || 1)), pixels = size * size;
  const meshBytes = (Math.max(0, Math.floor(targetVertexCount)) + Math.max(0, Math.floor(sourceVertexCount))) * 3 * 4 + (Math.max(0, Math.floor(targetIndexCount)) + Math.max(0, Math.floor(sourceIndexCount))) * 4;
  const rayBytes = pixels * (3 * 4 + 3 * 4 + 4 + 3 * 4 + 1 + 4); // origins, directions, pixel IDs, barycentrics, coverage, ownership.
  return meshBytes + rayBytes + pixels * 4 + Math.max(0, Number(sourceTextureBytes) || 0);
}

export function validateBakeResult(result) {
  if (!result || typeof result !== 'object' || Array.isArray(result)) throw new Error('Bake worker returned an invalid result object.');
  const size = result.resolution;
  if (!Number.isInteger(size) || size < 1 || size > 4096 || !(result.pixels instanceof Uint8Array || result.pixels instanceof Uint8ClampedArray) || result.pixels.length !== size * size * 4) throw new Error('Bake worker returned invalid RGBA pixels or resolution.');
  if (!Number.isInteger(result.covered) || !Number.isInteger(result.total) || result.total !== size * size || result.covered < 0 || result.covered > result.total || result.missedTexels !== result.total - result.covered || result.dilatedTexels != null && (!Number.isInteger(result.dilatedTexels) || result.dilatedTexels < 0 || result.dilatedTexels > result.covered) || result.islandCount != null && (!Number.isInteger(result.islandCount) || result.islandCount < 0 || result.islandCount > result.covered) || !Number.isFinite(result.coveredRatio) || result.coveredRatio < 0 || result.coveredRatio > 1 || Math.abs(result.coveredRatio - result.covered / result.total) > 1e-12) throw new Error('Bake worker returned invalid coverage statistics.');
  const difference = result.visualDifference;
  if (difference && (!Number.isFinite(difference.meanAbsoluteError) || !Number.isFinite(difference.maximumAbsoluteError) || !Number.isInteger(difference.comparedTexels) || difference.meanAbsoluteError < 0 || difference.maximumAbsoluteError < 0 || difference.meanAbsoluteError > 1 || difference.maximumAbsoluteError > 1 || difference.meanAbsoluteError > difference.maximumAbsoluteError || difference.comparedTexels < 0 || difference.comparedTexels > result.covered)) throw new Error('Bake worker returned invalid visual-difference statistics.');
  return result;
}

// Pack normalized or byte channel planes into an interleaved RGBA image.
// Missing RGB channels default to zero; missing alpha defaults to opaque.
export function packBakeChannels({ red = null, green = null, blue = null, alpha = null, resolution }) {
  const size = Number(resolution);
  if (!Number.isInteger(size) || size < 1 || size > 4096) throw new Error('Bake channel resolution must be an integer between 1 and 4096.');
  const count = size * size, output = new Uint8ClampedArray(count * 4);
  for (const [name, source] of [['red', red], ['green', green], ['blue', blue], ['alpha', alpha]]) {
    if (source && source.length !== count) throw new Error(`${name} bake channel must contain exactly one value per texel.`);
    if (source && hasInvalidValue(source, (value) => !Number.isFinite(value))) throw new Error(`${name} bake channel must contain finite values.`);
  }
  const channel = (source, index, fallback) => { if (!source) return fallback; const value = source[index]; return source instanceof Uint8Array || source instanceof Uint8ClampedArray ? value : clamp(value); };
  for (let i = 0; i < count; i++) { const o = i * 4; output[o] = channel(red, i, 0); output[o + 1] = channel(green, i, 0); output[o + 2] = channel(blue, i, 0); output[o + 3] = channel(alpha, i, 255); }
  return output;
}

// Resize an RGBA bake deterministically without relying on browser canvas APIs.
export function resizeBakeImage({ pixels, fromResolution, fromWidth = fromResolution, fromHeight = fromResolution, resolution, width = resolution, height = resolution, maxResolution = 4096, colorSpace = 'linear' }) {
  const sourceWidth = Number(fromWidth), sourceHeight = Number(fromHeight), targetRequestedWidth = Number(width), targetRequestedHeight = Number(height), limit = Number(maxResolution);
  if (![sourceWidth, sourceHeight, targetRequestedWidth, targetRequestedHeight, limit].every((value) => Number.isInteger(value) && value > 0)) throw new Error('Bake image dimensions and maximum resolution must be positive integers.');
  const targetWidth = Math.min(limit, targetRequestedWidth), targetHeight = Math.min(limit, targetRequestedHeight);
  if (!pixels || pixels.length !== sourceWidth * sourceHeight * 4 || hasInvalidValue(pixels, (value) => !Number.isFinite(value))) throw new Error('RGBA bake pixels must match source dimensions and contain finite values.');
  const transfer = resizeColorTransfer(colorSpace);
  if (sourceWidth === targetWidth && sourceHeight === targetHeight) return { pixels: new Uint8ClampedArray(pixels), width: targetWidth, height: targetHeight, resolution: targetWidth };
  const source = new Uint8ClampedArray(pixels), output = new Uint8ClampedArray(targetWidth * targetHeight * 4), scaleX = sourceWidth / targetWidth, scaleY = sourceHeight / targetHeight;
  for (let y = 0; y < targetHeight; y++) for (let x = 0; x < targetWidth; x++) {
    const sx = (x + .5) * scaleX - .5, sy = (y + .5) * scaleY - .5, x0 = Math.max(0, Math.floor(sx)), y0 = Math.max(0, Math.floor(sy)), x1 = Math.min(sourceWidth - 1, x0 + 1), y1 = Math.min(sourceHeight - 1, y0 + 1), tx = Math.max(0, Math.min(1, sx - x0)), ty = Math.max(0, Math.min(1, sy - y0)), wa = (1 - tx) * (1 - ty), wb = tx * (1 - ty), wd = (1 - tx) * ty, we = tx * ty, o = (y * targetWidth + x) * 4;
    for (let c = 0; c < 4; c++) { const a = source[(y0 * sourceWidth + x0) * 4 + c], b = source[(y0 * sourceWidth + x1) * 4 + c], d = source[(y1 * sourceWidth + x0) * 4 + c], e = source[(y1 * sourceWidth + x1) * 4 + c]; output[o + c] = transfer && c < 3 ? clamp(transfer.fromLinear(transfer.toLinear(a / 255) * wa + transfer.toLinear(b / 255) * wb + transfer.toLinear(d / 255) * wd + transfer.toLinear(e / 255) * we)) : Math.round(a * wa + b * wb + d * wd + e * we); }
  }
  return { pixels: output, width: targetWidth, height: targetHeight, resolution: targetWidth };
}

export function analyzeBakeAlpha({ pixels, resolution }) {
  const size = Number(resolution);
  if (!Number.isInteger(size) || size < 1 || size > 4096) throw new Error('RGBA bake resolution must be an integer between 1 and 4096.');
  const count = size * size;
  if (!pixels || pixels.length !== count * 4 || hasInvalidValue(pixels, (value) => !Number.isFinite(value))) throw new Error('RGBA bake pixels must match resolution and contain finite values.');
  let transparentTexels = 0;
  for (let i = 0; i < count; i++) if (pixels[i * 4 + 3] < 255) transparentTexels++;
  return { transparentTexels, coverageRatio: 1 - transparentTexels / count, allOpaque: transparentTexels === 0, allTransparent: transparentTexels === count, unusedAlpha: transparentTexels === 0 };
}

export function convertNormalMapY({ pixels, resolution, width = resolution, height = resolution, convention = 'opengl' }) {
  const imageWidth = Number(width), imageHeight = Number(height);
  if (!Number.isInteger(imageWidth) || imageWidth < 1 || !Number.isInteger(imageHeight) || imageHeight < 1) throw new Error('Normal-map image dimensions must be positive integers.');
  if (!['opengl', 'directx'].includes(String(convention).toLowerCase())) throw new Error(`Unsupported normal-map Y convention: ${convention}`);
  if (!pixels || pixels.length !== imageWidth * imageHeight * 4 || hasInvalidValue(pixels, (value) => !Number.isFinite(value))) throw new Error('Normal-map pixels do not match image dimensions and must contain finite values.');
  const output = new Uint8ClampedArray(pixels);
  if (String(convention).toLowerCase() === 'directx') for (let i = 0; i < output.length; i += 4) output[i + 1] = 255 - output[i + 1];
  return output;
}

// Encode vertex normals for a normal map. Tangent-space output follows the
// vec4 tangent convention used by Three.js/LightUSD: B = cross(N, T) * W.
// Object-space output intentionally keeps the legacy component values so the
// existing bake path remains byte-compatible.
export function encodeNormalVectors({ normals, tangents = null, space = 'object' } = {}) {
  if (!isIterableBuffer(normals) || normals.length % 3) throw new Error('Normal vectors must contain three components per vertex.');
  if (!['object', 'tangent'].includes(space)) throw new Error(`Unsupported normal-map space: ${space}`);
  if (space === 'tangent' && (!isIterableBuffer(tangents) || tangents.length !== normals.length / 3 * 4)) throw new Error('Tangent-space normal baking requires one vec4 tangent per normal.');
  const output = new Float32Array(normals.length);
  for (let i = 0; i < normals.length / 3; i++) {
    const no = i * 3, nx = Number(normals[no]), ny = Number(normals[no + 1]), nz = Number(normals[no + 2]);
    if (![nx, ny, nz].every(Number.isFinite)) throw new Error('Normal vectors must be finite.');
    if (space === 'object') { output[no] = nx * .5 + .5; output[no + 1] = ny * .5 + .5; output[no + 2] = nz * .5 + .5; continue; }
    const length = Math.hypot(nx, ny, nz); if (!(length > 1e-12)) throw new Error('Tangent-space normal baking requires non-zero normals.');
    const n = [nx / length, ny / length, nz / length], to = i * 4, rawTx = Number(tangents[to]), rawTy = Number(tangents[to + 1]), rawTz = Number(tangents[to + 2]), handedness = Number(tangents[to + 3]);
    if (![rawTx, rawTy, rawTz, handedness].every(Number.isFinite) || Math.abs(handedness) < .5) throw new Error('Tangent vectors must be finite and have a valid handedness.');
    const dot = n[0] * rawTx + n[1] * rawTy + n[2] * rawTz, tx = rawTx - n[0] * dot, ty = rawTy - n[1] * dot, tz = rawTz - n[2] * dot, tangentLength = Math.hypot(tx, ty, tz);
    if (!(tangentLength > 1e-12)) throw new Error('Tangent-space normal baking requires non-zero tangent directions.');
    const t = [tx / tangentLength, ty / tangentLength, tz / tangentLength], b = [(n[1] * t[2] - n[2] * t[1]) * Math.sign(handedness), (n[2] * t[0] - n[0] * t[2]) * Math.sign(handedness), (n[0] * t[1] - n[1] * t[0]) * Math.sign(handedness)], x = n[0] * t[0] + n[1] * t[1] + n[2] * t[2], y = n[0] * b[0] + n[1] * b[1] + n[2] * b[2], z = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    output[no] = x * .5 + .5; output[no + 1] = y * .5 + .5; output[no + 2] = z * .5 + .5;
  }
  return output;
}

export function encodeProjectedTangentNormals({ values, targetTriangle, targetBarycentrics, targetIndices, targetNormals, targetTangents } = {}) {
  const rayCount = values?.length / 3;
  if (!isIterableBuffer(values) || !Number.isInteger(rayCount) || !isIterableBuffer(targetTriangle) || targetTriangle.length !== rayCount || !isIterableBuffer(targetBarycentrics) || targetBarycentrics.length !== rayCount * 3 || !isIterableBuffer(targetIndices) || targetIndices.length % 3 || !isIterableBuffer(targetNormals) || targetNormals.length % 3 || !isIterableBuffer(targetTangents) || targetTangents.length !== targetNormals.length / 3 * 4) throw new Error('Projected tangent normal buffers have incompatible shapes.');
  const output = new Float32Array(values.length).fill(.5);
  for (let ray = 0; ray < rayCount; ray++) {
    const face = targetTriangle[ray], bo = ray * 3; if (!Number.isInteger(face) || face < 0 || face * 3 + 2 >= targetIndices.length) { output[bo + 2] = 1; continue; }
    const w = targetBarycentrics[bo], u = targetBarycentrics[bo + 1], v = targetBarycentrics[bo + 2], ids = [targetIndices[face * 3], targetIndices[face * 3 + 1], targetIndices[face * 3 + 2]];
    if (![w, u, v].every(Number.isFinite) || Math.abs(w + u + v - 1) > 1e-3 || ids.some((id) => !Number.isInteger(id) || id < 0 || id * 3 + 2 >= targetNormals.length)) { output[bo + 2] = 1; continue; }
    const weights = [w, u, v], normal = [0, 0, 0], tangent = [0, 0, 0], handedness = weights.reduce((sum, weight, corner) => sum + weight * targetTangents[ids[corner] * 4 + 3], 0);
    for (let corner = 0; corner < 3; corner++) { const no = ids[corner] * 3, to = ids[corner] * 4; for (let c = 0; c < 3; c++) { normal[c] += weights[corner] * targetNormals[no + c]; tangent[c] += weights[corner] * targetTangents[to + c]; } }
    const normalLength = Math.hypot(...normal), dot = normal[0] * tangent[0] + normal[1] * tangent[1] + normal[2] * tangent[2], tx = tangent[0] - normal[0] * dot / (normalLength * normalLength), ty = tangent[1] - normal[1] * dot / (normalLength * normalLength), tz = tangent[2] - normal[2] * dot / (normalLength * normalLength), tangentLength = Math.hypot(tx, ty, tz), source = [values[bo], values[bo + 1], values[bo + 2]], sourceLength = Math.hypot(...source);
    if (!(normalLength > 1e-12) || !(tangentLength > 1e-12) || !(sourceLength > 1e-12) || ![normalLength, tangentLength, sourceLength, handedness].every(Number.isFinite)) { output[bo + 2] = 1; continue; }
    const n = normal.map((value) => value / normalLength), t = [tx / tangentLength, ty / tangentLength, tz / tangentLength], sign = handedness < 0 ? -1 : 1, b = [(n[1] * t[2] - n[2] * t[1]) * sign, (n[2] * t[0] - n[0] * t[2]) * sign, (n[0] * t[1] - n[1] * t[0]) * sign], s = source.map((value) => value / sourceLength);
    output[bo] = (s[0] * t[0] + s[1] * t[1] + s[2] * t[2]) * .5 + .5; output[bo + 1] = (s[0] * b[0] + s[1] * b[1] + s[2] * b[2]) * .5 + .5; output[bo + 2] = (s[0] * n[0] + s[1] * n[1] + s[2] * n[2]) * .5 + .5;
  }
  return output;
}

export function rasterizeBaseColor({ positions, indices, uvs, colors = null, faceColors = null, materialColors = null, materialIndices = null, texturePixels = null, textureWidth = 0, textureHeight = 0, materialTextures = null, textureChannel = null, sourceColorSpace = 'srgb', sourceColorTransform = null, destinationColorSpace = 'srgb', color = [0.7, 0.7, 0.7], resolution = 512, dilation = 2, samples = 1 }) {
  if (!positions || !indices || !uvs || positions.length % 3 || uvs.length % 2 || indices.length % 3 || hasInvalidValue(positions, (value) => !Number.isFinite(value)) || hasInvalidValue(uvs, (value) => !Number.isFinite(value)) || hasInvalidValue(indices, (value) => !Number.isSafeInteger(value) || value < 0) || !Number.isSafeInteger(indices.length) || indices.length > INDEXED_MESH_MAX_INDICES) throw new Error('Bake geometry requires finite position/UV buffers and complete non-negative triangle indices.');
  let mesh;
  try {
    mesh = hasInvalidValue(indices, (value) => value >= positions.length / 3) ? { ...normalizeIndexedMesh({ positions, indices: new Uint32Array() }), indices: Uint32Array.from(indices) } : normalizeIndexedMesh({ positions, indices });
  } catch { throw new Error('Bake geometry requires finite position/UV buffers and complete non-negative triangle indices.'); }
  if (mesh.positions.length / 3 * 2 !== uvs.length) throw new Error('Bake geometry requires finite position/UV buffers and complete non-negative triangle indices.');
  const positionArray = mesh.positions, indexArray = mesh.indices, uvArray = new Float32Array(uvs), colorArray = colors ? new Float32Array(colors) : null, faceColorArray = faceColors ? new Float32Array(faceColors) : null, materialColorArray = materialColors ? new Float32Array(materialColors) : null, materialIndexArray = materialIndices ? new Uint32Array(materialIndices) : null, textureArray = texturePixels && textureWidth > 0 && textureHeight > 0 && texturePixels.length === textureWidth * textureHeight * 4 ? texturePixels : null, materialTextureArray = Array.isArray(materialTextures) ? materialTextures : null, size = Math.max(1, Math.min(4096, Math.floor(resolution))), pixels = new Uint8Array(size * size * 4);
  const rgb = color.map(clamp), covered = new Uint8Array(size * size), coveredIsland = new Int32Array(size * size).fill(-1); let coveredCount = 0, skippedFaces = 0, degenerateFaces = 0, rasterizedFaces = 0, differenceSum = 0, maximumDifference = 0, comparedTexels = 0;
  const faceCount = Math.floor(indexArray.length / 3);
  if (materialIndexArray && (materialIndices.length !== faceCount || hasInvalidValue(materialIndices, (value) => !Number.isSafeInteger(value) || value < 0))) throw new Error('Per-face material indices must contain one non-negative integer per triangle.');
  if (materialColorArray && (materialColorArray.length % 3 || hasInvalidValue(materialColorArray, (value) => !Number.isFinite(value)))) throw new Error('Material color buffers must contain finite RGB triplets.');
  if (materialTextureArray?.some((entry) => entry && (!Number.isInteger(entry.width) || !Number.isInteger(entry.height) || entry.width < 1 || entry.height < 1 || !entry.pixels || entry.pixels.length !== entry.width * entry.height * 4 || hasInvalidValue(entry.pixels, (value) => !Number.isFinite(value))))) throw new Error('Material texture buffers must contain valid dimensions and RGBA pixels.');
  const parents = Uint32Array.from({ length: faceCount }, (_, i) => i), find = (value) => { while (parents[value] !== value) { parents[value] = parents[parents[value]]; value = parents[value]; } return value; }, union = (a, b) => { a = find(a); b = find(b); if (a !== b) parents[b] = a; }, edges = buildIndexedEdgeUses(indexArray).edges;
  for (const entries of edges.values()) for (let index = 1; index < entries.length; index++) union(entries[index], entries[0]);
  forEachIndexedTriangle(indexArray, (faceIndex, a, b, c) => {
    const face = faceIndex * 3, ids = [a, b, c];
    if (ids.some((id) => id >= positionArray.length / 3 || id * 2 + 1 >= uvArray.length)) { skippedFaces++; return; }
    const uv = ids.map((id) => [uvArray[id * 2], uvArray[id * 2 + 1]]);
    if (!uv.flat().every(Number.isFinite)) { skippedFaces++; return; }
    const px = uv.map(([u]) => u * (size - 1)), py = uv.map(([, v]) => (1 - v) * (size - 1));
    const minX = Math.max(0, Math.floor(Math.min(...px))), maxX = Math.min(size - 1, Math.ceil(Math.max(...px))), minY = Math.max(0, Math.floor(Math.min(...py))), maxY = Math.min(size - 1, Math.ceil(Math.max(...py)));
    const denominator = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
    if (!Number.isFinite(denominator) || Math.abs(denominator) <= 1e-12) { degenerateFaces++; return; }
    rasterizedFaces++;
    // `face` is already the triangle ordinal. Dividing it by three aliases
    // unrelated triangles and makes dilation ownership depend on index-buffer
    // position rather than UV connectivity.
    const island = find(faceIndex);
    const grid = Math.max(1, Math.min(2, Math.round(Math.sqrt(Math.max(1, samples))))), sampleTotal = grid * grid;
    for (let y = minY; y <= maxY; y++) for (let x = minX; x <= maxX; x++) {
      let red = 0, green = 0, blue = 0, hits = 0;
      for (let sy = 0; sy < grid; sy++) for (let sx = 0; sx < grid; sx++) {
        const sampleX = x + (sx + .5) / grid - .5, sampleY = y + (sy + .5) / grid - .5;
        const a = ((py[1] - py[2]) * (sampleX - px[2]) + (px[2] - px[1]) * (sampleY - py[2])) / denominator;
        const b = ((py[2] - py[0]) * (sampleX - px[2]) + (px[0] - px[2]) * (sampleY - py[2])) / denominator, c = 1 - a - b;
        if (a < -1e-5 || b < -1e-5 || c < -1e-5) continue;
        const sampleU = a * uv[0][0] + b * uv[1][0] + c * uv[2][0], sampleV = a * uv[0][1] + b * uv[1][1] + c * uv[2][1];
        const materialIndex = materialIndexArray?.length === faceCount ? materialIndexArray[faceIndex] : 0;
        let texelColor = faceColorArray?.length >= (faceIndex + 1) * 3 ? [faceColorArray[faceIndex * 3], faceColorArray[faceIndex * 3 + 1], faceColorArray[faceIndex * 3 + 2]] : materialColorArray?.length >= (materialIndex + 1) * 3 ? [materialColorArray[materialIndex * 3], materialColorArray[materialIndex * 3 + 1], materialColorArray[materialIndex * 3 + 2]] : colorArray?.length === positionArray.length ? [a * colorArray[ids[0] * 3] + b * colorArray[ids[1] * 3] + c * colorArray[ids[2] * 3], a * colorArray[ids[0] * 3 + 1] + b * colorArray[ids[1] * 3 + 1] + c * colorArray[ids[2] * 3 + 1], a * colorArray[ids[0] * 3 + 2] + b * colorArray[ids[1] * 3 + 2] + c * colorArray[ids[2] * 3 + 2]] : color;
        const materialTexture = materialTextureArray?.[materialIndex], sampledTexture = materialTexture?.pixels || textureArray, sampledWidth = materialTexture?.width || textureWidth, sampledHeight = materialTexture?.height || textureHeight;
        if (sampledTexture && sampledWidth > 0 && sampledHeight > 0 && sampledTexture.length === sampledWidth * sampledHeight * 4) { const tx = Math.max(0, Math.min(sampledWidth - 1, Math.floor(Math.max(0, Math.min(1, sampleU)) * sampledWidth))), ty = Math.max(0, Math.min(sampledHeight - 1, Math.floor((1 - Math.max(0, Math.min(1, sampleV))) * sampledHeight))), textureOffset = (ty * sampledWidth + tx) * 4, textureScale = sampledTexture instanceof Uint8Array || sampledTexture instanceof Uint8ClampedArray ? 1 / 255 : 1; if (textureChannel == null) texelColor = convertBakeColor([sampledTexture[textureOffset] * textureScale, sampledTexture[textureOffset + 1] * textureScale, sampledTexture[textureOffset + 2] * textureScale], materialTexture?.colorSpace || sourceColorSpace, destinationColorSpace, materialTexture?.colorTransform || sourceColorTransform); else { const scalar = sampledTexture[textureOffset + Math.max(0, Math.min(3, Math.floor(textureChannel)))] * textureScale; texelColor = [scalar, scalar, scalar]; } }
        red += texelColor[0]; green += texelColor[1]; blue += texelColor[2]; hits++;
      }
      if (!hits) continue;
      const offset = (y * size + x) * 4;
      if (!covered[y * size + x]) { covered[y * size + x] = 1; coveredIsland[y * size + x] = island; coveredCount++; }
      const sourceColor = [red / hits, green / hits, blue / hits], encodedColor = sourceColor.map((value) => clamp(value)), outputColor = encodedColor.map((value) => value / 255);
      pixels[offset] = encodedColor[0]; pixels[offset + 1] = encodedColor[1]; pixels[offset + 2] = encodedColor[2]; pixels[offset + 3] = Math.round(255 * hits / sampleTotal);
      const error = Math.max(Math.abs(sourceColor[0] - outputColor[0]), Math.abs(sourceColor[1] - outputColor[1]), Math.abs(sourceColor[2] - outputColor[2]));
      differenceSum += (Math.abs(sourceColor[0] - outputColor[0]) + Math.abs(sourceColor[1] - outputColor[1]) + Math.abs(sourceColor[2] - outputColor[2])) / 3; maximumDifference = Math.max(maximumDifference, error); comparedTexels++;
    }
  });
  const passes = Math.max(0, Math.min(32, Math.floor(dilation))); let dilatedTexels = 0;
  for (let pass = 0; pass < passes; pass++) {
    const next = new Uint8Array(covered);
    for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
      const pixel = y * size + x; if (covered[pixel]) continue;
      const neighbors = [[x - 1, y], [x + 1, y], [x, y - 1], [x, y + 1]];
      const validNeighbors = neighbors.filter(([nx, ny]) => nx >= 0 && ny >= 0 && nx < size && ny < size && covered[ny * size + nx]);
      const source = validNeighbors.find(([nx, ny]) => { const owner = coveredIsland[ny * size + nx]; return validNeighbors.every(([ox, oy]) => coveredIsland[oy * size + ox] === owner); });
      if (!source) continue;
      const from = (source[1] * size + source[0]) * 4, to = pixel * 4;
      pixels.set(pixels.subarray(from, from + 4), to); next[pixel] = 1; coveredIsland[pixel] = coveredIsland[source[1] * size + source[0]]; coveredCount++; dilatedTexels++;
    }
    covered.set(next);
  }
  const islandCount = new Set([...coveredIsland].filter((island) => island >= 0)).size;
  return { pixels, resolution: size, covered: coveredCount, total: size * size, missedTexels: size * size - coveredCount, coveredRatio: coveredCount / (size * size), rasterizedFaces, skippedFaces, degenerateFaces, dilatedTexels, islandCount, visualDifference: { meanAbsoluteError: comparedTexels ? differenceSum / comparedTexels : 0, maximumAbsoluteError: maximumDifference, comparedTexels } };
}
