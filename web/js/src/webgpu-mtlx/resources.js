// SPDX-License-Identifier: Apache-2.0
import { DataUtils, FloatType, HalfFloatType } from 'three';
import { EXRLoader } from 'three/addons/loaders/EXRLoader.js';
import { parseMaterialX } from './graph.js';
import { normalizeColorSpace } from './color.js';

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

/** Build a bounded float RGBA atlas from decoded 1001-1100 UDIM tiles. */
export function atlasUDIMImages(tiles) {
  if (!Array.isArray(tiles) || tiles.length < 1 || tiles.length > 100) throw new Error('UDIM tile count exceeds budget');
  const first = tiles[0]?.image;
  if (!first || !Number.isInteger(first.width) || !Number.isInteger(first.height) || first.width < 1 || first.height < 1) throw new Error('Invalid UDIM tile image');
  const tileWidth = first.width, tileHeight = first.height;
  let columns = 1, rows = 1;
  for (const tile of tiles) {
    if (!Number.isInteger(tile.id) || tile.id < 1001 || tile.id > 1100 || tile.id % 1) throw new Error('Invalid UDIM tile id');
    if (tile.image.width !== tileWidth || tile.image.height !== tileHeight || tile.image.data.length !== tileWidth * tileHeight * 4) throw new Error('UDIM tiles must have matching dimensions');
    const u = (tile.id - 1001) % 10, v = Math.floor((tile.id - 1001) / 10);
    columns = Math.max(columns, u + 1); rows = Math.max(rows, v + 1);
  }
  const data = new Float32Array(tileWidth * columns * tileHeight * rows * 4);
  for (const tile of tiles) {
    const u = (tile.id - 1001) % 10, v = Math.floor((tile.id - 1001) / 10), source = tile.image.data;
    for (let y = 0; y < tileHeight; y++) {
      const sourceOffset = y * tileWidth * 4;
      const targetOffset = ((v * tileHeight + y) * tileWidth * columns + u * tileWidth) * 4;
      data.set(source.subarray(sourceOffset, sourceOffset + tileWidth * 4), targetOffset);
    }
  }
  return { width: tileWidth * columns, height: tileHeight * rows, data,
    colorspace: normalizeColorSpace(first.colorspace || 'lin_rec709'),
    udim: { columns, rows, tileIds: tiles.map(tile => tile.id) } };
}

// Bound EXR dimensions before the third-party decoder allocates pixel buffers.
export function inspectEXR(bytes, maxPixels = 4 * 1024 * 1024) {
  return inspectEXRHeader(bytes, maxPixels).dimensions;
}

/** Flatten document and nested nodegraph nodes for resource discovery. */
export function collectMaterialXNodes(document) {
  const nodes = [...(document?.nodes || [])];
  const visitGraph = graph => {
    nodes.push(...(graph?.nodes || []));
    for (const child of Object.values(graph?.graphs || {})) visitGraph(child);
  };
  for (const graph of Object.values(document?.graphs || {})) visitGraph(graph);
  return nodes;
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

// Decode only the source scanlines needed to build a bounded image. This is
// deliberately limited to uncompressed single-part scanline EXR: unlike the
// general EXRLoader path it never allocates a full-resolution pixel buffer.
function downsampleEXRScanlines(bytes, maxPixels) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) !== 20000630 || (view.getUint32(4, true) & ~255)) return null;
  let offset = 8, dimensions, colorSpace, compression = 255, channels = [];
  const readString = () => {
    const start = offset;
    while (offset < bytes.length && bytes[offset] !== 0) offset++;
    if (offset >= bytes.length) throw new Error('Invalid EXR header string');
    const value = new TextDecoder().decode(bytes.subarray(start, offset)); offset++; return value;
  };
  while (offset < bytes.length) {
    const name = readString(); if (!name) break;
    const type = readString();
    if (offset + 4 > bytes.length) throw new Error('Truncated EXR header');
    const size = view.getUint32(offset, true); offset += 4;
    if (size > bytes.length - offset) throw new Error('Truncated EXR attribute');
    if (name === 'dataWindow' && type === 'box2i' && size === 16) {
      const minX = view.getInt32(offset, true), minY = view.getInt32(offset + 4, true);
      const maxX = view.getInt32(offset + 8, true), maxY = view.getInt32(offset + 12, true);
      dimensions = { width: maxX - minX + 1, height: maxY - minY + 1, minX, minY };
    } else if (name === 'compression' && type === 'compression' && size >= 1) compression = bytes[offset];
    else if (name === 'colorSpace' && type === 'string' && size > 0 && bytes[offset + size - 1] === 0) colorSpace = new TextDecoder().decode(bytes.subarray(offset, offset + size - 1));
    else if (name === 'channels' && type === 'chlist') {
      let p = offset;
      while (p < offset + size && bytes[p]) {
        const start = p; while (p < offset + size && bytes[p]) p++;
        if (p >= offset + size || p + 17 > offset + size) return null;
        const channel = new TextDecoder().decode(bytes.subarray(start, p)); p++;
        const pixelType = view.getInt32(p, true); p += 4;
        p += 4; // pLinear and reserved bytes
        const xSampling = view.getInt32(p, true), ySampling = view.getInt32(p + 4, true); p += 8;
        channels.push({ name: channel, pixelType, xSampling, ySampling });
      }
    }
    offset += size;
  }
  if (!dimensions || compression !== 0 || !channels.length) return null;
  const supported = channels.length === 3 && channels.every(c => (c.name === 'R' || c.name === 'G' || c.name === 'B' || c.name === 'X' || c.name === 'Y' || c.name === 'Z') && c.xSampling === 1 && c.ySampling === 1 && (c.pixelType === 1 || c.pixelType === 2));
  if (!supported || ![['R','G','B'], ['X','Y','Z']].some(names => names.every(n => channels.some(c => c.name === n)))) return null;
  const width = dimensions.width, height = dimensions.height;
  if (width * height <= maxPixels) return null;
  const scale = Math.sqrt((width * height) / maxPixels);
  const outWidth = Math.max(1, Math.floor(width / scale)), outHeight = Math.max(1, Math.floor(height / scale));
  const data = new Float32Array(outWidth * outHeight * 4), weights = new Float64Array(outWidth * outHeight);
  const tableStart = offset, tableBytes = height * 8;
  if (tableStart + tableBytes > bytes.length) throw new Error('Truncated EXR scanline table');
  const offsets = new Array(height);
  for (let y = 0; y < height; y++) {
    const value = view.getBigUint64(tableStart + y * 8, true);
    if (value > BigInt(bytes.length - 8)) throw new Error('Invalid EXR scanline offset');
    offsets[y] = Number(value);
  }
  const ordered = channels.slice().sort((a, b) => a.name.localeCompare(b.name));
  const channelIndex = new Map(ordered.map((c, i) => [c.name, i]));
  for (let sy = 0; sy < height; sy++) {
    const row = offsets[sy];
    if (row + 8 > bytes.length) throw new Error('Truncated EXR scanline');
    const lineY = view.getInt32(row, true), packed = view.getUint32(row + 4, true), payload = row + 8;
    if (payload + packed > bytes.length || lineY < dimensions.minY || lineY >= dimensions.minY + height) throw new Error('Invalid EXR scanline');
    const sourceY = lineY - dimensions.minY, sy0 = sourceY * outHeight / height, sy1 = (sourceY + 1) * outHeight / height;
    const oy0 = Math.floor(sy0), oy1 = Math.min(outHeight - 1, Math.ceil(sy1) - 1);
    const rowValues = [new Float32Array(width), new Float32Array(width), new Float32Array(width)];
    let p = payload;
    for (const channel of ordered) {
      const target = channel.name === 'R' || channel.name === 'X' ? 0 : channel.name === 'G' || channel.name === 'Y' ? 1 : 2;
      const bytesPer = channel.pixelType === 1 ? 2 : 4;
      if (p + width * bytesPer > payload + packed) throw new Error('Truncated EXR channel data');
      for (let x = 0; x < width; x++) { rowValues[target][x] = channel.pixelType === 1 ? DataUtils.fromHalfFloat(view.getUint16(p, true)) : view.getFloat32(p, true); p += bytesPer; }
    }
    for (let oy = oy0; oy <= oy1; oy++) {
      const wy = Math.min(sy1, oy + 1) - Math.max(sy0, oy); if (wy <= 0) continue;
      const sxScale = outWidth / width;
      for (let x = 0; x < width; x++) {
        const sx0 = x * sxScale, sx1 = (x + 1) * sxScale, ox0 = Math.floor(sx0), ox1 = Math.min(outWidth - 1, Math.ceil(sx1) - 1);
        for (let ox = ox0; ox <= ox1; ox++) {
          const wx = Math.min(sx1, ox + 1) - Math.max(sx0, ox); if (wx <= 0) continue;
          const w = wx * wy, i = oy * outWidth + ox, d = i * 4; weights[i] += w;
          data[d] += rowValues[0][x] * w; data[d + 1] += rowValues[1][x] * w; data[d + 2] += rowValues[2][x] * w;
        }
      }
    }
  }
  for (let i = 0; i < weights.length; i++) { const d = i * 4, w = weights[i] || 1; data[d] /= w; data[d + 1] /= w; data[d + 2] /= w; data[d + 3] = 1; }
  return { width: outWidth, height: outHeight, data, colorspace: normalizeColorSpace(colorSpace || 'lin_rec709'), exrColorSpace: colorSpace, resizedFrom: { width, height } };
}

export async function decodeImage(bytes, { filename = '', colorspace, maxPixels = 4 * 1024 * 1024, allowDownsample = false } = {}) {
  if (/\.exr(?:$|[?#])/i.test(filename)) {
    const header = inspectEXRHeader(bytes, allowDownsample ? Number.MAX_SAFE_INTEGER : maxPixels), dimensions = header.dimensions;
    const oversized = dimensions.width * dimensions.height > maxPixels;
    if (oversized && !allowDownsample) throw new Error('EXR exceeds decoded pixel budget');
    if (oversized && allowDownsample) {
      const streamed = downsampleEXRScanlines(bytes, maxPixels);
      if (streamed) {
        streamed.colorspace = normalizeColorSpace(colorspace || header.colorSpace || streamed.colorspace || 'lin_rec709');
        streamed.exrColorSpace = header.colorSpace;
        return streamed;
      }
    }
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
      return { width, height, data, colorspace: normalizeColorSpace(colorspace || header.colorSpace || 'lin_rec709'), exrColorSpace: header.colorSpace, resizedFrom: dimensions };
    }
    // EXRLoader returns bottom-up rows, matching MaterialX v=0.
    return { width: image.width, height: image.height, data: image.data, colorspace: normalizeColorSpace(colorspace || header.colorSpace || 'lin_rec709'), exrColorSpace: header.colorSpace };
  }
  const bitmap = await createImageBitmap(new Blob([bytes]), { premultiplyAlpha: 'none', colorSpaceConversion: 'none', imageOrientation: 'none' });
  try {
    const oversized = bitmap.width * bitmap.height > maxPixels;
    if (oversized && !allowDownsample) throw new Error('Image exceeds decoded pixel budget');
    const scale = oversized ? Math.sqrt((bitmap.width * bitmap.height) / maxPixels) : 1;
    const width = oversized ? Math.max(1, Math.floor(bitmap.width / scale)) : bitmap.width;
    const height = oversized ? Math.max(1, Math.floor(bitmap.height / scale)) : bitmap.height;
    const canvas = new OffscreenCanvas(width, height), context = canvas.getContext('2d', { willReadFrequently: true });
    context.drawImage(bitmap, 0, 0, width, height);
    const rgba = context.getImageData(0, 0, width, height).data;
    const data = new Float32Array(rgba.length);
    for (let y = 0; y < height; y++) for (let x = 0; x < width * 4; x++) data[y * width * 4 + x] = rgba[(height - 1 - y) * width * 4 + x] / 255;
    return { width, height, data, colorspace: colorspace || 'srgb_texture', ...(oversized ? { resizedFrom: { width: bitmap.width, height: bitmap.height } } : {}) };
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
  const nodes = collectMaterialXNodes(document);
  let decodedBytes = 0;
  for (const node of nodes) {
    if (!['image', 'tiledimage', 'triplanarprojection', 'UsdUVTexture', 'usduvtexture', 'latlongimage'].includes(node.category)) continue;
    const fileNames = node.category === 'triplanarprojection' ? ['filex', 'filey', 'filez'] : ['file'];
    for (const fileName of fileNames) {
      const file = node.inputs?.[fileName];
      if (!file?.value || file.nodename || file.nodegraph || file.interfacename) continue;
      const resolved = new URL((node.fileprefix || '') + (file.fileprefix || '') + file.value, node.source || source);
      allowed(resolved.href);
      const colorspace = file.colorspace || node.colorspace || document.colorspace;
      const key = `${resolved.href}#colorspace=${colorspace || 'auto'}`;
      if (!document.images[key]) {
        const udim = /<UDIM>|<UVTILE>|%04d|%\(UDIM\)d/i.test(file.value);
        if (udim) {
          const tiles = [];
          for (let id = 1001; id <= 1100; id++) {
            const u = (id - 1001) % 10, v = Math.floor((id - 1001) / 10);
            const tileName = file.value.replace(/<UDIM>/ig, String(id)).replace(/<UVTILE>/ig, `u${u + 1}_v${v + 1}`).replace(/%04d/ig, String(id).padStart(4, '0')).replace(/%\(UDIM\)d/ig, String(id));
            const tileURL = new URL((node.fileprefix || '') + (file.fileprefix || '') + tileName, node.source || source);
            allowed(tileURL.href);
            try {
              const imageBytes = await fetchResource(tileURL.href, options);
              const image = await decodeImage(imageBytes, { filename: tileURL.href, colorspace, maxPixels: options.maxPixels, allowDownsample: options.allowDownsample === true });
              tiles.push({ id, image });
            } catch (error) {
              if (!/HTTP 404\b/.test(String(error?.message || error))) throw error;
            }
          }
          if (!tiles.length) throw new Error(`No UDIM tiles found for ${file.value}`);
          const image = atlasUDIMImages(tiles);
          decodedBytes += image.data.byteLength;
          if (decodedBytes > (options.maxDecodedBytes || 48 * 1024 * 1024)) throw new Error('Material images exceed decoded byte budget');
          document.images[key] = image;
        } else {
          const imageBytes = await fetchResource(resolved.href, options);
          const image = await decodeImage(imageBytes, { filename: resolved.href, colorspace, maxPixels: options.maxPixels, allowDownsample: options.allowDownsample === true });
          decodedBytes += image.data.byteLength;
          if (decodedBytes > (options.maxDecodedBytes || 48 * 1024 * 1024)) throw new Error('Material images exceed decoded byte budget');
          document.images[key] = image;
        }
      }
      file.value = key;
    }
  }
  document.resourceProvenance = { source, documents:[...loaded.keys()], textures: Object.keys(document.images) };
  return document;
}
