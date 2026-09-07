// SPDX-License-Identifier: Apache-2.0
import { DataUtils, FloatType, HalfFloatType } from 'three';
import { EXRLoader } from 'three/addons/loaders/EXRLoader.js';
import { parseMaterialX } from './graph.js';

/** Read with a streaming budget, including responses without Content-Length. */
export async function fetchResource(url, { fetcher = fetch, maxBytes = 32 * 1024 * 1024, signal } = {}) {
  const response = await fetcher(url, { signal, redirect: 'error' });
  if (!response.ok) throw new Error(`Resource ${url}: HTTP ${response.status}`);
  if (Number(response.headers.get('content-length')) > maxBytes) throw new Error('Resource exceeds byte budget');
  const reader = response.body?.getReader();
  if (!reader) throw new Error('Resource response has no readable body');
  const chunks = []; let size = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read(); if (done) break;
      size += value.byteLength;
      if (size > maxBytes) throw new Error('Resource exceeds byte budget');
      chunks.push(value);
    }
  } catch (e) { await reader.cancel().catch(() => {}); throw e; }
  finally { reader.releaseLock(); }
  const bytes = new Uint8Array(size); let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
  return bytes;
}

// Bound EXR dimensions before the third-party decoder allocates pixel buffers.
export function inspectEXR(bytes, maxPixels = 4 * 1024 * 1024) {
  return inspectEXRHeader(bytes, maxPixels).dimensions;
}

// Header-only EXR inspection. Pixel payload is never touched, so this remains
// safe for very large files. `colorSpace` is an authored header opinion only;
// filenames are deliberately not consulted.
export function inspectEXRHeader(bytes, maxPixels = 4 * 1024 * 1024) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < 9 || view.getUint32(0, true) !== 20000630) throw new Error('Invalid EXR signature');
  if (view.getUint32(4, true) & ~255) throw new Error('Only single-part scanline EXR files are supported');
  let offset = 8, dimensions, colorSpace;
  const string = () => {
    const start = offset;
    while (offset < bytes.length && bytes[offset] !== 0 && offset - start <= 255) offset++;
    if (offset >= bytes.length || offset - start > 255) throw new Error('Invalid EXR header string');
    return new TextDecoder().decode(bytes.subarray(start, offset++));
  };
  while (offset < bytes.length) {
    const name = string(); if (!name) break;
    const type = string();
    if (offset + 4 > bytes.length) throw new Error('Truncated EXR header');
    const size = view.getUint32(offset, true); offset += 4;
    if (size > bytes.length - offset) throw new Error('Truncated EXR attribute');
    if (name === 'dataWindow') {
      if (type !== 'box2i' || size !== 16 || dimensions) throw new Error('Invalid EXR data window');
      const width = view.getInt32(offset + 8, true) - view.getInt32(offset, true) + 1;
      const height = view.getInt32(offset + 12, true) - view.getInt32(offset + 4, true) + 1;
      if (width < 1 || height < 1 || width > 16384 || height > 16384 || width * height > maxPixels) throw new Error('EXR exceeds decoded pixel budget');
      dimensions = { width, height };
    }
    if (name === 'colorSpace' && type === 'string') {
      if (size > 1024 || bytes[offset + size - 1] !== 0) throw new Error('Invalid EXR colorSpace attribute');
      colorSpace = new TextDecoder().decode(bytes.subarray(offset, offset + size - 1));
    }
    offset += size;
  }
  if (!dimensions) throw new Error('EXR data window is missing');
  return { dimensions, colorSpace };
}

export async function decodeImage(bytes, { filename = '', colorspace, maxPixels = 4 * 1024 * 1024, allowDownsample = false } = {}) {
  if (/\.exr(?:$|[?#])/i.test(filename)) {
    const header = inspectEXRHeader(bytes, allowDownsample ? Number.MAX_SAFE_INTEGER : maxPixels), dimensions = header.dimensions;
    const oversized = dimensions.width * dimensions.height > maxPixels;
    if (oversized && !allowDownsample) throw new Error('EXR exceeds decoded pixel budget');
    const image = new EXRLoader().setDataType(oversized ? HalfFloatType : FloatType).parse(bytes.slice().buffer);
    if (image.width !== dimensions.width || image.height !== dimensions.height || image.data.length !== image.width * image.height * 4) throw new Error('Unexpected EXR decoded layout');
    if (oversized) {
      const scale = Math.sqrt((dimensions.width * dimensions.height) / maxPixels);
      const width = Math.max(1, Math.floor(dimensions.width / scale)), height = Math.max(1, Math.floor(dimensions.height / scale));
      const data = new Float32Array(width * height * 4), source = image.data;
      for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
        const sx = Math.min(dimensions.width - 1, Math.floor((x + .5) * dimensions.width / width));
        const sy = Math.min(dimensions.height - 1, Math.floor((y + .5) * dimensions.height / height));
        const si = (sy * dimensions.width + sx) * 4, di = (y * width + x) * 4;
        for (let c = 0; c < 4; c++) data[di + c] = DataUtils.fromHalfFloat(source[si + c]);
      }
      return { width, height, data, colorspace: colorspace || header.colorSpace || 'lin_rec709', exrColorSpace: header.colorSpace, resizedFrom: dimensions };
    }
    // EXRLoader returns bottom-up rows, matching MaterialX v=0.
    return { width: image.width, height: image.height, data: image.data, colorspace: colorspace || header.colorSpace || 'lin_rec709', exrColorSpace: header.colorSpace };
  }
  const bitmap = await createImageBitmap(new Blob([bytes]), { premultiplyAlpha: 'none', colorSpaceConversion: 'none', imageOrientation: 'none' });
  try {
    if (bitmap.width * bitmap.height > maxPixels) throw new Error('Image exceeds decoded pixel budget');
    const canvas = new OffscreenCanvas(bitmap.width, bitmap.height), context = canvas.getContext('2d', { willReadFrequently: true });
    context.drawImage(bitmap, 0, 0);
    const rgba = context.getImageData(0, 0, bitmap.width, bitmap.height).data;
    const data = new Float32Array(rgba.length);
    for (let y = 0; y < bitmap.height; y++) for (let x = 0; x < bitmap.width * 4; x++) data[y * bitmap.width * 4 + x] = rgba[(bitmap.height - 1 - y) * bitmap.width * 4 + x] / 255;
    return { width: bitmap.width, height: bitmap.height, data, colorspace: colorspace || 'srgb_texture' };
  } finally { bitmap.close(); }
}

/** Resolve authored image filenames without changing shader graph semantics. */
export async function loadMaterialXResources(url, options = {}) {
  const source = new URL(url, globalThis.location?.href).href;
  const origin = new URL(source).origin;
  const allowed = url => {
    const resolved = new URL(url);
    if (!['http:', 'https:'].includes(resolved.protocol) || (!options.allowCrossOrigin && resolved.origin !== origin)) throw new Error('Resource URL is outside the allowed origin');
    return resolved.href;
  };
  const loaded=new Map(),active=new Set();let documentBytes=0;
  async function load(source) {
    allowed(source);
    if(active.has(source))throw new Error('MaterialX include cycle');
    if(loaded.has(source))return loaded.get(source);
    if(loaded.size+active.size>=128)throw new Error('MaterialX include count exceeds budget');
    active.add(source);
    const remaining=(options.maxDocumentBytes || 16*1024*1024)-documentBytes;
    const bytes=await fetchResource(source,{...options,maxBytes:remaining});documentBytes+=bytes.length;
    const doc=parseMaterialX(new TextDecoder().decode(bytes),{source,parser:options.parser,allowIncludes:true});
    for(const href of doc.includes)await load(new URL(href,source).href);
    active.delete(source);loaded.set(source,doc);return doc;
  }
  const root=await load(source);
  const document={...root,nodes:[],definitions:Object.create(null),graphs:Object.create(null)};
  const names=new Set();
  for(const doc of loaded.values()) {
    for(const node of doc.nodes){if(names.has(node.name))throw new Error(`Duplicate included node: ${node.name}`);names.add(node.name);document.nodes.push(node);}
    for(const kind of ['definitions','graphs'])for(const [name,value]of Object.entries(doc[kind])){if(document[kind][name])throw new Error(`Duplicate included ${kind}: ${name}`);document[kind][name]=value;}
  }
  document.images = Object.create(null);
  const nodes = [...document.nodes, ...Object.values(document.graphs).flatMap(g => g.nodes)];
  let decodedBytes = 0;
  for (const node of nodes) {
    if (!['image', 'tiledimage'].includes(node.category)) continue;
    const file = node.inputs?.file;
    if (!file?.value || file.nodename || file.nodegraph || file.interfacename) continue;
    if (/<UDIM>|<UVTILE>/.test(file.value)) throw new Error('UDIM resource loading is not implemented');
    const resolved = new URL((node.fileprefix || '') + (file.fileprefix || '') + file.value, node.source || source);
    allowed(resolved.href);
    const colorspace = file.colorspace || node.colorspace || document.colorspace;
    const key = `${resolved.href}#colorspace=${colorspace || 'auto'}`;
    if (!document.images[key]) {
      const imageBytes = await fetchResource(resolved.href, options);
      const image = await decodeImage(imageBytes, { filename: resolved.href, colorspace, maxPixels: options.maxPixels });
      decodedBytes += image.data.byteLength;
      if (decodedBytes > (options.maxDecodedBytes || 48 * 1024 * 1024)) throw new Error('Material images exceed decoded byte budget');
      document.images[key] = image;
    }
    file.value = key;
  }
  document.resourceProvenance = { source, documents:[...loaded.keys()], textures: Object.keys(document.images) };
  return document;
}
